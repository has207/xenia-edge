/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <algorithm>
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_seq_util.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"

DECLARE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses);
DECLARE_bool(emit_inline_mmio_checks);

DEFINE_bool(inline_loadclock, false,
            "Directly read cached guest clock without calling the LoadClock "
            "method (it gets repeatedly updated by calls from other threads)",
            "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_memory = 0;

static bool IsPossibleMMIOInstruction(A64Emitter& e, const hir::Instr* i) {
  if (!cvars::emit_mmio_aware_stores_for_recorded_exception_addresses) {
    return false;
  }
  uint32_t guest_address = i->GuestAddressFor();
  if (!guest_address) {
    return false;
  }

  auto* guest_module = e.GuestModule();
  if (!guest_module) {
    return false;
  }
  auto* flags = guest_module->GetInstructionAddressFlags(guest_address);
  return flags && flags->accessed_mmio;
}

// ============================================================================
// OPCODE_DELAY_EXECUTION
// ============================================================================
static constexpr uint64_t kDb16cycSleepNs = 60000;
static constexpr uint64_t kDb16cycGapNs = 1000;

// CNTFRQ_EL0.
static uint64_t SpinWaitTimerFreq() {
  static const uint64_t freq = [] {
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
  }();
  return freq;
}

static void SpinWaitRelease(void* raw_context) {
  if (backend::spin_wait_release_handler) {
    backend::spin_wait_release_handler(raw_context, kDb16cycSleepNs);
    return;
  }
  xe::threading::NanoSleep(int64_t(kDb16cycSleepNs));
}

static constexpr uint32_t kSpinWaitRetripIters = 64;

// State is keyed to the loop header: the iteration counter is shared by every
// tagged site on the thread.
static void InjectedSpinWaitTrip(void* raw_context, uint64_t site) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* bctx = static_cast<A64Backend*>(context->processor->backend())
                   ->BackendContextForGuestContext(raw_context);
  uint64_t now;
  asm volatile("mrs %0, cntvct_el0" : "=r"(now));
  const uint32_t site32 = uint32_t(site);
  if (site32 != bctx->spin_wait_site) {
    bctx->spin_wait_site = site32;
    bctx->spin_wait_armed = 0;
    bctx->spin_wait_spins = 0;
    bctx->spin_wait_reset_tick = now;
    return;
  }
  const uint64_t elapsed_ticks = now - bctx->spin_wait_reset_tick;
  const uint64_t iters = bctx->spin_wait_armed ? kSpinWaitRetripIters
                                               : cvars::spin_wait_yield_after;
  const uint64_t budget_ticks = iters * uint64_t(cvars::spin_wait_max_iter_ns) *
                                SpinWaitTimerFreq() / 1000000000ull;
  if (elapsed_ticks <= budget_ticks) {
    if (!bctx->spin_wait_armed) {
      XELOGD("SpinWait: escalating guest loop {:08X}", site32);
    }
    SpinWaitRelease(raw_context);
    const uint32_t threshold = cvars::spin_wait_yield_after;
    bctx->spin_wait_armed = 1;
    bctx->spin_wait_spins =
        threshold > kSpinWaitRetripIters ? threshold - kSpinWaitRetripIters : 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(now));
    bctx->spin_wait_reset_tick = now;
  } else {
    bctx->spin_wait_armed = 0;
    bctx->spin_wait_spins = 0;
    bctx->spin_wait_reset_tick = now;
  }
}

