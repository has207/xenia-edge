/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
#define XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_

#include <utility>

#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/cpu_flags.h"

#include "xbyak_aarch64.h"

#if XE_COMPILER_MSVC
#include <intrin.h>
constexpr uint32_t DCZID_EL0 = ARM64_SYSREG(0b11, 0b011, 0b0000, 0b0000, 0b111);
#define xe_cpu_mrs(reg) _ReadStatusReg(reg)
#elif XE_COMPILER_CLANG || XE_COMPILER_GNUC
#include <arm_acle.h>
#define xe_cpu_mrs(reg) __arm_rsr64(#reg)
#else
#error "No MRS wrapper available for current compiler implemented."
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using Xbyak_aarch64::QReg;
using Xbyak_aarch64::SReg;
using Xbyak_aarch64::VReg;
using Xbyak_aarch64::WReg;
using Xbyak_aarch64::XReg;

template <typename Fn>
inline void EmitWithFpcrMode(A64Emitter& e, FPCRMode mode, Fn&& emit_op) {
  // Enter the requested FPCR mode using tracked lazy switching.  If the
  // emitter is already in that mode (e.g. consecutive VMX ops in the same
  // basic block) this is a no-op — no system register access at all.
  // FPU mode is restored at block boundaries and calls via ForgetFpcrMode,
  // or on demand by scalar FP sequences via ChangeFpcrMode(Fpu).
  e.ChangeFpcrMode(mode);
  emit_op();
}

template <typename Fn>
inline void EmitWithVmxFpcr(A64Emitter& e, Fn&& emit_op) {
  EmitWithFpcrMode(e, FPCRMode::Vmx, std::forward<Fn>(emit_op));
}

// The multiply-add family and the dot products flush denormals whatever NJM
// says. Pinning that on needs an FPCR of its own, which costs an msr wherever
// they interleave with the rest of VMX, so it is opt in. Without it they follow
// NJM like every other VMX op, which only differs once a title clears VSCR.NJ.
inline FPCRMode VmxDenormalFlushFpcrMode() {
  return cvars::accurate_vmx_denormal_flush ? FPCRMode::VmxDaz : FPCRMode::Vmx;
}

template <typename Fn>
inline void EmitWithVmxDenormalFlushFpcr(A64Emitter& e, Fn&& emit_op) {
  EmitWithFpcrMode(e, VmxDenormalFlushFpcrMode(), std::forward<Fn>(emit_op));
}

// True iff the 64-bit value is encodable as a MOVI Dn, #imm8 immediate.
// MOVI replicates the 8-bit immediate "a:b:c:d:e:f:g:h" into the bytes
// "aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffffgggggggghhhhhhhh", so
// every byte of the input must be 0x00 or 0xFF.
inline bool IsMovi64Imm(uint64_t value) {
  if (value == 0 || value == ~uint64_t(0)) {
    return true;
  }
  for (int shift = 0; shift < 8; ++shift) {
    const uint8_t shift_u8 = static_cast<uint8_t>(value >> (shift * 8));
    if (shift_u8 != 0 && shift_u8 != 0xFF) {
      return false;
    }
  }
  return true;
}

