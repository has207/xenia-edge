/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstdint>
#include <cstring>

#include "xenia/cpu/compiler/passes/control_flow_analysis_pass.h"
#include "xenia/cpu/compiler/passes/control_flow_simplification_pass.h"
#include "xenia/cpu/compiler/passes/simplification_pass.h"

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint64_t kOne = 0x3FF0000000000000ull;
constexpr uint64_t kZero = 0x0000000000000000ull;
constexpr uint64_t kDenormal = 0x0000000000000001ull;
constexpr uint64_t kNegDenormal = 0x800FFFFFFFFFFFFFull;
constexpr uint64_t kMinNormal = 0x0010000000000000ull;
constexpr uint64_t kBelowSingle = 0x3690000000000000ull;     // 2^-150
constexpr uint64_t kSmallestSingle = 0x36A0000000000000ull;  // 2^-149
constexpr uint64_t kBelow128 = 0x47EFFFFFFFFFFFFFull;
constexpr uint64_t k128 = 0x47F0000000000000ull;      // 2^128
constexpr uint64_t kNeg1000 = 0xFE70000000000000ull;  // -2^1000
constexpr uint64_t kInf = 0x7FF0000000000000ull;
constexpr uint64_t kNaN = 0x7FF8000000000000ull;

void SetF64Bits(PPCContext* ctx, int reg, uint64_t bits) {
  std::memcpy(&ctx->f[reg], &bits, sizeof(bits));
}

Value* F64Constant(HIRBuilder& b, uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  return b.LoadConstantFloat64(d);
}

bool HasSingleRangeQuirk(HIRBuilder& b) {
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_SINGLE_RANGE_QUIRK) {
        return true;
      }
    }
  }
  return false;
}

bool SimplifiesAway(void (*emit)(HIRBuilder& b)) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  const bool folded = !HasSingleRangeQuirk(b);
  b.RemoveCurrent();
  return folded;
}

// Merges blocks first, as the translator does, which can leave a branch in the
// middle of a block where the state differs from the block's end.
bool SimplifiesAwayAfterMerging(void (*emit)(HIRBuilder& b),
                                bool* mid_block_branch) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::ControlFlowSimplificationPass cfs;
  cfs.Run(&b);
  cfa.Run(&b);
  *mid_block_branch = false;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->BranchLabel() && i != block->instr_tail) {
        *mid_block_branch = true;
      }
    }
  }
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  const bool folded = !HasSingleRangeQuirk(b);
  b.RemoveCurrent();
  return folded;
}

// Whether the ADD the emitter builds still has ARITHMETIC_SINGLE_RANGE after
// the simplification pass.
bool KeepsSingleRangeFlag(void (*emit)(HIRBuilder& b)) {
  HIRBuilder b;
  b.MakeCurrent();
  emit(b);
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::SimplificationPass pass;
  bool changed = false;
  pass.Run(&b, changed);
  bool kept = false;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_ADD &&
          (i->flags & ARITHMETIC_SINGLE_RANGE)) {
        kept = true;
      }
    }
  }
  b.RemoveCurrent();
  return kept;
}

}  // namespace

// quirk = any operand nonzero and outside [2^-149, 2^128) && every operand
// finite.
TEST_CASE("SINGLE_RANGE_QUIRK", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.SingleRangeQuirk(LoadFPR(b, 1), LoadFPR(b, 2),
                                             LoadFPR(b, 3)),
                          INT64_TYPE));
    b.Return();
  });
  struct Case {
    uint64_t a, b, c;
    uint64_t quirk;
  };
  const Case cases[] = {
      {kOne, kOne, kOne, 0},
      {kZero, kZero, kZero, 0},
      {kMinNormal, kOne, kZero, 1},
      {kBelowSingle, kOne, kOne, 1},
      {kSmallestSingle, kOne, kZero, 0},
      {kOne, kBelow128, kOne, 0},
      {kOne, kOne, k128, 1},
      {kNeg1000, kOne, kOne, 1},
      {k128, kInf, kOne, 0},
      {kOne, k128, kNaN, 0},
      {kDenormal, kOne, kOne, 1},
      {kOne, kNegDenormal, kOne, 1},
      {kOne, kOne, kDenormal, 1},
      {kDenormal, kDenormal, kDenormal, 1},
      {kDenormal, kInf, kOne, 0},
      {kOne, kDenormal, kNaN, 0},
      {kInf, kInf, kInf, 0},
  };
  for (const Case& c : cases) {
    INFO("operands 0x" << std::hex << c.a << " 0x" << c.b << " 0x" << c.c);
    test.Run(
        [&](PPCContext* ctx) {
          SetF64Bits(ctx, 1, c.a);
          SetF64Bits(ctx, 2, c.b);
          SetF64Bits(ctx, 3, c.c);
        },
        [&](PPCContext* ctx) { REQUIRE(ctx->r[3] == c.quirk); });
  }
}

// Constant operands take the backends' materialisation path.
TEST_CASE("SINGLE_RANGE_QUIRK_CONSTANT_OPERAND", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.SingleRangeQuirk(LoadFPR(b, 1), F64Constant(b, kDenormal),
                                    LoadFPR(b, 1)),
                 INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, kOne); },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 1); });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, kInf); },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
}

