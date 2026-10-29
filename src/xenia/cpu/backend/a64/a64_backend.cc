/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_backend.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif
#if XE_PLATFORM_MAC
#include <pthread.h>
#endif
#include "xenia/cpu/backend/a64/a64_assembler.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/backend/vrsqrte_table.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

DEFINE_int64(a64_max_stackpoints, 65536,
             "Max number of host->guest stack mappings we can record.", "a64");

DEFINE_bool(a64_enable_host_guest_stack_synchronization, true,
            "Records entries for guest/host stack mappings at function starts "
            "and checks for reentry at return sites. Has slight performance "
            "impact, but fixes crashes in games that use setjmp/longjmp.",
            "a64");

DECLARE_bool(record_mmio_access_exceptions);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

// Resolve a guest function at runtime. Called by the resolve thunk when
// a guest address has not yet been compiled.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

uint32_t FindStackpointSyncDepth(const A64BackendStackpoint* stackpoints,
                                 uint32_t current_depth, uint32_t guest_sp) {
  if (!stackpoints || current_depth == 0) {
    return 0;
  }

  uint32_t idx = current_depth - 1;
  uint32_t frames_skipped = 0;
  while (idx != 0xFFFFFFFFu && guest_sp > stackpoints[idx].guest_stack_) {
    --idx;
    ++frames_skipped;
  }

  // >1 frames skipped = real longjmp, not an early SP restore.
  if (idx == 0xFFFFFFFFu || frames_skipped <= 1) {
    return 0;
  }
  return idx + 1;
}

// ==========================================================================
// A64HelperEmitter — generates thunks using xbyak_aarch64.
// ==========================================================================
class A64HelperEmitter : public A64Emitter {
 public:
  A64HelperEmitter(A64Backend* backend, XbyakA64Allocator* allocator);

  HostToGuestThunk EmitHostToGuestThunk();
  GuestToHostThunk EmitGuestToHostThunk();
  GuestToHostThunk EmitGuestToHostThunkNoVec();
  ResolveFunctionThunk EmitResolveFunctionThunk();
  void* EmitGuestAndHostSynchronizeStackHelper();
  void* EmitReturnToHostHelper();
  void* EmitVRsqrtefpHelper(void** out_vector_entry);
  void* EmitFrsqrteHelper();

 private:
  // Clobbers x11. Installs the AAPCS64 default FPCR for host code.
  void EmitEnterHostFpcr();
  // Clobbers x11/x12. Reads FPCR back first and skips the msr, which is
  // context-synchronizing, when it already holds fpcr_fpu.
  void EmitRestoreFpuFpcr();
};

A64HelperEmitter::A64HelperEmitter(A64Backend* backend,
                                   XbyakA64Allocator* allocator)
    : A64Emitter(backend, allocator) {}

void A64HelperEmitter::EmitEnterHostFpcr() {
  // Host C code runs with the AAPCS64 default FPCR (round to nearest, no
  // flush-to-zero), not the guest's scalar mode: mtfsf can put rounding
  // controls or FPSCR.NI (FZ) in fpcr_fpu, which host code must not inherit.
  Xbyak_aarch64::Label host_fpcr_ready;
  mrs(x11, 3, 3, 4, 4, 0);  // mrs x11, FPCR
  cbz(x11, host_fpcr_ready);
  msr(3, 3, 4, 4, 0, xzr);
  L(host_fpcr_ready);
}

