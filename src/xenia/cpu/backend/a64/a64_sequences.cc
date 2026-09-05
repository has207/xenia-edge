/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <cmath>
#include <cstdio>
#include <type_traits>

#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_seq_util.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace xe::cpu::hir;
using namespace Xbyak_aarch64;

std::unordered_map<uint32_t, SequenceSelectFn>& SequenceTable() {
  static auto* sequence_table =
      new std::unordered_map<uint32_t, SequenceSelectFn>();
  return *sequence_table;
}

// ============================================================================
// Debug validation helpers
// ============================================================================
// Validates that a binary op with constant src1 won't clobber src2
// when dest and src2 share the same physical register.
// Call this at JIT-compile time (not in emitted code).
template <typename DEST, typename SRC2>
static void AssertNoClobber(const DEST& dest, const SRC2& src2) {
  // If src2 is a register (not constant) and dest is the same register,
  // the caller must use a scratch register for the constant.
  if (!src2.is_constant) {
    assert_true(dest.reg().getIdx() != src2.reg().getIdx() &&
                "Binary op with constant src1: dest == src2 would clobber! "
                "Use a scratch register for the constant.");
  }
}

// ============================================================================
// Safe binary operation helpers
// ============================================================================
// Emits dest = op(src1_const, src2_reg) safely, using a scratch register
// to avoid clobbering src2 when dest and src2 are the same register.
// Usage: EmitSafeBinaryConst1(e, i.dest, imm, i.src2, op_fn)
template <typename REG, typename FN>
static void EmitBinaryConstLhs(A64Emitter& e, const REG& dest,
                               uint64_t src1_const, const REG& src2,
                               const FN& op_fn) {
  // Always use scratch to avoid clobbering src2 if dest == src2.
  if constexpr (std::is_same_v<REG, WReg>) {
    e.mov(e.w17, src1_const);
    op_fn(e, dest, WReg(17), src2);
  } else {
    e.mov(e.x17, src1_const);
    op_fn(e, dest, XReg(17), src2);
  }
}

// ============================================================================
// OPCODE_COMMENT
// ============================================================================
struct COMMENT : Sequence<COMMENT, I<OPCODE_COMMENT, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsTracingInstr()) {
      auto str = reinterpret_cast<const char*>(i.src1.value);
      auto str_copy = strdup(str);
      e.mov(e.x1, reinterpret_cast<uint64_t>(str_copy));
      e.CallNative(reinterpret_cast<void*>(TraceString));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMMENT, COMMENT);

// ============================================================================
// OPCODE_NOP
// ============================================================================
struct NOP : Sequence<NOP, I<OPCODE_NOP, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) { e.nop(); }
};
EMITTER_OPCODE_TABLE(OPCODE_NOP, NOP);

// ============================================================================
// OPCODE_SOURCE_OFFSET
// ============================================================================
struct SOURCE_OFFSET
    : Sequence<SOURCE_OFFSET, I<OPCODE_SOURCE_OFFSET, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MarkSourceOffset(i.instr);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SOURCE_OFFSET, SOURCE_OFFSET);

// ============================================================================
// OPCODE_CONTEXT_BARRIER
// ============================================================================
struct CONTEXT_BARRIER
    : Sequence<CONTEXT_BARRIER, I<OPCODE_CONTEXT_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // No-op on ARM64 (context is always in x20).
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CONTEXT_BARRIER, CONTEXT_BARRIER);

// ============================================================================
// OPCODE_ASSIGN
// ============================================================================
struct ASSIGN_I8 : Sequence<ASSIGN_I8, I<OPCODE_ASSIGN, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    } else {
      e.mov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_I16 : Sequence<ASSIGN_I16, I<OPCODE_ASSIGN, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
    } else {
      e.mov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_I32 : Sequence<ASSIGN_I32, I<OPCODE_ASSIGN, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
    } else {
      e.mov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_I64 : Sequence<ASSIGN_I64, I<OPCODE_ASSIGN, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.mov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_F32 : Sequence<ASSIGN_F32, I<OPCODE_ASSIGN, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      // Load constant float via GPR.
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(i.dest, e.w0);
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_F64 : Sequence<ASSIGN_F64, I<OPCODE_ASSIGN, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(i.dest, e.x0);
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
struct ASSIGN_V128 : Sequence<ASSIGN_V128, I<OPCODE_ASSIGN, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      LoadV128Const(e, i.dest.reg().getIdx(), i.src1.constant());
    } else {
      // mov vD.16b, vS.16b (via ORR trick: orr vD.16b, vS.16b, vS.16b)
      auto src_vreg = VReg(i.src1.reg().getIdx());
      auto dst_vreg = VReg(i.dest.reg().getIdx());
      e.orr(dst_vreg.b16, src_vreg.b16, src_vreg.b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ASSIGN, ASSIGN_I8, ASSIGN_I16, ASSIGN_I32,
                     ASSIGN_I64, ASSIGN_F32, ASSIGN_F64, ASSIGN_V128);

// ============================================================================
// OPCODE_LOAD_CONTEXT
// ============================================================================
struct LOAD_CONTEXT_I8
    : Sequence<LOAD_CONTEXT_I8, I<OPCODE_LOAD_CONTEXT, I8Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ldrb wD, [x20, #offset]
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldrb(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_I16
    : Sequence<LOAD_CONTEXT_I16, I<OPCODE_LOAD_CONTEXT, I16Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldrh(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_I32
    : Sequence<LOAD_CONTEXT_I32, I<OPCODE_LOAD_CONTEXT, I32Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldr(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_I64
    : Sequence<LOAD_CONTEXT_I64, I<OPCODE_LOAD_CONTEXT, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldr(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_F32
    : Sequence<LOAD_CONTEXT_F32, I<OPCODE_LOAD_CONTEXT, F32Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldr(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_F64
    : Sequence<LOAD_CONTEXT_F64, I<OPCODE_LOAD_CONTEXT, F64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldr(i.dest, ptr(e.GetContextReg(), offset));
  }
};
struct LOAD_CONTEXT_V128
    : Sequence<LOAD_CONTEXT_V128, I<OPCODE_LOAD_CONTEXT, V128Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    e.ldr(i.dest, ptr(e.GetContextReg(), offset));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CONTEXT, LOAD_CONTEXT_I8, LOAD_CONTEXT_I16,
                     LOAD_CONTEXT_I32, LOAD_CONTEXT_I64, LOAD_CONTEXT_F32,
                     LOAD_CONTEXT_F64, LOAD_CONTEXT_V128);

// ============================================================================
// OPCODE_STORE_CONTEXT
// ============================================================================
struct STORE_CONTEXT_I8
    : Sequence<STORE_CONTEXT_I8,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      if ((i.src2.constant() & 0xFF) == 0) {
        e.strb(e.wzr, ptr(e.GetContextReg(), offset));
      } else {
        e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFF));
        e.strb(e.w0, ptr(e.GetContextReg(), offset));
      }
    } else {
      e.strb(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_I16
    : Sequence<STORE_CONTEXT_I16,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      if ((i.src2.constant() & 0xFFFF) == 0) {
        e.strh(e.wzr, ptr(e.GetContextReg(), offset));
      } else {
        e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
        e.strh(e.w0, ptr(e.GetContextReg(), offset));
      }
    } else {
      e.strh(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_I32
    : Sequence<STORE_CONTEXT_I32,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      if (i.src2.constant() == 0) {
        e.str(e.wzr, ptr(e.GetContextReg(), offset));
      } else {
        e.mov(e.w0,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
        e.str(e.w0, ptr(e.GetContextReg(), offset));
      }
    } else {
      e.str(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_I64
    : Sequence<STORE_CONTEXT_I64,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      if (i.src2.constant() == 0) {
        e.str(e.xzr, ptr(e.GetContextReg(), offset));
      } else {
        e.mov(e.x0, static_cast<uint64_t>(i.src2.constant()));
        e.str(e.x0, ptr(e.GetContextReg(), offset));
      }
    } else {
      e.str(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_F32
    : Sequence<STORE_CONTEXT_F32,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.str(e.w0, ptr(e.GetContextReg(), offset));
    } else {
      e.str(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_F64
    : Sequence<STORE_CONTEXT_F64,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.str(e.x0, ptr(e.GetContextReg(), offset));
    } else {
      e.str(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
struct STORE_CONTEXT_V128
    : Sequence<STORE_CONTEXT_V128,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto offset = static_cast<uint32_t>(i.src1.value);
    if (i.src2.is_constant) {
      LoadV128Const(e, 0, i.src2.constant());
      e.str(QReg(0), ptr(e.GetContextReg(), offset));
    } else {
      e.str(i.src2, ptr(e.GetContextReg(), offset));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_CONTEXT, STORE_CONTEXT_I8, STORE_CONTEXT_I16,
                     STORE_CONTEXT_I32, STORE_CONTEXT_I64, STORE_CONTEXT_F32,
                     STORE_CONTEXT_F64, STORE_CONTEXT_V128);

// ============================================================================
// Immediate-form helpers
// ============================================================================
// dest = src + imm (wrapping), in a single instruction whenever the
// immediate or its negation is encodable as an add/sub immediate
// (12 bits, optionally shifted left by 12). Falls back to mov+add via w0/x0.
static void EmitAddConstant(A64Emitter& e, const WReg& dest, const WReg& src,
                            uint32_t imm) {
  const uint32_t neg = 0u - imm;
  if (imm == 0) {
    if (dest.getIdx() != src.getIdx()) {
      e.mov(dest, src);
    }
  } else if (imm <= 0xFFF) {
    e.add(dest, src, imm);
  } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
    e.add(dest, src, imm >> 12, 12);
  } else if (neg <= 0xFFF) {
    // Adding a negative immediate wraps identically to subtracting.
    e.sub(dest, src, neg);
  } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
    e.sub(dest, src, neg >> 12, 12);
  } else {
    e.mov(e.w0, static_cast<uint64_t>(imm));
    e.add(dest, src, e.w0);
  }
}
static void EmitAddConstant(A64Emitter& e, const XReg& dest, const XReg& src,
                            uint64_t imm) {
  const uint64_t neg = 0ull - imm;
  if (imm == 0) {
    if (dest.getIdx() != src.getIdx()) {
      e.mov(dest, src);
    }
  } else if (imm <= 0xFFF) {
    e.add(dest, src, static_cast<uint32_t>(imm));
  } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
    e.add(dest, src, static_cast<uint32_t>(imm >> 12), 12);
  } else if (neg <= 0xFFF) {
    e.sub(dest, src, static_cast<uint32_t>(neg));
  } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
    e.sub(dest, src, static_cast<uint32_t>(neg >> 12), 12);
  } else {
    e.mov(e.x0, imm);
    e.add(dest, src, e.x0);
  }
}

// cmp src, #imm in a single instruction whenever possible. For small
// negative immediates uses cmn: cmp a, -n and cmn a, n compute the same
// a + n and set identical NZCV for n in [1, 4095] (and shifted forms).
static void EmitCmpConstant(A64Emitter& e, const WReg& src, uint32_t imm) {
  const uint32_t neg = 0u - imm;
  if (imm <= 0xFFF) {
    e.cmp(src, imm);
  } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
    e.cmp(src, imm >> 12, 12);
  } else if (neg <= 0xFFF) {
    e.cmn(src, neg);
  } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
    e.cmn(src, neg >> 12, 12);
  } else {
    e.mov(e.w0, static_cast<uint64_t>(imm));
    e.cmp(src, e.w0);
  }
}
static void EmitCmpConstant(A64Emitter& e, const XReg& src, uint64_t imm) {
  const uint64_t neg = 0ull - imm;
  if (imm <= 0xFFF) {
    e.cmp(src, static_cast<uint32_t>(imm));
  } else if (!(imm & 0xFFF) && (imm >> 12) <= 0xFFF) {
    e.cmp(src, static_cast<uint32_t>(imm >> 12), 12);
  } else if (neg <= 0xFFF) {
    e.cmn(src, static_cast<uint32_t>(neg));
  } else if (!(neg & 0xFFF) && (neg >> 12) <= 0xFFF) {
    e.cmn(src, static_cast<uint32_t>(neg >> 12), 12);
  } else {
    e.mov(e.x0, imm);
    e.cmp(src, e.x0);
  }
}

// ============================================================================
// OPCODE_ADD (Integer)
// ============================================================================
// I8/I16 value convention: this backend keeps narrow integer values
// ZERO-EXTENDED in their W registers. Consumers rely on it — branches use
// full-width cbz/cbnz, unsigned compares use full-width cmp, SELECT compares
// the condition register against zero. Every sequence producing an I8/I16
// result must therefore mask the destination back to the type width when the
// computation can carry, borrow, or sign-fill into the upper bits (add, sub,
// neg, not, shifts, min/max via sign-extended scratch, ...). The x64 backend
// instead leaves upper bits stale and reads sub-width registers; mixing the
// two conventions here caused real divergences (e.g. ADD_I8(0x80,0x80)
// followed by BRANCH_TRUE_I8 branched on a64 but not on x64).
struct ADD_I8 : Sequence<ADD_I8, I<OPCODE_ADD, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() + i.src2.constant()) & 0xFF));
    } else {
      if (i.src2.is_constant) {
        e.add(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0xFF));
      } else if (i.src1.is_constant) {
        e.add(i.dest, i.src2, static_cast<uint32_t>(i.src1.constant() & 0xFF));
      } else {
        e.add(i.dest, i.src1, i.src2);
      }
      // The add can carry into bit 8; keep the I8 value zero-extended.
      e.uxtb(i.dest, i.dest);
    }
  }
};
struct ADD_I16 : Sequence<ADD_I16, I<OPCODE_ADD, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() + i.src2.constant()) & 0xFFFF));
    } else {
      if (i.src2.is_constant) {
        uint32_t imm = static_cast<uint32_t>(i.src2.constant() & 0xFFFF);
        if (imm <= 4095) {
          e.add(i.dest, i.src1, imm);
        } else {
          e.mov(e.w0, static_cast<uint64_t>(imm));
          e.add(i.dest, i.src1, e.w0);
        }
      } else if (i.src1.is_constant) {
        uint32_t imm = static_cast<uint32_t>(i.src1.constant() & 0xFFFF);
        if (imm <= 4095) {
          e.add(i.dest, i.src2, imm);
        } else {
          e.mov(e.w0, static_cast<uint64_t>(imm));
          e.add(i.dest, i.src2, e.w0);
        }
      } else {
        e.add(i.dest, i.src1, i.src2);
      }
      // The add can carry into bit 16; keep the I16 value zero-extended.
      e.uxth(i.dest, i.dest);
    }
  }
};
struct ADD_I32 : Sequence<ADD_I32, I<OPCODE_ADD, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() + i.src2.constant())));
    } else if (i.src2.is_constant) {
      EmitAddConstant(e, i.dest, i.src1,
                      static_cast<uint32_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitAddConstant(e, i.dest, i.src2,
                      static_cast<uint32_t>(i.src1.constant()));
    } else {
      e.add(i.dest, i.src1, i.src2);
    }
  }
};
struct ADD_I64 : Sequence<ADD_I64, I<OPCODE_ADD, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() + i.src2.constant()));
    } else if (i.src2.is_constant) {
      EmitAddConstant(e, i.dest, i.src1,
                      static_cast<uint64_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitAddConstant(e, i.dest, i.src2,
                      static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.add(i.dest, i.src1, i.src2);
    }
  }
};
// NaN canonicalization helpers.
// PPC NaN selection for 2-operand FP ops (add, sub, mul, div):
// First NaN by operand position wins, quieted if SNaN.
// ARM64 may propagate a different NaN than PPC's positional rule, so NaN
// inputs are handled entirely in software. An invalid operation with no NaN
// operand needs nothing: ARM's default NaN is the one PPC produces.
enum class FpBinOp { Add, Sub, Mul, Div };

static void EmitFpBinOpWithPpcNan_F32(A64Emitter& e, SReg dest, SReg s1,
                                      SReg s2, FpBinOp op) {
  // Ensure FPU FPCR (no flush-to-zero) for scalar operations.
  e.ChangeFpcrMode(FPCRMode::Fpu);
  auto& nan_path = e.NewCachedLabel();
  auto& done = e.NewCachedLabel();

  // Check if either input is NaN. fccmp sets NZCV from immediate if the
  // condition is false (i.e. s1 was already NaN), preserving V=1.
  e.fcmp(s1, s1);
  e.fccmp(s2, s2, 0b0001, VC);
  e.b_near(VS, nan_path);

  // Fast path: no NaN input — hardware op.
  switch (op) {
    case FpBinOp::Add:
      e.fadd(dest, s1, s2);
      break;
    case FpBinOp::Sub:
      e.fsub(dest, s1, s2);
      break;
    case FpBinOp::Mul:
      e.fmul(dest, s1, s2);
      break;
    case FpBinOp::Div:
      e.fdiv(dest, s1, s2);
      break;
  }
  e.b(done);

  // Slow path: first NaN by position wins, quiet if SNaN.
  e.L(nan_path);
  auto& s1_not_nan = e.NewCachedLabel();
  e.fcmp(s1, s1);
  e.b_near(VC, s1_not_nan);
  e.fmov(e.w0, s1);
  e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
  e.fmov(dest, e.w0);
  e.b(done);
  e.L(s1_not_nan);
  e.fmov(e.w0, s2);
  e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
  e.fmov(dest, e.w0);

  e.L(done);
}