struct DELAY_EXECUTION
    : Sequence<DELAY_EXECUTION, I<OPCODE_DELAY_EXECUTION, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & hir::DELAY_EXECUTION_INJECTED) {
      EmitInjectedSpinCheck(e, i);
      return;
    }
    // db16cyc throttles a guest spin loop. yield is the literal translation
    // but NOPs on Cortex-X/A7xx, so the sled cost nothing; isb is the usual
    // stand-in - a pipeline flush with no guest-observable effect. Coalesce
    // consecutive barriers so a long sled stays one instruction.
    constexpr uint32_t kIsbSy = 0xD5033FDFu;
    if (e.getSize() >= sizeof(uint32_t) &&
        *reinterpret_cast<const uint32_t*>(e.getCurr() - sizeof(uint32_t)) ==
            kIsbSy) {
      return;
    }
    const uint32_t yield_after = cvars::db16cyc_yield_after;
    if (!yield_after) {
      e.isb(Xbyak_aarch64::SY);
      return;
    }

    const int32_t spins_offset =
        static_cast<int32_t>(offsetof(A64BackendContext, db16cyc_spins));
    const int32_t last_tick_offset =
        static_cast<int32_t>(offsetof(A64BackendContext, db16cyc_last_tick));
    const uint64_t gap_ticks = std::max<uint64_t>(
        1, kDb16cycGapNs * SpinWaitTimerFreq() / 1000000000ull);
    auto& not_consecutive = e.NewCachedLabel();
    auto& do_release = e.NewCachedLabel();
    auto& do_delay = e.NewCachedLabel();
    // x16 = CNTVCT_EL0, x17 = ticks since the previous delay.
    e.mrs(e.x16, 3, 3, 14, 0, 2);
    e.ldr(e.x17, ptr(e.GetBackendCtxReg(), last_tick_offset));
    e.str(e.x16, ptr(e.GetBackendCtxReg(), last_tick_offset));
    e.sub(e.x17, e.x16, e.x17);
    e.mov(e.x16, gap_ticks);
    e.cmp(e.x17, e.x16);
    e.b(HI, not_consecutive);
    e.ldr(e.w16, ptr(e.GetBackendCtxReg(), spins_offset));
    e.add(e.w16, e.w16, 1);
    e.mov(e.w17, static_cast<uint64_t>(yield_after));
    e.cmp(e.w16, e.w17);
    e.b(HS, do_release);
    e.str(e.w16, ptr(e.GetBackendCtxReg(), spins_offset));
    e.b(do_delay);
    e.L(not_consecutive);
    e.mov(e.w16, 1);
    e.str(e.w16, ptr(e.GetBackendCtxReg(), spins_offset));
    e.b(do_delay);
    e.L(do_release);
    e.str(e.wzr, ptr(e.GetBackendCtxReg(), spins_offset));
    e.CallNativeSafe(reinterpret_cast<void*>(SpinWaitRelease));
    e.L(do_delay);
    // Every path ends in the isb the coalescing test looks for.
    e.isb(Xbyak_aarch64::SY);
  }

  static void EmitInjectedSpinCheck(A64Emitter& e, const EmitArgType& i) {
    const uint32_t threshold = cvars::spin_wait_yield_after;
    if (!threshold) {
      return;
    }
    const int32_t spins_offset =
        static_cast<int32_t>(offsetof(A64BackendContext, spin_wait_spins));
    auto& not_yet = e.NewCachedLabel();
    e.ldr(e.w16, ptr(e.GetBackendCtxReg(), spins_offset));
    e.add(e.w16, e.w16, 1);
    e.str(e.w16, ptr(e.GetBackendCtxReg(), spins_offset));
    e.mov(e.w17, static_cast<uint64_t>(threshold));
    e.cmp(e.w16, e.w17);
    e.b(LO, not_yet);
    // x1 passes through the thunk as the helper's second argument.
    e.mov(e.w1, static_cast<uint64_t>(uint32_t(i.instr->src1.offset)));
    e.CallNativeSafe(reinterpret_cast<void*>(InjectedSpinWaitTrip));
    e.L(not_yet);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DELAY_EXECUTION, DELAY_EXECUTION);

// ============================================================================
// OPCODE_MEMORY_BARRIER
// ============================================================================
struct MEMORY_BARRIER
    : Sequence<MEMORY_BARRIER, I<OPCODE_MEMORY_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.dmb(Xbyak_aarch64::ISH);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMORY_BARRIER, MEMORY_BARRIER);

// ============================================================================
// OPCODE_LOAD_BARRIER
// ============================================================================
struct LOAD_BARRIER : Sequence<LOAD_BARRIER, I<OPCODE_LOAD_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.dmb(Xbyak_aarch64::ISHLD);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_BARRIER, LOAD_BARRIER);

// ============================================================================
// OPCODE_CACHE_CONTROL
// ============================================================================
struct CACHE_CONTROL
    : Sequence<CACHE_CONTROL,
               I<OPCODE_CACHE_CONTROL, VoidOp, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    bool is_clflush = false, is_prefetch = false, is_prefetchw = false;
    switch (CacheControlType(i.instr->flags)) {
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH:
        is_prefetch = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
        is_prefetchw = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE:
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
        is_clflush = true;
        break;
      default:
        return;
    }
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    size_t cache_line_size = i.src2.value;
    if (is_clflush) {
      // dc civac, x0
      e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
    }
    if (is_prefetch) {
      e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
    } else if (is_prefetchw) {
      e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
    }
    if (cache_line_size >= 128) {
      e.eor(e.x0, e.x0, 64);
      if (is_clflush) {
        // dc civac, x0
        e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
      }
      if (is_prefetch) {
        e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
      } else if (is_prefetchw) {
        e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CACHE_CONTROL, CACHE_CONTROL);

template <typename T, bool swap>
static void MMIOAwareStore(void* _ctx, unsigned int guestaddr, T value) {
  if (swap) {
    value = xe::byte_swap(value);
  }
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr) = value;
  } else {
    value = xe::byte_swap(value);
    gaddr->write(nullptr, gaddr->callback_context, guestaddr, value);
  }
}