void A64HelperEmitter::EmitRestoreFpuFpcr() {
  Xbyak_aarch64::Label fpcr_unchanged;
  ldr(w11,
      ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  mrs(x12, 3, 3, 4, 4, 0);  // mrs x12, FPCR
  cmp(w11, w12);
  b(Xbyak_aarch64::EQ, fpcr_unchanged);
  msr(3, 3, 4, 4, 0, x11);
  L(fpcr_unchanged);
}

// --------------------------------------------------------------------------
// HostToGuestThunk
// --------------------------------------------------------------------------
// Called from host C++ code to enter JIT'd guest code.
//   x0 = target machine code address
//   x1 = PPCContext* (arg0)
//   x2 = return address value (arg1)
//
// ARM64 AAPCS64 calling convention:
//   Caller-saved: x0-x18, v0-v7, v16-v31
//   Callee-saved: x19-x28, x29(FP), x30(LR), d8-d15
//
// We save all callee-saved regs, set up context (x20) and membase (x21),
// then call the target. On return, restore and return to host.
HostToGuestThunk A64HelperEmitter::EmitHostToGuestThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // Allocate thunk stack frame.
  // Save x29(FP) and x30(LR) first, then callee-saved GPRs and NEON regs.
  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;

  // sub sp, sp, #thunk_stack
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save callee-saved GPRs: x19-x28, x29, x30
  stp(x19, x20, ptr(sp, 0x00));
  stp(x21, x22, ptr(sp, 0x10));
  stp(x23, x24, ptr(sp, 0x20));
  stp(x25, x26, ptr(sp, 0x30));
  stp(x27, x28, ptr(sp, 0x40));
  stp(x29, x30, ptr(sp, 0x50));

  // Save callee-saved NEON regs: full q8-q15 (JIT uses all 128 bits).
  stp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x60));
  stp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x80));
  stp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0xA0));
  stp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0xC0));

  code_offsets.body = getSize();

  // Set up guest execution state.
  // x20 = context (PPCContext*)
  mov(x20, x1);
  // x19 = backend context (immediately before PPCContext in memory)
  sub(x19, x20, static_cast<uint32_t>(sizeof(A64BackendContext)));
  // x21 = virtual_membase (loaded from context)
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
  // Keep the host FPCR for the return, and enter in the guest's scalar mode.
  mrs(x12, 3, 3, 4, 4, 0);  // mrs x12, FPCR
  str(x12, ptr(sp, static_cast<uint32_t>(StackLayout::THUNK_HOST_FPCR)));
  EmitRestoreFpuFpcr();
  // x0 still holds target, x2 holds return address.
  // The guest function's prolog stores x0 to GUEST_RET_ADDR on its stack
  // frame. Move the target to a scratch reg and put the guest return
  // address into x0.
  mov(x9, x0);  // x9 = target (scratch reg)
  // Pass guest return address in x0 (convention for guest function entry).
  mov(x0, x2);  // x0 = guest return address

  // Call the guest function.
  blr(x9);

  // Host code after the guest returns runs in the host's FPCR, not the guest's.
  Xbyak_aarch64::Label host_fpcr_restored;
  ldr(x11, ptr(sp, static_cast<uint32_t>(StackLayout::THUNK_HOST_FPCR)));
  mrs(x12, 3, 3, 4, 4, 0);  // mrs x12, FPCR
  cmp(x11, x12);
  b(Xbyak_aarch64::EQ, host_fpcr_restored);
  msr(3, 3, 4, 4, 0, x11);
  L(host_fpcr_restored);

  code_offsets.epilog = getSize();

  // Restore callee-saved NEON regs (full q8-q15).
  ldp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0xC0));
  ldp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0xA0));
  ldp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x80));
  ldp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x60));

  // Restore callee-saved GPRs.
  ldp(x29, x30, ptr(sp, 0x50));
  ldp(x27, x28, ptr(sp, 0x40));
  ldp(x25, x26, ptr(sp, 0x30));
  ldp(x23, x24, ptr(sp, 0x20));
  ldp(x21, x22, ptr(sp, 0x10));
  ldp(x19, x20, ptr(sp, 0x00));

  // Deallocate stack.
  add(sp, sp, static_cast<uint32_t>(thunk_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;
  func_info.is_host_to_guest_thunk = true;
  func_info.lr_save_offset = 0x058;  // stp x29, x30, [sp, #0x50]

  void* fn = Emplace(func_info);
  return reinterpret_cast<HostToGuestThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestToHostThunk
// --------------------------------------------------------------------------
// Called from guest JIT code to transition into a host (C++) function.
//   x0 = target host function
//   x1 = arg0
//   x2 = arg1
//
// We save volatile guest registers that we need to preserve across the
// host call, then call the host function with context as the first arg.
GuestToHostThunk A64HelperEmitter::EmitGuestToHostThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // The guest JIT uses v4-v15, v16-v31 as allocatable VEC regs.
  // v0-v7, v16-v31 are caller-saved in AAPCS64 (fully clobbered by C).
  // v8-v15 lower 64 bits are callee-saved, but upper 64 bits are not.
  // We must save all guest-allocated VEC regs (full 128-bit Q regs).
  // GPRs x19-x28 are callee-saved in AAPCS64, so the C function preserves them.
  //
  // Stack layout:
  //   q4, q5       sp + 0x000  (32 bytes)
  //   q6, q7       sp + 0x020
  //   q8, q9       sp + 0x040
  //   q10, q11     sp + 0x060
  //   q12, q13     sp + 0x080
  //   q14, q15     sp + 0x0A0
  //   q16, q17     sp + 0x0C0
  //   q18, q19     sp + 0x0E0
  //   q20, q21     sp + 0x100
  //   q22, q23     sp + 0x120
  //   q24, q25     sp + 0x140
  //   q26, q27     sp + 0x160
  //   q28, q29     sp + 0x180
  //   q30, q31     sp + 0x1A0
  //   x29, x30     sp + 0x1C0
  //   Total: 0x1D0 = 464 bytes (16-byte aligned)
  const size_t g2h_stack = 464;
  // x30's slot in the x29/x30 pair at the top of the frame.
  constexpr int32_t lr_offset = 0x1C8;
  static_assert(g2h_stack - lr_offset == StackLayout::THUNK_CALLER_RET_ADDR);
  sub(sp, sp, static_cast<uint32_t>(g2h_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save guest-allocated VEC regs (full Q = 128-bit).
  stp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));
  stp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  stp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x040));
  stp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x060));
  stp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0x080));
  stp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0x0A0));
  stp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x0C0));
  stp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x0E0));
  stp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x100));
  stp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x120));
  stp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x140));
  stp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x160));
  stp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x180));
  stp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x1A0));
  // Save x29/x30 (FP/LR).
  stp(x29, x30, ptr(sp, lr_offset - 8));

  code_offsets.body = getSize();

  EmitEnterHostFpcr();

  // Call host function.
  // AAPCS64: x0=first arg. We set x0=context (from x20).
  mov(x9, x0);   // x9 = target function (scratch)
  mov(x0, x20);  // x0 = PPCContext* (our context reg)
  // x1, x2, x3 already hold args from the caller.
  blr(x9);

  // Host callbacks may change FPCR. Restore the guest scalar FPCR before
  // resuming the JIT so later guest ops observe the cached PPC mode.
  // x19 (backend context) is callee-saved, so it survives the host call.
  // x0 holds the host return value here and must not be touched.
  EmitRestoreFpuFpcr();

  code_offsets.epilog = getSize();

  // Restore.
  ldp(x29, x30, ptr(sp, lr_offset - 8));
  ldp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x1A0));
  ldp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x180));
  ldp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x160));
  ldp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x140));
  ldp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x120));
  ldp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x100));
  ldp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x0E0));
  ldp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x0C0));
  ldp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0x0A0));
  ldp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0x080));
  ldp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x060));
  ldp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x040));
  ldp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  ldp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));

  add(sp, sp, static_cast<uint32_t>(g2h_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = g2h_stack;
  func_info.lr_save_offset = lr_offset;

  void* fn = Emplace(func_info);
  return reinterpret_cast<GuestToHostThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestToHostThunkNoVec
// --------------------------------------------------------------------------
// GuestToHostThunk without the q4-q31 save/restore, for CallExtern. No HIR
// value is live in a vector register across a call: guest-to-guest calls are
// a bare blr into a callee that uses q4-q31 freely, and ContextPromotionPass
// stops at the first volatile instruction. CallExtern checks and takes the
// full thunk where a value is live anyway. CallNativeSafe, which sequences
// emit with operands in flight, always takes the full thunk.
GuestToHostThunk A64HelperEmitter::EmitGuestToHostThunkNoVec() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  const size_t g2h_stack = 16;  // x29/x30 only
  constexpr int32_t lr_offset = 0x08;
  static_assert(g2h_stack - lr_offset == StackLayout::THUNK_CALLER_RET_ADDR);
  sub(sp, sp, static_cast<uint32_t>(g2h_stack));
  code_offsets.prolog_stack_alloc = getSize();
  stp(x29, x30, ptr(sp, lr_offset - 8));

  code_offsets.body = getSize();

  EmitEnterHostFpcr();
  mov(x9, x0);   // x9 = target function (scratch)
  mov(x0, x20);  // x0 = PPCContext*
  blr(x9);
  EmitRestoreFpuFpcr();

  code_offsets.epilog = getSize();

  ldp(x29, x30, ptr(sp, lr_offset - 8));
  add(sp, sp, static_cast<uint32_t>(g2h_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = g2h_stack;
  func_info.lr_save_offset = lr_offset;

  void* fn = Emplace(func_info);
  return reinterpret_cast<GuestToHostThunk>(fn);
}

// --------------------------------------------------------------------------
// ResolveFunctionThunk
// --------------------------------------------------------------------------
// Called when guest code calls an unresolved function address.
// The indirection table initially points all entries here.
// We call ResolveFunction to compile/lookup the target, then jump to it.
//
// On entry from the indirection table:
//   w16 = guest PPC address (loaded by the call sequence)
//   x20 = context
//   x30 = return address (from the BLR that got us here)
ResolveFunctionThunk A64HelperEmitter::EmitResolveFunctionThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save x29/x30 and x0 (guest return address, needed by the resolved
  // function's prolog). x19 is callee-saved so it survives the C call.
  stp(x29, x30, ptr(sp, 0x50));
  stp(x0, x19, ptr(sp, 0x00));  // save x0 (guest ret addr) and x19

  code_offsets.body = getSize();

  // Call ResolveFunction(context, target_address).
  mov(x0, x20);  // x0 = PPCContext*
  mov(x1, x16);  // x1 = guest address (32-bit in w16)
  // Load address of ResolveFunction.
  mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
  blr(x9);
  // x0 now holds the resolved host machine code address.
  mov(x9, x0);

  // The resolved function is entered in the scalar FPU mode, and the host
  // resolve may have changed FPCR.
  EmitRestoreFpuFpcr();

  code_offsets.epilog = getSize();

  // Restore x0 (guest return address) and saved regs.
  ldp(x0, x19, ptr(sp, 0x00));
  ldp(x29, x30, ptr(sp, 0x50));
  add(sp, sp, static_cast<uint32_t>(thunk_stack));

  cbz(x9, 8);   // skip br x9 if null, fall through to brk
  br(x9);       // Jump to the resolved function (tail call — preserves LR).
  brk(0xF000);  // Resolution failed — trap for debugging.

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;
  func_info.lr_save_offset = 0x058;  // stp x29, x30, [sp, #0x50]

  void* fn = Emplace(func_info);
  return reinterpret_cast<ResolveFunctionThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestAndHostSynchronizeStackHelper
// --------------------------------------------------------------------------
// Called when ResolveFunction detected a longjmp return-site reentry. Restores
// the host SP for the existing frame and jumps back to the caller.
//
// On entry (set by the tail-emitted sync check in the guest function):
//   x8  = return address (where to jump after fixup)
//   x19 = A64BackendContext*
void* A64HelperEmitter::EmitGuestAndHostSynchronizeStackHelper() {
  using namespace Xbyak_aarch64;
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // x19 = backend context pointer (already set up by HostToGuestThunk)

  // x10 = stackpoints array pointer
  ldr(x10, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, stackpoints))));
  // w11 = current_stackpoint_depth
  ldr(w11, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));

  // w13 = target depth computed by ResolveFunction.
  ldr(w13, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  auto& underflow = NewCachedLabel();

  cbz(x10, underflow);
  // A zero target means this helper was called without a pending repair.
  cbz(w13, underflow);
  // The pending target must not be deeper than the current live depth.
  cmp(w13, w11);
  b(HI, underflow);

  // x14 = &stackpoints[target_depth - 1]
  sub(w13, w13, 1);

  mov(w14, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
  umull(x14, w13, w14);
  add(x14, x10, x14);

  // Restore host SP from stackpoints[index].host_stack_. A64 stackpoints are
  // recorded after the function frame allocation, so this is already the SP
  // expected by the return-site code.
  ldr(x16, ptr(x14, static_cast<uint32_t>(
                        offsetof(A64BackendStackpoint, host_stack_))));
  mov(sp, x16);

  // Update current_stackpoint_depth = index + 1
  // (the entry we restored to has been consumed)
  add(w13, w13, 1);
  str(w13, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));
  mov(w15, 0);
  str(w15, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));

  // Jump back to the caller.
  br(x8);

  L(underflow);
  // Should be impossible — stackpoint array underflowed.
  brk(0xF001);  // assertion failure

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  return Emplace(func_info);
}

// --------------------------------------------------------------------------
// ReturnToHostHelper
// --------------------------------------------------------------------------
// Branched to in place of a dynamic code target that is the host return
// address. ResolveHostReturn recorded the frame host code entered, which
// returns there as its epilog would.
void* A64HelperEmitter::EmitReturnToHostHelper() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  ldr(w9, ptr(x19, static_cast<uint32_t>(offsetof(
                       A64BackendContext, host_return_stackpoint_depth))));
  str(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
  ldr(x30, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, host_return_address))));
  ldr(x9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, host_return_stack))));
  mov(sp, x9);
  ret();

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  return Emplace(func_info);
}

