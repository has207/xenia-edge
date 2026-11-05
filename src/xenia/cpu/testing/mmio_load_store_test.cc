/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/memory.h"

DECLARE_bool(emit_inline_mmio_checks);

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// An MMIO handler sees a register as a byte-swapping access (lwz, stw) gives
// it. A load without the swap (lwbrx) reads the handler's value reversed and a
// store without it (stwbrx) hands the handler the register reversed. Accesses
// reach the handler through the exception handler, or through MMIOAwareLoad
// and MMIOAwareStore with emit_inline_mmio_checks, and both must agree.
// =============================================================================

namespace {

constexpr uint32_t kRegisterBase = 0x7FEA0000;
constexpr uint32_t kRegister = kRegisterBase + 0x18;
constexpr uint32_t kRegisterValue = 0x11223344;

uint32_t ReadRegister(void* ppc_context, void* callback_context,
                      uint32_t addr) {
  return kRegisterValue;
}

void WriteRegister(void* ppc_context, void* callback_context, uint32_t addr,
                   uint32_t value) {
  *static_cast<uint32_t*>(callback_context) = value;
}

class ScopedInlineMmioChecks {
 public:
  explicit ScopedInlineMmioChecks(bool enabled)
      : saved_(cvars::emit_inline_mmio_checks) {
    cvars::emit_inline_mmio_checks = enabled;
  }
  ~ScopedInlineMmioChecks() { cvars::emit_inline_mmio_checks = saved_; }

  ScopedInlineMmioChecks(const ScopedInlineMmioChecks&) = delete;
  ScopedInlineMmioChecks& operator=(const ScopedInlineMmioChecks&) = delete;

 private:
  bool saved_;
};

}  // namespace

TEST_CASE("MMIO_LOAD_I32", "[instr]") {
  const bool inline_checks = GENERATE(false, true);
  const bool swap = GENERATE(true, false);
  INFO("inline checks " << inline_checks << ", byte swap " << swap);
  ScopedInlineMmioChecks scoped_inline_checks(inline_checks);
  uint32_t written = 0;
  TestFunction test([swap](HIRBuilder& b) {
    auto value =
        b.Load(LoadGPR(b, 4), INT32_TYPE, swap ? LOAD_STORE_BYTE_SWAP : 0);
    StoreGPR(b, 3, b.ZeroExtend(value, INT64_TYPE));
    b.Return();
  });
  test.memory->AddVirtualMappedRange(kRegisterBase, 0xFFFF0000, 0xFFFF,
                                     &written, ReadRegister, WriteRegister);
  test.Run([](PPCContext* ctx) { ctx->r[4] = kRegister; },
           [swap](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) ==
                     (swap ? kRegisterValue : xe::byte_swap(kRegisterValue)));
           });
}

TEST_CASE("MMIO_STORE_I32", "[instr]") {
  const bool inline_checks = GENERATE(false, true);
  const bool swap = GENERATE(true, false);
  INFO("inline checks " << inline_checks << ", byte swap " << swap);
  ScopedInlineMmioChecks scoped_inline_checks(inline_checks);
  uint32_t written = 0;
  TestFunction test([swap](HIRBuilder& b) {
    b.Store(LoadGPR(b, 4), b.Truncate(LoadGPR(b, 5), INT32_TYPE),
            swap ? LOAD_STORE_BYTE_SWAP : 0);
    b.Return();
  });
  test.memory->AddVirtualMappedRange(kRegisterBase, 0xFFFF0000, 0xFFFF,
                                     &written, ReadRegister, WriteRegister);
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = kRegister;
        ctx->r[5] = kRegisterValue;
      },
      [swap, &written](PPCContext* ctx) {
        REQUIRE(written ==
                (swap ? kRegisterValue : xe::byte_swap(kRegisterValue)));
      });
}