// True iff `imm` is encodable as an AArch64 logical immediate (AND/ORR/EOR
// immediate forms) for the given register size (32 or 64). A logical
// immediate is a power-of-two-sized element (2..64 bits) containing a single
// circular run of ones, replicated to fill the register. All-zeros and
// all-ones are not encodable.
inline bool IsValidLogicalImm(uint64_t imm, unsigned reg_size) {
  if (reg_size == 32) {
    imm &= 0xFFFFFFFFull;
    imm |= imm << 32;
  }
  if (imm == 0 || imm == ~0ull) {
    return false;
  }
  // Find the smallest power-of-two period of the pattern.
  unsigned size = 64;
  do {
    const unsigned half = size / 2;
    const uint64_t mask = (1ull << half) - 1;
    if ((imm & mask) != ((imm >> half) & mask)) {
      break;
    }
    size = half;
  } while (size > 2);
  // The element must be a single circular run of ones: exactly two 0<->1
  // transitions when traversed circularly.
  const uint64_t elem_mask = (size == 64) ? ~0ull : ((1ull << size) - 1);
  const uint64_t elem = imm & elem_mask;
  const uint64_t rot1 = ((elem >> 1) | (elem << (size - 1))) & elem_mask;
  return xe::bit_count(elem ^ rot1) == 2;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbc defgh000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov32Imm(float f32) {
  const uint32_t u32 = std::bit_cast<uint32_t, float>(f32);
  const uint32_t sign = (u32 >> 31) & 1;
  int32_t exp = ((u32 >> 23) & 0xff) - 127;
  int64_t mantissa = u32 & 0x7fffff;

  // Too many mantissa bits
  if (mantissa & 0x7ffff) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 19;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }

  return true;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbb bbcdefgh 00000000 00000000 00000000 00000000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov64Imm(double f64) {
  const uint64_t u64 = std::bit_cast<uint64_t, double>(f64);
  int32_t exp = ((u64 >> 52) & 0x7ff) - 1023;
  int64_t mantissa = u64 & 0xfffffffffffffULL;

  // Too many mantissa bits
  if (mantissa & 0xffffffffffffULL) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 48;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }
  return true;
}