// --------------------------------------------------------------------------
// vrsqrtefp
// --------------------------------------------------------------------------
// One blob with two entry points:
//   scalar (the returned pointer): w0 = float bits in, w0 = result out
//   vector (*out_vector_entry):    v0.4s in, v0.4s out
//
// The scalar entry clobbers w0-w5 and touches no vector register; the vector
// entry adds v0-v2 and x10. Both stay inside the scratch set the register
// allocator never hands out (x0-x18, v0-v3), so guest values in v4-v31 survive
// without EmitGuestToHostThunk's 28-register spill.
void* A64HelperEmitter::EmitVRsqrtefpHelper(void** out_vector_entry) {
  using namespace Xbyak_aarch64;
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  Label interpolate_setup, slow, exp_nonzero, check_negative;
  Label signed_inf, quiet_nan, ret_qnan, oddball, table;
  Label lane;

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  L(lane);
  // Positive normals are a straight lookup: the table holds the result for a
  // canonical exponent, and the real exponent is a subtract on top of it.
  ubfx(w1, w0, 23, 9);  // sign:exponent
  sub(w1, w1, 1);
  cmp(w1, 253);
  b(HI, slow);
  ubfx(w1, w0, 9, 15);
  mov(x2, reinterpret_cast<uint64_t>(GetNormalVRsqrteTable()));
  ldr(w2, ptr(x2, x1, LSL, 2));
  lsr(w0, w0, 24);
  sub(w0, w0, 63);
  sub(w0, w2, w0, LSL, 23);
  ret();

  L(slow);
  and_(w1, w0, 0x7FFFFF);  // mantissa
  ubfx(w2, w0, 23, 8);     // biased exponent
  cbnz(w2, exp_nonzero);
  cbz(w1, signed_inf);  // +-0 -> +-inf
  // Denormal. With NJM on it flushes to +-0 first and takes the same path.
  ldr(w3, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
  tbnz(w3, kA64BackendNJMOn, signed_inf);
  eor(w3, w1, 0x400000);
  cbz(w3, oddball);
  tbnz(w0, 31, ret_qnan);
  // Renormalize into the same (mantissa, biased exponent) form a normal has.
  clz(w3, w1);
  sub(w4, w3, 8);
  lsl(w1, w0, w4);
  and_(w1, w1, 0x7FFFFE);
  mov(w4, 9);
  sub(w2, w4, w3);
  b(interpolate_setup);

  L(exp_nonzero);
  cmp(w2, 255);
  b(NE, check_negative);
  cbnz(w1, quiet_nan);     // NaN -> quiet it, keeping sign and payload
  tbnz(w0, 31, ret_qnan);  // -inf -> QNaN
  mov(w0, 0);              // +inf -> +0
  ret();

  L(check_negative);
  tbnz(w0, 31, ret_qnan);
  // Positive normals took the fast path above, so nothing reaches the fall
  // through; it computes the same result they would.

  L(interpolate_setup);
  // w1 = mantissa, w2 = biased exponent (<= 0 once renormalized)
  ubfx(w4, w1, 19, 4);
  and_(w5, w2, 1);
  mov(w3, 127);
  sub(w3, w3, w2);
  asr(w3, w3, 1);
  ubfx(w2, w1, 9, 10);
  orr(w1, w4, w5, LSL, 4);

  // w1 = coefficient index, w2 = interpolation factor, w3 = exponent term.
  adr(x4, table);
  ldr(w4, ptr(x4, x1, LSL, 2));
  lsr(w5, w4, 16);  // slope
  mul(w2, w2, w5);
  lsl(w4, w4, 10);
  and_(w4, w4, 0x3FFFC00);  // base
  sub(w4, w4, w2);
  // The interpolated estimate is always within [2^24, 2^26), so normalizing it
  // is a one-bit shift and needs no leading zero count.
  tst(w4, 0x2000000);
  lsl(w5, w4, 1);
  csel(w4, w4, w5, NE);
  sub(w3, w3, 1);
  cinc(w3, w3, NE);
  // Round up by 4 when bit 1 and either bit 0 or bit 2 is set.
  orr(w5, w4, w4, LSR, 2);
  and_(w5, w5, w4, LSR, 1);
  and_(w5, w5, 1);
  add(w4, w4, w5, LSL, 2);
  // Every input reaching here yields a biased exponent of 63..201 and a clear
  // sign, so the result can never be denormal and needs no flush.
  lsl(w3, w3, 23);
  mov(w5, 0x3F800000);
  add(w3, w3, w5);
  ubfx(w4, w4, 2, 23);
  orr(w0, w3, w4);
  ret();

  L(signed_inf);
  and_(w0, w0, 0x80000000);
  orr(w0, w0, 0x7F800000);
  ret();

  L(quiet_nan);
  orr(w0, w0, 0x400000);
  ret();

  L(oddball);
  tbnz(w0, 31, ret_qnan);
  mov(w0, 0x5F34FD00);
  ret();

  L(ret_qnan);
  mov(w0, 0x7FC00000);
  ret();

  const size_t vector_entry_offset = getSize();
  // bl reaches the lane routine because both live in this one blob; the code
  // cache is far larger than its +-128 MiB range.
  mov(x10, x30);
  mov(VReg(1).b16, VReg(0).b16);
  for (uint32_t element = 0; element < 4; ++element) {
    umov(w0, VReg(1).s4[element]);
    bl(lane);
    ins(VReg(2).s4[element], w0);
  }
  mov(VReg(0).b16, VReg(2).b16);
  mov(x30, x10);
  ret();

  L(table);
  static constexpr uint32_t kCoefficients[32] = {
      0x0568B4FD, 0x04F3AF97, 0x048DAAA5, 0x0435A618, 0x03E7A1E4, 0x03A29DFE,
      0x03659A5C, 0x032E96F8, 0x02FC93CA, 0x02D090CE, 0x02A88DFE, 0x02838B57,
      0x026188D4, 0x02438673, 0x02268431, 0x020B820B, 0x03D27FFA, 0x03807C29,
      0x033878AA, 0x02F97572, 0x02C27279, 0x02926FB7, 0x02666D26, 0x023F6AC0,
      0x021D6881, 0x01FD6665, 0x01E16468, 0x01C76287, 0x01AF60C1, 0x01995F12,
      0x01855D79, 0x01735BF4,
  };
  for (uint32_t coefficient : kCoefficients) {
    dd(coefficient);
  }

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  void* fn = Emplace(func_info);
  *out_vector_entry = static_cast<uint8_t*>(fn) + vector_entry_offset;
  return fn;
}

// --------------------------------------------------------------------------
// frsqrte
// --------------------------------------------------------------------------
// x0 = double bits in, x0 = result out. Clobbers x0-x5 and no vector register,
// so it needs no guest->host thunk.
void* A64HelperEmitter::EmitFrsqrteHelper() {
  using namespace Xbyak_aarch64;
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  Label exp_nonmax, check_negative, compute;
  Label signed_inf, quiet_nan, ret_qnan, table;

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  ubfx(x1, x0, 52, 11);              // biased exponent
  and_(x2, x0, 0xFFFFFFFFFFFFFULL);  // mantissa
  cmp(x1, 0x7FF);
  b(NE, exp_nonmax);
  cbnz(x2, quiet_nan);     // NaN -> quiet it, keeping sign and payload
  tbnz(x0, 63, ret_qnan);  // -inf -> QNaN
  mov(x0, 0);              // +inf -> +0
  ret();

  L(exp_nonmax);
  cbnz(x1, check_negative);
  cbz(x2, signed_inf);  // +-0 -> +-inf
  // Denormal. Non-IEEE mode flushes it to +-0 and takes the same path.
  ldr(w3, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
  tbnz(w3, kA64BackendNonIEEEMode, signed_inf);
  tbnz(x0, 63, ret_qnan);
  // Renormalize; the implicit 1 lands at bit 52 and the index masks it off.
  clz(x3, x2);
  sub(x4, x3, 11);
  lsl(x2, x2, x4);
  mov(w4, 12);
  sub(w1, w4, w3);
  b(compute);

  L(check_negative);
  tbnz(x0, 63, ret_qnan);

  L(compute);
  // index = (((exponent & 1) << 3) | (mantissa >> 49)) ^ 8
  ubfx(x4, x2, 49, 3);
  and_(w5, w1, 1);
  orr(w4, w4, w5, LSL, 3);
  eor(w4, w4, 8);
  adr(x5, table);
  ldrb(w4, ptr(x5, x4));
  // result exponent = 1022 - ((exponent - 1023) >> 1)
  sub(w1, w1, 1023);
  asr(w1, w1, 1);
  mov(w5, 1022);
  sub(w1, w5, w1);
  lsl(x1, x1, 52);
  lsl(x4, x4, 44);
  orr(x0, x1, x4);
  ret();

  L(signed_inf);
  and_(x0, x0, 0x8000000000000000ULL);
  orr(x0, x0, 0x7FF0000000000000ULL);
  ret();

  L(quiet_nan);
  orr(x0, x0, 1ULL << 51);
  ret();

  L(ret_qnan);
  mov(x0, 0x7FF8000000000000ULL);
  ret();

  L(table);
  static constexpr uint8_t kEstimates[16] = {
      241, 216, 192, 168, 152, 136, 128, 112, 96, 76, 60, 48, 32, 24, 16, 8};
  for (uint32_t word = 0; word < 4; ++word) {
    dd(kEstimates[word * 4] | (kEstimates[word * 4 + 1] << 8) |
       (kEstimates[word * 4 + 2] << 16) | (kEstimates[word * 4 + 3] << 24));
  }

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  return Emplace(func_info);
}

// ==========================================================================
// Reservation helpers. A global per-granule generation counter so
// cross-thread stores invalidate other threads' reservations. A CAS on the
// value alone would be ABA-vulnerable.
// ==========================================================================
namespace {

A64BackendContext* BackendContextFromRawContext(void* raw_context) {
  return reinterpret_cast<A64BackendContext*>(
      reinterpret_cast<uint8_t*>(raw_context) - sizeof(A64BackendContext));
}

std::atomic<uint32_t>& ReserveGranule(ReserveHelper* reserve_helper,
                                      uint32_t guest_address) {
  const uint32_t granule = guest_address >> A64_RESERVE_GRANULE_SHIFT;
  return reserve_helper->generations[granule & A64_RESERVE_ENTRY_MASK];
}

uint64_t TryAcquireReservationHelper(void* raw_context,
                                     uint64_t guest_address) {
  auto* bctx = BackendContextFromRawContext(raw_context);
  auto& granule =
      ReserveGranule(bctx->reserve_helper_, uint32_t(guest_address));
  // snapshot the generation first, the acquire pins the value read after
  bctx->reserve_generation = granule.load(std::memory_order_acquire);
  bctx->reserve_address = uint32_t(guest_address);
  // lwarx replaces any reservation this thread already held
  bctx->flags |= 1u << kA64BackendHasReserveBit;
  return 1;
}

template <typename T>
T ReservedLoadImpl(ppc::PPCContext* context, uint32_t address) {
  T* host_address = context->TranslateVirtual<T*>(address);
  TryAcquireReservationHelper(context, address);
  auto* bctx = BackendContextFromRawContext(context);
  const T raw = *host_address;
  bctx->cached_reserve_value_ = static_cast<uint64_t>(raw);
  return xe::byte_swap(raw);
}

template <typename T>
uint64_t ReservedStoreImpl(void* raw_context, uint64_t guest_address,
                           uint64_t host_address, uint64_t value) {
  auto* bctx = BackendContextFromRawContext(raw_context);
  const uint32_t reserve_flag = 1u << kA64BackendHasReserveBit;
  const bool had_reservation = (bctx->flags & reserve_flag) != 0;
  // stwcx. always clears the reservation, stored or not
  bctx->flags &= ~reserve_flag;
  // the reservation must be for the address we're storing to
  if (!had_reservation || bctx->reserve_address != uint32_t(guest_address)) {
    return 0;
  }

  auto& granule =
      ReserveGranule(bctx->reserve_helper_, uint32_t(guest_address));
  // claim the generation, see EmitReservedStore
  uint32_t generation = bctx->reserve_generation;
  if ((generation & 1) ||
      !granule.compare_exchange_strong(generation, generation + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
    return 0;
  }

  bool exchange_ok;
  if constexpr (sizeof(T) == sizeof(uint64_t)) {
    exchange_ok = xe::atomic_cas(
        bctx->cached_reserve_value_, uint64_t(value),
        reinterpret_cast<volatile uint64_t*>(uintptr_t(host_address)));
  } else {
    exchange_ok = xe::atomic_cas(
        uint32_t(bctx->cached_reserve_value_), uint32_t(value),
        reinterpret_cast<volatile uint32_t*>(uintptr_t(host_address)));
  }
  granule.store(generation + 2, std::memory_order_release);

  return exchange_ok ? 1 : 0;
}

}  // namespace

uint32_t A64Backend::ReservedLoad32(ppc::PPCContext* context,
                                    uint32_t address) {
  return ReservedLoadImpl<uint32_t>(context, address);
}

uint64_t A64Backend::ReservedLoad64(ppc::PPCContext* context,
                                    uint32_t address) {
  return ReservedLoadImpl<uint64_t>(context, address);
}

bool A64Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                                 uint32_t value) {
  return ReservedStoreImpl<uint32_t>(
             context, address,
             reinterpret_cast<uint64_t>(
                 context->TranslateVirtual<uint32_t*>(address)),
             xe::byte_swap(value)) != 0;
}

bool A64Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                                 uint64_t value) {
  return ReservedStoreImpl<uint64_t>(
             context, address,
             reinterpret_cast<uint64_t>(
                 context->TranslateVirtual<uint64_t*>(address)),
             xe::byte_swap(value)) != 0;
}

// ==========================================================================
// ResolveFunction — runtime function resolution.
// ==========================================================================

// The function the live frame stackpoints[index] runs, from the address its
// call into the next frame returns to, or else its call into host code.
static A64Function* LookupFrameFunction(A64Backend* backend,
                                        const A64BackendStackpoint* stackpoints,
                                        uint32_t index, uint32_t depth) {
  auto* code_cache = backend->code_cache();
  if (index + 1 < depth) {
    // The next frame's prolog saved it, unless host code entered that frame.
    if (auto* function = static_cast<A64Function*>(
            code_cache->LookupFunction(*reinterpret_cast<const uint64_t*>(
                stackpoints[index + 1].host_stack_ +
                StackLayout::HOST_RET_ADDR)))) {
      return function;
    }
  }
  // Otherwise the frame is in a call through a thunk, which saved x30 below it.
  return static_cast<A64Function*>(code_cache->LookupFunction(
      *reinterpret_cast<const uint64_t*>(stackpoints[index].host_stack_ -
                                         StackLayout::THUNK_CALLER_RET_ADDR)));
}

// Where a return site in an already translated function continues, when the
// guest is unwinding to it past more than one frame, or 0. The caller decides
// that |target_address| is a return site. |pending_pops| is how many
// stackpoints the branch still pops after resolving, which the stack
// synchronization helper will not see.
static uint64_t ResolveLongjmp(ppc::PPCContext* guest_context,
                               uint32_t target_address, uint32_t pending_pops) {
  auto* processor = guest_context->processor;
  auto* backend = static_cast<A64Backend*>(processor->backend());
  auto* backend_context = backend->BackendContextForGuestContext(guest_context);
  if (backend_context->current_stackpoint_depth <= pending_pops) {
    return 0;
  }
  const uint32_t live_frames =
      backend_context->current_stackpoint_depth - pending_pops;
  const A64BackendStackpoint* stackpoints = backend_context->stackpoints;
  const uint32_t stack_pointer = static_cast<uint32_t>(guest_context->r[1]);
  const uint32_t sync_depth =
      FindStackpointSyncDepth(stackpoints, live_frames, stack_pointer);
  if (!sync_depth) {
    return 0;
  }
  // The target's code only fits the frame of the function it was translated
  // in. r1 alone can name the wrong frame: a callee entered at the stack
  // pointer longjmp restores records the same r1 as its caller runs at, and
  // XeFu's generated code keeps the x86 stack pointer in r1.
  for (uint32_t i = sync_depth; i-- > 0;) {
    // The frame longjmp returns to made a call, so it runs below the stack
    // pointer it was entered with.
    if (stackpoints[i].guest_stack_ <= stack_pointer) {
      continue;
    }
    auto* function = LookupFrameFunction(backend, stackpoints, i, live_frames);
    // Landing on a function's first instruction would skip its prolog.
    if (!function || function->address() == target_address) {
      continue;
    }
    // A function's range can hold unreachable instructions it has no code for.
    if (const uintptr_t host_address =
            function->MapGuestAddressToMachineCode(target_address)) {
      // The stack synchronization helper at the target restores this frame.
      backend_context->pending_stackpoint_sync_depth = i + 1;
      return host_address;
    }
  }
  return 0;
}

uint64_t ResolveFunction(void* raw_context, uint64_t target_address) {
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto thread_state = guest_context->thread_state;
  assert_not_zero(target_address);

  auto* processor = thread_state->processor();
  if (cvars::a64_enable_host_guest_stack_synchronization &&
      target_address <= 0xFFFFFFFFu) {
    auto* module_for_address =
        processor->LookupModule(static_cast<uint32_t>(target_address));
    auto* xexmod = dynamic_cast<XexModule*>(module_for_address);
    if (xexmod) {
      InfoCacheFlags* flags = xexmod->GetInstructionAddressFlags(
          static_cast<uint32_t>(target_address));
      if (flags && flags->is_return_site) {
        if (uint64_t host_address = ResolveLongjmp(
                guest_context, static_cast<uint32_t>(target_address), 0)) {
          return host_address;
        }
      }
    }
  }

  auto fn = processor->ResolveFunction(static_cast<uint32_t>(target_address));
  if (!fn) {
    // Unresolvable — return 0 which will fault.
    return 0;
  }

  auto guest_fn = static_cast<GuestFunction*>(fn);
  auto code = guest_fn->machine_code();
  if (!code) {
    return 0;
  }
  return reinterpret_cast<uint64_t>(code);
}

// Where a dynamic code branch to kHostReturnAddress continues, or 0. Guest code
// can divert its return and branch to the saved address from deeper frames, as
// XeFu's trap handler does, so the branch returns to host code from the
// innermost frame host code entered. That can be the frame a tail branch is
// about to pop, which returning pops anyway.
static uint64_t ResolveHostReturn(ppc::PPCContext* guest_context) {
  auto* backend = static_cast<A64Backend*>(guest_context->processor->backend());
  auto* backend_context = backend->BackendContextForGuestContext(guest_context);
  const uint32_t depth = backend_context->current_stackpoint_depth;
  for (uint32_t i = depth; i-- > 0;) {
    const uint64_t frame = backend_context->stackpoints[i].host_stack_;
    // A tail call passes the address on to the function taking the frame.
    if (*reinterpret_cast<const uint32_t*>(
            frame + StackLayout::GUEST_RET_ADDR) != kHostReturnAddress) {
      continue;
    }
    auto* function =
        LookupFrameFunction(backend, backend_context->stackpoints, i, depth);
    if (!function) {
      return 0;
    }
    backend_context->host_return_stackpoint_depth = i;
    backend_context->host_return_stack = frame + function->stack_size();
    backend_context->host_return_address =
        *reinterpret_cast<const uint64_t*>(frame + StackLayout::HOST_RET_ADDR);
    return reinterpret_cast<uint64_t>(backend->return_to_host_helper());
  }
  return 0;
}

// The host address in a live frame that a dynamic code branch to the
// instruction after its call returns to, or 0. The branch returns there however
// many frames it skips or bytes the callee popped. The stack synchronization
// helper at the target restores the calling frame recorded here.
// |pending_pops| is how many stackpoints the branch still pops after resolving.
static uint64_t FindLiveReturn(ppc::PPCContext* guest_context,
                               uint32_t target_address, uint32_t pending_pops) {
  auto* backend = static_cast<A64Backend*>(guest_context->processor->backend());
  auto* backend_context = backend->BackendContextForGuestContext(guest_context);
  const uint32_t depth = backend_context->current_stackpoint_depth;
  const uint32_t live_frames = depth > pending_pops ? depth - pending_pops : 0;
  const uint32_t stack_pointer = static_cast<uint32_t>(guest_context->r[1]);
  // A live frame matches only once the guest stack is back at or above its call
  // point, which a nested call never reaches. The difference is what the callee
  // popped from the caller's own frame, so the smallest one is the frame
  // returned to, which a recursive caller can have several of.
  uint32_t found_index = 0;
  uint32_t found_popped = UINT32_MAX;
  uint64_t found_host_address = 0;
  // Index 0 was entered from host code, which has no frame to return into.
  for (uint32_t i = live_frames; i-- > 1;) {
    const A64BackendStackpoint& stackpoint = backend_context->stackpoints[i];
    if (stackpoint.guest_return_address_ != target_address ||
        stackpoint.guest_stack_ > stack_pointer ||
        stack_pointer - stackpoint.guest_stack_ >= found_popped) {
      continue;
    }
    // The host call that entered the frame returns to the translated target.
    const uint64_t host_return = *reinterpret_cast<const uint64_t*>(
        stackpoint.host_stack_ + StackLayout::HOST_RET_ADDR);
    auto function = backend->code_cache()->LookupFunction(host_return);
    if (!function ||
        function->MapGuestAddressToMachineCode(target_address) != host_return) {
      continue;
    }
    found_index = i;
    found_popped = stack_pointer - stackpoint.guest_stack_;
    found_host_address = host_return;
    if (!found_popped) {
      break;
    }
  }
  if (found_host_address) {
    // The helper restores stackpoints[depth - 1], so this names the calling
    // frame, whose record is the host stack it called with because a64 records
    // one after the prolog allocation.
    backend_context->pending_stackpoint_sync_depth = found_index;
  }
  return found_host_address;
}

// Where a dynamic code branch to the instruction after a call continues in
// translated code, or 0. While the frame of that call is live, the branch
// returns into it. Once the call has returned, as for setjmp, the guest stack
// decides how far to unwind. |pending_pops| is as for FindLiveReturn and
// |out_is_return_site| tells the caller whether the target follows a call.
static uint64_t ResolveDynamicReturn(ppc::PPCContext* guest_context,
                                     uint32_t target_address,
                                     uint32_t pending_pops,
                                     bool* out_is_return_site) {
  *out_is_return_site = false;
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return 0;
  }
  if (target_address == kHostReturnAddress) {
    return ResolveHostReturn(guest_context);
  }
  if (uint64_t host_address =
          FindLiveReturn(guest_context, target_address, pending_pops)) {
    // Matching a recorded return address proves the target follows a call.
    *out_is_return_site = true;
    return host_address;
  }
  auto* processor = guest_context->processor;

  // Dynamic code has no instruction flags, so recognize the call before it.
  if (target_address < 4) {
    return 0;
  }
  auto module = processor->LookupModule(target_address - 4);
  if (!module) {
    return 0;
  }
  const uint32_t previous =
      xe::load_and_swap<uint32_t>(module->TranslateCode(target_address - 4));
  const bool is_return_site = ((previous & 0xFC000001) == 0x48000001) ||
                              ((previous & 0xFC000001) == 0x40000001) ||
                              ((previous & 0xFC0007FF) == 0x4C000421) ||
                              ((previous & 0xFC0007FF) == 0x4C000021);
  *out_is_return_site = is_return_site;
  if (!is_return_site) {
    return 0;
  }
  return ResolveLongjmp(guest_context, target_address, pending_pops);
}