static void EmitFpBinOpWithPpcNan_F64(A64Emitter& e, DReg dest, DReg s1,
                                      DReg s2, FpBinOp op) {
  e.ChangeFpcrMode(FPCRMode::Fpu);
  auto& nan_path = e.NewCachedLabel();
  auto& done = e.NewCachedLabel();

  // Check if either input is NaN. fccmp sets NZCV from immediate if the
  // condition is false (i.e. s1 was already NaN), preserving V=1.
  e.fcmp(s1, s1);
  e.fccmp(s2, s2, 0b0001, VC);
  e.b_near(VS, nan_path);

  // Fast path: no NaN input — hardware op.
  switch (op) {
    case FpBinOp::Add:
      e.fadd(dest, s1, s2);
      break;
    case FpBinOp::Sub:
      e.fsub(dest, s1, s2);
      break;
    case FpBinOp::Mul:
      e.fmul(dest, s1, s2);
      break;
    case FpBinOp::Div:
      e.fdiv(dest, s1, s2);
      break;
  }
  e.b(done);

  // Slow path: first NaN by position wins, quiet if SNaN.
  e.L(nan_path);
  auto& s1_not_nan = e.NewCachedLabel();
  e.fcmp(s1, s1);
  e.b_near(VC, s1_not_nan);
  e.fmov(e.x0, s1);
  e.orr(e.x0, e.x0, static_cast<uint64_t>(1ull << 51));
  e.fmov(dest, e.x0);
  e.b(done);
  e.L(s1_not_nan);
  e.fmov(e.x0, s2);
  e.orr(e.x0, e.x0, static_cast<uint64_t>(1ull << 51));
  e.fmov(dest, e.x0);

  e.L(done);
}
// PPC FMA NaN selection (PowerISA 4.6.7.2):
// The first NaN operand by position (frA=s1, frC=s2, frB=s3) wins,
// regardless of QNaN vs SNaN.  If it's an SNaN, quiet it (set the
// quiet bit).  If no operand is NaN, use hardware FMA; generated NaN
// (from 0*inf or inf-inf) becomes the PPC default QNaN.
// ARM64's fmadd may propagate a different NaN than PPC's positional
// rule, so NaN inputs are handled entirely in software.
// The HIR operands are (A, C, B) but hardware returns the first NaN in
// A, B, C order, so the walk below is s1, s3, s2 — measured off the captured
// vectors, not the operand order. The NaN is returned un-negated even for
// fnmadd/fnmsub, which is why the negation only reaches the arithmetic path.
static void EmitFmaWithPpcNan_F64(A64Emitter& e, DReg dest, DReg s1, DReg s2,
                                  DReg s3, bool is_sub, bool negate) {
  e.ChangeFpcrMode(FPCRMode::Fpu);
  auto& nan_path = e.NewCachedLabel();
  auto& done = e.NewCachedLabel();

  // Quick check: any NaN among the three operands?
  e.fcmp(s1, s1);
  e.fccmp(s2, s2, 0b0001, VC);
  e.fccmp(s3, s3, 0b0001, VC);
  e.b_near(VS, nan_path);

  // Fast path: no NaN input → hardware FMA, then negate the result. Not
  // fnmadd: that negates the operands (-Ra - Rn*Rm), which differs from
  // -(Ra + Rn*Rm) when the two addends are zeros of opposite sign.
  auto& invalid = e.NewCachedLabel();
  if (is_sub) {
    e.fnmsub(dest, s1, s2, s3);
  } else {
    e.fmadd(dest, s1, s2, s3);
  }
  // If result is NaN (0*inf or inf-inf), canonicalize to PPC default.
  e.fcmp(dest, dest);
  e.b_near(VS, invalid);
  if (negate) {
    e.fneg(dest, dest);
  }
  e.b(done);
  e.L(invalid);
  e.mov(e.x0, static_cast<uint64_t>(0x7FF8000000000000ull));
  e.fmov(dest, e.x0);
  e.b(done);

  // Slow path: first NaN in A, B, C order wins (quiet if SNaN).
  e.L(nan_path);
  DReg order[3] = {s1, s3, s2};
  for (int step = 0; step < 2; ++step) {
    auto& not_nan = e.NewCachedLabel();
    e.fcmp(order[step], order[step]);
    e.b_near(VC, not_nan);
    e.fmov(e.x0, order[step]);
    e.orr(e.x0, e.x0, static_cast<uint64_t>(1ull << 51));  // ensure quiet
    e.fmov(dest, e.x0);
    e.b(done);
    e.L(not_nan);
  }
  // At least one operand is a NaN, so it must be this one.
  e.fmov(e.x0, order[2]);
  e.orr(e.x0, e.x0, static_cast<uint64_t>(1ull << 51));
  e.fmov(dest, e.x0);

  e.L(done);
}

static void EmitFmaWithPpcNan_F32(A64Emitter& e, SReg dest, SReg s1, SReg s2,
                                  SReg s3, bool is_sub, bool negate) {
  e.ChangeFpcrMode(FPCRMode::Fpu);
  auto& nan_path = e.NewCachedLabel();
  auto& done = e.NewCachedLabel();

  e.fcmp(s1, s1);
  e.fccmp(s2, s2, 0b0001, VC);
  e.fccmp(s3, s3, 0b0001, VC);
  e.b_near(VS, nan_path);

  auto& invalid = e.NewCachedLabel();
  if (is_sub) {
    e.fnmsub(dest, s1, s2, s3);
  } else {
    e.fmadd(dest, s1, s2, s3);
  }
  e.fcmp(dest, dest);
  e.b_near(VS, invalid);
  if (negate) {
    e.fneg(dest, dest);
  }
  e.b(done);
  e.L(invalid);
  e.mov(e.w0, static_cast<uint64_t>(0x7FC00000u));
  e.fmov(dest, e.w0);
  e.b(done);

  e.L(nan_path);
  SReg order[3] = {s1, s3, s2};
  for (int step = 0; step < 2; ++step) {
    auto& not_nan = e.NewCachedLabel();
    e.fcmp(order[step], order[step]);
    e.b_near(VC, not_nan);
    e.fmov(e.w0, order[step]);
    e.orr(e.w0, e.w0, static_cast<uint32_t>(1u << 22));
    e.fmov(dest, e.w0);
    e.b(done);
    e.L(not_nan);
  }
  e.fmov(e.w0, order[2]);
  e.orr(e.w0, e.w0, static_cast<uint32_t>(1u << 22));
  e.fmov(dest, e.w0);

  e.L(done);
}

struct ADD_F32 : Sequence<ADD_F32, I<OPCODE_ADD, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    SReg s1 = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    SReg s2 = i.src2.is_constant ? e.s1 : SReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w0);
    }
    EmitFpBinOpWithPpcNan_F32(e, i.dest, s1, s2, FpBinOp::Add);
  }
};
struct ADD_F64 : Sequence<ADD_F64, I<OPCODE_ADD, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    EmitFpBinOpWithPpcNan_F64(e, i.dest, s1, s2, FpBinOp::Add);
  }
};
struct ADD_V128 : Sequence<ADD_V128, I<OPCODE_ADD, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                        VmxFpBinOp::Add);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD, ADD_I8, ADD_I16, ADD_I32, ADD_I64, ADD_F32,
                     ADD_F64, ADD_V128);

// ============================================================================
// OPCODE_ZERO_EXTEND
// ============================================================================
struct ZERO_EXTEND_I16_I8
    : Sequence<ZERO_EXTEND_I16_I8, I<OPCODE_ZERO_EXTEND, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // uxtb wD, wS (same as and wD, wS, #0xFF)
    e.uxtb(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I32_I8
    : Sequence<ZERO_EXTEND_I32_I8, I<OPCODE_ZERO_EXTEND, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.uxtb(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I8
    : Sequence<ZERO_EXTEND_I64_I8, I<OPCODE_ZERO_EXTEND, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Zero-extend 8-bit to 64-bit: AND with 0xFF in 32-bit clears upper 32.
    auto w_dest = WReg(i.dest.reg().getIdx());
    e.uxtb(w_dest, i.src1);
  }
};
struct ZERO_EXTEND_I32_I16
    : Sequence<ZERO_EXTEND_I32_I16, I<OPCODE_ZERO_EXTEND, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.uxth(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I16
    : Sequence<ZERO_EXTEND_I64_I16, I<OPCODE_ZERO_EXTEND, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto w_dest = WReg(i.dest.reg().getIdx());
    e.uxth(w_dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I32
    : Sequence<ZERO_EXTEND_I64_I32, I<OPCODE_ZERO_EXTEND, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // mov wD, wS implicitly zero-extends to 64 bits on ARM64.
    if (!i.src1.is_constant && i.dest.reg().getIdx() == i.src1.reg().getIdx()) {
      // I32 values only come from W forms, so the upper half is already zero.
      return;
    }
    auto w_dest = WReg(i.dest.reg().getIdx());
    e.mov(w_dest, i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ZERO_EXTEND, ZERO_EXTEND_I16_I8, ZERO_EXTEND_I32_I8,
                     ZERO_EXTEND_I64_I8, ZERO_EXTEND_I32_I16,
                     ZERO_EXTEND_I64_I16, ZERO_EXTEND_I64_I32);

// ============================================================================
// OPCODE_SIGN_EXTEND
// ============================================================================
struct SIGN_EXTEND_I16_I8
    : Sequence<SIGN_EXTEND_I16_I8, I<OPCODE_SIGN_EXTEND, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Sign-extend into the low 16 bits only; bits [31:16] of an I16 value
    // must stay zero.
    e.sxtb(i.dest, i.src1);
    e.uxth(i.dest, i.dest);
  }
};
struct SIGN_EXTEND_I32_I8
    : Sequence<SIGN_EXTEND_I32_I8, I<OPCODE_SIGN_EXTEND, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.sxtb(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I8
    : Sequence<SIGN_EXTEND_I64_I8, I<OPCODE_SIGN_EXTEND, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.sxtb(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I32_I16
    : Sequence<SIGN_EXTEND_I32_I16, I<OPCODE_SIGN_EXTEND, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.sxth(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I16
    : Sequence<SIGN_EXTEND_I64_I16, I<OPCODE_SIGN_EXTEND, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.sxth(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I32
    : Sequence<SIGN_EXTEND_I64_I32, I<OPCODE_SIGN_EXTEND, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.sxtw(i.dest, i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SIGN_EXTEND, SIGN_EXTEND_I16_I8, SIGN_EXTEND_I32_I8,
                     SIGN_EXTEND_I64_I8, SIGN_EXTEND_I32_I16,
                     SIGN_EXTEND_I64_I16, SIGN_EXTEND_I64_I32);

// ============================================================================
// OPCODE_TRUNCATE
// ============================================================================
struct TRUNCATE_I8_I16
    : Sequence<TRUNCATE_I8_I16, I<OPCODE_TRUNCATE, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Keep only low 8 bits.
    e.uxtb(i.dest, i.src1);
  }
};
struct TRUNCATE_I8_I32
    : Sequence<TRUNCATE_I8_I32, I<OPCODE_TRUNCATE, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.uxtb(i.dest, i.src1);
  }
};
struct TRUNCATE_I8_I64
    : Sequence<TRUNCATE_I8_I64, I<OPCODE_TRUNCATE, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto w_src = WReg(i.src1.reg().getIdx());
    e.uxtb(i.dest, w_src);
  }
};
struct TRUNCATE_I16_I32
    : Sequence<TRUNCATE_I16_I32, I<OPCODE_TRUNCATE, I16Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.uxth(i.dest, i.src1);
  }
};
struct TRUNCATE_I16_I64
    : Sequence<TRUNCATE_I16_I64, I<OPCODE_TRUNCATE, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto w_src = WReg(i.src1.reg().getIdx());
    e.uxth(i.dest, w_src);
  }
};
struct TRUNCATE_I32_I64
    : Sequence<TRUNCATE_I32_I64, I<OPCODE_TRUNCATE, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // mov wD, wS — implicitly truncates (upper 32 bits zeroed).
    auto w_src = WReg(i.src1.reg().getIdx());
    e.mov(i.dest, w_src);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TRUNCATE, TRUNCATE_I8_I16, TRUNCATE_I8_I32,
                     TRUNCATE_I8_I64, TRUNCATE_I16_I32, TRUNCATE_I16_I64,
                     TRUNCATE_I32_I64);

// ============================================================================
// OPCODE_SUB
// ============================================================================
template <typename T, typename REG>
static void EmitSubInt(A64Emitter& e, const T& i) {
  if (i.src1.is_constant && i.src2.is_constant) {
    e.mov(
        i.dest,
        static_cast<uint64_t>(
            static_cast<
                typename std::make_unsigned<decltype(i.src1.constant())>::type>(
                i.src1.constant() - i.src2.constant())));
  } else if (i.src2.is_constant) {
    uint64_t imm = static_cast<uint64_t>(
        static_cast<
            typename std::make_unsigned<decltype(i.src2.constant())>::type>(
            i.src2.constant()));
    // src - imm == src + (-imm) with 32-bit wrap; the helper picks the
    // shortest add/sub immediate encoding.
    EmitAddConstant(e, i.dest, i.src1, 0u - static_cast<uint32_t>(imm));
  } else if (i.src1.is_constant) {
    uint64_t imm = static_cast<uint64_t>(
        static_cast<
            typename std::make_unsigned<decltype(i.src1.constant())>::type>(
            i.src1.constant()));
    // Use scratch register to avoid clobbering src2 when dest == src2.
    e.mov(e.w17, imm);
    e.sub(i.dest, REG(17), i.src2);
  } else {
    e.sub(i.dest, i.src1, i.src2);
  }
}
struct SUB_I8 : Sequence<SUB_I8, I<OPCODE_SUB, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubInt<EmitArgType, WReg>(e, i);
    if (!(i.src1.is_constant && i.src2.is_constant)) {
      // The borrow propagates into the upper bits; keep the I8 value
      // zero-extended.
      e.uxtb(i.dest, i.dest);
    }
  }
};
struct SUB_I16 : Sequence<SUB_I16, I<OPCODE_SUB, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubInt<EmitArgType, WReg>(e, i);
    if (!(i.src1.is_constant && i.src2.is_constant)) {
      // The borrow propagates into the upper bits; keep the I16 value
      // zero-extended.
      e.uxth(i.dest, i.dest);
    }
  }
};
struct SUB_I32 : Sequence<SUB_I32, I<OPCODE_SUB, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubInt<EmitArgType, WReg>(e, i);
  }
};
struct SUB_I64 : Sequence<SUB_I64, I<OPCODE_SUB, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() - i.src2.constant()));
    } else if (i.src2.is_constant) {
      EmitAddConstant(e, i.dest, i.src1,
                      0ull - static_cast<uint64_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      // Use scratch register to avoid clobbering src2 when dest == src2.
      e.mov(e.x17, static_cast<uint64_t>(i.src1.constant()));
      e.sub(i.dest, e.x17, i.src2);
    } else {
      e.sub(i.dest, i.src1, i.src2);
    }
  }
};
struct SUB_F32 : Sequence<SUB_F32, I<OPCODE_SUB, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    SReg s1 = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    SReg s2 = i.src2.is_constant ? e.s1 : SReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w0);
    }
    EmitFpBinOpWithPpcNan_F32(e, i.dest, s1, s2, FpBinOp::Sub);
  }
};
struct SUB_F64 : Sequence<SUB_F64, I<OPCODE_SUB, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    EmitFpBinOpWithPpcNan_F64(e, i.dest, s1, s2, FpBinOp::Sub);
  }
};
struct SUB_V128 : Sequence<SUB_V128, I<OPCODE_SUB, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                        VmxFpBinOp::Sub);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SUB, SUB_I8, SUB_I16, SUB_I32, SUB_I64, SUB_F32,
                     SUB_F64, SUB_V128);

// ============================================================================
// OPCODE_ADD_CARRY
// ============================================================================
struct ADD_CARRY_I8
    : Sequence<ADD_CARRY_I8, I<OPCODE_ADD_CARRY, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 + src2 + src3 (carry in)
    if (i.src1.is_constant && i.src2.is_constant && i.src3.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(
                (i.src1.constant() + i.src2.constant() + i.src3.constant()) &
                0xFF));
    } else {
      // Load src1 into dest (or w0 if constant).
      if (i.src1.is_constant) {
        e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
      } else {
        e.mov(e.w0, i.src1);
      }
      // Add src2.
      if (i.src2.is_constant) {
        e.add(e.w0, e.w0, static_cast<uint32_t>(i.src2.constant() & 0xFF));
      } else {
        e.add(e.w0, e.w0, i.src2);
      }
      // Add carry.
      if (i.src3.is_constant) {
        if (i.src3.constant()) {
          e.add(e.w0, e.w0, 1);
        }
      } else {
        e.add(e.w0, e.w0, i.src3);
      }
      // The adds can carry into bit 8; keep the I8 value zero-extended.
      e.uxtb(i.dest, e.w0);
    }
  }
};
struct ADD_CARRY_I16
    : Sequence<ADD_CARRY_I16, I<OPCODE_ADD_CARRY, I16Op, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
    } else {
      e.mov(e.w0, i.src1);
    }
    if (i.src2.is_constant) {
      e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
      e.add(e.w0, e.w0, e.w1);
    } else {
      e.add(e.w0, e.w0, i.src2);
    }
    if (i.src3.is_constant) {
      if (i.src3.constant()) {
        e.add(e.w0, e.w0, 1);
      }
    } else {
      e.add(e.w0, e.w0, i.src3);
    }
    // The adds can carry into bit 16; keep the I16 value zero-extended.
    e.uxth(i.dest, e.w0);
  }
};
struct ADD_CARRY_I32
    : Sequence<ADD_CARRY_I32, I<OPCODE_ADD_CARRY, I32Op, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
    } else {
      e.mov(e.w0, i.src1);
    }
    if (i.src2.is_constant) {
      e.mov(e.w1,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      e.add(e.w0, e.w0, e.w1);
    } else {
      e.add(e.w0, e.w0, i.src2);
    }
    if (i.src3.is_constant) {
      if (i.src3.constant()) {
        e.add(e.w0, e.w0, 1);
      }
    } else {
      e.add(e.w0, e.w0, i.src3);
    }
    e.mov(i.dest, e.w0);
  }
};
struct ADD_CARRY_I64
    : Sequence<ADD_CARRY_I64, I<OPCODE_ADD_CARRY, I64Op, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.mov(e.x0, i.src1);
    }
    if (i.src2.is_constant) {
      e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));
      e.add(e.x0, e.x0, e.x1);
    } else {
      e.add(e.x0, e.x0, i.src2);
    }
    if (i.src3.is_constant) {
      if (i.src3.constant()) {
        e.add(e.x0, e.x0, 1);
      }
    } else {
      // Zero-extend the I8 carry to 64-bit.
      e.mov(e.w1, i.src3);
      e.uxtb(e.w1, e.w1);
      e.add(e.x0, e.x0, e.x1);
    }
    e.mov(i.dest, e.x0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD_CARRY, ADD_CARRY_I8, ADD_CARRY_I16,
                     ADD_CARRY_I32, ADD_CARRY_I64);