template <typename T, bool swap>
static T MMIOAwareLoad(void* _ctx, unsigned int guestaddr) {
  T value;
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    value = *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr);
    if (swap) {
      value = xe::byte_swap(value);
    }
  } else {
    value = gaddr->read(nullptr, gaddr->callback_context, guestaddr);
  }
  return value;
}

// ============================================================================
// OPCODE_LOAD
// ============================================================================
struct LOAD_I8 : Sequence<LOAD_I8, I<OPCODE_LOAD, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldrb(i.dest, ptr(e.GetMembaseReg(), addr));
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w2, i.dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI8));
    }
  }
};
struct LOAD_I16 : Sequence<LOAD_I16, I<OPCODE_LOAD, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldrh(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(i.dest, i.dest);
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w2, i.dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI16));
    }
  }
};
struct LOAD_I32 : Sequence<LOAD_I32, I<OPCODE_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        auto addr = ComputeMemoryAddress(e, i.src1);
        e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(i.dest, i.dest);
        }
      }
      e.L(done);
    } else {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(i.dest, i.dest);
      }
      if (IsTracingData()) {
        addr = ComputeMemoryAddress(e, i.src1);
        e.mov(e.w2, i.dest);
        e.mov(e.w1, WReg(addr.getIdx()));
        e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI32));
      }
    }
  }
};
struct LOAD_I64 : Sequence<LOAD_I64, I<OPCODE_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.x2, i.dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI64));
    }
  }
};
struct LOAD_F32 : Sequence<LOAD_F32, I<OPCODE_LOAD, F32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.ldr(e.w0, ptr(e.GetMembaseReg(), addr));
      e.rev(e.w0, e.w0);
      e.fmov(i.dest, e.w0);
    } else {
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.mov(VReg(0).b16, VReg(i.dest.reg().getIdx()).b16);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF32));
    }
  }
};
struct LOAD_F64 : Sequence<LOAD_F64, I<OPCODE_LOAD, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.ldr(e.x0, ptr(e.GetMembaseReg(), addr));
      e.rev(e.x0, e.x0);
      e.fmov(i.dest, e.x0);
    } else {
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.mov(VReg(0).b16, VReg(i.dest.reg().getIdx()).b16);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF64));
    }
  }
};
struct LOAD_V128 : Sequence<LOAD_V128, I<OPCODE_LOAD, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word (PPC BE -> ARM64 LE).
      auto idx = i.dest.reg().getIdx();
      e.rev32(VReg16B(idx), VReg16B(idx));
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x2, e.GetMembaseReg(), addr);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD, LOAD_I8, LOAD_I16, LOAD_I32, LOAD_I64,
                     LOAD_F32, LOAD_F64, LOAD_V128);

