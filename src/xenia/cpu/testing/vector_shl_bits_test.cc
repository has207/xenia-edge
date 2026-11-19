/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

// Each byte shifted by the low 3 bits of its own byte of counts, taking bits
// from the byte next to it, in guest byte order.
vec128_t ShiftBits(const vec128_t& value, const vec128_t& counts, bool left) {
  vec128_t result;
  for (int i = 0; i < 16; ++i) {
    const unsigned count = counts.u8[i ^ 3] & 7;
    const unsigned byte = value.u8[i ^ 3];
    if (left) {
      const unsigned next = i < 15 ? value.u8[(i + 1) ^ 3] : 0;
      result.u8[i ^ 3] = uint8_t(((byte << 8 | next) << count) >> 8);
    } else {
      const unsigned previous = i > 0 ? value.u8[(i - 1) ^ 3] : 0;
      result.u8[i ^ 3] = uint8_t((previous << 8 | byte) >> count);
    }
  }
  return result;
}

const vec128_t kValues[] = {
    vec128b(0x80, 0x01, 0xFF, 0x7F, 0x55, 0xAA, 0x00, 0xC3, 0x3C, 0x81, 0x18,
            0xE7, 0x96, 0x69, 0x0F, 0xF0),
    vec128b(0xFF),
    vec128b(0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA,
            0x98, 0x76, 0x54, 0x32, 0x10),
};

const vec128_t kCounts[] = {
    // The same count everywhere, only the low 3 bits read
    vec128b(0),
    vec128b(0xF8),
    vec128b(1),
    vec128b(7),
    vec128b(0x03, 0x0B, 0x13, 0x1B, 0x23, 0x2B, 0x33, 0x3B, 0x43, 0x4B, 0x53,
            0x5B, 0x63, 0x6B, 0x73, 0xFB),
    // Counts that differ
    vec128b(0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0),
    vec128b(5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4),
    vec128b(6, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    vec128b(0x7F, 0x80, 0x09, 0xF2, 0x3B, 0x44, 0x15, 0xEE, 0x07, 0x00, 0x61,
            0x92, 0xA3, 0x5C, 0x2D, 0x16),
};

// One TestFunction at a time: each installs the process-wide MMIO handler.
void TestShiftBits(bool left) {
  auto build = [left](HIRBuilder& b, Value* value, Value* counts) {
    return left ? b.VectorShlBits(value, counts)
                : b.VectorShrBits(value, counts);
  };
  {
    TestFunction test([&build](HIRBuilder& b) {
      StoreVR(b, 3, build(b, LoadVR(b, 4), LoadVR(b, 5)));
      b.Return();
    });
    for (const vec128_t& value : kValues) {
      for (const vec128_t& counts : kCounts) {
        test.Run(
            [&](PPCContext* ctx) {
              ctx->v[4] = value;
              ctx->v[5] = counts;
            },
            [&](PPCContext* ctx) {
              REQUIRE(ctx->v[3] == ShiftBits(value, counts, left));
            });
      }
    }
  }
  // The emitters' compile-time paths
  for (const vec128_t& value : kValues) {
    for (const vec128_t& counts : kCounts) {
      RequireConstantOperandsMatchRegisters(value, counts, build);
    }
  }
  // The counts in the value being shifted
  TestFunction same([&build](HIRBuilder& b) {
    Value* v = LoadVR(b, 4);
    StoreVR(b, 3, build(b, v, v));
    b.Return();
  });
  for (const vec128_t& value : kValues) {
    same.Run([&](PPCContext* ctx) { ctx->v[4] = value; },
             [&](PPCContext* ctx) {
               REQUIRE(ctx->v[3] == ShiftBits(value, value, left));
             });
  }
}

}  // namespace

TEST_CASE("VECTOR_SHL_BITS", "[instr]") { TestShiftBits(true); }

TEST_CASE("VECTOR_SHR_BITS", "[instr]") { TestShiftBits(false); }
