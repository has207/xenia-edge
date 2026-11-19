/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// Scalar ops that a backend may run in whatever FPCR mode a VMX op left,
// rather than switching back to the scalar one, must give what they give
// after no VMX op at all: for every FPSCR rounding mode, with VSCR NJ
// on and off, and for operands at every edge FZ or rounding could move.

namespace {

// What runs before the op under test: nothing, a VMX op in the same block,
// or a join of a path through a VMX op (taken when r10 is nonzero) and one
// around it, where a backend cannot know the mode.
enum class Before { kNothing, kVmx, kJoin };

std::unique_ptr<TestFunction> MakeTest(Before before, bool njm,
                                       std::function<void(HIRBuilder&)> op) {
  return std::make_unique<TestFunction>([=](HIRBuilder& b) {
    b.SetNJM(b.LoadConstantInt8(njm ? 1 : 0));
    auto vmx_op = [&]() {
      StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    };
    if (before == Before::kVmx) {
      vmx_op();
    } else if (before == Before::kJoin) {
      auto join = b.NewLabel();
      b.BranchFalse(b.CompareNE(LoadGPR(b, 10), b.LoadZeroInt64()), join);
      vmx_op();
      b.MarkLabel(join);
    }
    op(b);
    b.Return();
  });
}

struct Operands {
  uint64_t a;
  uint64_t b;
};

// Runs `op` on every operand pair in every mode, after each kind of
// preceding code, and requires r3-r5 and f3 to match the run after nothing.
// One test function at a time: each maps guest memory at the same address.
void RequireSameInAnyMode(std::function<void(HIRBuilder&)> op,
                          const std::vector<Operands>& operands) {
  // Runs every mode and operand pair, in that order.
  auto run_all =
      [&](Before before, bool njm, uint64_t through_vmx,
          const std::function<void(size_t, const uint64_t*)>& check) {
        auto test = MakeTest(before, njm, op);
        size_t n = 0;
        for (unsigned int mode = 0; mode < 4; ++mode) {
          for (const Operands& in : operands) {
            test->Run(
                [&](PPCContext* ctx) {
                  test->processors[0]->backend()->SetGuestRoundingMode(ctx,
                                                                       mode);
                  ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
                  ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
                  ctx->r[6] = in.a;
                  ctx->r[7] = in.b;
                  std::memcpy(&ctx->f[6], &in.a, sizeof(in.a));
                  std::memcpy(&ctx->f[7], &in.b, sizeof(in.b));
                  ctx->r[10] = through_vmx;
                },
                [&](PPCContext* ctx) {
                  uint64_t result[4] = {ctx->r[3], ctx->r[4], ctx->r[5]};
                  std::memcpy(&result[3], &ctx->f[3], sizeof(result[3]));
                  INFO("njm " << njm << " mode " << mode << " a 0x" << std::hex
                              << in.a << " b 0x" << in.b << " through_vmx "
                              << through_vmx);
                  check(n++, result);
                });
          }
        }
      };
  for (bool njm : {true, false}) {
    std::vector<std::array<uint64_t, 4>> expected;
    run_all(Before::kNothing, njm, 0, [&](size_t, const uint64_t* result) {
      expected.push_back({result[0], result[1], result[2], result[3]});
    });
    auto require_expected = [&](size_t n, const uint64_t* result) {
      for (int k = 0; k < 4; ++k) {
        REQUIRE(result[k] == expected[n][k]);
      }
    };
    run_all(Before::kVmx, njm, 0, require_expected);
    run_all(Before::kJoin, njm, 0, require_expected);
    run_all(Before::kJoin, njm, 1, require_expected);
  }
}

// Singles at the edges FZ can move: zeros, denormals, the smallest normals.
const uint32_t kSingles[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF,
    0x00800000, 0x80800000, 0x00800001, 0x3F800000, 0x7F7FFFFF,
    0x7F800000, 0xFF800000, 0x7FC00001, 0x7F800001, 0xFFA00000,
};

// Doubles for the conversions to single: exact and inexact, a tie, values
// that round into or out of the single normal range, double denormals and
// NaNs whose payload does and does not fit a single.
const uint64_t kDoubles[] = {
    0x0000000000000000ull,  // +0
    0x8000000000000000ull,  // -0
    0x0000000000000001ull,  // double denormal
    0x800FFFFFFFFFFFFFull,  // -double denormal
    0x0010000000000000ull,  // smallest normal double
    0x3730000000000000ull,  // 2^-140, a single denormal
    0x3690000000000000ull,  // 2^-150, half the smallest single denormal
    0x3810000000000000ull,  // 2^-126, the smallest normal single
    0x380FFFFFF0000000ull,  // just below it, rounds up to it
    0xB80FFFFFE0000000ull,  // -largest single denormal
    0x3FF0000000000000ull,  // 1.0
    0x3FF0000004000000ull,  // 1 + 2^-30, inexact
    0x3FF0000010000000ull,  // 1 + 2^-24, a tie
    0xBFF8000000000000ull,  // -1.5
    0x47EFFFFFE0000000ull,  // largest single
    0x47EFFFFFF0000000ull,  // rounds to infinity or the largest single
    0x47F0000000000000ull,  // 2^128
    0x7FF0000000000000ull,  // +inf
    0xFFF0000000000000ull,  // -inf
    0x7FF8000000000001ull,  // QNaN, payload lost in a single
    0x7FF8000020000000ull,  // QNaN, payload kept
    0xFFF0000000000001ull,  // SNaN
    0x7FF4000000000000ull,  // SNaN, payload kept
};

std::vector<Operands> Singles() {
  std::vector<Operands> operands;
  for (uint32_t a : kSingles) {
    for (uint32_t b : kSingles) {
      operands.push_back({a, b});
    }
  }
  return operands;
}

std::vector<Operands> Doubles() {
  std::vector<Operands> operands;
  for (uint64_t a : kDoubles) {
    operands.push_back({a, 0});
  }
  return operands;
}

Value* Single(HIRBuilder& b, int gpr) {
  return b.UnpackSingle(b.Truncate(LoadGPR(b, gpr), INT32_TYPE));
}

}  // namespace