// ============================================================================
// OPCODE_MUL
// ============================================================================
struct MUL_I32 : Sequence<MUL_I32, I<OPCODE_MUL, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() * i.src2.constant())));
    } else if (i.src1.is_constant) {
      EmitMulConstant(e, i.dest, i.src2,
                      static_cast<uint32_t>(i.src1.constant()));
    } else if (i.src2.is_constant) {
      EmitMulConstant(e, i.dest, i.src1,
                      static_cast<uint32_t>(i.src2.constant()));
    } else {
      e.mul(i.dest, i.src1, i.src2);
    }
  }

  // dest = src * imm; power-of-two multiplies become shifts (the low 32
  // result bits are identical for signed/unsigned and for shift vs mul).
  static void EmitMulConstant(A64Emitter& e, const WReg& dest, const WReg& src,
                              uint32_t imm) {
    if (imm == 0) {
      // mov(dest, wzr) would assemble as ADD dest, SP, #0 (reg 31 = SP in
      // the register-mov alias); the immediate form is MOVZ.
      e.mov(dest, uint64_t(0));
    } else if (!(imm & (imm - 1))) {
      e.lsl(dest, src, xe::tzcnt(imm));
    } else {
      e.mov(e.w0, static_cast<uint64_t>(imm));
      e.mul(dest, src, e.w0);
    }
  }
};
struct MUL_I64 : Sequence<MUL_I64, I<OPCODE_MUL, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() * i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitMulConstant(e, i.dest, i.src2,
                      static_cast<uint64_t>(i.src1.constant()));
    } else if (i.src2.is_constant) {
      EmitMulConstant(e, i.dest, i.src1,
                      static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mul(i.dest, i.src1, i.src2);
    }
  }

  // dest = src * imm; power-of-two multiplies become shifts (the low 64
  // result bits are identical for signed/unsigned and for shift vs mul).
  static void EmitMulConstant(A64Emitter& e, const XReg& dest, const XReg& src,
                              uint64_t imm) {
    if (imm == 0) {
      // mov(dest, xzr) would assemble as ADD dest, SP, #0 (reg 31 = SP in
      // the register-mov alias); the immediate form is MOVZ.
      e.mov(dest, uint64_t(0));
    } else if (!(imm & (imm - 1))) {
      e.lsl(dest, src, static_cast<uint32_t>(xe::tzcnt(imm)));
    } else {
      e.mov(e.x0, imm);
      e.mul(dest, src, e.x0);
    }
  }
};
struct MUL_F32 : Sequence<MUL_F32, I<OPCODE_MUL, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    SReg s1 = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    SReg s2 = i.src2.is_constant ? e.s1 : SReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w0);
    }
    EmitFpBinOpWithPpcNan_F32(e, i.dest, s1, s2, FpBinOp::Mul);
  }
};
struct MUL_F64 : Sequence<MUL_F64, I<OPCODE_MUL, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    EmitFpBinOpWithPpcNan_F64(e, i.dest, s1, s2, FpBinOp::Mul);
  }
};
struct MUL_V128 : Sequence<MUL_V128, I<OPCODE_MUL, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                        VmxFpBinOp::Mul);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL, MUL_I32, MUL_I64, MUL_F32, MUL_F64, MUL_V128);

// ============================================================================
// OPCODE_MUL_HI
// ============================================================================
struct MUL_HI_I64
    : Sequence<MUL_HI_I64, I<OPCODE_MUL_HI, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    XReg s1 = i.src1.is_constant ? e.x0 : XReg(i.src1.reg().getIdx());
    XReg s2 = i.src2.is_constant ? e.x1 : XReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
    }
    if (i.src2.is_constant) {
      e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));
    }
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      e.umulh(i.dest, s1, s2);
    } else {
      e.smulh(i.dest, s1, s2);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_HI, MUL_HI_I64);

// ============================================================================
// OPCODE_DIV
// ============================================================================
struct DIV_I32 : Sequence<DIV_I32, I<OPCODE_DIV, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ARM64 sdiv/udiv returns 0 on divide by zero (no exception).
    WReg s1 = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    WReg s2 = i.src2.is_constant ? e.w1 : WReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
    }
    if (i.src2.is_constant) {
      e.mov(e.w1,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    }
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      e.udiv(i.dest, s1, s2);
    } else {
      e.sdiv(i.dest, s1, s2);
    }
  }
};
struct DIV_I64 : Sequence<DIV_I64, I<OPCODE_DIV, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    XReg s1 = i.src1.is_constant ? e.x0 : XReg(i.src1.reg().getIdx());
    XReg s2 = i.src2.is_constant ? e.x1 : XReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
    }
    if (i.src2.is_constant) {
      e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));
    }
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      e.udiv(i.dest, s1, s2);
    } else {
      e.sdiv(i.dest, s1, s2);
    }
  }
};
struct DIV_F32 : Sequence<DIV_F32, I<OPCODE_DIV, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    SReg s1 = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    SReg s2 = i.src2.is_constant ? e.s1 : SReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w0);
    }
    EmitFpBinOpWithPpcNan_F32(e, i.dest, s1, s2, FpBinOp::Div);
  }
};
struct DIV_F64 : Sequence<DIV_F64, I<OPCODE_DIV, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    EmitFpBinOpWithPpcNan_F64(e, i.dest, s1, s2, FpBinOp::Div);
  }
};
struct DIV_V128 : Sequence<DIV_V128, I<OPCODE_DIV, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitVmxFpBinOp_V128(e, i.dest.reg().getIdx(), i.src1, i.src2,
                        VmxFpBinOp::Div);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DIV, DIV_I32, DIV_I64, DIV_F32, DIV_F64, DIV_V128);

// ============================================================================
// OPCODE_NEG
// ============================================================================
struct NEG_I8 : Sequence<NEG_I8, I<OPCODE_NEG, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint8_t>(-i.src1.constant())));
    } else {
      // Negation sign-fills the upper bits; keep the I8 value zero-extended.
      e.neg(i.dest, i.src1);
      e.uxtb(i.dest, i.dest);
    }
  }
};
struct NEG_I16 : Sequence<NEG_I16, I<OPCODE_NEG, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint16_t>(-i.src1.constant())));
    } else {
      // Negation sign-fills the upper bits; keep the I16 value zero-extended.
      e.neg(i.dest, i.src1);
      e.uxth(i.dest, i.dest);
    }
  }
};
struct NEG_I32 : Sequence<NEG_I32, I<OPCODE_NEG, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint32_t>(-i.src1.constant())));
    } else {
      e.neg(i.dest, i.src1);
    }
  }
};
struct NEG_I64 : Sequence<NEG_I64, I<OPCODE_NEG, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(-i.src1.constant()));
    } else {
      e.neg(i.dest, i.src1);
    }
  }
};
struct NEG_F32 : Sequence<NEG_F32, I<OPCODE_NEG, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = -i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(i.dest, e.w0);
    } else {
      e.fneg(i.dest, i.src1);
    }
  }
};
struct NEG_F64 : Sequence<NEG_F64, I<OPCODE_NEG, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = -i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(i.dest, e.x0);
    } else {
      e.fneg(i.dest, i.src1);
    }
  }
};
struct NEG_V128 : Sequence<NEG_V128, I<OPCODE_NEG, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      e.fneg(VReg(i.dest.reg().getIdx()).s4, VReg(s).s4);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NEG, NEG_I8, NEG_I16, NEG_I32, NEG_I64, NEG_F32,
                     NEG_F64, NEG_V128);

// ============================================================================
// OPCODE_ABS
// ============================================================================
struct ABS_F32 : Sequence<ABS_F32, I<OPCODE_ABS, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      c.u &= 0x7FFFFFFF;
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(i.dest, e.w0);
    } else {
      e.fabs(i.dest, i.src1);
    }
  }
};
struct ABS_F64 : Sequence<ABS_F64, I<OPCODE_ABS, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      c.u &= 0x7FFFFFFFFFFFFFFFULL;
      e.mov(e.x0, c.u);
      e.fmov(i.dest, e.x0);
    } else {
      e.fabs(i.dest, i.src1);
    }
  }
};
struct ABS_V128 : Sequence<ABS_V128, I<OPCODE_ABS, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      e.fabs(VReg(i.dest.reg().getIdx()).s4, VReg(s).s4);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ABS, ABS_F32, ABS_F64, ABS_V128);

// ============================================================================
// OPCODE_AND
// ============================================================================
struct AND_I8 : Sequence<AND_I8, I<OPCODE_AND, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() & i.src2.constant()) & 0xFF));
    } else if (i.src2.is_constant) {
      e.and_imm(i.dest, i.src1, i.src2.constant() & 0xFF, e.w0);
    } else if (i.src1.is_constant) {
      e.and_imm(i.dest, i.src2, i.src1.constant() & 0xFF, e.w0);
    } else {
      e.and_(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_I16 : Sequence<AND_I16, I<OPCODE_AND, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() & i.src2.constant()) & 0xFFFF));
    } else if (i.src2.is_constant) {
      e.and_imm(i.dest, i.src1, i.src2.constant() & 0xFFFF, e.w0);
    } else if (i.src1.is_constant) {
      e.and_imm(i.dest, i.src2, i.src1.constant() & 0xFFFF, e.w0);
    } else {
      e.and_(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_I32 : Sequence<AND_I32, I<OPCODE_AND, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() & i.src2.constant())));
    } else if (i.src2.is_constant) {
      e.and_imm(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant()), e.w0);
    } else if (i.src1.is_constant) {
      e.and_imm(i.dest, i.src2, static_cast<uint32_t>(i.src1.constant()), e.w0);
    } else {
      e.and_(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_I64 : Sequence<AND_I64, I<OPCODE_AND, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() & i.src2.constant()));
    } else if (i.src2.is_constant) {
      EmitAndImm(e, i.dest, i.src1, static_cast<uint64_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitAndImm(e, i.dest, i.src2, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.and_(i.dest, i.src1, i.src2);
    }
  }

  static void EmitAndImm(A64Emitter& e, const XReg& dest, const XReg& src,
                         uint64_t imm) {
    if (IsValidLogicalImm(imm, 64)) {
      e.and_(dest, src, imm);
    } else {
      e.mov(e.x0, imm);
      e.and_(dest, src, e.x0);
    }
  }
};
struct AND_V128 : Sequence<AND_V128, I<OPCODE_AND, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    e.and_(VReg(i.dest.reg().getIdx()).b16, VReg(s1).b16, VReg(s2).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND, AND_I8, AND_I16, AND_I32, AND_I64, AND_V128);

// ============================================================================
// OPCODE_AND_NOT
// ============================================================================
struct AND_NOT_I8 : Sequence<AND_NOT_I8, I<OPCODE_AND_NOT, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 & ~src2 -> bic dest, src1, src2
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() & ~i.src2.constant()) & 0xFF));
    } else if (i.src2.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      e.bic(i.dest, i.src1, e.w0);
    } else if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
      e.bic(i.dest, e.w0, i.src2);
    } else {
      e.bic(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_NOT_I16
    : Sequence<AND_NOT_I16, I<OPCODE_AND_NOT, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() & ~i.src2.constant()) & 0xFFFF));
    } else if (i.src2.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
      e.bic(i.dest, i.src1, e.w0);
    } else if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
      e.bic(i.dest, e.w0, i.src2);
    } else {
      e.bic(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_NOT_I32
    : Sequence<AND_NOT_I32, I<OPCODE_AND_NOT, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() & ~i.src2.constant())));
    } else if (i.src2.is_constant) {
      // src1 & ~imm: prefer the AND immediate form with the complement.
      const uint32_t not_imm = ~static_cast<uint32_t>(i.src2.constant());
      if (IsValidLogicalImm(not_imm, 32)) {
        e.and_(i.dest, i.src1, static_cast<uint64_t>(not_imm));
      } else {
        e.mov(e.w0,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
        e.bic(i.dest, i.src1, e.w0);
      }
    } else if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      e.bic(i.dest, e.w0, i.src2);
    } else {
      e.bic(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_NOT_I64
    : Sequence<AND_NOT_I64, I<OPCODE_AND_NOT, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() & ~i.src2.constant()));
    } else if (i.src2.is_constant) {
      // src1 & ~imm: prefer the AND immediate form with the complement.
      const uint64_t not_imm = ~static_cast<uint64_t>(i.src2.constant());
      if (IsValidLogicalImm(not_imm, 64)) {
        e.and_(i.dest, i.src1, not_imm);
      } else {
        e.mov(e.x0, static_cast<uint64_t>(i.src2.constant()));
        e.bic(i.dest, i.src1, e.x0);
      }
    } else if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
      e.bic(i.dest, e.x0, i.src2);
    } else {
      e.bic(i.dest, i.src1, i.src2);
    }
  }
};
struct AND_NOT_V128
    : Sequence<AND_NOT_V128, I<OPCODE_AND_NOT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // AND_NOT = src1 AND (NOT src2) = BIC(src1, src2)
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    e.bic(VReg(i.dest.reg().getIdx()).b16, VReg(s1).b16, VReg(s2).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND_NOT, AND_NOT_I8, AND_NOT_I16, AND_NOT_I32,
                     AND_NOT_I64, AND_NOT_V128);

// ============================================================================
// OPCODE_OR
// ============================================================================
struct OR_I8 : Sequence<OR_I8, I<OPCODE_OR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() | i.src2.constant()) & 0xFF));
    } else if (i.src2.is_constant) {
      e.orr_imm(i.dest, i.src1, i.src2.constant() & 0xFF, e.w0);
    } else if (i.src1.is_constant) {
      e.orr_imm(i.dest, i.src2, i.src1.constant() & 0xFF, e.w0);
    } else {
      e.orr(i.dest, i.src1, i.src2);
    }
  }
};
struct OR_I16 : Sequence<OR_I16, I<OPCODE_OR, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() | i.src2.constant()) & 0xFFFF));
    } else if (i.src2.is_constant) {
      e.orr_imm(i.dest, i.src1, i.src2.constant() & 0xFFFF, e.w0);
    } else if (i.src1.is_constant) {
      e.orr_imm(i.dest, i.src2, i.src1.constant() & 0xFFFF, e.w0);
    } else {
      e.orr(i.dest, i.src1, i.src2);
    }
  }
};
struct OR_I32 : Sequence<OR_I32, I<OPCODE_OR, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() | i.src2.constant())));
    } else if (i.src2.is_constant) {
      e.orr_imm(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant()), e.w0);
    } else if (i.src1.is_constant) {
      e.orr_imm(i.dest, i.src2, static_cast<uint32_t>(i.src1.constant()), e.w0);
    } else {
      e.orr(i.dest, i.src1, i.src2);
    }
  }
};
struct OR_I64 : Sequence<OR_I64, I<OPCODE_OR, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() | i.src2.constant()));
    } else if (i.src2.is_constant) {
      EmitOrrImm(e, i.dest, i.src1, static_cast<uint64_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitOrrImm(e, i.dest, i.src2, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.orr(i.dest, i.src1, i.src2);
    }
  }

  static void EmitOrrImm(A64Emitter& e, const XReg& dest, const XReg& src,
                         uint64_t imm) {
    if (IsValidLogicalImm(imm, 64)) {
      e.orr(dest, src, imm);
    } else {
      e.mov(e.x0, imm);
      e.orr(dest, src, e.x0);
    }
  }
};
struct OR_V128 : Sequence<OR_V128, I<OPCODE_OR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    e.orr(VReg(i.dest.reg().getIdx()).b16, VReg(s1).b16, VReg(s2).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_OR, OR_I8, OR_I16, OR_I32, OR_I64, OR_V128);

// ============================================================================
// OPCODE_XOR
// ============================================================================
struct XOR_I8 : Sequence<XOR_I8, I<OPCODE_XOR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() ^ i.src2.constant()) & 0xFF));
    } else if (i.src2.is_constant) {
      e.eor_imm(i.dest, i.src1, i.src2.constant() & 0xFF, e.w0);
    } else if (i.src1.is_constant) {
      e.eor_imm(i.dest, i.src2, i.src1.constant() & 0xFF, e.w0);
    } else {
      e.eor(i.dest, i.src1, i.src2);
    }
  }
};
struct XOR_I16 : Sequence<XOR_I16, I<OPCODE_XOR, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(
                        (i.src1.constant() ^ i.src2.constant()) & 0xFFFF));
    } else if (i.src2.is_constant) {
      e.eor_imm(i.dest, i.src1, i.src2.constant() & 0xFFFF, e.w0);
    } else if (i.src1.is_constant) {
      e.eor_imm(i.dest, i.src2, i.src1.constant() & 0xFFFF, e.w0);
    } else {
      e.eor(i.dest, i.src1, i.src2);
    }
  }
};
struct XOR_I32 : Sequence<XOR_I32, I<OPCODE_XOR, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                        i.src1.constant() ^ i.src2.constant())));
    } else if (i.src2.is_constant) {
      e.eor_imm(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant()), e.w0);
    } else if (i.src1.is_constant) {
      e.eor_imm(i.dest, i.src2, static_cast<uint32_t>(i.src1.constant()), e.w0);
    } else {
      e.eor(i.dest, i.src1, i.src2);
    }
  }
};
struct XOR_I64 : Sequence<XOR_I64, I<OPCODE_XOR, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant && i.src2.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(i.src1.constant() ^ i.src2.constant()));
    } else if (i.src2.is_constant) {
      EmitEorImm(e, i.dest, i.src1, static_cast<uint64_t>(i.src2.constant()));
    } else if (i.src1.is_constant) {
      EmitEorImm(e, i.dest, i.src2, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.eor(i.dest, i.src1, i.src2);
    }
  }

  static void EmitEorImm(A64Emitter& e, const XReg& dest, const XReg& src,
                         uint64_t imm) {
    if (IsValidLogicalImm(imm, 64)) {
      e.eor(dest, src, imm);
    } else {
      e.mov(e.x0, imm);
      e.eor(dest, src, e.x0);
    }
  }
};
struct XOR_V128 : Sequence<XOR_V128, I<OPCODE_XOR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s1 = SrcVReg(e, i.src1, 0);
    int s2 = SrcVReg(e, i.src2, 1);
    e.eor(VReg(i.dest.reg().getIdx()).b16, VReg(s1).b16, VReg(s2).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_XOR, XOR_I8, XOR_I16, XOR_I32, XOR_I64, XOR_V128);

