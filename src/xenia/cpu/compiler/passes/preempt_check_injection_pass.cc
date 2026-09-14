/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/preempt_check_injection_pass.h"

#include <unordered_set>

#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/passes/back_edge_walk.h"
#include "xenia/cpu/hir/hir_builder.h"

DECLARE_bool(guest_scheduler);

// Defined here rather than in a backend: the backends are mutually exclusive by
// target arch, and both emitters read this.
DEFINE_bool(
    log_safepoint_pc, false,
    "Record the guest address of every JIT safepoint a fiber passes, so the "
    "cooperative scheduler's no-progress report can name where a wedged "
    "fiber last checked in rather than only its link register. Costs a "
    "store on every loop back-edge; diagnostic only.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

PreemptCheckInjectionPass::PreemptCheckInjectionPass() : CompilerPass() {}

PreemptCheckInjectionPass::~PreemptCheckInjectionPass() {}

bool PreemptCheckInjectionPass::Run(HIRBuilder* builder) {
  // The bool return is pass success, not whether anything changed, and Compile
  // aborts the whole function on false.
  //
  // Read the cvar here, not in the ctor, so a per-title override applies.
  if (!cvars::guest_scheduler || !builder->first_block()) {
    return true;
  }
  // Calls, recursion and indirect branches re-enter at the entry block.
  std::unordered_set<Block*> check_blocks;
  check_blocks.insert(builder->first_block());
  ForEachBackEdge(builder, [&check_blocks](Block* header, Block*) {
    check_blocks.insert(header);
  });
  for (auto block : check_blocks) {
    Instr* first = FirstRealInstr(block);
    if (first && first->GetOpcodeNum() != OPCODE_CHECK_PREEMPT) {
      // Carry the guest address of the safepoint so the backend can record
      // where a fiber last checked in. A fiber that stops yielding is
      // otherwise only locatable by its link register, which points at the
      // last call it made rather than the loop it is stuck in.
      Instr* check = builder->CheckPreempt();
      check->src1.offset = BlockGuestAddress(first->block);
      check->MoveBefore(first);
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