// ============================================================================
// OPCODE_STORE
// ============================================================================
struct STORE_I8 : Sequence<STORE_I8, I<OPCODE_STORE, VoidOp, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.src2.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      e.strb(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      e.strb(i.src2, ptr(e.GetMembaseReg(), addr));
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.ldrb(e.w2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI8));
    }
  }
};
struct STORE_I16 : Sequence<STORE_I16, I<OPCODE_STORE, VoidOp, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src2.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, i.src2);
      }
      e.strh(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
        e.strh(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.strh(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.ldrh(e.w2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI16));
    }
  }
};
struct STORE_I32 : Sequence<STORE_I32, I<OPCODE_STORE, VoidOp, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, i.src2);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path — copy value to w2 before w1 in case src2 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, i.src2);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        auto addr = ComputeMemoryAddress(e, i.src1);
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src2.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, i.src2);
          }
          e.str(e.w17, ptr(e.GetMembaseReg(), addr));
        } else {
          if (i.src2.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src2.constant())));
            e.str(e.w17, ptr(e.GetMembaseReg(), addr));
          } else {
            e.str(i.src2, ptr(e.GetMembaseReg(), addr));
          }
        }
      }
      e.L(done);
    } else {
      auto addr = ComputeMemoryAddress(e, i.src1);
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src2.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, i.src2);
        }
        e.str(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        if (i.src2.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src2.constant())));
          e.str(e.w17, ptr(e.GetMembaseReg(), addr));
        } else {
          e.str(i.src2, ptr(e.GetMembaseReg(), addr));
        }
      }
      if (IsTracingData()) {
        addr = ComputeMemoryAddress(e, i.src1);
        e.ldr(e.w2, ptr(e.GetMembaseReg(), addr));
        e.mov(e.w1, WReg(addr.getIdx()));
        e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI32));
      }
    }
  }
};
struct STORE_I64 : Sequence<STORE_I64, I<OPCODE_STORE, VoidOp, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src2.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src2);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.constant()));
        e.str(e.x17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.x2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI64));
    }
  }
};
struct STORE_F32 : Sequence<STORE_F32, I<OPCODE_STORE, VoidOp, I64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint32_t val =
            xe::byte_swap(static_cast<uint32_t>(i.src2.value->constant.i32));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.fmov(e.w17, i.src2);
        e.rev(e.w17, e.w17);
      }
      e.str(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.value->constant.i32));
        e.str(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.s0, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF32));
    }
  }
};
struct STORE_F64 : Sequence<STORE_F64, I<OPCODE_STORE, VoidOp, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val =
            xe::byte_swap(static_cast<uint64_t>(i.src2.value->constant.i64));
        e.mov(e.x17, val);
      } else {
        e.fmov(e.x17, i.src2);
        e.rev(e.x17, e.x17);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.value->constant.i64));
        e.str(e.x17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.d0, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF64));
    }
  }
};
struct STORE_V128
    : Sequence<STORE_V128, I<OPCODE_STORE, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word, store via scratch v0.
      int idx = SrcVReg(e, i.src2, 0);
      e.rev32(VReg16B(0), VReg16B(idx));
      e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        LoadV128Const(e, 0, i.src2.constant());
        e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      auto trace_addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x2, e.GetMembaseReg(), trace_addr);
      e.mov(e.w1, WReg(trace_addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE, STORE_I8, STORE_I16, STORE_I32, STORE_I64,
                     STORE_F32, STORE_F64, STORE_V128);

// ============================================================================
// OPCODE_LOAD_CLOCK
// ============================================================================
struct LOAD_CLOCK : Sequence<LOAD_CLOCK, I<OPCODE_LOAD_CLOCK, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (cvars::inline_loadclock) {
      // Read the cached guest tick count maintained by the other subsystems
      // that call Clock::QueryGuestTickCount (same tradeoff as the x64
      // backend's inline_loadclock: consecutive mftb reads may observe the
      // same value between updates).
      e.ldr(e.x0, ptr(e.GetBackendCtxReg(),
                      static_cast<uint32_t>(
                          offsetof(A64BackendContext, guest_tick_count))));
      e.ldr(i.dest, ptr(e.x0));
      return;
    }
    if (cvars::clock_no_scaling && cvars::clock_source_raw) {
      // Mirror of the x64 backend's inline rdtsc path: with no scaling and
      // the raw source, Clock::QueryGuestTickCount is exactly
      // host_ticks * num / den. CNTVCT_EL0 is EL0-readable on the supported
      // hosts. The multiply is done in 64 bits, which is exact as long as
      // host_ticks * num cannot overflow; the bound below keeps that true for
      // centuries of typical (tens-of-MHz) counter uptime, and the reduced
      // ratio numerator is tiny on real hosts (e.g. 133/64 for a 49.875 MHz
      // guest clock over Apple's 24 MHz counter). Hosts with an oversized
      // numerator fall back to the helper call.
      const auto ratio = Clock::guest_tick_ratio();
      if (ratio.first <= (uint64_t(1) << 20)) {
        e.mrs(e.x0, 3, 3, 14, 0, 2);  // mrs x0, CNTVCT_EL0
        if (ratio.first != 1) {
          e.mov(e.x1, ratio.first);
          e.mul(e.x0, e.x0, e.x1);
        }
        if (ratio.second != 1) {
          e.mov(e.x1, ratio.second);
          e.udiv(e.x0, e.x0, e.x1);
        }
        e.mov(i.dest, e.x0);
        return;
      }
    }
    // Call QueryGuestTickCount which updates the clock from host ticks.
    // Reading the cached pointer directly would return stale values for
    // consecutive mftb instructions.
    e.CallNative(reinterpret_cast<void*>(LoadClock));
    e.mov(i.dest, e.x0);
  }
  static uint64_t LoadClock(void* raw_context) {
    return Clock::QueryGuestTickCount();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CLOCK, LOAD_CLOCK);