// ============================================================================
// OPCODE_NOT
// ============================================================================
struct NOT_I8 : Sequence<NOT_I8, I<OPCODE_NOT, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint8_t>(~i.src1.constant())));
    } else {
      // mvn would set bits [31:8]; invert within the type width instead to
      // keep the I8 value zero-extended.
      e.eor(i.dest, i.src1, static_cast<uint64_t>(0xFF));
    }
  }
};
struct NOT_I16 : Sequence<NOT_I16, I<OPCODE_NOT, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint16_t>(~i.src1.constant())));
    } else {
      // mvn would set bits [31:16]; invert within the type width instead to
      // keep the I16 value zero-extended.
      e.eor(i.dest, i.src1, static_cast<uint64_t>(0xFFFF));
    }
  }
};
struct NOT_I32 : Sequence<NOT_I32, I<OPCODE_NOT, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest,
            static_cast<uint64_t>(static_cast<uint32_t>(~i.src1.constant())));
    } else {
      e.mvn(i.dest, i.src1);
    }
  }
};
struct NOT_I64 : Sequence<NOT_I64, I<OPCODE_NOT, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(~i.src1.constant()));
    } else {
      e.mvn(i.dest, i.src1);
    }
  }
};
struct NOT_V128 : Sequence<NOT_V128, I<OPCODE_NOT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 0);
    e.not_(VReg(i.dest.reg().getIdx()).b16, VReg(s).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NOT, NOT_I8, NOT_I16, NOT_I32, NOT_I64, NOT_V128);

// ============================================================================
// OPCODE_SHL
// ============================================================================
struct SHL_I8 : Sequence<SHL_I8, I<OPCODE_SHL, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      // Shift counts follow the x64 reference backend: count mod 32, result
      // truncated to the type width (counts 8..31 give 0).
      const uint32_t amt = static_cast<uint32_t>(i.src2.constant()) & 0x1F;
      if (i.src1.is_constant) {
        const uint8_t folded =
            amt >= 8
                ? 0
                : static_cast<uint8_t>(
                      static_cast<uint32_t>(i.src1.constant() & 0xFF) << amt);
        e.mov(i.dest, static_cast<uint64_t>(folded));
      } else {
        e.lsl(i.dest, i.src1, amt);
        e.uxtb(i.dest, i.dest);
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFF));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsl(i.dest, i.dest, e.w0);
      // Bits can shift past the type width; keep the I8 value zero-extended.
      e.uxtb(i.dest, i.dest);
    }
  }
};
struct SHL_I16 : Sequence<SHL_I16, I<OPCODE_SHL, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      // Shift counts follow the x64 reference backend: count mod 32, result
      // truncated to the type width (counts 16..31 give 0).
      const uint32_t amt = static_cast<uint32_t>(i.src2.constant()) & 0x1F;
      if (i.src1.is_constant) {
        const uint16_t folded =
            amt >= 16
                ? 0
                : static_cast<uint16_t>(
                      static_cast<uint32_t>(i.src1.constant() & 0xFFFF) << amt);
        e.mov(i.dest, static_cast<uint64_t>(folded));
      } else {
        e.lsl(i.dest, i.src1, amt);
        e.uxth(i.dest, i.dest);
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsl(i.dest, i.dest, e.w0);
      // Bits can shift past the type width; keep the I16 value zero-extended.
      e.uxth(i.dest, i.dest);
    }
  }
};
struct SHL_I32 : Sequence<SHL_I32, I<OPCODE_SHL, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                          i.src1.constant() << (i.src2.constant() & 0x1F))));
      } else {
        e.lsl(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x1F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsl(i.dest, i.dest, e.w0);
    }
  }
};
struct SHL_I64 : Sequence<SHL_I64, I<OPCODE_SHL, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()
                                            << (i.src2.constant() & 0x3F)));
      } else {
        e.lsl(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x3F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.x0, XReg(i.src2.reg().getIdx()));
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsl(i.dest, i.dest, e.x0);
    }
  }
};
struct SHL_V128 : Sequence<SHL_V128, I<OPCODE_SHL, V128Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // PPC 128-bit SHL by N bits (0-7). The value is stored as 4 word-swapped
    // 32-bit lanes. Carries flow from higher NEON lanes to lower:
    //   lane[i] = (lane[i] << N) | (lane[i+1] >> (32-N))
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    if (s < 4) {
      // A constant landed in the scratch bank the shifts below clobber.
      e.mov(VReg(3).b16, VReg(s).b16);
      s = 3;
    }
    if (i.src2.is_constant) {
      uint8_t sh = i.src2.constant() & 0x7;
      if (sh == 0) {
        if (d != s) {
          e.mov(VReg(d).b16, VReg(s).b16);
        }
        return;
      }
      // Read carry before writing result (handles dest==src aliasing).
      e.ushr(VReg(0).s4, VReg(s).s4, 32 - sh);
      e.shl(VReg(d).s4, VReg(s).s4, sh);
    } else {
      // Variable shift: mask to 0-7, splat, use ushl.
      e.and_(e.w0, WReg(i.src2.reg().getIdx()), 7);
      e.dup(VReg(1).s4, e.w0);
      e.movi(VReg(2).s4, 32);
      e.sub(VReg(2).s4, VReg(2).s4, VReg(1).s4);   // 32-N
      e.neg(VReg(2).s4, VReg(2).s4);               // -(32-N) for right shift
      e.ushl(VReg(0).s4, VReg(s).s4, VReg(2).s4);  // carry: lane >> (32-N)
      e.ushl(VReg(d).s4, VReg(s).s4, VReg(1).s4);  // result: lane << N
    }
    // Shift carries from lane i+1 to lane i; lane 3 gets zero.
    e.movi(VReg(1).s4, 0);
    e.ext(VReg(0).b16, VReg(0).b16, VReg(1).b16, 4);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHL, SHL_I8, SHL_I16, SHL_I32, SHL_I64, SHL_V128);

// ============================================================================
// OPCODE_SHR (logical shift right)
// ============================================================================
struct SHR_I8 : Sequence<SHR_I8, I<OPCODE_SHR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      // Shift counts follow the x64 reference backend: count mod 32
      // (counts 8..31 give 0).
      if (i.src1.is_constant) {
        const uint32_t amt = static_cast<uint32_t>(i.src2.constant()) & 0x1F;
        const uint8_t folded =
            amt >= 8 ? 0
                     : static_cast<uint8_t>(
                           static_cast<uint8_t>(i.src1.constant()) >> amt);
        e.mov(i.dest, static_cast<uint64_t>(folded));
      } else {
        e.lsr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x1F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFF));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsr(i.dest, i.dest, e.w0);
    }
  }
};
struct SHR_I16 : Sequence<SHR_I16, I<OPCODE_SHR, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      // Shift counts follow the x64 reference backend: count mod 32
      // (counts 16..31 give 0).
      if (i.src1.is_constant) {
        const uint32_t amt = static_cast<uint32_t>(i.src2.constant()) & 0x1F;
        const uint16_t folded =
            amt >= 16 ? 0
                      : static_cast<uint16_t>(
                            static_cast<uint16_t>(i.src1.constant()) >> amt);
        e.mov(i.dest, static_cast<uint64_t>(folded));
      } else {
        e.lsr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x1F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsr(i.dest, i.dest, e.w0);
    }
  }
};
struct SHR_I32 : Sequence<SHR_I32, I<OPCODE_SHR, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant()) >>
                                    (i.src2.constant() & 0x1F)));
      } else {
        e.lsr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x1F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsr(i.dest, i.dest, e.w0);
    }
  }
};
struct SHR_I64 : Sequence<SHR_I64, I<OPCODE_SHR, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()) >>
                          (i.src2.constant() & 0x3F));
      } else {
        e.lsr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x3F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.x0, XReg(i.src2.reg().getIdx()));
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.lsr(i.dest, i.dest, e.x0);
    }
  }
};
struct SHR_V128 : Sequence<SHR_V128, I<OPCODE_SHR, V128Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // PPC 128-bit SHR by N bits (0-7). Carries flow from lower NEON lanes
    // to higher:
    //   lane[i] = (lane[i] >> N) | (lane[i-1] << (32-N))
    int s = SrcVReg(e, i.src1, 0);
    int d = i.dest.reg().getIdx();
    if (s < 4) {
      // A constant landed in the scratch bank the shifts below clobber.
      e.mov(VReg(3).b16, VReg(s).b16);
      s = 3;
    }
    if (i.src2.is_constant) {
      uint8_t sh = i.src2.constant() & 0x7;
      if (sh == 0) {
        if (d != s) {
          e.mov(VReg(d).b16, VReg(s).b16);
        }
        return;
      }
      // Read carry before writing result (handles dest==src aliasing).
      e.shl(VReg(0).s4, VReg(s).s4, 32 - sh);
      e.ushr(VReg(d).s4, VReg(s).s4, sh);
    } else {
      // Variable shift: mask to 0-7, splat, use ushl.
      e.and_(e.w0, WReg(i.src2.reg().getIdx()), 7);
      e.dup(VReg(1).s4, e.w0);
      e.movi(VReg(2).s4, 32);
      e.sub(VReg(2).s4, VReg(2).s4, VReg(1).s4);   // 32-N
      e.ushl(VReg(0).s4, VReg(s).s4, VReg(2).s4);  // carry: lane << (32-N)
      e.neg(VReg(1).s4, VReg(1).s4);               // -N for right shift
      e.ushl(VReg(d).s4, VReg(s).s4, VReg(1).s4);  // result: lane >> N
    }
    // Shift carries from lane i-1 to lane i; lane 0 gets zero.
    e.movi(VReg(1).s4, 0);
    e.ext(VReg(0).b16, VReg(1).b16, VReg(0).b16, 12);
    e.orr(VReg(d).b16, VReg(d).b16, VReg(0).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHR, SHR_I8, SHR_I16, SHR_I32, SHR_I64, SHR_V128);

// ============================================================================
// OPCODE_SHA (arithmetic shift right)
// ============================================================================
struct SHA_I8 : Sequence<SHA_I8, I<OPCODE_SHA, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Sign-extend to 32-bit, then ASR.
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    } else {
      e.mov(e.w0, i.src1);
    }
    e.sxtb(e.w0, e.w0);
    if (i.src2.is_constant) {
      e.asr(i.dest, e.w0, static_cast<uint32_t>(i.src2.constant() & 0x1F));
    } else {
      e.asr(i.dest, e.w0, i.src2);
    }
    // The arithmetic shift sign-fills bits [31:8]; keep the I8 value
    // zero-extended.
    e.uxtb(i.dest, i.dest);
  }
};
struct SHA_I16 : Sequence<SHA_I16, I<OPCODE_SHA, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
    } else {
      e.mov(e.w0, i.src1);
    }
    e.sxth(e.w0, e.w0);
    if (i.src2.is_constant) {
      e.asr(i.dest, e.w0, static_cast<uint32_t>(i.src2.constant() & 0x1F));
    } else {
      e.asr(i.dest, e.w0, i.src2);
    }
    // The arithmetic shift sign-fills bits [31:16]; keep the I16 value
    // zero-extended.
    e.uxth(i.dest, i.dest);
  }
};
struct SHA_I32 : Sequence<SHA_I32, I<OPCODE_SHA, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(static_cast<uint32_t>(
                          i.src1.constant() >> (i.src2.constant() & 0x1F))));
      } else {
        e.asr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x1F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.asr(i.dest, i.dest, e.w0);
    }
  }
};
struct SHA_I64 : Sequence<SHA_I64, I<OPCODE_SHA, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant() >>
                                            (i.src2.constant() & 0x3F)));
      } else {
        e.asr(i.dest, i.src1, static_cast<uint32_t>(i.src2.constant() & 0x3F));
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.x0, XReg(i.src2.reg().getIdx()));
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      e.asr(i.dest, i.dest, e.x0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHA, SHA_I8, SHA_I16, SHA_I32, SHA_I64);

// ============================================================================
// OPCODE_ROTATE_LEFT
// ============================================================================
struct ROTATE_LEFT_I8
    : Sequence<ROTATE_LEFT_I8, I<OPCODE_ROTATE_LEFT, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ARM64 has ROR but no ROL. ROL(x, n) = ROR(x, size - n).
    // For 8-bit: duplicate into both halves of a 16-bit val, then shift.
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    } else {
      e.uxtb(e.w0, i.src1);
    }
    // Duplicate byte into bits [15:8] too: w0 = (w0 | (w0 << 8))
    e.orr(e.w0, e.w0, e.w0, Xbyak_aarch64::LSL, 8);
    if (i.src2.is_constant) {
      uint32_t amt = i.src2.constant() & 0x7;
      if (amt) {
        e.lsr(e.w0, e.w0, static_cast<uint32_t>(8 - amt));
      }
    } else {
      // shift = 8 - (src2 & 7)
      e.mov(e.w1, 8);
      e.and_(e.w2, i.src2, 7);
      e.sub(e.w1, e.w1, e.w2);
      e.lsr(e.w0, e.w0, e.w1);
    }
    e.uxtb(i.dest, e.w0);
  }
};
struct ROTATE_LEFT_I16
    : Sequence<ROTATE_LEFT_I16, I<OPCODE_ROTATE_LEFT, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
    } else {
      e.uxth(e.w0, i.src1);
    }
    e.orr(e.w0, e.w0, e.w0, Xbyak_aarch64::LSL, 16);
    if (i.src2.is_constant) {
      uint32_t amt = i.src2.constant() & 0xF;
      if (amt) {
        e.lsr(e.w0, e.w0, static_cast<uint32_t>(16 - amt));
      }
    } else {
      e.mov(e.w1, 16);
      e.and_(e.w2, i.src2, 0xF);
      e.sub(e.w1, e.w1, e.w2);
      e.lsr(e.w0, e.w0, e.w1);
    }
    e.uxth(i.dest, e.w0);
  }
};
struct ROTATE_LEFT_I32
    : Sequence<ROTATE_LEFT_I32, I<OPCODE_ROTATE_LEFT, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ROL(x, n) = ROR(x, 32 - n)
    if (i.src2.is_constant) {
      uint32_t amt = i.src2.constant() & 0x1F;
      if (amt == 0) {
        if (i.src1.is_constant) {
          e.mov(i.dest, static_cast<uint64_t>(
                            static_cast<uint32_t>(i.src1.constant())));
        } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
          e.mov(i.dest, i.src1);
        }
      } else {
        if (i.src1.is_constant) {
          e.mov(e.w0, static_cast<uint64_t>(
                          static_cast<uint32_t>(i.src1.constant())));
          e.ror(i.dest, e.w0, static_cast<uint32_t>(32 - amt));
        } else {
          e.ror(i.dest, i.src1, static_cast<uint32_t>(32 - amt));
        }
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.w0, i.src2);
      if (i.src1.is_constant) {
        e.mov(i.dest,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      // ROL(x, n) = ROR(x, -n) since ROR uses amount mod 32
      e.neg(e.w0, e.w0);
      e.ror(i.dest, i.dest, e.w0);
    }
  }
};
struct ROTATE_LEFT_I64
    : Sequence<ROTATE_LEFT_I64, I<OPCODE_ROTATE_LEFT, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      uint32_t amt = i.src2.constant() & 0x3F;
      if (amt == 0) {
        if (i.src1.is_constant) {
          e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
        } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
          e.mov(i.dest, i.src1);
        }
      } else {
        if (i.src1.is_constant) {
          e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
          e.ror(i.dest, e.x0, static_cast<uint32_t>(64 - amt));
        } else {
          e.ror(i.dest, i.src1, static_cast<uint32_t>(64 - amt));
        }
      }
    } else {
      // Read shift amount first — dest may alias src2.
      e.mov(e.x0, XReg(i.src2.reg().getIdx()));
      if (i.src1.is_constant) {
        e.mov(i.dest, static_cast<uint64_t>(i.src1.constant()));
      } else if (i.dest.reg().getIdx() != i.src1.reg().getIdx()) {
        e.mov(i.dest, i.src1);
      }
      // ROL(x, n) = ROR(x, -n) since ROR uses amount mod 64
      e.neg(e.x0, e.x0);
      e.ror(i.dest, i.dest, e.x0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROTATE_LEFT, ROTATE_LEFT_I8, ROTATE_LEFT_I16,
                     ROTATE_LEFT_I32, ROTATE_LEFT_I64);

// ============================================================================
// OPCODE_BYTE_SWAP
// ============================================================================
struct BYTE_SWAP_I16
    : Sequence<BYTE_SWAP_I16, I<OPCODE_BYTE_SWAP, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint16_t v = i.src1.constant();
      v = (v >> 8) | (v << 8);
      e.mov(i.dest, static_cast<uint64_t>(v));
    } else {
      e.rev16(i.dest, i.src1);
    }
  }
};
struct BYTE_SWAP_I32
    : Sequence<BYTE_SWAP_I32, I<OPCODE_BYTE_SWAP, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, static_cast<uint64_t>(xe::byte_swap(
                        static_cast<uint32_t>(i.src1.constant()))));
    } else {
      e.rev(i.dest, i.src1);
    }
  }
};
struct BYTE_SWAP_I64
    : Sequence<BYTE_SWAP_I64, I<OPCODE_BYTE_SWAP, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(i.dest, xe::byte_swap(static_cast<uint64_t>(i.src1.constant())));
    } else {
      e.rev(i.dest, i.src1);
    }
  }
};
struct BYTE_SWAP_V128
    : Sequence<BYTE_SWAP_V128, I<OPCODE_BYTE_SWAP, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int s = SrcVReg(e, i.src1, 0);
    e.rev32(VReg(i.dest.reg().getIdx()).b16, VReg(s).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BYTE_SWAP, BYTE_SWAP_I16, BYTE_SWAP_I32,
                     BYTE_SWAP_I64, BYTE_SWAP_V128);