static uint32_t DynamicCallCacheIndex(uint32_t guest_address) {
  return ((guest_address >> 2) ^ (guest_address >> 14)) &
         (kA64DynamicCallCacheSize - 1);
}

// Resolves a target without an indirection slot and caches it per thread. A
// direct bl is never a return and skips the return site checks.
static uint64_t ResolveDynamicFunction(void* raw_context,
                                       uint64_t target_address,
                                       uint32_t pending_pops,
                                       bool direct = false) {
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  bool is_return_site = false;
  if (!direct) {
    if (uint64_t return_address = ResolveDynamicReturn(
            guest_context, static_cast<uint32_t>(target_address), pending_pops,
            &is_return_site)) {
      static_cast<A64Backend*>(guest_context->processor->backend())
          ->BackendContextForGuestContext(raw_context)
          ->dynamic_target_in_body = 1;
      return return_address;
    }
  }
  const uint64_t sweep_count = guest_context->processor->code_sweep_count();
  auto function = guest_context->processor->ResolveFunction(
      static_cast<uint32_t>(target_address));
  bool redirected = false;
  if (!function || !function->is_guest()) {
    // The guest can handle the fetch fault and continue the call elsewhere.
    if (auto hook = guest_context->processor->code_fault_hook()) {
      if (const uint32_t resume_address =
              hook(guest_context, static_cast<uint32_t>(target_address))) {
        target_address = resume_address;
        function = guest_context->processor->ResolveFunction(resume_address);
        redirected = true;
      }
    }
  }
  if (!function || !function->is_guest()) {
    XELOGE("No guest code at {:08X} for a dynamic call",
           static_cast<uint32_t>(target_address));
    return 0;
  }
  const uint64_t host_address = reinterpret_cast<uint64_t>(
      static_cast<A64Function*>(function)->machine_code());
  // A redirect isn't cached, since the checks above were for the original
  // target.
  if (redirected) {
    return host_address;
  }
  auto backend = static_cast<A64Backend*>(guest_context->processor->backend());
  auto bctx = backend->BackendContextForGuestContext(raw_context);
  if (!bctx->dynamic_call_cache) {
    bctx->dynamic_call_cache =
        new A64DynamicCallCacheEntry[kA64DynamicCallCacheSize];
    for (uint32_t i = 0; i < kA64DynamicCallCacheSize; ++i) {
      bctx->dynamic_call_cache[i] = {UINT32_MAX, kA64DynamicCallAny, 0};
    }
  }
  // An unwind lands at an address only valid for the frame it found and is
  // never cached. A return site's entry has a branch look for a live frame
  // first.
  auto& entry = bctx->dynamic_call_cache[DynamicCallCacheIndex(
      static_cast<uint32_t>(target_address))];
  entry.host_address = host_address;
  entry.kind = direct           ? kA64DynamicCallDirectOnly
               : is_return_site ? kA64DynamicCallReturnSite
                                : kA64DynamicCallAny;
  entry.guest_address = static_cast<uint32_t>(target_address);
  // A sweep since the resolve began may have dropped the function before the
  // entry was there for it to clear. The fence pairs with the sweep's count
  // bump before it clears entries.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (guest_context->processor->code_sweep_count() != sweep_count) {
    entry.host_address = 0;
    entry.guest_address = UINT32_MAX;
  }
  return host_address;
}