// Load a compile-time vec128_t constant into a NEON register.
// Values with no immediate form come from the literal pool, never a GPR.
inline void LoadV128Const(A64Emitter& e, int vreg_idx, const vec128_t& val) {
  // Fast common cases
  if (!val.low && !val.high) {
    // 0000...
    e.movi(VReg2D(vreg_idx), 0);
    return;
  } else if (val.low == ~uint64_t(0) && val.high == ~uint64_t(0)) {
    // 1111...
    e.movi(VReg2D(vreg_idx), ~0ULL);
    return;
  }

  // Element splats
  bool all_equal_u8 = true;
  const uint8_t splat_u8 = val.u8[0];
  for (unsigned i = 1; i < 16; ++i) {
    if (val.u8[i] != splat_u8) {
      all_equal_u8 = false;
      break;
    }
  }
  if (all_equal_u8) {
    e.movi(VReg(vreg_idx).b16, static_cast<uint8_t>(splat_u8));
    return;
  }

  bool all_equal_u16 = true;
  const uint16_t splat_u16 = val.u16[0];
  for (unsigned i = 1; i < 8; ++i) {
    if (val.u16[i] != splat_u16) {
      all_equal_u16 = false;
      break;
    }
  }
  if (all_equal_u16) {
    if ((splat_u16 & 0xFF'00) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 0), LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 8), LSL, 8);
    } else if ((splat_u16 & 0xFF'00) == 0xFF'00) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0x00'FF) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 8) & 0xFF,
             LSL, 8);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  bool all_equal_u32 = true;
  const uint32_t splat_u32 = val.u32[0];
  const float splat_f32 = val.f32[0];
  for (unsigned i = 1; i < 4; ++i) {
    if (val.u32[i] != splat_u32) {
      all_equal_u32 = false;
      break;
    }
  }
  if (all_equal_u32) {
    if ((splat_u32 & 0xFF'FF'FF'00) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 0), LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 8), LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 16), LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 24), LSL, 24);
    } else if ((splat_u32 & 0xFF'FF'FF'00) == 0xFF'FF'FF'00) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0xFF'FF'00'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 8) & 0xFF,
             LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0xFF'00'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 16) & 0xFF,
             LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0x00'FF'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 24) & 0xFF,
             LSL, 24);
    } else if (IsFmov32Imm(splat_f32)) {
      e.fmov(VReg(vreg_idx).s4, splat_f32);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  const bool all_equal_u64 = val.low == val.high;
  const double splat_f64 = val.f64[0];
  if (all_equal_u64) {
    if (IsMovi64Imm(val.low)) {
      e.movi(VReg2D(vreg_idx), val.low);
    } else if (IsFmov64Imm(splat_f64)) {
      e.fmov(VReg(vreg_idx).d2, splat_f64);
    } else {
      e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
    }
    return;
  }

  e.ldr(QReg(vreg_idx), e.GetV128ConstLabel(val));
}

// Resolve a V128 operand to a register index, loading constants into
// scratch_idx if needed.
template <typename T>
inline int SrcVReg(A64Emitter& e, const T& op, int scratch_idx) {
  if (op.is_constant) {
    LoadV128Const(e, scratch_idx, op.constant());
    return scratch_idx;
  }
  return op.reg().getIdx();
}

inline bool NeedsPhysicalRemap() {
  return xe::memory::allocation_granularity() > 0x1000;
}

inline void ApplyPhysicalRemapW0(A64Emitter& e) {
  using namespace Xbyak_aarch64;
  Xbyak_aarch64::Label skip;
  e.lsr(e.w17, e.w0, 29);
  e.cmp(e.w17, 7);
  // skip is bound one instruction later, so b_near is in range.
  e.b_near(NE, skip);
  e.add(e.w0, e.w0, 1, 12);  // + 0x1000 via LSL #12
  e.L(skip);
}

// Compute a guest memory address, returning the XReg for [x21, xN] addressing.
// For constants, loads the address into x0 (scratch).
inline XReg ComputeMemoryAddress(A64Emitter& e, const I64Op& guest) {
  using namespace Xbyak_aarch64;
  if (guest.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant());
    if (address >= 0xE0000000 && NeedsPhysicalRemap()) {
      address += 0x1000;
    }
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  } else {
    auto src = guest.reg();
    // Guest addresses are always 32-bit. Clear any stale upper bits before
    // applying the host membase so guest pointers can't escape above 4 GB.
    e.mov(e.w0, WReg(src.getIdx()));
    if (NeedsPhysicalRemap()) {
      ApplyPhysicalRemapW0(e);
    }
    return e.x0;
  }
}

inline bool GuestMemDirectIndex(const I64Op& guest, int* out_w_idx) {
  if (guest.is_constant || NeedsPhysicalRemap()) {
    return false;
  }
  *out_w_idx = guest.reg().getIdx();
  return true;
}

// The fallback address lives in x0; emit_access must not clobber it first.
template <typename Fn>
inline void EmitGuestMemAccess(A64Emitter& e, const I64Op& guest,
                               Fn&& emit_access) {
  int w_idx;
  if (GuestMemDirectIndex(guest, &w_idx)) {
    emit_access(ptr(e.GetMembaseReg(), WReg(w_idx), Xbyak_aarch64::UXTW));
  } else {
    emit_access(ptr(e.GetMembaseReg(), ComputeMemoryAddress(e, guest)));
  }
}

// The remap is decided on the effective address, not the base, as on x64.
template <typename OffsetOp>
inline XReg ComputeMemoryAddressOffset(A64Emitter& e, const I64Op& guest,
                                       const OffsetOp& offset) {
  using namespace Xbyak_aarch64;
  if (guest.is_constant && offset.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant()) +
                       static_cast<uint32_t>(offset.constant());
    if (address >= 0xE0000000 && NeedsPhysicalRemap()) {
      address += 0x1000;
    }
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  }
  if (guest.is_constant) {
    e.mov(e.w0, static_cast<uint64_t>(static_cast<uint32_t>(guest.constant())));
  } else {
    e.mov(e.w0, WReg(guest.reg().getIdx()));
  }
  // Guest address arithmetic wraps at 32 bits before the host membase is
  // applied. Keep the add in W registers so stale high bits can't escape into
  // the final host pointer.
  if (offset.is_constant) {
    const uint32_t imm = static_cast<uint32_t>(offset.constant());
    const uint32_t neg = 0u - imm;
    if (imm == 0) {
      // Nothing to add.
    } else if (imm <= 0xFFF) {
      e.add(e.w0, e.w0, imm);
    } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
      e.add(e.w0, e.w0, imm >> 12, 12);
    } else if (neg <= 0xFFF) {
      // Adding a small negative offset wraps identically to subtracting.
      e.sub(e.w0, e.w0, neg);
    } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
      e.sub(e.w0, e.w0, neg >> 12, 12);
    } else {
      e.mov(e.w17, static_cast<uint64_t>(imm));
      e.add(e.w0, e.w0, e.w17);
    }
  } else {
    e.add(e.w0, e.w0, WReg(offset.reg().getIdx()));
  }
  if (NeedsPhysicalRemap()) {
    ApplyPhysicalRemapW0(e);
  }
  return e.x0;
}

