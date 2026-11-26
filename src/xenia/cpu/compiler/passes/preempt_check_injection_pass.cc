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
#include "xenia/cpu/hir/hir_builder.h"

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
  if (!builder->first_block()) {
    return true;
  }
  // Blocks are laid out in guest address order, so every intra-function cycle
  // branches to an already-seen block. Calls, recursion and indirect branches
  // (bcctr lowers to CallIndirect) re-enter at the entry block.
  std::unordered_set<Block*> seen;
  std::unordered_set<Block*> check_blocks;
  check_blocks.insert(builder->first_block());
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    seen.insert(block);
    // Scan every instruction: a back-edge is not always in the trailing
    // branch run. A loop ending its block with a call hides the loop branch
    // from a tail-backward walk, leaving it to spin with no safepoint.
    for (auto instr = block->instr_head; instr != nullptr;
         instr = instr->next) {
      Label* label = nullptr;
      if (instr->opcode == &OPCODE_BRANCH_info) {
        label = instr->src1.label;
      } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                 instr->opcode == &OPCODE_BRANCH_FALSE_info) {
        label = instr->src2.label;
      }
      if (label && label->block && seen.count(label->block)) {
        check_blocks.insert(label->block);
      }
    }
  }
  for (auto block : check_blocks) {
    // A block holding only fake instructions falls through, so the check
    // lands in the first real successor, still on every cycle through it.
    for (auto b = block; b != nullptr; b = b->next) {
      auto first = b->instr_head;
      for (; first && first->IsFake(); first = first->next) {
      }
      if (first) {
        if (first->GetOpcodeNum() != OPCODE_CHECK_PREEMPT) {
          // Carry the guest address of the safepoint so the backend can record
          // where a fiber last checked in. A fiber that stops yielding is
          // otherwise only locatable by its link register, which points at the
          // last call it made rather than the loop it is stuck in.
          uint32_t guest_address = 0;
          for (auto s = b->instr_head; s; s = s->next) {
            if (s->opcode == &OPCODE_SOURCE_OFFSET_info) {
              guest_address = uint32_t(s->src1.offset);
              break;
            }
          }
          Instr* check = builder->CheckPreempt();
          check->src1.offset = guest_address;
          check->MoveBefore(first);
        }
        break;
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