uint64_t ResolveDynamicCall(void* raw_context, uint64_t target_address) {
  return ResolveDynamicFunction(raw_context, target_address, 0);
}

uint64_t ResolveDirectDynamicCall(void* raw_context, uint64_t target_address) {
  return ResolveDynamicFunction(raw_context, target_address, 0, true);
}

// A tail branch pops the current function's stackpoint after resolving.
uint64_t ResolveDynamicTailCall(void* raw_context, uint64_t target_address) {
  return ResolveDynamicFunction(raw_context, target_address, 1);
}

// Whether ResolveLongjmp could land a branch inside a function body, which
// takes the top two live frames being below r1.
static bool MayResolveLongjmp(ppc::PPCContext* guest_context,
                              uint32_t pending_pops) {
  auto bctx = static_cast<A64Backend*>(guest_context->processor->backend())
                  ->BackendContextForGuestContext(guest_context);
  const uint32_t depth = bctx->current_stackpoint_depth;
  if (depth < pending_pops + 3) {
    return false;
  }
  const uint32_t top = depth - pending_pops - 1;
  const uint32_t stack_pointer = static_cast<uint32_t>(guest_context->r[1]);
  return stack_pointer > bctx->stackpoints[top].guest_stack_ &&
         stack_pointer > bctx->stackpoints[top - 1].guest_stack_;
}

// A branch to a return site with a cached function takes the function unless
// a live frame returns there or a longjmp could land in a body, which the full
// resolve handles.
static uint64_t ResolveCachedReturnSite(void* raw_context,
                                        uint64_t target_address,
                                        uint32_t pending_pops) {
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto bctx = static_cast<A64Backend*>(guest_context->processor->backend())
                  ->BackendContextForGuestContext(raw_context);
  const uint32_t target = static_cast<uint32_t>(target_address);
  if (uint64_t host_address =
          FindLiveReturn(guest_context, target, pending_pops)) {
    bctx->dynamic_target_in_body = 1;
    return host_address;
  }
  // Read once, as another thread can invalidate the entry meanwhile.
  const auto& entry = bctx->dynamic_call_cache[DynamicCallCacheIndex(target)];
  const uint64_t cached_address = entry.host_address;
  if (MayResolveLongjmp(guest_context, pending_pops) ||
      entry.guest_address != target ||
      entry.kind != kA64DynamicCallReturnSite || !cached_address) {
    return ResolveDynamicFunction(raw_context, target_address, pending_pops);
  }
  return cached_address;
}

uint64_t ResolveCachedReturnSiteCall(void* raw_context,
                                     uint64_t target_address) {
  return ResolveCachedReturnSite(raw_context, target_address, 0);
}

uint64_t ResolveCachedReturnSiteTail(void* raw_context,
                                     uint64_t target_address) {
  return ResolveCachedReturnSite(raw_context, target_address, 1);
}

// ==========================================================================
// A64Backend
// ==========================================================================

// ARM64 guest trampoline template.
// Loads proc, userdata1, userdata2 into x0-x2, then jumps to guest_to_host
// thunk via x9.  Each 64-bit immediate uses movz + 3x movk (16 bytes).
// Total: 4 registers × 16 bytes + 4 bytes (br x9) = 68 bytes.
//
// Template layout (offsets where 64-bit immediates are patched):
//   +0x00: movz x0, #imm16; movk x0, ..., lsl 16/32/48  -> proc
//   +0x10: movz x1, #imm16; movk x1, ..., lsl 16/32/48  -> userdata1
//   +0x20: movz x2, #imm16; movk x2, ..., lsl 16/32/48  -> userdata2
//   +0x30: movz x9, #imm16; movk x9, ..., lsl 16/32/48  -> g2h thunk
//   +0x40: br x9
//
// ARM64 encoding helpers:
//   movz xN, #imm16          = 0xD2800000 | (imm16 << 5) | N
//   movk xN, #imm16, lsl #S  = 0xF2800000 | (hw << 21) | (imm16 << 5) | N
//     where hw = S/16 (0,1,2,3)
static void EncodeMovImm64(uint32_t* out, uint32_t reg, uint64_t imm) {
  out[0] = 0xD2800000 | (static_cast<uint32_t>(imm & 0xFFFF) << 5) | reg;
  out[1] =
      0xF2A00000 | (static_cast<uint32_t>((imm >> 16) & 0xFFFF) << 5) | reg;
  out[2] =
      0xF2C00000 | (static_cast<uint32_t>((imm >> 32) & 0xFFFF) << 5) | reg;
  out[3] =
      0xF2E00000 | (static_cast<uint32_t>((imm >> 48) & 0xFFFF) << 5) | reg;
}