// Guest addresses wrap at 32 bits: a nonzero displacement is added in W first.
template <typename OffsetOp, typename Fn>
inline void EmitGuestMemAccessOffset(A64Emitter& e, const I64Op& guest,
                                     const OffsetOp& offset, Fn&& emit_access) {
  int w_idx;
  if (offset.is_constant && offset.constant() == 0 &&
      GuestMemDirectIndex(guest, &w_idx)) {
    emit_access(ptr(e.GetMembaseReg(), WReg(w_idx), Xbyak_aarch64::UXTW));
  } else {
    emit_access(
        ptr(e.GetMembaseReg(), ComputeMemoryAddressOffset(e, guest, offset)));
  }
}

// Flush denormal float32 lanes to zero in a NEON register (in-place).
// A float32 is denormal when 0 < abs(val) < 0x00800000.
// vreg must not equal sa or sb.
// This is needed because FPCR.FZ may not flush denormal inputs on all ARM64
// implementations (the ARM spec says input flushing is implementation-defined).
inline void FlushDenormals_V128(A64Emitter& e, int vreg, int sa = 2,
                                int sb = 3) {
  // val<<1 removes the sign bit and doubles the value.
  // Denormals become [0x00000002, 0x00FFFFFE]; zeros become 0x00000000.
  // (val<<1) - 1: wraps 0→0xFFFFFFFF (excluded),
  // denorms→[0x00000001,0x00FFFFFD]. Denormal iff ((val<<1) - 1) < 0x00FFFFFF
  // (unsigned).
  e.shl(VReg(sa).s4, VReg(vreg).s4, 1);
  e.movi(VReg(sb).s4, 1u);
  e.sub(VReg(sa).s4, VReg(sa).s4, VReg(sb).s4);
  e.mvni(VReg(sb).s4, 0xFFu, LSL, 24);  // 0x00FFFFFF
  e.cmhi(VReg(sb).s4, VReg(sb).s4,
         VReg(sa).s4);  // mask: all-1s for denormal lanes
  // Clear only bits 30:0 (preserve sign bit 31) so -denormal → -0, +denormal →
  // +0.
  e.ushr(VReg(sa).s4, VReg(sb).s4, 1);  // sa = mask with bit 31 cleared
  e.bic(VReg(vreg).b16, VReg(vreg).b16, VReg(sa).b16);
}