// ============================================================================
// OPCODE_LOAD_OFFSET / OPCODE_STORE_OFFSET
// ============================================================================
struct LOAD_OFFSET_I8
    : Sequence<LOAD_OFFSET_I8, I<OPCODE_LOAD_OFFSET, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldrb(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.mov(e.w2, i.dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI8));
    }
  }
};
struct LOAD_OFFSET_I16
    : Sequence<LOAD_OFFSET_I16, I<OPCODE_LOAD_OFFSET, I16Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldrh(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(i.dest, i.dest);
    }
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.mov(e.w2, i.dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI16));
    }
  }
};
struct LOAD_OFFSET_I32
    : Sequence<LOAD_OFFSET_I32, I<OPCODE_LOAD_OFFSET, I32Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(i.dest, i.dest);
        }
      }
      e.L(done);
    } else {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(i.dest, i.dest);
      }
      if (IsTracingData()) {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        e.mov(e.w2, i.dest);
        e.mov(e.w1, e.w0);
        e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI32));
      }
    }
  }
};
struct LOAD_OFFSET_I64
    : Sequence<LOAD_OFFSET_I64, I<OPCODE_LOAD_OFFSET, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.mov(e.x2, i.dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI64));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_OFFSET, LOAD_OFFSET_I8, LOAD_OFFSET_I16,
                     LOAD_OFFSET_I32, LOAD_OFFSET_I64);

struct STORE_OFFSET_I8
    : Sequence<STORE_OFFSET_I8,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.src3.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFF));
      e.strb(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      e.strb(i.src3, ptr(e.GetMembaseReg(), e.x0));
    }
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.ldrb(e.w2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI8));
    }
  }
};
struct STORE_OFFSET_I16
    : Sequence<STORE_OFFSET_I16,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src3.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, i.src3);
      }
      e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFFFF));
        e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.strh(i.src3, ptr(e.GetMembaseReg(), e.x0));
      }
    }
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.ldrh(e.w2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI16));
    }
  }
};
struct STORE_OFFSET_I32
    : Sequence<STORE_OFFSET_I32,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, i.src3);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path — copy value to w2 before w1 in case src3 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, i.src3);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src3.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, i.src3);
          }
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          if (i.src3.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src3.constant())));
            e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
          } else {
            e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
          }
        }
      }
      e.L(done);
    } else {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src3.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, i.src3);
        }
        e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        if (i.src3.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src3.constant())));
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
        }
      }
      if (IsTracingData()) {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        e.ldr(e.w2, ptr(e.GetMembaseReg(), e.x0));
        e.mov(e.w1, e.w0);
        e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI32));
      }
    }
  }
};
struct STORE_OFFSET_I64
    : Sequence<STORE_OFFSET_I64,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src3.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src3);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src3.constant()));
        e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
      }
    }
    if (IsTracingData()) {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.ldr(e.x2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI64));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_OFFSET, STORE_OFFSET_I8, STORE_OFFSET_I16,
                     STORE_OFFSET_I32, STORE_OFFSET_I64);