// ============================================================================
// OPCODE_CNTLZ
// ============================================================================
struct CNTLZ_I8 : Sequence<CNTLZ_I8, I<OPCODE_CNTLZ, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint8_t v = static_cast<uint8_t>(i.src1.constant());
      uint8_t count = 0;
      while (count < 8 && !(v & 0x80)) {
        v <<= 1;
        count++;
      }
      e.mov(i.dest, static_cast<uint64_t>(count));
    } else {
      // clz operates on 32-bit, so shift left 24 to put byte in top.
      // OR a sentinel bit at position 23 so that a zero byte yields 8,
      // not 32.
      e.lsl(e.w0, i.src1, 24);
      e.orr(e.w0, e.w0, 1u << 23);
      e.clz(i.dest, e.w0);
    }
  }
};
struct CNTLZ_I16 : Sequence<CNTLZ_I16, I<OPCODE_CNTLZ, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint16_t v = static_cast<uint16_t>(i.src1.constant());
      uint8_t count = 0;
      while (count < 16 && !(v & 0x8000)) {
        v <<= 1;
        count++;
      }
      e.mov(i.dest, static_cast<uint64_t>(count));
    } else {
      // Sentinel bit at position 15 caps the result at 16 for zero input.
      e.lsl(e.w0, i.src1, 16);
      e.orr(e.w0, e.w0, 1u << 15);
      e.clz(i.dest, e.w0);
    }
  }
};
struct CNTLZ_I32 : Sequence<CNTLZ_I32, I<OPCODE_CNTLZ, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint32_t v = static_cast<uint32_t>(i.src1.constant());
      e.mov(i.dest, static_cast<uint64_t>(xe::lzcnt(v)));
    } else {
      e.clz(i.dest, i.src1);
    }
  }
};
struct CNTLZ_I64 : Sequence<CNTLZ_I64, I<OPCODE_CNTLZ, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      uint64_t v = static_cast<uint64_t>(i.src1.constant());
      e.mov(i.dest, static_cast<uint64_t>(xe::lzcnt(v)));
    } else {
      // clz on XReg returns into XReg, we need WReg dest.
      e.clz(e.x0, i.src1);
      e.mov(i.dest, e.w0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CNTLZ, CNTLZ_I8, CNTLZ_I16, CNTLZ_I32, CNTLZ_I64);

// ============================================================================
// Compare helpers
// ============================================================================
// ARM64: cmp src1, src2; cset dest, <cond>
// For I8/I16/I32 the dest is I8Op (WReg).
// For constants, load into scratch first.

#define DEFINE_COMPARE_XX(NAME, COND)                                          \
  struct NAME##_I8 : Sequence<NAME##_I8, I<OPCODE_##NAME, I8Op, I8Op, I8Op>> { \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));          \
        if (i.src2.is_constant) {                                              \
          e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFF));        \
          e.cmp(e.w0, e.w1);                                                   \
        } else {                                                               \
          e.cmp(e.w0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1,                                             \
                        static_cast<uint32_t>(i.src2.constant() & 0xFF));      \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I16                                                            \
      : Sequence<NAME##_I16, I<OPCODE_##NAME, I8Op, I16Op, I16Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));        \
        if (i.src2.is_constant) {                                              \
          e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));      \
          e.cmp(e.w0, e.w1);                                                   \
        } else {                                                               \
          e.cmp(e.w0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1,                                             \
                        static_cast<uint32_t>(i.src2.constant() & 0xFFFF));    \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I32                                                            \
      : Sequence<NAME##_I32, I<OPCODE_##NAME, I8Op, I32Op, I32Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(                                     \
                        static_cast<uint32_t>(i.src1.constant())));            \
        if (i.src2.is_constant) {                                              \
          e.mov(e.w1, static_cast<uint64_t>(                                   \
                          static_cast<uint32_t>(i.src2.constant())));          \
          e.cmp(e.w0, e.w1);                                                   \
        } else {                                                               \
          e.cmp(e.w0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1, static_cast<uint32_t>(i.src2.constant()));  \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I64                                                            \
      : Sequence<NAME##_I64, I<OPCODE_##NAME, I8Op, I64Op, I64Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));                 \
        if (i.src2.is_constant) {                                              \
          e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));               \
          e.cmp(e.x0, e.x1);                                                   \
        } else {                                                               \
          e.cmp(e.x0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1, static_cast<uint64_t>(i.src2.constant()));  \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct _tag_##NAME {}

DEFINE_COMPARE_XX(COMPARE_EQ, EQ);
DEFINE_COMPARE_XX(COMPARE_NE, NE);
// Signed I8/I16 comparisons need sign-extension to 32-bit because ARM64
// cmp always operates on full 32-bit WRegs. Without sign-extension,
// 0xFF (which is -1 as signed I8) would compare as 255, giving wrong results.
#define DEFINE_SIGNED_COMPARE_XX(NAME, COND)                                   \
  struct NAME##_I8 : Sequence<NAME##_I8, I<OPCODE_##NAME, I8Op, I8Op, I8Op>> { \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));          \
      } else {                                                                 \
        e.mov(e.w0, i.src1);                                                   \
      }                                                                        \
      e.sxtb(e.w0, e.w0);                                                      \
      if (i.src2.is_constant) {                                                \
        e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFF));          \
        e.sxtb(e.w1, e.w1);                                                    \
        e.cmp(e.w0, e.w1);                                                     \
      } else {                                                                 \
        e.sxtb(e.w1, i.src2);                                                  \
        e.cmp(e.w0, e.w1);                                                     \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I16                                                            \
      : Sequence<NAME##_I16, I<OPCODE_##NAME, I8Op, I16Op, I16Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));        \
      } else {                                                                 \
        e.mov(e.w0, i.src1);                                                   \
      }                                                                        \
      e.sxth(e.w0, e.w0);                                                      \
      if (i.src2.is_constant) {                                                \
        e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));        \
        e.sxth(e.w1, e.w1);                                                    \
        e.cmp(e.w0, e.w1);                                                     \
      } else {                                                                 \
        e.sxth(e.w1, i.src2);                                                  \
        e.cmp(e.w0, e.w1);                                                     \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I32                                                            \
      : Sequence<NAME##_I32, I<OPCODE_##NAME, I8Op, I32Op, I32Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.w0, static_cast<uint64_t>(                                     \
                        static_cast<uint32_t>(i.src1.constant())));            \
        if (i.src2.is_constant) {                                              \
          e.mov(e.w1, static_cast<uint64_t>(                                   \
                          static_cast<uint32_t>(i.src2.constant())));          \
          e.cmp(e.w0, e.w1);                                                   \
        } else {                                                               \
          e.cmp(e.w0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1, static_cast<uint32_t>(i.src2.constant()));  \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct NAME##_I64                                                            \
      : Sequence<NAME##_I64, I<OPCODE_##NAME, I8Op, I64Op, I64Op>> {           \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      if (i.src1.is_constant) {                                                \
        e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));                 \
        if (i.src2.is_constant) {                                              \
          e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));               \
          e.cmp(e.x0, e.x1);                                                   \
        } else {                                                               \
          e.cmp(e.x0, i.src2);                                                 \
        }                                                                      \
      } else if (i.src2.is_constant) {                                         \
        EmitCmpConstant(e, i.src1, static_cast<uint64_t>(i.src2.constant()));  \
      } else {                                                                 \
        e.cmp(i.src1, i.src2);                                                 \
      }                                                                        \
      e.cset(i.dest, Xbyak_aarch64::COND);                                     \
    }                                                                          \
  };                                                                           \
  struct _tag_##NAME {}

DEFINE_SIGNED_COMPARE_XX(COMPARE_SLT, LT);
DEFINE_SIGNED_COMPARE_XX(COMPARE_SLE, LE);
DEFINE_SIGNED_COMPARE_XX(COMPARE_SGT, GT);
DEFINE_SIGNED_COMPARE_XX(COMPARE_SGE, GE);
DEFINE_COMPARE_XX(COMPARE_ULT, LO);
DEFINE_COMPARE_XX(COMPARE_ULE, LS);
DEFINE_COMPARE_XX(COMPARE_UGT, HI);
DEFINE_COMPARE_XX(COMPARE_UGE, HS);

#undef DEFINE_COMPARE_XX

// Integer-only compare registrations are deferred until after float
// compare definitions below.

// ============================================================================
// OPCODE_SELECT
// ============================================================================
// dest = src1 ? src2 : src3
struct SELECT_I8
    : Sequence<SELECT_I8, I<OPCODE_SELECT, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFF));
    }
    if (i.src3.is_constant) {
      e.mov(e.w2, static_cast<uint64_t>(i.src3.constant() & 0xFF));
    }
    WReg s2 = i.src2.is_constant ? e.w1 : WReg(i.src2.reg().getIdx());
    WReg s3 = i.src3.is_constant ? e.w2 : WReg(i.src3.reg().getIdx());
    e.csel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_I16
    : Sequence<SELECT_I16, I<OPCODE_SELECT, I16Op, I8Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      e.mov(e.w1, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
    }
    if (i.src3.is_constant) {
      e.mov(e.w2, static_cast<uint64_t>(i.src3.constant() & 0xFFFF));
    }
    WReg s2 = i.src2.is_constant ? e.w1 : WReg(i.src2.reg().getIdx());
    WReg s3 = i.src3.is_constant ? e.w2 : WReg(i.src3.reg().getIdx());
    e.csel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_I32
    : Sequence<SELECT_I32, I<OPCODE_SELECT, I32Op, I8Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      e.mov(e.w1,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    }
    if (i.src3.is_constant) {
      e.mov(e.w2,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
    }
    WReg s2 = i.src2.is_constant ? e.w1 : WReg(i.src2.reg().getIdx());
    WReg s3 = i.src3.is_constant ? e.w2 : WReg(i.src3.reg().getIdx());
    e.csel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_I64
    : Sequence<SELECT_I64, I<OPCODE_SELECT, I64Op, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      e.mov(e.x1, static_cast<uint64_t>(i.src2.constant()));
    }
    if (i.src3.is_constant) {
      e.mov(e.x2, static_cast<uint64_t>(i.src3.constant()));
    }
    XReg s2 = i.src2.is_constant ? e.x1 : XReg(i.src2.reg().getIdx());
    XReg s3 = i.src3.is_constant ? e.x2 : XReg(i.src3.reg().getIdx());
    e.csel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_F32
    : Sequence<SELECT_F32, I<OPCODE_SELECT, F32Op, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w1, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w1);
    }
    if (i.src3.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src3.constant();
      e.mov(e.w1, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w1);
    }
    SReg s2 = i.src2.is_constant ? e.s0 : SReg(i.src2.reg().getIdx());
    SReg s3 = i.src3.is_constant ? e.s1 : SReg(i.src3.reg().getIdx());
    e.fcsel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_F64
    : Sequence<SELECT_F64, I<OPCODE_SELECT, F64Op, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg cond = i.src1.is_constant ? e.w0 : WReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    }
    e.cmp(cond, 0);
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x1, c.u);
      e.fmov(e.d0, e.x1);
    }
    if (i.src3.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src3.constant();
      e.mov(e.x1, c.u);
      e.fmov(e.d1, e.x1);
    }
    DReg s2 = i.src2.is_constant ? e.d0 : DReg(i.src2.reg().getIdx());
    DReg s3 = i.src3.is_constant ? e.d1 : DReg(i.src3.reg().getIdx());
    e.fcsel(i.dest, s2, s3, Xbyak_aarch64::NE);
  }
};
struct SELECT_V128_V128
    : Sequence<SELECT_V128_V128,
               I<OPCODE_SELECT, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    int d = i.dest.reg().getIdx();
    int s1 = SrcVReg(e, i.src1, 0);  // condition mask
    int s2 = SrcVReg(e, i.src2, 1);  // value for mask=0
    int s3 = SrcVReg(e, i.src3, 2);  // value for mask=1
    // PPC vsel / HIR SELECT V128: bit=1 → src3, bit=0 → src2
    // ARM64 BIT: dest = (op1 & mask) | (dest & ~mask) — keeps dest where mask=0
    // ARM64 BIF: dest = (dest & mask) | (op1 & ~mask) — keeps dest where mask=1
    // Use BIT/BIF to avoid clobbering when dest aliases an operand.
    if (d == s1) {
      // dest already holds the mask. BSL is safe here.
      e.bsl(VReg(d).b16, VReg(s3).b16, VReg(s2).b16);
    } else if (d == s3) {
      // dest holds the mask=1 value. BIT inserts mask=1 bits from s3, keeps
      // dest (=s2-candidate) where mask=0... no, dest=s3 not s2.
      // Use: copy s2 to scratch, then BIT(scratch, s3, mask), move to dest.
      // Or: copy mask to scratch v0, copy s2 to dest, BIT(dest, s3_orig, v0).
      // Simplest: use scratch v0 for mask, then BSL.
      e.orr(VReg(0).b16, VReg(s1).b16, VReg(s1).b16);  // v0 = mask
      e.bsl(VReg(0).b16, VReg(s3).b16, VReg(s2).b16);  // v0 = result
      e.orr(VReg(d).b16, VReg(0).b16, VReg(0).b16);    // dest = result
    } else if (d == s2) {
      // dest holds the mask=0 value. BIF inserts ~mask bits from s2,
      // but dest=s2... Use scratch for mask.
      e.orr(VReg(0).b16, VReg(s1).b16, VReg(s1).b16);  // v0 = mask
      e.bsl(VReg(0).b16, VReg(s3).b16, VReg(s2).b16);  // v0 = result
      e.orr(VReg(d).b16, VReg(0).b16, VReg(0).b16);    // dest = result
    } else {
      // No aliasing — copy mask to dest, then BSL.
      e.orr(VReg(d).b16, VReg(s1).b16, VReg(s1).b16);
      e.bsl(VReg(d).b16, VReg(s3).b16, VReg(s2).b16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SELECT, SELECT_I8, SELECT_I16, SELECT_I32,
                     SELECT_I64, SELECT_F32, SELECT_F64, SELECT_V128_V128);

// ============================================================================
// OPCODE_LOAD_LOCAL
// ============================================================================
// Note: all types are always aligned on the stack.
// For large offsets that don't fit in the unsigned immediate field of
// LDR/STR, compute the effective address in a temp register first.
static inline bool LocalOffsetFitsImm(uint32_t offset, uint32_t scale) {
  return (offset % scale) == 0 && (offset / scale) <= 0xFFF;
}
// Compute base register for local access; returns {base, imm} pair.
// If the offset fits the scaled immediate, returns {sp, offset}.
// Otherwise loads sp+offset into x17 and returns {x17, 0}.
static inline XReg PrepareLocalBase(A64Emitter& e, uint32_t offset,
                                    uint32_t scale) {
  if (LocalOffsetFitsImm(offset, scale)) {
    return e.sp;
  }
  e.mov(e.x17, static_cast<uint64_t>(offset));
  // SP is only encodable as the source operand of the extended-register add,
  // the plain shifted-register form rejecting register index 31.
  e.add(e.x17, e.sp, e.x17, UXTX);
  return e.x17;
}
static inline uint32_t PrepareLocalImm(uint32_t offset, uint32_t scale) {
  if (LocalOffsetFitsImm(offset, scale)) {
    return offset;
  }
  return 0;
}

struct LOAD_LOCAL_I8
    : Sequence<LOAD_LOCAL_I8, I<OPCODE_LOAD_LOCAL, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 1);
    e.ldrb(i.dest, ptr(base, PrepareLocalImm(off, 1)));
  }
};
struct LOAD_LOCAL_I16
    : Sequence<LOAD_LOCAL_I16, I<OPCODE_LOAD_LOCAL, I16Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 2);
    e.ldrh(i.dest, ptr(base, PrepareLocalImm(off, 2)));
  }
};
struct LOAD_LOCAL_I32
    : Sequence<LOAD_LOCAL_I32, I<OPCODE_LOAD_LOCAL, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 4);
    e.ldr(i.dest, ptr(base, PrepareLocalImm(off, 4)));
  }
};
struct LOAD_LOCAL_I64
    : Sequence<LOAD_LOCAL_I64, I<OPCODE_LOAD_LOCAL, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 8);
    e.ldr(i.dest, ptr(base, PrepareLocalImm(off, 8)));
  }
};
struct LOAD_LOCAL_F32
    : Sequence<LOAD_LOCAL_F32, I<OPCODE_LOAD_LOCAL, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 4);
    e.ldr(i.dest, ptr(base, PrepareLocalImm(off, 4)));
  }
};
struct LOAD_LOCAL_F64
    : Sequence<LOAD_LOCAL_F64, I<OPCODE_LOAD_LOCAL, F64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 8);
    e.ldr(i.dest, ptr(base, PrepareLocalImm(off, 8)));
  }
};
struct LOAD_LOCAL_V128
    : Sequence<LOAD_LOCAL_V128, I<OPCODE_LOAD_LOCAL, V128Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 16);
    e.ldr(i.dest, ptr(base, PrepareLocalImm(off, 16)));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_LOCAL, LOAD_LOCAL_I8, LOAD_LOCAL_I16,
                     LOAD_LOCAL_I32, LOAD_LOCAL_I64, LOAD_LOCAL_F32,
                     LOAD_LOCAL_F64, LOAD_LOCAL_V128);