// Fixup for vmaxfp/vminfp NaN lanes.
// ARM64 fmax/fmin do propagate a NaN, but by ARM's rules: an SNaN in either
// operand outranks a QNaN in the other. PPC is strictly positional, so a NaN
// lane takes src1 where src1 is NaN and src2 otherwise.
// Expects: v0=flushed src1, v1=flushed src2, v2=hardware fmax/fmin result.
// Modifies v2 in place. Clobbers v1, v3.
inline void FixupVmxMaxMinNan(A64Emitter& e) {
  // The NaN to return: src1 where src1 is NaN, else src2.
  e.fcmeq(VReg(3).s4, VReg(0).s4, VReg(0).s4);  // v3 = non-NaN mask for src1
  e.bsl(VReg(3).b16, VReg(1).b16, VReg(0).b16);
  // Quieted, which is what hardware returns.
  e.movi(VReg(1).s4, 0x40, LSL, 16);  // v1 = quiet bit
  e.orr(VReg(3).b16, VReg(3).b16, VReg(1).b16);
  // fmax/fmin only produce a NaN out of a NaN operand, so the arithmetic
  // result's own NaN lanes are exactly the ones to replace.
  e.mov(VReg(1).b16, VReg(2).b16);
  e.fcmeq(VReg(2).s4, VReg(1).s4, VReg(1).s4);
  e.bsl(VReg(2).b16, VReg(1).b16, VReg(3).b16);
}

// Prepare two V128 operands for a VMX FP operation: copy to scratch v0/v1
// and flush denormals. Returns the flushed register indices (always 0 and 1).
template <typename T1, typename T2>
inline void PrepareVmxFpSources(A64Emitter& e, const T1& op1, const T2& op2,
                                int& out_s1, int& out_s2) {
  int s1 = SrcVReg(e, op1, 0);
  int s2 = SrcVReg(e, op2, 1);
  // Copy to scratch v0/v1 so we don't modify live allocated registers.
  if (s1 != 0) {
    e.mov(VReg(0).b16, VReg(s1).b16);
  }
  if (s2 != 1) {
    e.mov(VReg(1).b16, VReg(s2).b16);
  }
  // Flush denormal inputs in software only if FPCR.FZ doesn't handle it.
  if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    FlushDenormals_V128(e, 0);
    FlushDenormals_V128(e, 1);
  }
  out_s1 = 0;
  out_s2 = 1;
}

// Fix PPC NaN propagation for V128 float32 lanes after a NEON FP operation.
// Hardware returns the first NaN by operand position, quieted. An invalid
// operation with no NaN operand needs nothing: ARM's default NaN is already
// the one PPC produces.
// Expects: v0=flushed src1, v1=flushed src2, v2=hardware FP result.
// Modifies v2 in place. Clobbers v1, v3.
inline void FixupVmxNan_V128(A64Emitter& e) {
  using namespace Xbyak_aarch64;
  // Lowest priority first, so src1 overwrites src2. BIF inserts an operand
  // wherever its self-compare is false, which is exactly where it is NaN.
  e.fcmeq(VReg(3).s4, VReg(1).s4, VReg(1).s4);
  e.bif(VReg(2).b16, VReg(1).b16, VReg(3).b16);
  e.fcmeq(VReg(3).s4, VReg(0).s4, VReg(0).s4);
  e.bif(VReg(2).b16, VReg(0).b16, VReg(3).b16);

  // Quiet whatever NaN each lane ended up with. A lane still holding the
  // arithmetic result is either not NaN or already the default NaN, so this
  // only ever quiets an operand that was signalling.
  e.fcmeq(VReg(3).s4, VReg(2).s4, VReg(2).s4);
  e.movi(VReg(1).s4, 0x40, LSL, 16);
  e.bic(VReg(1).b16, VReg(1).b16, VReg(3).b16);
  e.orr(VReg(2).b16, VReg(2).b16, VReg(1).b16);
}

// Load an FMA's three sources into the registers FixupVmxNan_V128_Fma reads:
// v0 = src1 (A), v1 = src2 (C), v3 = src3 (B), flushing input denormals in
// software where FPCR.FZ does not. Uses v2 and `tmp` as flush scratch.
template <typename T1, typename T2, typename T3>
inline void PrepareVmxFmaSources(A64Emitter& e, const T1& op1, const T2& op2,
                                 const T3& op3, int tmp) {
  const int s1 = SrcVReg(e, op1, 0);
  if (s1 != 0) {
    e.mov(VReg(0).b16, VReg(s1).b16);
  }
  const int s2 = SrcVReg(e, op2, 1);
  if (s2 != 1) {
    e.mov(VReg(1).b16, VReg(s2).b16);
  }
  const int s3 = SrcVReg(e, op3, 3);
  if (s3 != 3) {
    e.mov(VReg(3).b16, VReg(s3).b16);
  }
  if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    FlushDenormals_V128(e, 0, 2, tmp);
    FlushDenormals_V128(e, 1, 2, tmp);
    FlushDenormals_V128(e, 3, 2, tmp);
  }
}