// ============================================================================
// OPCODE_MEMSET
// ============================================================================
static const bool zva_enable = (xe_cpu_mrs(DCZID_EL0) & 0b1'0000) == 0;
static const uint64_t zva_length = (4ULL << (xe_cpu_mrs(DCZID_EL0) & 0b0'1111));

struct MEMSET_I64
    : Sequence<MEMSET_I64, I<OPCODE_MEMSET, VoidOp, I64Op, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    assert_true(i.src3.is_constant);
    assert_true(i.src2.constant() == 0);
    // memset(membase + guest_addr, 0, length)
    // Only used by dcbz/dcbz128: constant zero value, constant aligned size.
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    const uint64_t len = i.src3.constant();
    uint64_t off = 0;

    // Use `dc zva` if it writes more bytes at a time than STP
    if (zva_enable && len >= zva_length && zva_length > 16) {
      for (; off + zva_length <= len; off += zva_length) {
        // dc zva, x0
        e.sys(0b011, 0b0111, 0b0100, 0b001, e.x0);
        if (off + zva_length < len) {
          e.add(e.x0, e.x0, zva_length);
        }
      }
    }

    // Inline with STP xzr, xzr pairs (16 bytes each)
    for (; off + 16 <= len; off += 16) {
      e.stp(e.xzr, e.xzr, AdrPostImm(e.x0, 16));
    }
    // Handle remaining bytes (0-15)
    if (off + 8 <= len) {
      e.str(e.xzr, AdrPostImm(e.x0, 8));
      off += 8;
    }
    if (off + 4 <= len) {
      e.str(e.wzr, AdrPostImm(e.x0, 4));
      off += 4;
    }
    // Byte loop for any remaining 0-3 bytes
    for (; off + 1 <= len; off += 1) {
      e.strb(e.wzr, AdrPostImm(e.x0, 1));
    }

    if (IsTracingData()) {
      auto trace_addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w3, static_cast<uint64_t>(i.src3.constant()));
      e.mov(e.w2, static_cast<uint64_t>(i.src2.constant()));
      e.mov(e.w1, WReg(trace_addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemset));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMSET, MEMSET_I64);

// ============================================================================
// ============================================================================
// OPCODE_ATOMIC_COMPARE_EXCHANGE
// ============================================================================
struct ATOMIC_COMPARE_EXCHANGE_I32
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I32,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Compute full host address (ldxr/stxr need base-only [Xn] addressing).
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x4, e.GetMembaseReg(), addr);
    // src2 = expected (use w5), src3 = desired (use w6).
    if (i.src2.is_constant) {
      e.mov(e.w5,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    } else {
      e.mov(e.w5, i.src2);
    }
    if (i.src3.is_constant) {
      e.mov(e.w6,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
    } else {
      e.mov(e.w6, i.src3);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.w0, e.w5);
      e.casal(e.w5, e.w6, ptr(e.x4));
      e.cmp(e.w5, e.w0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      return;
    }

    auto& retry = e.NewCachedLabel();
    auto& fail = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    e.L(retry);
    e.ldaxr(e.w2, ptr(e.x4));
    e.cmp(e.w2, e.w5);
    e.b(Xbyak_aarch64::NE, fail);
    e.stlxr(e.w3, e.w6, ptr(e.x4));
    e.cbnz(e.w3, retry);
    e.mov(i.dest, 1);
    e.b(done);
    e.L(fail);
    e.clrex(15);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
struct ATOMIC_COMPARE_EXCHANGE_I64
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I64,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x4, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.x5, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x5, i.src2);
    }
    if (i.src3.is_constant) {
      e.mov(e.x6, static_cast<uint64_t>(i.src3.constant()));
    } else {
      e.mov(e.x6, i.src3);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.x0, e.x5);
      e.casal(e.x5, e.x6, ptr(e.x4));
      e.cmp(e.x5, e.x0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      return;
    }

    auto& retry = e.NewCachedLabel();
    auto& fail = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    e.L(retry);
    e.ldaxr(e.x2, ptr(e.x4));
    e.cmp(e.x2, e.x5);
    e.b(Xbyak_aarch64::NE, fail);
    e.stlxr(e.w3, e.x6, ptr(e.x4));
    e.cbnz(e.w3, retry);
    e.mov(i.dest, 1);
    e.b(done);
    e.L(fail);
    e.clrex(15);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ATOMIC_COMPARE_EXCHANGE,
                     ATOMIC_COMPARE_EXCHANGE_I32, ATOMIC_COMPARE_EXCHANGE_I64);

// ============================================================================
// OPCODE_LOAD_MMIO / OPCODE_STORE_MMIO
// ============================================================================
struct LOAD_MMIO_I32
    : Sequence<LOAD_MMIO_I32, I<OPCODE_LOAD_MMIO, I32Op, OffsetOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto read_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOReadCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(read_address));
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->read));
    e.rev(e.w0, e.w0);
    e.mov(i.dest, e.w0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_MMIO, LOAD_MMIO_I32);

struct STORE_MMIO_I32
    : Sequence<STORE_MMIO_I32,
               I<OPCODE_STORE_MMIO, VoidOp, OffsetOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto write_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOWriteCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr,
    //                   uint32_t value).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(write_address));
    if (i.src3.is_constant) {
      e.mov(e.w3, static_cast<uint64_t>(
                      xe::byte_swap(static_cast<uint32_t>(i.src3.constant()))));
    } else {
      e.mov(e.w3, i.src3);
      e.rev(e.w3, e.w3);
    }
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->write));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_MMIO, STORE_MMIO_I32);