// ============================================================================
// OPCODE_STORE_LOCAL
// ============================================================================
struct STORE_LOCAL_I8
    : Sequence<STORE_LOCAL_I8, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 1);
    uint32_t imm = PrepareLocalImm(off, 1);
    if (i.src2.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      e.strb(e.w0, ptr(base, imm));
    } else {
      e.strb(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_I16
    : Sequence<STORE_LOCAL_I16, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 2);
    uint32_t imm = PrepareLocalImm(off, 2);
    if (i.src2.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
      e.strh(e.w0, ptr(base, imm));
    } else {
      e.strh(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_I32
    : Sequence<STORE_LOCAL_I32, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 4);
    uint32_t imm = PrepareLocalImm(off, 4);
    if (i.src2.is_constant) {
      if (i.src2.constant() == 0) {
        e.str(e.wzr, ptr(base, imm));
      } else {
        e.mov(e.w0,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
        e.str(e.w0, ptr(base, imm));
      }
    } else {
      e.str(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_I64
    : Sequence<STORE_LOCAL_I64, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 8);
    uint32_t imm = PrepareLocalImm(off, 8);
    if (i.src2.is_constant) {
      if (i.src2.constant() == 0) {
        e.str(e.xzr, ptr(base, imm));
      } else {
        e.mov(e.x0, static_cast<uint64_t>(i.src2.constant()));
        e.str(e.x0, ptr(base, imm));
      }
    } else {
      e.str(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_F32
    : Sequence<STORE_LOCAL_F32, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 4);
    uint32_t imm = PrepareLocalImm(off, 4);
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.str(e.w0, ptr(base, imm));
    } else {
      e.str(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_F64
    : Sequence<STORE_LOCAL_F64, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 8);
    uint32_t imm = PrepareLocalImm(off, 8);
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.str(e.x0, ptr(base, imm));
    } else {
      e.str(i.src2, ptr(base, imm));
    }
  }
};
struct STORE_LOCAL_V128
    : Sequence<STORE_LOCAL_V128, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    uint32_t off = static_cast<uint32_t>(i.src1.constant());
    auto base = PrepareLocalBase(e, off, 16);
    uint32_t imm = PrepareLocalImm(off, 16);
    if (i.src2.is_constant) {
      LoadV128Const(e, 0, i.src2.constant());
      e.str(QReg(0), ptr(base, imm));
    } else {
      e.str(i.src2, ptr(base, imm));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_LOCAL, STORE_LOCAL_I8, STORE_LOCAL_I16,
                     STORE_LOCAL_I32, STORE_LOCAL_I64, STORE_LOCAL_F32,
                     STORE_LOCAL_F64, STORE_LOCAL_V128);

// ============================================================================
// OPCODE_CAST
// ============================================================================
struct CAST_I32_F32 : Sequence<CAST_I32_F32, I<OPCODE_CAST, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Bitcast float -> int (not conversion).
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(i.dest, static_cast<uint64_t>(c.u));
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
struct CAST_I64_F64 : Sequence<CAST_I64_F64, I<OPCODE_CAST, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(i.dest, c.u);
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
struct CAST_F32_I32 : Sequence<CAST_F32_I32, I<OPCODE_CAST, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      e.fmov(i.dest, e.w0);
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
struct CAST_F64_I64 : Sequence<CAST_F64_I64, I<OPCODE_CAST, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
      e.fmov(i.dest, e.x0);
    } else {
      e.fmov(i.dest, i.src1);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CAST, CAST_I32_F32, CAST_I64_F64, CAST_F32_I32,
                     CAST_F64_I64);

// ============================================================================
// OPCODE_DID_SATURATE
// ============================================================================
struct DID_SATURATE
    : Sequence<DID_SATURATE, I<OPCODE_DID_SATURATE, I8Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // TODO(has207): Implement saturation tracking. ARM64 NEON saturating
    // ops (sqadd/uqadd/etc.) set FPSR.QC — clear it before the saturating
    // op, then read it here with mrs. Requires coordinating with all
    // ARITHMETIC_SATURATE vector paths. Always returns 0 for now (same as
    // x64 backend).
    e.mov(i.dest, 0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DID_SATURATE, DID_SATURATE);

// ============================================================================
// OPCODE_MAX / OPCODE_MIN (scalar)
// ============================================================================
struct MAX_F32 : Sequence<MAX_F32, I<OPCODE_MAX, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fmax(i.dest, e.s0, i.src2.is_constant ? e.s1 : i.src2);
    } else if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fmax(i.dest, i.src1, e.s0);
    } else {
      e.fmax(i.dest, i.src1, i.src2);
    }
  }
};
struct MAX_F64 : Sequence<MAX_F64, I<OPCODE_MAX, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fmax(i.dest, e.d0, i.src2.is_constant ? e.d1 : i.src2);
    } else if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fmax(i.dest, i.src1, e.d0);
    } else {
      e.fmax(i.dest, i.src1, i.src2);
    }
  }
};
struct MAX_V128 : Sequence<MAX_V128, I<OPCODE_MAX, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s1, s2;
      PrepareVmxFpSources(e, i.src1, i.src2, s1, s2);
      e.fmax(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
      // PPC vmaxfp: a NaN input is returned as-is, vA before vB.
      FixupVmxMaxMinNan(e);
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, 2, 0, 1);
      }
      e.mov(VReg(i.dest.reg().getIdx()).b16, VReg(2).b16);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MAX, MAX_F32, MAX_F64, MAX_V128);

// MIN has signed semantics (HIR builder constant-folds using CompareSLT).
// I8/I16 need sign-extension; all need signed condition code (LT not LO).
struct MIN_I8 : Sequence<MIN_I8, I<OPCODE_MIN, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFF));
    } else {
      e.mov(e.w0, i.src1);
    }
    e.sxtb(e.w0, e.w0);
    if (i.src2.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      e.sxtb(e.w17, e.w17);
    } else {
      e.sxtb(e.w17, i.src2);
    }
    e.cmp(e.w0, e.w17);
    e.csel(i.dest, e.w0, e.w17, LT);
    // The selected value is sign-extended scratch; keep the I8 value
    // zero-extended.
    e.uxtb(i.dest, i.dest);
  }
};
struct MIN_I16 : Sequence<MIN_I16, I<OPCODE_MIN, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0, static_cast<uint64_t>(i.src1.constant() & 0xFFFF));
    } else {
      e.mov(e.w0, i.src1);
    }
    e.sxth(e.w0, e.w0);
    if (i.src2.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
      e.sxth(e.w17, e.w17);
    } else {
      e.sxth(e.w17, i.src2);
    }
    e.cmp(e.w0, e.w17);
    e.csel(i.dest, e.w0, e.w17, LT);
    // The selected value is sign-extended scratch; keep the I16 value
    // zero-extended.
    e.uxth(i.dest, i.dest);
  }
};
struct MIN_I32 : Sequence<MIN_I32, I<OPCODE_MIN, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
    } else {
      e.mov(e.w0, i.src1);
    }
    if (i.src2.is_constant) {
      e.mov(e.w17,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    } else {
      e.mov(e.w17, i.src2);
    }
    e.cmp(e.w0, e.w17);
    e.csel(i.dest, e.w0, e.w17, LT);
  }
};
struct MIN_I64 : Sequence<MIN_I64, I<OPCODE_MIN, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
    } else {
      e.mov(e.x0, i.src1);
    }
    if (i.src2.is_constant) {
      e.mov(e.x17, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x17, i.src2);
    }
    e.cmp(e.x0, e.x17);
    e.csel(i.dest, e.x0, e.x17, LT);
  }
};
struct MIN_F32 : Sequence<MIN_F32, I<OPCODE_MIN, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fmin(i.dest, e.s0, i.src2.is_constant ? e.s1 : i.src2);
    } else if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fmin(i.dest, i.src1, e.s0);
    } else {
      e.fmin(i.dest, i.src1, i.src2);
    }
  }
};
struct MIN_F64 : Sequence<MIN_F64, I<OPCODE_MIN, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fmin(i.dest, e.d0, i.src2.is_constant ? e.d1 : i.src2);
    } else if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fmin(i.dest, i.src1, e.d0);
    } else {
      e.fmin(i.dest, i.src1, i.src2);
    }
  }
};
struct MIN_V128 : Sequence<MIN_V128, I<OPCODE_MIN, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s1, s2;
      PrepareVmxFpSources(e, i.src1, i.src2, s1, s2);
      e.fmin(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
      // PPC vminfp: a NaN input is returned as-is, vA before vB.
      FixupVmxMaxMinNan(e);
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, 2, 0, 1);
      }
      e.mov(VReg(i.dest.reg().getIdx()).b16, VReg(2).b16);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MIN, MIN_I8, MIN_I16, MIN_I32, MIN_I64, MIN_F32,
                     MIN_F64, MIN_V128);

// ============================================================================
// OPCODE_CONVERT
// ============================================================================
struct CONVERT_I32_F32
    : Sequence<CONVERT_I32_F32, I<OPCODE_CONVERT, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    SReg src = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.fcvtzs(i.dest, src);
    } else {
      e.frintx(e.s0, src);
      e.fcvtzs(i.dest, e.s0);
    }
  }
};
struct CONVERT_I32_F64
    : Sequence<CONVERT_I32_F64, I<OPCODE_CONVERT, I32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    DReg src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.fcvtzs(i.dest, src);
    } else {
      // Use current FPCR rounding mode: round first, then truncate.
      e.frintx(e.d0, src);
      e.fcvtzs(i.dest, e.d0);
    }
  }
};
struct CONVERT_I64_F64
    : Sequence<CONVERT_I64_F64, I<OPCODE_CONVERT, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    DReg src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.fcvtzs(i.dest, src);
    } else {
      e.frintx(e.d0, src);
      e.fcvtzs(i.dest, e.d0);
    }
  }
};
struct CONVERT_F32_I32
    : Sequence<CONVERT_F32_I32, I<OPCODE_CONVERT, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.w0,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      e.scvtf(i.dest, e.w0);
    } else {
      e.scvtf(i.dest, i.src1);
    }
  }
};
struct CONVERT_F64_I64
    : Sequence<CONVERT_F64_I64, I<OPCODE_CONVERT, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.mov(e.x0, static_cast<uint64_t>(i.src1.constant()));
      e.scvtf(i.dest, e.x0);
    } else {
      e.scvtf(i.dest, i.src1);
    }
  }
};
struct CONVERT_F32_F64
    : Sequence<CONVERT_F32_F64, I<OPCODE_CONVERT, F32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fcvt(i.dest, e.d0);
    } else {
      e.fcvt(i.dest, i.src1);
    }
  }
};
struct CONVERT_F64_F32
    : Sequence<CONVERT_F64_F32, I<OPCODE_CONVERT, F64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fcvt(i.dest, e.s0);
    } else {
      e.fcvt(i.dest, i.src1);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CONVERT, CONVERT_I32_F32, CONVERT_I32_F64,
                     CONVERT_I64_F64, CONVERT_F32_I32, CONVERT_F64_I64,
                     CONVERT_F32_F64, CONVERT_F64_F32);

// ============================================================================
// OPCODE_ROUND
// ============================================================================
struct ROUND_F32 : Sequence<ROUND_F32, I<OPCODE_ROUND, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Round mode is in i.instr->flags.
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    auto src = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.frintz(i.dest, src);
        break;
      case ROUND_TO_NEAREST:
        e.frintn(i.dest, src);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.frintm(i.dest, src);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.frintp(i.dest, src);
        break;
      default:
        // ROUND_DYNAMIC - use current rounding mode.
        e.frinti(i.dest, src);
        break;
    }
  }
};
struct ROUND_F64 : Sequence<ROUND_F64, I<OPCODE_ROUND, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    auto src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.frintz(i.dest, src);
        break;
      case ROUND_TO_NEAREST:
        e.frintn(i.dest, src);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.frintm(i.dest, src);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.frintp(i.dest, src);
        break;
      default:
        e.frinti(i.dest, src);
        break;
    }
  }
};
struct ROUND_V128 : Sequence<ROUND_V128, I<OPCODE_ROUND, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      auto src = VReg(s).s4;
      auto dst = VReg(i.dest.reg().getIdx()).s4;
      switch (i.instr->flags) {
        case ROUND_TO_ZERO:
          e.frintz(dst, src);
          break;
        case ROUND_TO_NEAREST:
          e.frintn(dst, src);
          break;
        case ROUND_TO_MINUS_INFINITY:
          e.frintm(dst, src);
          break;
        case ROUND_TO_POSITIVE_INFINITY:
          e.frintp(dst, src);
          break;
        default:
          // ROUND_DYNAMIC - use current rounding mode.
          e.frinti(dst, src);
          break;
      }
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROUND, ROUND_F32, ROUND_F64, ROUND_V128);

// ============================================================================
// OPCODE_CLEAR_FP_EXCEPTIONS
// ============================================================================
struct CLEAR_FP_EXCEPTIONS
    : Sequence<CLEAR_FP_EXCEPTIONS, I<OPCODE_CLEAR_FP_EXCEPTIONS, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    e.msr(3, 3, 4, 4, 1, e.xzr);  // msr FPSR, xzr
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CLEAR_FP_EXCEPTIONS, CLEAR_FP_EXCEPTIONS);

// ============================================================================
// OPCODE_LOAD_FP_EXCEPTIONS
// ============================================================================
struct LOAD_FP_EXCEPTIONS
    : Sequence<LOAD_FP_EXCEPTIONS, I<OPCODE_LOAD_FP_EXCEPTIONS, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    e.mrs(e.x0, 3, 3, 4, 4, 1);  // mrs x0, FPSR
    // IOC DZC OFC UFC IXC already sit in the order FpExceptionFlags wants.
    e.and_(i.dest, e.w0, FP_EXCEPTION_ALL);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_FP_EXCEPTIONS, LOAD_FP_EXCEPTIONS);

// ============================================================================
// OPCODE_SQRT
// ============================================================================
struct SQRT_F32 : Sequence<SQRT_F32, I<OPCODE_SQRT, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fsqrt(i.dest, e.s0);
    } else {
      e.fsqrt(i.dest, i.src1);
    }
  }
};
struct SQRT_F64 : Sequence<SQRT_F64, I<OPCODE_SQRT, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fsqrt(i.dest, e.d0);
    } else {
      e.fsqrt(i.dest, i.src1);
    }
  }
};
struct SQRT_V128 : Sequence<SQRT_V128, I<OPCODE_SQRT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      int s = SrcVReg(e, i.src1, 0);
      e.fsqrt(VReg(i.dest.reg().getIdx()).s4, VReg(s).s4);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SQRT, SQRT_F32, SQRT_F64, SQRT_V128);

// ============================================================================
// OPCODE_IS_NAN
// ============================================================================
struct IS_NAN_F32 : Sequence<IS_NAN_F32, I<OPCODE_IS_NAN, I8Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fcmp(e.s0, e.s0);
    } else {
      e.fcmp(i.src1, i.src1);
    }
    // VS (overflow) set when either operand is NaN.
    e.cset(i.dest, Xbyak_aarch64::VS);
  }
};
struct IS_NAN_F64 : Sequence<IS_NAN_F64, I<OPCODE_IS_NAN, I8Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fcmp(e.d0, e.d0);
    } else {
      e.fcmp(i.src1, i.src1);
    }
    e.cset(i.dest, Xbyak_aarch64::VS);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_IS_NAN, IS_NAN_F32, IS_NAN_F64);

// ============================================================================
// OPCODE_COMPARE_EQ/NE for float
// ============================================================================
struct COMPARE_EQ_F32
    : Sequence<COMPARE_EQ_F32, I<OPCODE_COMPARE_EQ, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      if (i.src2.is_constant) {
        union {
          float f;
          uint32_t u;
        } c2;
        c2.f = i.src2.constant();
        e.mov(e.w0, static_cast<uint64_t>(c2.u));
        e.fmov(e.s1, e.w0);
        e.fcmp(e.s0, e.s1);
      } else {
        e.fcmp(e.s0, i.src2);
      }
    } else if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fcmp(i.src1, e.s0);
    } else {
      e.fcmp(i.src1, i.src2);
    }
    e.cset(i.dest, Xbyak_aarch64::EQ);
  }
};
struct COMPARE_EQ_F64
    : Sequence<COMPARE_EQ_F64, I<OPCODE_COMPARE_EQ, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      if (i.src2.is_constant) {
        union {
          double d;
          uint64_t u;
        } c2;
        c2.d = i.src2.constant();
        e.mov(e.x0, c2.u);
        e.fmov(e.d1, e.x0);
        e.fcmp(e.d0, e.d1);
      } else {
        e.fcmp(e.d0, i.src2);
      }
    } else if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fcmp(i.src1, e.d0);
    } else {
      e.fcmp(i.src1, i.src2);
    }
    e.cset(i.dest, Xbyak_aarch64::EQ);
  }
};

struct COMPARE_NE_F32
    : Sequence<COMPARE_NE_F32, I<OPCODE_COMPARE_NE, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      if (i.src2.is_constant) {
        union {
          float f;
          uint32_t u;
        } c2;
        c2.f = i.src2.constant();
        e.mov(e.w0, static_cast<uint64_t>(c2.u));
        e.fmov(e.s1, e.w0);
        e.fcmp(e.s0, e.s1);
      } else {
        e.fcmp(e.s0, i.src2);
      }
    } else if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
      e.fcmp(i.src1, e.s0);
    } else {
      e.fcmp(i.src1, i.src2);
    }
    e.cset(i.dest, Xbyak_aarch64::NE);
  }
};
struct COMPARE_NE_F64
    : Sequence<COMPARE_NE_F64, I<OPCODE_COMPARE_NE, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      if (i.src2.is_constant) {
        union {
          double d;
          uint64_t u;
        } c2;
        c2.d = i.src2.constant();
        e.mov(e.x0, c2.u);
        e.fmov(e.d1, e.x0);
        e.fcmp(e.d0, e.d1);
      } else {
        e.fcmp(e.d0, i.src2);
      }
    } else if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
      e.fcmp(i.src1, e.d0);
    } else {
      e.fcmp(i.src1, i.src2);
    }
    e.cset(i.dest, Xbyak_aarch64::NE);
  }
};