static constexpr size_t kGuestTrampolineSize = 68;  // 17 instructions × 4
static constexpr uint32_t kTrampolineOffsetProc = 0x00;
static constexpr uint32_t kTrampolineOffsetArg1 = 0x10;
static constexpr uint32_t kTrampolineOffsetArg2 = 0x20;
static constexpr uint32_t kTrampolineOffsetThunk = 0x30;

static void BuildGuestTrampoline(uint8_t* buf, void* proc, void* userdata1,
                                 void* userdata2, void* g2h_thunk) {
  auto* code = reinterpret_cast<uint32_t*>(buf);
  // x0 = proc (target function for guest-to-host thunk)
  EncodeMovImm64(&code[0], 0, reinterpret_cast<uint64_t>(proc));
  // x1 = userdata1
  EncodeMovImm64(&code[4], 1, reinterpret_cast<uint64_t>(userdata1));
  // x2 = userdata2
  EncodeMovImm64(&code[8], 2, reinterpret_cast<uint64_t>(userdata2));
  // x9 = guest_to_host_thunk
  EncodeMovImm64(&code[12], 9, reinterpret_cast<uint64_t>(g2h_thunk));
  // br x9
  code[16] = 0xD61F0120;  // br x9
}

A64Backend::A64Backend() {
  code_cache_ = A64CodeCache::Create();

  // Prefer a sub-2GB slot so fast indirection (rel32) is usable; fall back
  // to an OS-chosen address if none is available. macOS rejects fixed
  // PROT_EXEC mappings in this range, so skip the scan entirely there.
  void* buf = nullptr;
#if !XE_PLATFORM_MAC
  for (uint32_t base_address = 0x10000; base_address < 0x80000000;
       base_address += 65536) {
    buf = memory::AllocFixed(
        reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)),
        kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
        xe::memory::AllocationType::kReserveCommit,
        xe::memory::PageAccess::kExecuteReadWrite);
    if (buf) {
      break;
    }
  }
#endif
  if (!buf) {
    buf = memory::AllocFixed(nullptr,
                             kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                             xe::memory::AllocationType::kReserveCommit,
                             xe::memory::PageAccess::kExecuteReadWrite);
  }
  xenia_assert(buf);
  guest_trampoline_memory_ = reinterpret_cast<uint8_t*>(buf);
  guest_trampolines_sub4gb_ = reinterpret_cast<uintptr_t>(buf) < 0x100000000ull;
  guest_trampoline_address_bitmap_.Resize(MAX_GUEST_TRAMPOLINES);
}

A64Backend::~A64Backend() {
  ExceptionHandler::Uninstall(&ExceptionCallbackThunk, this);
  if (guest_trampoline_memory_) {
    memory::DeallocFixed(guest_trampoline_memory_,
                         kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                         memory::DeallocationType::kRelease);
    guest_trampoline_memory_ = nullptr;
  }
}

static void ForwardMMIOAccessForRecording(void* context, void* hostaddr) {
  reinterpret_cast<A64Backend*>(context)
      ->RecordMMIOExceptionForGuestInstruction(hostaddr);
}

bool A64Backend::Initialize(Processor* processor) {
  if (!Backend::Initialize(processor)) {
    return false;
  }

  // Fast indirection is only viable if trampolines made it under 4GB.
  code_cache_->set_allow_fast_indirection(guest_trampolines_sub4gb_);
  if (!code_cache_->Initialize()) {
    XELOGE("A64Backend: Failed to initialize code cache");
    return false;
  }

  // Expose the code cache to the base Backend class.
  Backend::code_cache_ = code_cache_.get();

  // Set up machine info for the register allocator.
  machine_info_.supports_extended_load_store = true;
  // GPR set: x22-x28 (7 registers; x19=backend ctx, x20=context, x21=membase)
  auto& gpr_set = machine_info_.register_sets[0];
  gpr_set.id = 0;
  std::strcpy(gpr_set.name, "gpr");
  gpr_set.types = MachineInfo::RegisterSet::INT_TYPES;
  gpr_set.count = A64Emitter::GPR_COUNT;
  // VEC set: v4-v15, v16-v31 (28 registers, v0-v3 scratch)
  auto& vec_set = machine_info_.register_sets[1];
  vec_set.id = 1;
  std::strcpy(vec_set.name, "vec");
  vec_set.types = MachineInfo::RegisterSet::FLOAT_TYPES |
                  MachineInfo::RegisterSet::VEC_TYPES;
  vec_set.count = A64Emitter::VEC_COUNT;

  // Generate thunks using ARM64 assembler.
  XbyakA64Allocator allocator;
  A64HelperEmitter thunk_emitter(this, &allocator);

  // First, at offset 0 of the code cache, so that an encoded slot of 0 names
  // it. See CommitIndirectionChunks.
  resolve_function_thunk_ = thunk_emitter.EmitResolveFunctionThunk();
  host_to_guest_thunk_ = thunk_emitter.EmitHostToGuestThunk();
  guest_to_host_thunk_ = thunk_emitter.EmitGuestToHostThunk();
  guest_to_host_thunk_no_vec_ = thunk_emitter.EmitGuestToHostThunkNoVec();

  if (!host_to_guest_thunk_ || !guest_to_host_thunk_ ||
      !guest_to_host_thunk_no_vec_ || !resolve_function_thunk_) {
    XELOGE("A64Backend: Failed to generate thunks");
    return false;
  }

  if (cvars::a64_enable_host_guest_stack_synchronization) {
    synchronize_guest_and_host_stack_helper_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackHelper();
    return_to_host_helper_ = thunk_emitter.EmitReturnToHostHelper();
  }

  vrsqrtefp_scalar_helper_ =
      thunk_emitter.EmitVRsqrtefpHelper(&vrsqrtefp_vector_helper_);
  frsqrte_helper_ = thunk_emitter.EmitFrsqrteHelper();

  // Set the indirection table default to point at the resolve thunk.
  // Use 64-bit encoding: the resolve thunk address is encoded as a rel32
  // offset if it lands inside the code cache, or as a tagged external-table
  // index otherwise.
  static_cast<A64CodeCache*>(code_cache_.get())
      ->set_indirection_default_64(
          reinterpret_cast<uint64_t>(resolve_function_thunk_));

  // Commit the indirection table range used by guest trampolines.
  code_cache_->CommitExecutableRange(GUEST_TRAMPOLINE_BASE,
                                     GUEST_TRAMPOLINE_END);

  // Commit special indirection ranges (force return address, etc.).
  code_cache_->CommitExecutableRange(0x9FFF0000, 0x9FFFFFFF);

  // Register exception handler for MMIO access from JIT code.
  ExceptionHandler::Install(ExceptionCallbackThunk, this);

  // Lets the memory system tell us which guest instructions faulted on MMIO,
  // so they get recompiled into a direct call instead of taking the signal
  // handler on every access.
  if (cvars::record_mmio_access_exceptions) {
    processor->memory()->SetMMIOExceptionRecordingCallback(
        ForwardMMIOAccessForRecording, this);
  }

  return true;
}

void A64Backend::CommitExecutableRange(uint32_t guest_low,
                                       uint32_t guest_high) {
  code_cache_->CommitExecutableRange(guest_low, guest_high);
}

std::unique_ptr<Assembler> A64Backend::CreateAssembler() {
  return std::make_unique<A64Assembler>(this);
}

std::unique_ptr<GuestFunction> A64Backend::CreateGuestFunction(
    Module* module, uint32_t address) {
  return std::make_unique<A64Function>(module, address);
}

namespace {

// Encoding 31 reads as XZR in every branch form below.
uint64_t ReadXReg(const HostThreadContext& ctx, uint32_t n) {
  return n == 31 ? 0 : ctx.x[n];
}

bool TestCondition(uint64_t pstate, uint32_t cond) {
  const bool n = (pstate >> 31) & 1;
  const bool z = (pstate >> 30) & 1;
  const bool c = (pstate >> 29) & 1;
  const bool v = (pstate >> 28) & 1;
  bool result;
  switch (cond >> 1) {
    case 0b000:
      result = z;
      break;
    case 0b001:
      result = c;
      break;
    case 0b010:
      result = n;
      break;
    case 0b011:
      result = v;
      break;
    case 0b100:
      result = c && !z;
      break;
    case 0b101:
      result = n == v;
      break;
    case 0b110:
      result = (n == v) && !z;
      break;
    default:
      result = true;
      break;
  }
  if ((cond & 1) && cond != 0b1111) {
    result = !result;
  }
  return result;
}

int64_t SignExtend(uint64_t value, uint32_t bits) {
  const uint32_t shift = 64 - bits;
  return int64_t(value << shift) >> shift;
}

}  // namespace