// ============================================================================
// OPCODE_RESERVED_LOAD / OPCODE_RESERVED_STORE
// ============================================================================
// RESERVED_LOAD/STORE keep a global per-granule generation counter so a stwcx.
// on one thread invalidates concurrent lwarx reservations on others (PPC
// semantics). A bare ldaxr/stlxr pair would only protect against contention on
// the same host cache line; ABA on the cached value would silently succeed.
//
// The sequences below mirror the host-side helpers in a64_backend.cc and share
// their state, so a reservation taken by either can be completed by the other.
// Only x22-x28 and v4-v31 hold guest values, so x0-x17 are free scratch here.
static constexpr uint32_t kReserveFlagBit = 1u << kA64BackendHasReserveBit;

static constexpr uint32_t BackendCtxOffset(size_t offset) {
  return static_cast<uint32_t>(offset);
}

// Materializes the guest (untranslated) address the reservation is keyed on.
// Non-constant sources already sit in a callee-saved register that
// ComputeMemoryAddress leaves alone.
static WReg GuestAddressReg(A64Emitter& e, const I64Op& src,
                            const WReg& scratch) {
  if (src.is_constant) {
    e.mov(scratch,
          static_cast<uint64_t>(static_cast<uint32_t>(src.constant())));
    return scratch;
  }
  return WReg(src.reg().getIdx());
}

// Leaves the granule counter's address in x_granule.
static void EmitGranuleAddress(A64Emitter& e, const WReg& guest_address,
                               const XReg& x_granule, const WReg& w_scratch) {
  e.ldr(x_granule,
        ptr(e.GetBackendCtxReg(),
            BackendCtxOffset(offsetof(A64BackendContext, reserve_helper_))));
  e.ubfx(w_scratch, guest_address, A64_RESERVE_GRANULE_SHIFT,
         A64_RESERVE_ENTRY_BITS);
  e.add(x_granule, x_granule, XReg(w_scratch.getIdx()), Xbyak_aarch64::LSL, 2);
}

static void EmitTryAcquireReservation(A64Emitter& e,
                                      const WReg& guest_address) {
  const auto bctx = e.GetBackendCtxReg();
  EmitGranuleAddress(e, guest_address, e.x8, e.w9);
  // snapshot the generation first, the acquire pins the value read after
  e.ldar(e.w10, ptr(e.x8));
  e.str(e.w10, ptr(bctx, BackendCtxOffset(
                             offsetof(A64BackendContext, reserve_generation))));
  e.str(guest_address, ptr(bctx, BackendCtxOffset(offsetof(A64BackendContext,
                                                           reserve_address))));
  // lwarx replaces any reservation this thread already held
  e.ldr(e.w11, ptr(bctx, BackendCtxOffset(offsetof(A64BackendContext, flags))));
  e.orr(e.w11, e.w11, kReserveFlagBit);
  e.str(e.w11, ptr(bctx, BackendCtxOffset(offsetof(A64BackendContext, flags))));
}

struct RESERVED_LOAD_I32
    : Sequence<RESERVED_LOAD_I32, I<OPCODE_RESERVED_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg guest_address = GuestAddressReg(e, i.src1, e.w12);
    auto addr = ComputeMemoryAddress(e, i.src1);
    EmitTryAcquireReservation(e, guest_address);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    e.mov(e.w0, i.dest);
    e.str(e.x0, ptr(e.GetBackendCtxReg(),
                    BackendCtxOffset(
                        offsetof(A64BackendContext, cached_reserve_value_))));
  }
};
struct RESERVED_LOAD_I64
    : Sequence<RESERVED_LOAD_I64, I<OPCODE_RESERVED_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg guest_address = GuestAddressReg(e, i.src1, e.w12);
    auto addr = ComputeMemoryAddress(e, i.src1);
    EmitTryAcquireReservation(e, guest_address);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    e.str(i.dest, ptr(e.GetBackendCtxReg(),
                      BackendCtxOffset(
                          offsetof(A64BackendContext, cached_reserve_value_))));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_LOAD, RESERVED_LOAD_I32,
                     RESERVED_LOAD_I64);

