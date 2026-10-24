/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Tags spin-shaped loop headers with DELAY_EXECUTION_INJECTED so the a64
// backend can release the core. Gated on --spin_wait_yield_after.
class SpinWaitInjectionPass : public CompilerPass {
 public:
  SpinWaitInjectionPass();
  ~SpinWaitInjectionPass() override;

  bool Run(hir::HIRBuilder* builder) override;

  static constexpr uint32_t kMaxBodyGuestInstrs = 8;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