uint64_t A64Backend::CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                                  uint64_t current_pc) {
  const auto& ctx = thread_info->host_context;
  const uint32_t insn = xe::load<uint32_t>(reinterpret_cast<void*>(current_pc));
  const uint64_t next_pc = current_pc + 4;

  // B  imm26  0b000101...    BL imm26  0b100101...
  if ((insn & 0x7C000000) == 0x14000000) {
    return current_pc + SignExtend(insn & 0x03FFFFFF, 26) * 4;
  }
  // B.cond / BC.cond imm19   0b01010100 imm19 x cond
  if ((insn & 0xFF000000) == 0x54000000) {
    if (!TestCondition(ctx.pstate, insn & 0xF)) {
      return next_pc;
    }
    return current_pc + SignExtend((insn >> 5) & 0x7FFFF, 19) * 4;
  }
  // CBZ / CBNZ  sf 011010 op imm19 Rt
  if ((insn & 0x7E000000) == 0x34000000) {
    uint64_t value = ReadXReg(ctx, insn & 0x1F);
    if (!(insn & 0x80000000)) {
      value = uint32_t(value);
    }
    const bool nonzero = value != 0;
    const bool is_cbnz = (insn >> 24) & 1;
    if (nonzero != is_cbnz) {
      return next_pc;
    }
    return current_pc + SignExtend((insn >> 5) & 0x7FFFF, 19) * 4;
  }
  // TBZ / TBNZ  b5 011011 op b40 imm14 Rt
  if ((insn & 0x7E000000) == 0x36000000) {
    const uint32_t bit = ((insn >> 26) & 0x20) | ((insn >> 19) & 0x1F);
    const bool set = (ReadXReg(ctx, insn & 0x1F) >> bit) & 1;
    const bool is_tbnz = (insn >> 24) & 1;
    if (set != is_tbnz) {
      return next_pc;
    }
    return current_pc + SignExtend((insn >> 5) & 0x3FFF, 14) * 4;
  }
  // BR / BLR / RET  Rn
  if ((insn & 0xFFFFFC1F) == 0xD61F0000 || (insn & 0xFFFFFC1F) == 0xD63F0000 ||
      (insn & 0xFFFFFC1F) == 0xD65F0000) {
    return ReadXReg(ctx, (insn >> 5) & 0x1F);
  }
  return next_pc;
}

// ARM64 BRK #0 encoding (4 bytes, fixed-width instruction).
static constexpr uint32_t kArm64Brk0 = 0xD4200000;

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint) {
  breakpoint->ForEachHostAddress([this, breakpoint](uint64_t host_address) {
    auto ptr = reinterpret_cast<void*>(host_address);
    auto original_bytes = xe::load<uint32_t>(ptr);
    assert_true(original_bytes != kArm64Brk0);
    if (!code_cache()->PatchCode(ptr, &kArm64Brk0, sizeof(kArm64Brk0))) {
      assert_always();
      return;
    }
    breakpoint->backend_data().emplace_back(host_address, original_bytes);
  });
}

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint, Function* fn) {
  assert_true(breakpoint->address_type() == Breakpoint::AddressType::kGuest);
  assert_true(fn->is_guest());
  auto guest_function = reinterpret_cast<cpu::GuestFunction*>(fn);
  auto host_address =
      guest_function->MapGuestAddressToMachineCode(breakpoint->guest_address());
  if (!host_address) {
    assert_always();
    return;
  }

  auto ptr = reinterpret_cast<void*>(host_address);
  auto original_bytes = xe::load<uint32_t>(ptr);
  assert_true(original_bytes != kArm64Brk0);
  if (!code_cache()->PatchCode(ptr, &kArm64Brk0, sizeof(kArm64Brk0))) {
    assert_always();
    return;
  }
  breakpoint->backend_data().emplace_back(host_address, original_bytes);
}

void A64Backend::UninstallBreakpoint(Breakpoint* breakpoint) {
  for (auto& pair : breakpoint->backend_data()) {
    auto ptr = reinterpret_cast<uint8_t*>(pair.first);
    auto instruction_bytes = xe::load<uint32_t>(ptr);
    assert_true(instruction_bytes == kArm64Brk0);
    const uint32_t original_bytes = static_cast<uint32_t>(pair.second);
    code_cache()->PatchCode(ptr, &original_bytes, sizeof(original_bytes));
  }
  breakpoint->backend_data().clear();
}

// The backend context is carved out of the allocation granule immediately
// before the guest context, so it has to stay inside one page.
static_assert(sizeof(A64BackendContext) < 4096,
              "A64BackendContext must fit in the granule before the context");

void A64Backend::InitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  std::memset(a64_ctx, 0, sizeof(A64BackendContext));
  a64_ctx->reserve_helper_ = &reserve_helper_;
  a64_ctx->Ox1000 = 0x1000;
  a64_ctx->fpcr_fpu = DEFAULT_FPU_FPCR;
  a64_ctx->fpcr_vmx = DEFAULT_VMX_FPCR;
  a64_ctx->fpcr_vmx_daz = DEFAULT_VMX_FPCR;   // never follows NJM
  a64_ctx->flags = (1U << kA64BackendNJMOn);  // NJM on by default
  a64_ctx->guest_tick_count = Clock::GetGuestTickCountPointer();
  auto* cache = code_cache_.get();
  a64_ctx->indirection_table_bias = cache->indirection_table_base_bias();
  a64_ctx->code_execute_base = cache->execute_base_address();
  a64_ctx->external_indirection_table =
      cache->external_indirection_table_base_address();
  a64_ctx->guest_to_host_thunk_address =
      reinterpret_cast<uint64_t>(guest_to_host_thunk_);
  a64_ctx->guest_to_host_thunk_no_vec_address =
      reinterpret_cast<uint64_t>(guest_to_host_thunk_no_vec_);
  a64_ctx->synchronize_guest_and_host_stack_helper_address =
      reinterpret_cast<uint64_t>(synchronize_guest_and_host_stack_helper_);
  a64_ctx->vrsqrtefp_scalar_helper_address =
      reinterpret_cast<uint64_t>(vrsqrtefp_scalar_helper_);
  a64_ctx->vrsqrtefp_vector_helper_address =
      reinterpret_cast<uint64_t>(vrsqrtefp_vector_helper_);
  a64_ctx->frsqrte_helper_address = reinterpret_cast<uint64_t>(frsqrte_helper_);
  a64_ctx->preempt_yield_handler_address =
      reinterpret_cast<uint64_t>(&preempt_yield_handler);

  auto set_est = [&](int index, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int lane = 0; lane < 4; lane++) {
      a64_ctx->est_consts[index][lane] = bits;
    }
  };
  auto set_est_bits = [&](int index, uint32_t bits) {
    for (int lane = 0; lane < 4; lane++) {
      a64_ctx->est_consts[index][lane] = bits;
    }
  };
  // 2^f on [0,1), max relative error 7.7e-08.
  set_est(kEstExp2Poly + 0, 0.9999999266823865f);
  set_est(kEstExp2Poly + 1, 0.6931530239113992f);
  set_est(kEstExp2Poly + 2, 0.24015381838022493f);
  set_est(kEstExp2Poly + 3, 0.055826172900559086f);
  set_est(kEstExp2Poly + 4, 0.008989127362479102f);
  set_est(kEstExp2Poly + 5, 0.0018777841277241077f);
  // log2(1+u) on [0,1], max absolute error 1.85e-06.
  set_est(kEstLog2Poly + 0, 1.8456866772102942e-06f);
  set_est(kEstLog2Poly + 1, 1.4424953159391898f);
  set_est(kEstLog2Poly + 2, -0.7177910762015521f);
  set_est(kEstLog2Poly + 3, 0.4565216600899004f);
  set_est(kEstLog2Poly + 4, -0.2765407398023532f);
  set_est(kEstLog2Poly + 5, 0.12100223739860312f);
  set_est(kEstLog2Poly + 6, -0.025691088797142478f);
  set_est(kEstScale, 2048.0f);
  set_est(kEstUnscale, 1.0f / 2048.0f);
  set_est(kEstExp2Max, 128.0f);
  set_est(kEstExp2Min, -126.0f);
  set_est_bits(kEstOne, 0x3F800000u);
  set_est_bits(kEstInt127, 127u);
  set_est_bits(kEstPosInf, 0x7F800000u);
  set_est_bits(kEstNegInf, 0xFF800000u);
  set_est_bits(kEstQNaN, 0x7FC00000u);
  set_est_bits(kEstMantissaMask, 0x007FFFFFu);
  set_est_bits(kEstQuietBit, 0x00400000u);

  a64_ctx->stackpoints = AllocStackpoints();
  a64_ctx->dynamic_call_cache = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    backend_contexts_.push_back(ctx);
  }

  // Reset the live host FPCR for a fresh PPC context so one test's rounding
  // state does not leak into the next on the shared PPC test runner thread.
  SetGuestRoundingMode(ctx, 0);
}

void A64Backend::DeinitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  if (a64_ctx->stackpoints) {
    delete[] a64_ctx->stackpoints;
    a64_ctx->stackpoints = nullptr;
  }
  auto global_lock = global_critical_region_.Acquire();
  backend_contexts_.erase(
      std::remove(backend_contexts_.begin(), backend_contexts_.end(), ctx),
      backend_contexts_.end());
  // InvalidateDynamicCalls walks a registered context's cache under the lock.
  delete[] a64_ctx->dynamic_call_cache;
  a64_ctx->dynamic_call_cache = nullptr;
}