TEST_CASE("VMX_MODE_SCALAR_UNPACK_SINGLE", "[backend]") {
  RequireSameInAnyMode([](HIRBuilder& b) { StoreFPR(b, 3, Single(b, 6)); },
                       Singles());
}

TEST_CASE("VMX_MODE_SCALAR_PACK_SINGLE", "[backend]") {
  RequireSameInAnyMode(
      [](HIRBuilder& b) {
        StoreGPR(b, 3, b.ZeroExtend(b.PackSingle(LoadFPR(b, 6)), INT64_TYPE));
      },
      Doubles());
}

TEST_CASE("VMX_MODE_SCALAR_TO_SINGLE", "[backend]") {
  RequireSameInAnyMode(
      [](HIRBuilder& b) { StoreFPR(b, 3, b.ToSingle(LoadFPR(b, 6))); },
      Doubles());
}

TEST_CASE("VMX_MODE_SCALAR_IS_NAN", "[backend]") {
  RequireSameInAnyMode(
      [](HIRBuilder& b) {
        StoreGPR(b, 3, b.ZeroExtend(b.IsNan(LoadFPR(b, 6)), INT64_TYPE));
      },
      Doubles());
}

TEST_CASE("VMX_MODE_SCALAR_TRUNCATE_TO_INTEGER", "[backend]") {
  RequireSameInAnyMode(
      [](HIRBuilder& b) {
        StoreGPR(b, 3, b.Convert(LoadFPR(b, 6), INT64_TYPE, ROUND_TO_ZERO));
        StoreGPR(
            b, 4,
            b.ZeroExtend(b.Convert(LoadFPR(b, 6), INT32_TYPE, ROUND_TO_ZERO),
                         INT64_TYPE));
      },
      Doubles());
}

// Widened singles are never denormal doubles, so a backend may compare them
// in any mode; loaded doubles may be, and must still compare exactly.
TEST_CASE("VMX_MODE_SCALAR_COMPARE", "[backend]") {
  RequireSameInAnyMode(
      [](HIRBuilder& b) {
        Value* a = Single(b, 6);
        Value* c = Single(b, 7);
        StoreGPR(b, 3, b.ZeroExtend(b.CompareSLT(a, c), INT64_TYPE));
        StoreGPR(b, 4, b.ZeroExtend(b.CompareEQ(a, c), INT64_TYPE));
        StoreGPR(b, 5, b.ZeroExtend(b.CompareULT(a, c), INT64_TYPE));
      },
      Singles());
  std::vector<Operands> doubles;
  for (uint64_t a : kDoubles) {
    for (uint64_t c : kDoubles) {
      doubles.push_back({a, c});
    }
  }
  RequireSameInAnyMode(
      [](HIRBuilder& b) {
        Value* a = LoadFPR(b, 6);
        Value* c = LoadFPR(b, 7);
        StoreGPR(b, 3, b.ZeroExtend(b.CompareSGT(a, c), INT64_TYPE));
        StoreGPR(b, 4, b.ZeroExtend(b.CompareNE(a, c), INT64_TYPE));
      },
      doubles);
}