TEST_CASE("SINGLE_RANGE_QUIRK_FOLD", "[instr]") {
  // Widened singles and constants in the single range never set it off.
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(b.SingleRangeQuirk(single, b.Neg(single),
                                             F64Constant(b, kOne)),
                          INT64_TYPE));
    b.Return();
  }));
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    Value* single =
        b.Convert(b.Convert(LoadFPR(b, 1), FLOAT32_TYPE), FLOAT64_TYPE);
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(single, single, single), INT64_TYPE));
    b.Return();
  }));
  // A guest FPR or a constant outside the range may.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(LoadFPR(b, 1), F64Constant(b, kOne),
                                        F64Constant(b, kOne)),
                     INT64_TYPE));
    b.Return();
  }));
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.SingleRangeQuirk(single, F64Constant(b, kDenormal), single),
                 INT64_TYPE));
    b.Return();
  }));
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.SingleRangeQuirk(single, F64Constant(b, k128), single),
                     INT64_TYPE));
    b.Return();
  }));
}

// The same proof drops ARITHMETIC_SINGLE_RANGE from the operation.
TEST_CASE("SINGLE_RANGE_FLAG_FOLD", "[instr]") {
  REQUIRE_FALSE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreFPR(b, 3,
             b.Add(single, F64Constant(b, kOne), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
  REQUIRE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    StoreFPR(
        b, 3,
        b.Add(LoadFPR(b, 1), F64Constant(b, kOne), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
  REQUIRE(KeepsSingleRangeFlag([](HIRBuilder& b) {
    Value* single = b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE));
    StoreFPR(b, 3,
             b.Add(single, F64Constant(b, k128), ARITHMETIC_SINGLE_RANGE));
    b.Return();
  }));
}

// The operations' own check: the default QNaN for an operand outside the range,
// unless an operand is infinite or NaN, and the plain result otherwise.
TEST_CASE("SINGLE_RANGE_FLAG", "[instr]") {
  struct Case {
    uint64_t a, b;
    uint64_t sum, product, fused;  // fused = a * b + 1
  };
  const Case cases[] = {
      {kOne, kOne, 0x4000000000000000ull, kOne, 0x4000000000000000ull},
      {kSmallestSingle, kOne, kOne, kSmallestSingle, kOne},
      {kBelowSingle, kOne, kNaN, kNaN, kNaN},
      {kOne, k128, kNaN, kNaN, kNaN},
      {kMinNormal, kZero, kNaN, kNaN, kNaN},
      {kZero, kZero, kZero, kZero, kOne},
      {k128, kInf, kInf, kInf, kInf},
  };
  struct Op {
    void (*build)(HIRBuilder& b);
    uint64_t Case::* expected;
  };
  const Op ops[] = {
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.Add(LoadFPR(b, 1), LoadFPR(b, 2), ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::sum},
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.Mul(LoadFPR(b, 1), LoadFPR(b, 2), ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::product},
      {[](HIRBuilder& b) {
         StoreFPR(b, 3,
                  b.MulAdd(LoadFPR(b, 1), LoadFPR(b, 2), LoadFPR(b, 0), false,
                           ARITHMETIC_SINGLE_RANGE));
         b.Return();
       },
       &Case::fused},
  };
  // One function at a time: each installs the process-wide MMIO handler.
  for (const Op& op : ops) {
    TestFunction test(op.build);
    for (const Case& c : cases) {
      INFO("operands 0x" << std::hex << c.a << " 0x" << c.b);
      test.Run(
          [&](PPCContext* ctx) {
            SetF64Bits(ctx, 0, kOne);
            SetF64Bits(ctx, 1, c.a);
            SetF64Bits(ctx, 2, c.b);
          },
          [&](PPCContext* ctx) {
            uint64_t bits;
            std::memcpy(&bits, &ctx->f[3], sizeof(bits));
            REQUIRE(bits == c.*op.expected);
          });
    }
  }
}

// The proof follows a widened single through a guest FPR across blocks.
TEST_CASE("SINGLE_RANGE_QUIRK_FOLD_ACROSS_BLOCKS", "[instr]") {
  REQUIRE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto label = b.NewLabel();
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
    StoreFPR(b, 2, LoadFPR(b, 1));
    b.MarkLabel(label);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Not when one path stores something unproven.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto label = b.NewLabel();
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
    StoreFPR(b, 1, LoadFPR(b, 2));
    b.MarkLabel(label);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Nor across a call, which may write any FPR.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    b.Call(nullptr);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    b.Return();
  }));
  // Nor when the branch leaves mid-block, before a later store proves it.
  bool mid_block_branch = false;
  REQUIRE_FALSE(SimplifiesAwayAfterMerging(
      [](HIRBuilder& b) {
        StoreFPR(b, 1, LoadFPR(b, 2));
        auto label = b.NewLabel();
        b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), label);
        StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
        b.MarkLabel(label);
        Value* f1 = LoadFPR(b, 1);
        StoreGPR(b, 3,
                 b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
        b.Return();
      },
      &mid_block_branch));
  REQUIRE(mid_block_branch);
  // Nor around a loop whose body stores something unproven.
  REQUIRE_FALSE(SimplifiesAway([](HIRBuilder& b) {
    StoreFPR(b, 1, b.UnpackSingle(b.Truncate(LoadGPR(b, 1), INT32_TYPE)));
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    Value* f1 = LoadFPR(b, 1);
    StoreGPR(b, 3, b.ZeroExtend(b.SingleRangeQuirk(f1, f1, f1), INT64_TYPE));
    StoreFPR(b, 1, LoadFPR(b, 2));
    b.BranchTrue(b.Truncate(LoadGPR(b, 2), INT8_TYPE), loop);
    b.Return();
  }));
}