void A64Backend::InvalidateDynamicCalls(uint32_t start, uint32_t end) {
  auto global_lock = global_critical_region_.Acquire();
  // A range with fewer instructions than the cache has slots only checks the
  // slots those hash to.
  const bool targeted = end - start < kA64DynamicCallCacheSize * 4;
  auto invalidate = [start, end](A64DynamicCallCacheEntry& entry) {
    if (entry.guest_address >= start && entry.guest_address <= end) {
      // The lookup only rejects an entry whose host address is zero.
      entry.host_address = 0;
      entry.guest_address = UINT32_MAX;
    }
  };
  for (void* ctx : backend_contexts_) {
    A64BackendContext* bctx = BackendContextForGuestContext(ctx);
    if (!bctx->dynamic_call_cache) {
      continue;
    }
    if (targeted) {
      for (uint64_t address = start & ~uint32_t(3); address <= end;
           address += 4) {
        invalidate(bctx->dynamic_call_cache[DynamicCallCacheIndex(
            static_cast<uint32_t>(address))]);
      }
    } else {
      for (uint32_t i = 0; i < kA64DynamicCallCacheSize; ++i) {
        invalidate(bctx->dynamic_call_cache[i]);
      }
    }
  }
}

void A64Backend::PrepareForReentry(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  a64_ctx->current_stackpoint_depth = 0;
  a64_ctx->pending_stackpoint_sync_depth = 0;
}

A64BackendStackpoint* A64Backend::AllocStackpoints() {
  if (!cvars::a64_enable_host_guest_stack_synchronization ||
      cvars::a64_max_stackpoints <= 0) {
    return nullptr;
  }
  return new A64BackendStackpoint[cvars::a64_max_stackpoints]();
}

namespace {
struct A64StackpointState {
  A64BackendStackpoint* stackpoints = nullptr;
  unsigned int depth = 0;
};
}  // namespace

void* A64Backend::CreateStackpointState() {
  auto state = new A64StackpointState();
  state->stackpoints = AllocStackpoints();
  return state;
}

void A64Backend::DestroyStackpointState(void* state) {
  if (!state) {
    return;
  }
  auto stackpoint_state = static_cast<A64StackpointState*>(state);
  delete[] stackpoint_state->stackpoints;
  delete stackpoint_state;
}

void A64Backend::SwapStackpointState(void* ctx, void* state) {
  if (!state) {
    return;
  }
  A64BackendContext* bctx = BackendContextForGuestContext(ctx);
  auto stackpoint_state = static_cast<A64StackpointState*>(state);
  std::swap(bctx->stackpoints, stackpoint_state->stackpoints);
  std::swap(bctx->current_stackpoint_depth, stackpoint_state->depth);
  // A pending sync names the host stack being swapped out.
  bctx->pending_stackpoint_sync_depth = 0;
}

uint32_t A64Backend::CreateGuestTrampoline(GuestTrampolineProc proc,
                                           void* userdata1, void* userdata2,
                                           bool long_term) {
  size_t new_index;
  if (long_term) {
    new_index = guest_trampoline_address_bitmap_.AcquireFromBack();
  } else {
    new_index = guest_trampoline_address_bitmap_.Acquire();
  }
  xenia_assert(new_index != static_cast<size_t>(-1));

  uint8_t* write_pos =
      &guest_trampoline_memory_[kGuestTrampolineSize * new_index];

#if XE_PLATFORM_MAC
  pthread_jit_write_protect_np(0);
#endif
  BuildGuestTrampoline(write_pos, reinterpret_cast<void*>(proc), userdata1,
                       userdata2,
                       reinterpret_cast<void*>(guest_to_host_thunk_));
#if XE_PLATFORM_MAC
  pthread_jit_write_protect_np(1);
#endif

  // Flush instruction cache for the new trampoline code.
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), write_pos, kGuestTrampolineSize);
#else
  __builtin___clear_cache(
      reinterpret_cast<char*>(write_pos),
      reinterpret_cast<char*>(write_pos + kGuestTrampolineSize));
#endif

  uint32_t indirection_guest_addr =
      GUEST_TRAMPOLINE_BASE +
      (static_cast<uint32_t>(new_index) * GUEST_TRAMPOLINE_MIN_LEN);

  code_cache()->AddIndirection64(indirection_guest_addr,
                                 reinterpret_cast<uint64_t>(write_pos));

  return indirection_guest_addr;
}

void A64Backend::FreeGuestTrampoline(uint32_t trampoline_addr) {
  xenia_assert(trampoline_addr >= GUEST_TRAMPOLINE_BASE &&
               trampoline_addr < GUEST_TRAMPOLINE_END);
  size_t index =
      (trampoline_addr - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;
  guest_trampoline_address_bitmap_.Release(index);
}

bool A64Backend::trace_instr_available() const { return IsTracingInstr(); }
bool A64Backend::trace_data_available() const { return IsTracingData(); }
bool A64Backend::trace_func_available() const { return IsTracingFunc(); }
bool A64Backend::trace_instr_enabled() const { return GetTraceInstrEnabled(); }
void A64Backend::set_trace_instr_enabled(bool value) {
  SetTraceInstrEnabled(value);
}
bool A64Backend::trace_data_enabled() const { return GetTraceDataEnabled(); }
void A64Backend::set_trace_data_enabled(bool value) {
  SetTraceDataEnabled(value);
}
bool A64Backend::trace_func_enabled() const { return GetTraceFuncEnabled(); }
void A64Backend::set_trace_func_enabled(bool value) {
  SetTraceFuncEnabled(value);
}

std::string A64Backend::FormatSequenceKey(uint64_t key) const {
  return a64::FormatSequenceKey(key);
}

void A64Backend::SetGuestRoundingMode(void* ctx, unsigned int mode) {
  A64BackendContext* bctx = BackendContextForGuestContext(ctx);
  uint32_t control = mode & 7;
  uint32_t fpcr_val = kGuestFpcrTable[control];
#if XE_COMPILER_MSVC
  // MSVC ARM64 intrinsic: ARM64_FPCR = register ID 0x5A20.
  _WriteStatusReg(0x5A20, static_cast<uint64_t>(fpcr_val));
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(static_cast<uint64_t>(fpcr_val)));
#endif
  bctx->fpcr_fpu = fpcr_val;
  if (control & 0b100) {
    bctx->flags |= (1u << kA64BackendNonIEEEMode);
  } else {
    bctx->flags &= ~(1u << kA64BackendNonIEEEMode);
  }
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(ctx);
  ppc_context->fpscr.bits.rn = control;
  ppc_context->fpscr.bits.ni = control >> 2;
}

bool A64Backend::PopulatePseudoStacktrace(GuestPseudoStackTrace* st) {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return false;
  }

  ThreadState* thrd_state = ThreadState::Get();
  if (!thrd_state) {
    return false;
  }
  ppc::PPCContext* ctx = thrd_state->context();
  A64BackendContext* backend_ctx = BackendContextForGuestContext(ctx);

  if (!backend_ctx->stackpoints || backend_ctx->current_stackpoint_depth < 2) {
    return false;
  }
  uint32_t depth = backend_ctx->current_stackpoint_depth - 1;
  uint32_t num_entries_to_populate =
      std::min(MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES, depth);

  st->count = num_entries_to_populate;
  st->truncated_flag = num_entries_to_populate < depth ? 1 : 0;

  A64BackendStackpoint* current_stackpoint =
      &backend_ctx->stackpoints[backend_ctx->current_stackpoint_depth - 1];

  for (uint32_t stp_index = 0; stp_index < num_entries_to_populate;
       ++stp_index) {
    st->return_addrs[stp_index] = current_stackpoint->guest_return_address_;
    current_stackpoint--;
  }
  return true;
}

void A64Backend::RecordMMIOExceptionForGuestInstruction(void* host_address) {
  uint64_t host_addr_u64 = reinterpret_cast<uint64_t>(host_address);
  auto fnfor = code_cache()->LookupFunction(host_addr_u64);
  if (fnfor) {
    uint32_t guestaddr = fnfor->MapMachineCodeToGuestAddress(host_addr_u64);
    Module* guest_module = fnfor->module();
    if (guest_module) {
      XexModule* xex_guest_module = dynamic_cast<XexModule*>(guest_module);
      if (xex_guest_module) {
        cpu::InfoCacheFlags* icf =
            xex_guest_module->GetInstructionAddressFlags(guestaddr);
        if (icf) {
          cpu::InfoCacheFlags bits{};
          bits.accessed_mmio = true;
          cpu::AtomicSetInfoCacheFlags(icf, bits);
        }
      }
    }
  }
}

bool A64Backend::ExceptionCallbackThunk(Exception* ex, void* data) {
  auto* backend = reinterpret_cast<A64Backend*>(data);
  return backend->ExceptionCallback(ex);
}

bool A64Backend::ExceptionCallback(Exception* ex) {
  if (ex->code() == Exception::Code::kAccessViolation) {
    if (code_cache_->CommitIndirectionFault(ex->fault_address())) {
      // A slot in a chunk of the table nothing had committed yet.
      return true;
    }
    if (!ex->pc() && !code_cache_->encoded_indirection()) {
      // A call through a slot of a chunk another thread was still filling.
      // JIT calls leave the guest address in w16, as the resolve thunk takes
      // it, and return into the code cache.
      const HostThreadContext& context = *ex->thread_context();
      const uint64_t code_base = code_cache_->execute_base_address();
      if (code_cache_->HasIndirectionSlot(uint32_t(context.x[16])) &&
          context.x[30] - code_base < code_cache_->total_size()) {
        ex->set_resume_pc(reinterpret_cast<uint64_t>(resolve_function_thunk_));
        return true;
      }
    }
  }
  if (ex->code() != Exception::Code::kIllegalInstruction) {
    return false;
  }

  // Verify it's our BRK #0 instruction.
  auto instruction_bytes =
      xe::load<uint32_t>(reinterpret_cast<void*>(ex->pc()));
  if (instruction_bytes != kArm64Brk0) {
    return false;
  }

  return processor()->OnThreadBreakpointHit(ex);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