// Float compares for SLT/SLE/SGT/SGE (use MI/LS/GT/GE for ordered compares)
#define DEFINE_FLOAT_COMPARE(NAME, COND_S, COND_D)                   \
  struct NAME##_F32                                                  \
      : Sequence<NAME##_F32, I<OPCODE_##NAME, I8Op, F32Op, F32Op>> { \
    static void Emit(A64Emitter& e, const EmitArgType& i) {          \
      if (i.src1.is_constant) {                                      \
        union {                                                      \
          float f;                                                   \
          uint32_t u;                                                \
        } c;                                                         \
        c.f = i.src1.constant();                                     \
        e.mov(e.w0, static_cast<uint64_t>(c.u));                     \
        e.fmov(e.s0, e.w0);                                          \
        if (i.src2.is_constant) {                                    \
          union {                                                    \
            float f;                                                 \
            uint32_t u;                                              \
          } c2;                                                      \
          c2.f = i.src2.constant();                                  \
          e.mov(e.w0, static_cast<uint64_t>(c2.u));                  \
          e.fmov(e.s1, e.w0);                                        \
          e.fcmp(e.s0, e.s1);                                        \
        } else {                                                     \
          e.fcmp(e.s0, i.src2);                                      \
        }                                                            \
      } else if (i.src2.is_constant) {                               \
        union {                                                      \
          float f;                                                   \
          uint32_t u;                                                \
        } c;                                                         \
        c.f = i.src2.constant();                                     \
        e.mov(e.w0, static_cast<uint64_t>(c.u));                     \
        e.fmov(e.s0, e.w0);                                          \
        e.fcmp(i.src1, e.s0);                                        \
      } else {                                                       \
        e.fcmp(i.src1, i.src2);                                      \
      }                                                              \
      e.cset(i.dest, Xbyak_aarch64::COND_S);                         \
    }                                                                \
  };                                                                 \
  struct NAME##_F64                                                  \
      : Sequence<NAME##_F64, I<OPCODE_##NAME, I8Op, F64Op, F64Op>> { \
    static void Emit(A64Emitter& e, const EmitArgType& i) {          \
      if (i.src1.is_constant) {                                      \
        union {                                                      \
          double d;                                                  \
          uint64_t u;                                                \
        } c;                                                         \
        c.d = i.src1.constant();                                     \
        e.mov(e.x0, c.u);                                            \
        e.fmov(e.d0, e.x0);                                          \
        if (i.src2.is_constant) {                                    \
          union {                                                    \
            double d;                                                \
            uint64_t u;                                              \
          } c2;                                                      \
          c2.d = i.src2.constant();                                  \
          e.mov(e.x0, c2.u);                                         \
          e.fmov(e.d1, e.x0);                                        \
          e.fcmp(e.d0, e.d1);                                        \
        } else {                                                     \
          e.fcmp(e.d0, i.src2);                                      \
        }                                                            \
      } else if (i.src2.is_constant) {                               \
        union {                                                      \
          double d;                                                  \
          uint64_t u;                                                \
        } c;                                                         \
        c.d = i.src2.constant();                                     \
        e.mov(e.x0, c.u);                                            \
        e.fmov(e.d0, e.x0);                                          \
        e.fcmp(i.src1, e.d0);                                        \
      } else {                                                       \
        e.fcmp(i.src1, i.src2);                                      \
      }                                                              \
      e.cset(i.dest, Xbyak_aarch64::COND_D);                         \
    }                                                                \
  }

DEFINE_FLOAT_COMPARE(COMPARE_SLT, MI, MI);
DEFINE_FLOAT_COMPARE(COMPARE_SLE, LS, LS);
DEFINE_FLOAT_COMPARE(COMPARE_SGT, GT, GT);
DEFINE_FLOAT_COMPARE(COMPARE_SGE, GE, GE);
// For fcmp: LT = N!=V = "less than or unordered" (correct for ULT on floats).
DEFINE_FLOAT_COMPARE(COMPARE_ULT, LT, LT);
// For fcmp: LE = Z=1 or N!=V = "less/equal or unordered" (correct for ULE on
// floats).
DEFINE_FLOAT_COMPARE(COMPARE_ULE, LE, LE);
DEFINE_FLOAT_COMPARE(COMPARE_UGT, HI, HI);
DEFINE_FLOAT_COMPARE(COMPARE_UGE, HS, HS);
#undef DEFINE_FLOAT_COMPARE

// Register all compare opcodes with integer + float variants.
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_EQ, COMPARE_EQ_I8, COMPARE_EQ_I16,
                     COMPARE_EQ_I32, COMPARE_EQ_I64, COMPARE_EQ_F32,
                     COMPARE_EQ_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_NE, COMPARE_NE_I8, COMPARE_NE_I16,
                     COMPARE_NE_I32, COMPARE_NE_I64, COMPARE_NE_F32,
                     COMPARE_NE_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_SLT, COMPARE_SLT_I8, COMPARE_SLT_I16,
                     COMPARE_SLT_I32, COMPARE_SLT_I64, COMPARE_SLT_F32,
                     COMPARE_SLT_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_SLE, COMPARE_SLE_I8, COMPARE_SLE_I16,
                     COMPARE_SLE_I32, COMPARE_SLE_I64, COMPARE_SLE_F32,
                     COMPARE_SLE_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_SGT, COMPARE_SGT_I8, COMPARE_SGT_I16,
                     COMPARE_SGT_I32, COMPARE_SGT_I64, COMPARE_SGT_F32,
                     COMPARE_SGT_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_SGE, COMPARE_SGE_I8, COMPARE_SGE_I16,
                     COMPARE_SGE_I32, COMPARE_SGE_I64, COMPARE_SGE_F32,
                     COMPARE_SGE_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_ULT, COMPARE_ULT_I8, COMPARE_ULT_I16,
                     COMPARE_ULT_I32, COMPARE_ULT_I64, COMPARE_ULT_F32,
                     COMPARE_ULT_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_ULE, COMPARE_ULE_I8, COMPARE_ULE_I16,
                     COMPARE_ULE_I32, COMPARE_ULE_I64, COMPARE_ULE_F32,
                     COMPARE_ULE_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_UGT, COMPARE_UGT_I8, COMPARE_UGT_I16,
                     COMPARE_UGT_I32, COMPARE_UGT_I64, COMPARE_UGT_F32,
                     COMPARE_UGT_F64);
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_UGE, COMPARE_UGE_I8, COMPARE_UGE_I16,
                     COMPARE_UGE_I32, COMPARE_UGE_I64, COMPARE_UGE_F32,
                     COMPARE_UGE_F64);

// ============================================================================
// OPCODE_MUL_ADD (fused multiply-add)
// ============================================================================
struct MUL_ADD_F32
    : Sequence<MUL_ADD_F32, I<OPCODE_MUL_ADD, F32Op, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 * src2 + src3
    // ARM64: fmadd dest, src1, src2, src3
    SReg s1 = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    SReg s2 = i.src2.is_constant ? e.s1 : SReg(i.src2.reg().getIdx());
    SReg s3 = i.src3.is_constant ? e.s2 : SReg(i.src3.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    if (i.src2.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src2.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s1, e.w0);
    }
    if (i.src3.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src3.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s2, e.w0);
    }
    EmitFmaWithPpcNan_F32(e, i.dest, s1, s2, s3, /*is_sub=*/false,
                          i.instr->flags & ARITHMETIC_NEGATE_RESULT);
  }
};
struct MUL_ADD_F64
    : Sequence<MUL_ADD_F64, I<OPCODE_MUL_ADD, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    DReg s3 = i.src3.is_constant ? e.d2 : DReg(i.src3.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    if (i.src3.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src3.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d2, e.x0);
    }
    EmitFmaWithPpcNan_F64(e, i.dest, s1, s2, s3, /*is_sub=*/false,
                          i.instr->flags & ARITHMETIC_NEGATE_RESULT);
  }
};
struct MUL_ADD_V128
    : Sequence<MUL_ADD_V128,
               I<OPCODE_MUL_ADD, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = s1*s2 + s3 with VMX denormal flushing and PPC NaN propagation.
    EmitWithVmxDenormalFlushFpcr(e, [&] {
      const int d = i.dest.reg().getIdx();

      // dest is free until the result is stored, so it lends the fixup the
      // scratch register v0-v3 cannot cover.
      PrepareVmxFmaSources(e, i.src1, i.src2, i.src3, d);

      e.mov(VReg(2).b16, VReg(3).b16);
      e.fmla(VReg(2).s4, VReg(0).s4, VReg(1).s4);

      if (i.instr->flags & ARITHMETIC_NEGATE_RESULT) {
        e.fneg(VReg(2).s4, VReg(2).s4);
      }
      FixupVmxNan_V128_Fma(e, d);

      // Flush output denormals.
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, 2, 0, 1);
      }
      e.mov(VReg(d).b16, VReg(2).b16);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_ADD, MUL_ADD_F32, MUL_ADD_F64, MUL_ADD_V128);

// ============================================================================
// OPCODE_MUL_SUB (fused multiply-subtract)
// ============================================================================
struct MUL_SUB_F64
    : Sequence<MUL_SUB_F64, I<OPCODE_MUL_SUB, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 * src2 - src3
    // ARM64 fnmsub(d,n,m,a) = -a + n*m = n*m - a
    DReg s1 = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    DReg s2 = i.src2.is_constant ? e.d1 : DReg(i.src2.reg().getIdx());
    DReg s3 = i.src3.is_constant ? e.d2 : DReg(i.src3.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    if (i.src2.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src2.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d1, e.x0);
    }
    if (i.src3.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src3.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d2, e.x0);
    }
    EmitFmaWithPpcNan_F64(e, i.dest, s1, s2, s3, /*is_sub=*/true,
                          i.instr->flags & ARITHMETIC_NEGATE_RESULT);
  }
};
struct MUL_SUB_V128
    : Sequence<MUL_SUB_V128,
               I<OPCODE_MUL_SUB, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = s1*s2 - s3 with VMX denormal flushing and PPC NaN propagation.
    // Same as MUL_ADD but negate s3 before the fmla. v3 keeps the un-negated
    // s3, which is the operand the fixup propagates.
    EmitWithVmxDenormalFlushFpcr(e, [&] {
      const int d = i.dest.reg().getIdx();

      PrepareVmxFmaSources(e, i.src1, i.src2, i.src3, d);

      e.mov(VReg(2).b16, VReg(3).b16);
      e.fneg(VReg(2).s4, VReg(2).s4);
      e.fmla(VReg(2).s4, VReg(0).s4, VReg(1).s4);

      // Negate before the fixup, never after: the fixup overwrites every NaN
      // lane, and hardware leaves a NaN result's sign alone.
      if (i.instr->flags & ARITHMETIC_NEGATE_RESULT) {
        e.fneg(VReg(2).s4, VReg(2).s4);
      }

      FixupVmxNan_V128_Fma(e, d);

      // Flush output denormals.
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, 2, 0, 1);
      }
      e.mov(VReg(d).b16, VReg(2).b16);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_SUB, MUL_SUB_F64, MUL_SUB_V128);

// ============================================================================
// OPCODE_POW2
// ============================================================================
static void LoadEstConst(A64Emitter& e, int vreg_idx, int const_idx) {
  e.ldr(QReg(vreg_idx),
        ptr(e.GetBackendCtxReg(),
            static_cast<int32_t>(offsetof(A64BackendContext, est_consts) +
                                 const_idx * 16)));
}

// Horner in v2/v3 with the variable in v1, alternating so each fmla has its
// accumulator in the other register. Returns the register holding the result.
static int EmitEstPoly(A64Emitter& e, int first, int count) {
  int acc = 2;
  LoadEstConst(e, acc, first + count - 1);
  for (int k = count - 2; k >= 0; k--) {
    const int next = acc ^ 1;  // 2 <-> 3
    LoadEstConst(e, next, first + k);
    e.fmla(VReg(next).s4, VReg(acc).s4, VReg(1).s4);
    acc = next;
  }
  return acc;
}

// Snap onto the guest's 2^-11 estimate grid. Not cosmetic: it is what keeps
// 2^0 == 1.0 and log2(2^n) == n exact once the math is a polynomial.
static void EmitEstGridSnap(A64Emitter& e, int r, int tmp) {
  LoadEstConst(e, tmp, kEstScale);
  e.fmul(VReg(r).s4, VReg(r).s4, VReg(tmp).s4);
  e.frintn(VReg(r).s4, VReg(r).s4);
  LoadEstConst(e, tmp, kEstUnscale);
  e.fmul(VReg(r).s4, VReg(r).s4, VReg(tmp).s4);
}

// NaN in, quieted NaN out. Consumes the source and writes the destination.
static void EmitEstQuietNan(A64Emitter& e, int r, int s, int d) {
  e.fcmeq(VReg(0).s4, VReg(s).s4, VReg(s).s4);  // all-ones where NOT NaN
  LoadEstConst(e, 1, kEstQuietBit);
  e.orr(VReg(1).b16, VReg(s).b16, VReg(1).b16);
  e.bif(VReg(r).b16, VReg(1).b16, VReg(0).b16);
  e.mov(VReg(d).b16, VReg(r).b16);
}

struct POW2_F32 : Sequence<POW2_F32, I<OPCODE_POW2, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("POW2_F32 should not be emitted");
  }
};
struct POW2_F64 : Sequence<POW2_F64, I<OPCODE_POW2, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("POW2_F64 should not be emitted");
  }
};
struct POW2_V128 : Sequence<POW2_V128, I<OPCODE_POW2, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      const int d = i.dest.reg().getIdx();
      int s = SrcVReg(e, i.src1, 0);
      if (s < 4) {
        // A constant landed in the scratch bank, which the math below needs.
        e.mov(VReg(d).b16, VReg(s).b16);
        s = d;
      }
      // 2^x = 2^floor(x) * 2^frac(x). Splitting on floor rather than nearest
      // puts the second factor in [1,2), so it lands on the grid directly.
      e.frintm(VReg(0).s4, VReg(s).s4);
      e.fsub(VReg(1).s4, VReg(s).s4, VReg(0).s4);
      e.fcvtzs(VReg(0).s4, VReg(0).s4);
      e.shl(VReg(0).s4, VReg(0).s4, 23);
      LoadEstConst(e, 2, kEstOne);
      e.add(VReg(0).s4, VReg(0).s4, VReg(2).s4);  // (127 + n) << 23
      const int r = EmitEstPoly(e, kEstExp2Poly, 6);
      EmitEstGridSnap(e, r, r ^ 1);
      e.fmul(VReg(r).s4, VReg(r).s4, VReg(0).s4);

      // Out-of-range and non-finite inputs never reached the guest's
      // estimator. Denormals need no case of their own: floor is 0 either way,
      // so the grid snap returns exactly 1.0.
      LoadEstConst(e, 0, kEstExp2Max);
      e.fcmge(VReg(0).s4, VReg(s).s4, VReg(0).s4);
      LoadEstConst(e, 1, kEstPosInf);
      e.bit(VReg(r).b16, VReg(1).b16, VReg(0).b16);
      LoadEstConst(e, 0, kEstExp2Min);
      e.fcmgt(VReg(0).s4, VReg(0).s4, VReg(s).s4);
      e.movi(VReg(1).s4, 0);
      e.bit(VReg(r).b16, VReg(1).b16, VReg(0).b16);
      EmitEstQuietNan(e, r, s, d);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_POW2, POW2_F32, POW2_F64, POW2_V128);

// ============================================================================
// OPCODE_LOG2
// ============================================================================
struct LOG2_F32 : Sequence<LOG2_F32, I<OPCODE_LOG2, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("LOG2_F32 should not be emitted");
  }
};
struct LOG2_F64 : Sequence<LOG2_F64, I<OPCODE_LOG2, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always("LOG2_F64 should not be emitted");
  }
};
struct LOG2_V128 : Sequence<LOG2_V128, I<OPCODE_LOG2, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      const int d = i.dest.reg().getIdx();
      int s = SrcVReg(e, i.src1, 0);
      if (s < 4) {
        e.mov(VReg(d).b16, VReg(s).b16);
        s = d;
      }
      // log2(x) = exponent(x) + log2(mantissa(x)). Negatives are masked off
      // below, so the sign bit can ride along in the exponent shift.
      e.ushr(VReg(0).s4, VReg(s).s4, 23);
      LoadEstConst(e, 2, kEstInt127);
      e.sub(VReg(0).s4, VReg(0).s4, VReg(2).s4);
      e.scvtf(VReg(0).s4, VReg(0).s4);
      LoadEstConst(e, 1, kEstMantissaMask);
      e.and_(VReg(1).b16, VReg(s).b16, VReg(1).b16);
      LoadEstConst(e, 2, kEstOne);
      e.orr(VReg(1).b16, VReg(1).b16, VReg(2).b16);  // mantissa in [1,2)
      e.fsub(VReg(1).s4, VReg(1).s4, VReg(2).s4);
      const int r = EmitEstPoly(e, kEstLog2Poly, 7);
      e.fadd(VReg(r).s4, VReg(r).s4, VReg(0).s4);
      EmitEstGridSnap(e, r, r ^ 1);

      LoadEstConst(e, 1, kEstPosInf);
      e.fcmeq(VReg(0).s4, VReg(s).s4, VReg(1).s4);
      e.bit(VReg(r).b16, VReg(1).b16, VReg(0).b16);
      e.sshr(VReg(0).s4, VReg(s).s4, 31);
      LoadEstConst(e, 1, kEstQNaN);
      e.bit(VReg(r).b16, VReg(1).b16, VReg(0).b16);
      // Zero and denormal both reach the estimator as zero, so both give -inf.
      LoadEstConst(e, 0, kEstPosInf);
      e.and_(VReg(0).b16, VReg(s).b16, VReg(0).b16);
      e.cmeq(VReg(0).s4, VReg(0).s4, 0);
      LoadEstConst(e, 1, kEstNegInf);
      e.bit(VReg(r).b16, VReg(1).b16, VReg(0).b16);
      EmitEstQuietNan(e, r, s, d);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOG2, LOG2_F32, LOG2_F64, LOG2_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_3