// Fix PPC NaN propagation for a V128 FMA result (3 source operands).
// Hardware returns the first NaN in A, B, C order, quieted, and the HIR
// operands are (A, C, B). An invalid operation with no NaN operand needs
// nothing: ARM's default NaN is already the one PPC produces.
// Expects: v0 = flushed A, v1 = flushed C, v3 = flushed B, v2 = the FMA result.
// `tmp` must be a register free until the result is stored, which v0-v3 cannot
// supply. Modifies v2 in place. Clobbers v1 and tmp.
inline void FixupVmxNan_V128_Fma(A64Emitter& e, int tmp) {
  using namespace Xbyak_aarch64;
  // Lowest priority first, so an earlier operand overwrites a later one: C,
  // then B, then A. BIF inserts an operand wherever its self-compare is false,
  // which is exactly where that operand is NaN.
  e.fcmeq(VReg(tmp).s4, VReg(1).s4, VReg(1).s4);
  e.bif(VReg(2).b16, VReg(1).b16, VReg(tmp).b16);
  e.fcmeq(VReg(tmp).s4, VReg(3).s4, VReg(3).s4);
  e.bif(VReg(2).b16, VReg(3).b16, VReg(tmp).b16);
  e.fcmeq(VReg(tmp).s4, VReg(0).s4, VReg(0).s4);
  e.bif(VReg(2).b16, VReg(0).b16, VReg(tmp).b16);

  // Quiet whatever NaN each lane ended up with. A lane still holding the
  // arithmetic result is either not NaN or already the default NaN, so this
  // only ever quiets an operand that was signalling.
  e.fcmeq(VReg(tmp).s4, VReg(2).s4, VReg(2).s4);
  e.movi(VReg(1).s4, 0x40, LSL, 16);
  e.bic(VReg(1).b16, VReg(1).b16, VReg(tmp).b16);
  e.orr(VReg(2).b16, VReg(2).b16, VReg(1).b16);
}

// VMX float32x4 binary operations with full PPC semantics.
enum class VmxFpBinOp { Add, Sub, Mul, Div };

// Execute a VMX float32x4 binary operation with denormal flushing and PPC NaN
// propagation.  Result goes into dest_idx.
// Clobbers v0-v3, w0.
template <typename T1, typename T2>
inline void EmitVmxFpBinOp_V128(A64Emitter& e, int dest_idx, const T1& src1,
                                const T2& src2, VmxFpBinOp op) {
  EmitWithVmxFpcr(e, [&] {
    // Flush input denormals → v0=s1, v1=s2.
    int s1, s2;
    PrepareVmxFpSources(e, src1, src2, s1, s2);

    // Hardware FP op → v2.
    switch (op) {
      case VmxFpBinOp::Add:
        e.fadd(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Sub:
        e.fsub(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Mul:
        e.fmul(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Div:
        e.fdiv(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
    }

    // PPC NaN propagation fixup.
    FixupVmxNan_V128(e);

    // Flush output denormals. FPCR.FZ guarantees output flushing per the
    // ARM spec, so skip when FZ is known to also handle inputs (implying
    // the core fully supports FZ denormal handling).
    if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
      FlushDenormals_V128(e, 2, 0, 1);
    }

    // Move to dest.
    e.mov(VReg(dest_idx).b16, VReg(2).b16);
  });
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