// x1 = host address, x2 = value to store, both set up by the caller.
// Writes 1 to dest if the store landed, 0 otherwise.
static void EmitReservedStore(A64Emitter& e, const I8Op& dest,
                              const WReg& guest_address, bool bit64) {
  using namespace Xbyak_aarch64;
  const auto bctx = e.GetBackendCtxReg();
  const uint32_t flags_offset =
      BackendCtxOffset(offsetof(A64BackendContext, flags));
  const uint32_t dest_idx = dest.reg().getIdx();

  auto& done = e.NewCachedLabel();
  auto& fail =
      e.AddToTail([dest_idx, &done](A64Emitter& e, Xbyak_aarch64::Label&) {
        e.mov(WReg(dest_idx), 0);
        e.b(done);
      });

  // stwcx. always clears the reservation, stored or not
  e.ldr(e.w8, ptr(bctx, flags_offset));
  e.and_(e.w9, e.w8, static_cast<uint64_t>(~kReserveFlagBit));
  e.str(e.w9, ptr(bctx, flags_offset));
  e.tst(e.w8, static_cast<uint64_t>(kReserveFlagBit));
  e.b(EQ, fail);

  // the reservation must be for the address we're storing to
  e.ldr(e.w10, ptr(bctx, BackendCtxOffset(
                             offsetof(A64BackendContext, reserve_address))));
  e.cmp(e.w10, guest_address);
  e.b(NE, fail);

  EmitGranuleAddress(e, guest_address, e.x11, e.w13);
  // a store to this granule since our lwarx kills the reservation
  e.ldar(e.w14, ptr(e.x11));
  e.ldr(e.w15, ptr(bctx, BackendCtxOffset(
                             offsetof(A64BackendContext, reserve_generation))));
  e.cmp(e.w14, e.w15);
  e.b(NE, fail);

  e.ldr(e.x16, ptr(bctx, BackendCtxOffset(offsetof(A64BackendContext,
                                                   cached_reserve_value_))));

  if (e.IsFeatureEnabled(kA64EmitLSE)) {
    // casal returns the old value in the compare register, so keep a copy
    e.mov(e.x3, e.x16);
    if (bit64) {
      e.casal(e.x16, e.x2, ptr(e.x1));
      e.cmp(e.x16, e.x3);
    } else {
      e.casal(e.w16, e.w2, ptr(e.x1));
      e.cmp(e.w16, e.w3);
    }
    e.b(NE, fail);
    // the store landed, so kill other reservations on this granule
    e.mov(e.w17, 1);
    e.staddl(e.w17, ptr(e.x11));
  } else {
    // The exclusive monitor is still held when the compare fails, so that
    // path has to clear it before joining the common failure tail.
    auto& cas_mismatch =
        e.AddToTail([dest_idx, &done](A64Emitter& e, Xbyak_aarch64::Label&) {
          e.clrex(15);
          e.mov(WReg(dest_idx), 0);
          e.b(done);
        });
    auto& cas_retry = e.NewCachedLabel();
    auto& bump_retry = e.NewCachedLabel();

    e.L(cas_retry);
    if (bit64) {
      e.ldaxr(e.x14, ptr(e.x1));
      e.cmp(e.x14, e.x16);
      e.b(NE, cas_mismatch);
      e.stlxr(e.w15, e.x2, ptr(e.x1));
    } else {
      e.ldaxr(e.w14, ptr(e.x1));
      e.cmp(e.w14, e.w16);
      e.b(NE, cas_mismatch);
      e.stlxr(e.w15, e.w2, ptr(e.x1));
    }
    e.cbnz(e.w15, cas_retry);

    // the store landed, so kill other reservations on this granule
    e.L(bump_retry);
    e.ldxr(e.w14, ptr(e.x11));
    e.add(e.w14, e.w14, 1);
    e.stlxr(e.w15, e.w14, ptr(e.x11));
    e.cbnz(e.w15, bump_retry);
  }

  e.mov(dest, 1);
  e.L(done);
}

struct RESERVED_STORE_I32
    : Sequence<RESERVED_STORE_I32,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg guest_address = GuestAddressReg(e, i.src1, e.w12);
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x1, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.w2,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    } else {
      e.mov(e.w2, WReg(i.src2.reg().getIdx()));
    }
    EmitReservedStore(e, i.dest, guest_address, false);
  }
};
struct RESERVED_STORE_I64
    : Sequence<RESERVED_STORE_I64,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg guest_address = GuestAddressReg(e, i.src1, e.w12);
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x1, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.x2, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x2, XReg(i.src2.reg().getIdx()));
    }
    EmitReservedStore(e, i.dest, guest_address, true);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_STORE, RESERVED_STORE_I32,
                     RESERVED_STORE_I64);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