// ============================================================================
// The guest returns QNaN when the double sum overflows on the narrowing to
// float32, but preserves an infinity that came from an infinite input. Both
// leave an infinite result; only the overflow leaves a finite sum behind, so
// the second test only has to run once the first one says the result is
// infinite. Takes the result in s0 and the sum in d1; clobbers v2, v3 and x17.
static void EmitDotProductResult(A64Emitter& e, int dest_idx) {
  auto& done = e.NewCachedLabel();
  e.fabs(SReg(2), SReg(0));
  e.mov(e.w17, 0x7F800000u);  // +inf
  e.fmov(SReg(3), e.w17);
  e.fcmp(SReg(2), SReg(3));
  e.b(Xbyak_aarch64::NE, done);
  e.fabs(DReg(2), DReg(1));
  e.mov(e.x17, 0x7FF0000000000000ULL);  // +inf
  e.fmov(DReg(3), e.x17);
  e.fcmp(DReg(2), DReg(3));
  e.b(Xbyak_aarch64::EQ, done);
  e.mov(e.w17, 0x7FC00000u);  // QNaN
  e.fmov(SReg(0), e.w17);
  e.L(done);
  // Splat result to all 4 lanes.
  e.dup(VReg(dest_idx).s4, VReg(0).s4[0]);
}

struct DOT_PRODUCT_3_V128
    : Sequence<DOT_PRODUCT_3_V128,
               I<OPCODE_DOT_PRODUCT_3, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxDenormalFlushFpcr(e, [&] {
      // Inline NEON: multiply in double precision, sum 3 elements, convert
      // back. Uses v0-v3 as scratch.
      int s1 = SrcVReg(e, i.src1, 0);
      int s2 = SrcVReg(e, i.src2, 1);
      int d = i.dest.reg().getIdx();
      // High lanes first: the low widen overwrites a constant source in v0/v1.
      e.fcvtl2(VReg(2).d2, VReg(s1).s4);           // v2 = {s1[2], s1[3]} as f64
      e.fcvtl2(VReg(3).d2, VReg(s2).s4);           // v3 = {s2[2], s2[3]} as f64
      e.fmul(VReg(2).d2, VReg(2).d2, VReg(3).d2);  // v2 = {a2*b2, a3*b3}
      // Widen low 2 floats of each source to double.
      e.fcvtl(VReg(0).d2, VReg(s1).s2);            // v0 = {s1[0], s1[1]} as f64
      e.fcvtl(VReg(1).d2, VReg(s2).s2);            // v1 = {s2[0], s2[1]} as f64
      e.fmul(VReg(0).d2, VReg(0).d2, VReg(1).d2);  // v0 = {a0*b0, a1*b1}
      // Sum: d0 = v0[0] + v0[1] + v2[0] (skip v2[1] = element 3).
      e.faddp(DReg(1), VReg(0).d2);
      e.fadd(DReg(1), DReg(1), DReg(2));
      // Convert back to float.
      e.fcvt(SReg(0), DReg(1));
      EmitDotProductResult(e, d);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_3, DOT_PRODUCT_3_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_4
// ============================================================================
struct DOT_PRODUCT_4_V128
    : Sequence<DOT_PRODUCT_4_V128,
               I<OPCODE_DOT_PRODUCT_4, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxDenormalFlushFpcr(e, [&] {
      // Inline NEON: multiply in double precision, sum all 4 elements.
      int s1 = SrcVReg(e, i.src1, 0);
      int s2 = SrcVReg(e, i.src2, 1);
      int d = i.dest.reg().getIdx();
      // High lanes first: the low widen overwrites a constant source in v0/v1.
      e.fcvtl2(VReg(2).d2, VReg(s1).s4);
      e.fcvtl2(VReg(3).d2, VReg(s2).s4);
      e.fmul(VReg(2).d2, VReg(2).d2, VReg(3).d2);
      // Widen low 2 floats to double, multiply.
      e.fcvtl(VReg(0).d2, VReg(s1).s2);
      e.fcvtl(VReg(1).d2, VReg(s2).s2);
      e.fmul(VReg(0).d2, VReg(0).d2, VReg(1).d2);
      // Sum all 4 products: v0 = {a0*b0+a2*b2, a1*b1+a3*b3}
      e.fadd(VReg(0).d2, VReg(0).d2, VReg(2).d2);
      e.faddp(DReg(1), VReg(0).d2);
      // Convert back to float.
      e.fcvt(SReg(0), DReg(1));
      EmitDotProductResult(e, d);
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_4, DOT_PRODUCT_4_V128);

// ============================================================================
// OPCODE_SET_ROUNDING_MODE
// ============================================================================
// PPC rounding mode (input bits 0-2) to ARM64 FPCR value table.
// Bits 0-1: PPC RN (rounding mode), Bit 2: PPC NI (non-IEEE / flush-to-zero).
//   PPC RN=0 (nearest) -> ARM64 RMode=00, PPC RN=1 (toward zero) -> RMode=11,
//   PPC RN=2 (toward +inf) -> RMode=01, PPC RN=3 (toward -inf) -> RMode=10.
// ARM64 FPCR RMode is bits 23:22, FZ is bit 24.
// Index 0-3: NI=0 (IEEE), Index 4-7: NI=1 (non-IEEE, FZ set).
static constexpr uint32_t fpcr_table[8] = {
    (0b00 << 22),              // PPC 0: nearest, IEEE
    (0b11 << 22),              // PPC 1: toward zero, IEEE
    (0b01 << 22),              // PPC 2: toward +inf, IEEE
    (0b10 << 22),              // PPC 3: toward -inf, IEEE
    (0b00 << 22) | (1 << 24),  // PPC 4: nearest, flush-to-zero
    (0b11 << 22) | (1 << 24),  // PPC 5: toward zero, flush-to-zero
    (0b01 << 22) | (1 << 24),  // PPC 6: toward +inf, flush-to-zero
    (0b10 << 22) | (1 << 24),  // PPC 7: toward -inf, flush-to-zero
};
struct SET_ROUNDING_MODE
    : Sequence<SET_ROUNDING_MODE, I<OPCODE_SET_ROUNDING_MODE, VoidOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Input is PPC FPSCR bits (already masked to 0-7 by the frontend).
    // We set FPCR RMode + FZ bits and cache the value in the backend context.
    auto bctx = e.GetBackendCtxReg();

    if (i.src1.is_constant) {
      uint32_t fpcr_val = fpcr_table[i.src1.constant() & 7];
      e.mov(e.x0, static_cast<uint64_t>(fpcr_val));
      e.msr(3, 3, 4, 4, 0, e.x0);  // msr FPCR, x0
      // Cache in backend context.
      e.str(e.w0, ptr(bctx, static_cast<uint32_t>(
                                offsetof(A64BackendContext, fpcr_fpu))));
      // Update NonIEEE flag.
      e.ldr(
          e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
      if (i.src1.constant() & 4) {
        e.orr(e.w0, e.w0, 1u << kA64BackendNonIEEEMode);
      } else {
        // Clear bit kA64BackendNonIEEEMode using BIC (avoids bitmask encoding).
        e.mov(e.w1, 1u << kA64BackendNonIEEEMode);
        e.bic(e.w0, e.w0, e.w1);
      }
      e.str(
          e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    } else {
      // Dynamic: look up FPCR value from table.
      e.mov(e.x0, reinterpret_cast<uint64_t>(fpcr_table));
      e.and_(e.w1, i.src1, 7);
      e.ldr(e.w0, Xbyak_aarch64::ptr(e.x0, e.x1, Xbyak_aarch64::LSL, 2));
      // Write FPCR.
      e.msr(3, 3, 4, 4, 0, e.x0);
      // Cache in backend context.
      e.str(e.w0, ptr(bctx, static_cast<uint32_t>(
                                offsetof(A64BackendContext, fpcr_fpu))));
      // Update NonIEEE flag based on bit 2 of input.
      e.ldr(
          e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
      // Clear bit kA64BackendNonIEEEMode using BIC (avoids bitmask encoding).
      e.mov(e.w1, 1u << kA64BackendNonIEEEMode);
      e.bic(e.w0, e.w0, e.w1);
      // Conditionally set it back if input bit 2 is set.
      e.tst(i.src1, 4);
      e.csel(e.w1, e.w1, e.wzr, Xbyak_aarch64::Cond::NE);
      e.orr(e.w0, e.w0, e.w1);
      e.str(
          e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    }
    e.ChangeFpcrMode(FPCRMode::Fpu, /*already_set=*/true);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SET_ROUNDING_MODE, SET_ROUNDING_MODE);

// ============================================================================
// OPCODE_RSQRT
// ============================================================================
struct RSQRT_F32 : Sequence<RSQRT_F32, I<OPCODE_RSQRT, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    SReg src = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    e.fsqrt(e.s1, src);
    e.mov(e.w0, static_cast<uint64_t>(0x3F800000u));
    e.fmov(e.s2, e.w0);
    e.fdiv(i.dest, e.s2, e.s1);
  }
};
struct RSQRT_F64 : Sequence<RSQRT_F64, I<OPCODE_RSQRT, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // PPC frsqrte uses a specific lookup table, not a high-precision estimate.
    DReg src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    e.fmov(e.x0, src);
    e.mov(e.x9, reinterpret_cast<uint64_t>(e.backend()->frsqrte_helper()));
    e.blr(e.x9);
    e.fmov(i.dest, e.x0);
  }
};
struct RSQRT_V128 : Sequence<RSQRT_V128, I<OPCODE_RSQRT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const int src_idx = SrcVReg(e, i.src1, 0);
    // The vast majority of inputs to vrsqrte come from vmsum3 or vmsum4 as part
    // of a vector normalization sequence, where every lane holds the same value
    // and one estimate covers all four.
    if (i.src1.value && i.src1.value->AllFloatVectorLanesSameValue()) {
      e.umov(e.w0, VReg(src_idx).s4[0]);
      e.mov(e.x9,
            reinterpret_cast<uint64_t>(e.backend()->vrsqrtefp_scalar_helper()));
      e.blr(e.x9);
      e.dup(VReg(i.dest.reg().getIdx()).s4, e.w0);
      return;
    }
    if (src_idx != 0) {
      e.mov(VReg(0).b16, VReg(src_idx).b16);
    }
    e.mov(e.x9,
          reinterpret_cast<uint64_t>(e.backend()->vrsqrtefp_vector_helper()));
    e.blr(e.x9);
    e.mov(VReg(i.dest.reg().getIdx()).b16, VReg(0).b16);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RSQRT, RSQRT_F32, RSQRT_F64, RSQRT_V128);

// ============================================================================
// OPCODE_RECIP
// ============================================================================
struct RECIP_F32 : Sequence<RECIP_F32, I<OPCODE_RECIP, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    SReg src = i.src1.is_constant ? e.s0 : SReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        float f;
        uint32_t u;
      } c;
      c.f = i.src1.constant();
      e.mov(e.w0, static_cast<uint64_t>(c.u));
      e.fmov(e.s0, e.w0);
    }
    e.fmov(e.s2, 1.0f);
    e.fdiv(i.dest, e.s2, src);
  }
};
struct RECIP_F64 : Sequence<RECIP_F64, I<OPCODE_RECIP, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    DReg src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    e.fmov(e.d2, 1.0);
    e.fdiv(i.dest, e.d2, src);
  }
};
struct RECIP_V128 : Sequence<RECIP_V128, I<OPCODE_RECIP, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitWithVmxFpcr(e, [&] {
      if (i.src1.is_constant) {
        LoadV128Const(e, 1, i.src1.constant());
      } else {
        e.mov(VReg(1).b16, VReg(i.src1.reg().getIdx()).b16);
      }
      // Flush input denormals.
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, 1);  // scratch v2, v3
      }
      auto d = VReg(i.dest.reg().getIdx()).s4;
      // Load 1.0f vector.
      e.fmov(VReg(0).s4, 1.0f);
      e.fdiv(d, VReg(0).s4, VReg(1).s4);
      // Flush output denormals.
      if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
        FlushDenormals_V128(e, i.dest.reg().getIdx(), 0, 1);
      }
    });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RECIP, RECIP_F32, RECIP_F64, RECIP_V128);

// ============================================================================
// OPCODE_TO_SINGLE
// ============================================================================
struct TOSINGLE : Sequence<TOSINGLE, I<OPCODE_TO_SINGLE, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.ChangeFpcrMode(FPCRMode::Fpu);
    DReg src = i.src1.is_constant ? e.d0 : DReg(i.src1.reg().getIdx());
    if (i.src1.is_constant) {
      union {
        double d;
        uint64_t u;
      } c;
      c.d = i.src1.constant();
      e.mov(e.x0, c.u);
      e.fmov(e.d0, e.x0);
    }
    // Round double->single->double.
    // NaN sign is already correct from upstream arithmetic (EmitFmaWithPpcNan
    // etc.) or fneg.  fcvt with DN=0 preserves NaN sign, so no fixup needed.
    e.fcvt(e.s0, src);
    e.fcvt(i.dest, e.s0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TO_SINGLE, TOSINGLE);

// ============================================================================
// OPCODE_SET_NJM
// ============================================================================
struct SET_NJM : Sequence<SET_NJM, I<OPCODE_SET_NJM, VoidOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // NJM (Non-Java Mode) is a VMX/AltiVec feature (VSCR bit 16) that
    // controls flush-to-zero for vector operations.  It does NOT affect
    // scalar FPU behaviour.  On ARM64 this maps to FPCR.FZ (bit 24) in
    // the cached fpcr_vmx value, which EmitWithVmxFpcr loads before
    // each vector FP operation.
    auto bctx = e.GetBackendCtxReg();

    // Toggle FZ bit in cached fpcr_vmx.
    e.ldr(e.w0, ptr(bctx, static_cast<uint32_t>(
                              offsetof(A64BackendContext, fpcr_vmx))));
    if (i.src1.is_constant) {
      if (i.src1.constant()) {
        e.orr(e.w0, e.w0, (1u << 24));  // NJM=1: set FZ
      } else {
        e.and_(e.w0, e.w0, ~(1u << 24));  // NJM=0: clear FZ
      }
    } else {
      auto& set_fz = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.cbnz(i.src1, set_fz);
      e.and_(e.w0, e.w0, ~(1u << 24));  // NJM=0: clear FZ
      e.b(done);
      e.L(set_fz);
      e.orr(e.w0, e.w0, (1u << 24));  // NJM=1: set FZ
      e.L(done);
    }
    e.str(e.w0, ptr(bctx, static_cast<uint32_t>(
                              offsetof(A64BackendContext, fpcr_vmx))));

    // Update kA64BackendNJMOn flag.
    e.ldr(e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    if (i.src1.is_constant) {
      if (i.src1.constant()) {
        e.orr(e.w0, e.w0, 1u << kA64BackendNJMOn);
      } else {
        e.mov(e.w1, 1u << kA64BackendNJMOn);
        e.bic(e.w0, e.w0, e.w1);
      }
    } else {
      e.mov(e.w1, 1u << kA64BackendNJMOn);
      e.bic(e.w0, e.w0, e.w1);
      e.tst(i.src1, 0xFF);
      e.csel(e.w1, e.w1, e.wzr, Xbyak_aarch64::Cond::NE);
      e.orr(e.w0, e.w0, e.w1);
    }
    e.str(e.w0,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));

    e.ForgetFpcrMode();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SET_NJM, SET_NJM);

// Force-link the split sequence files so their static initializers run.
extern volatile int anchor_control;
static int anchor_control_dest = anchor_control;

extern volatile int anchor_memory;
static int anchor_memory_dest = anchor_memory;

extern volatile int anchor_vector;
static int anchor_vector_dest = anchor_vector;

// ============================================================================
// SelectSequence — dispatch an instruction to its sequence handler
// ============================================================================
static const char* KeyTypeName(uint32_t type) {
  switch (type) {
    case KEY_TYPE_X:
      return "-";
    case KEY_TYPE_L:
      return "label";
    case KEY_TYPE_O:
      return "offset";
    case KEY_TYPE_S:
      return "symbol";
    case KEY_TYPE_V_I8:
      return "i8";
    case KEY_TYPE_V_I16:
      return "i16";
    case KEY_TYPE_V_I32:
      return "i32";
    case KEY_TYPE_V_I64:
      return "i64";
    case KEY_TYPE_V_F32:
      return "f32";
    case KEY_TYPE_V_F64:
      return "f64";
    case KEY_TYPE_V_V128:
      return "v128";
    default:
      return "?";
  }
}

std::string FormatSequenceKey(uint64_t key) {
  const InstrKey decoded(hir::SequenceSampleBackendKey(key));
  std::string result = GetOpcodeName(static_cast<Opcode>(decoded.opcode));
  // Space separated so the rendered label stays free of commas and the CSV
  // column can be split naively.
  result += ' ';
  result += KeyTypeName(decoded.dest);
  const uint32_t srcs[3] = {decoded.src1, decoded.src2, decoded.src3};
  for (uint32_t n = 0; n < 3; ++n) {
    result += ' ';
    if (hir::SequenceSampleSrcIsConstant(key, n)) {
      result += 'c';
    }
    result += KeyTypeName(srcs[n]);
  }
  const uint16_t flags = hir::SequenceSampleFlags(key);
  if (flags) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), " f%X",
                  static_cast<unsigned int>(flags));
    result += buffer;
  }
  return result;
}

bool SelectSequence(A64Emitter* e, const hir::Instr* i,
                    const hir::Instr** new_tail) {
  const InstrKey key(i);
  auto& sequence_table = SequenceTable();
  auto it = sequence_table.find(key);
  if (it != sequence_table.end()) {
    const size_t size_before = e->getSize();
    if (it->second(*e, i, InstrKeyValue(key))) {
      // Skip the bookkeeping opcodes: they carry no guest work, and
      // SOURCE_OFFSET would otherwise charge the coverage counter's own
      // code to the instruction it is counting.
      const Opcode num = i->GetOpcodeNum();
      if (num != OPCODE_SOURCE_OFFSET && num != OPCODE_COMMENT &&
          num != OPCODE_NOP) {
        e->RecordSequenceSample(
            i, key.value, static_cast<uint32_t>(e->getSize() - size_before));
      }
      *new_tail = i->next;
      return true;
    }
  }
  XELOGE("A64: No sequence match for opcode: {} ({})",
         hir::GetOpcodeName(i->GetOpcodeInfo()),
         static_cast<int>(i->GetOpcodeInfo()->num));
  fprintf(stderr, "A64: No sequence match for opcode: %s (%d)\n",
          hir::GetOpcodeName(i->GetOpcodeInfo()),
          static_cast<int>(i->GetOpcodeInfo()->num));
  return false;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
