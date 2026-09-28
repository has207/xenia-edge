/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_DEAD_CR_STORE_ELIMINATION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_DEAD_CR_STORE_ELIMINATION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Removes condition-register stores that no path can observe.
class DeadCRStoreEliminationPass : public CompilerPass {
 public:
  DeadCRStoreEliminationPass();
  ~DeadCRStoreEliminationPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_DEAD_CR_STORE_ELIMINATION_PASS_H_
