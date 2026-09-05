/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_BACK_EDGE_WALK_H_
#define XENIA_CPU_COMPILER_PASSES_BACK_EDGE_WALK_H_

#include <cstdint>
#include <unordered_set>

#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Blocks are laid out in guest address order, so every intra-function cycle
// branches to an already-seen block; fn(header, latch) is called for each.
template <typename Fn>
void ForEachBackEdge(hir::HIRBuilder* builder, Fn&& fn) {
  std::unordered_set<hir::Block*> seen;
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    seen.insert(block);
    for (auto instr = block->instr_head; instr != nullptr;
         instr = instr->next) {
      hir::Label* label = nullptr;
      if (instr->opcode == &hir::OPCODE_BRANCH_info) {
        label = instr->src1.label;
      } else if (instr->opcode == &hir::OPCODE_BRANCH_TRUE_info ||
                 instr->opcode == &hir::OPCODE_BRANCH_FALSE_info) {
        label = instr->src2.label;
      }
      if (label && label->block && seen.count(label->block)) {
        fn(label->block, block);
      }
    }
  }
}

// A block holding only fake instructions falls through to the next.
inline hir::Instr* FirstRealInstr(hir::Block* block) {
  for (auto b = block; b != nullptr; b = b->next) {
    auto first = b->instr_head;
    for (; first && first->IsFake(); first = first->next) {
    }
    if (first) {
      return first;
    }
  }
  return nullptr;
}

inline uint32_t BlockGuestAddress(hir::Block* block) {
  for (auto s = block->instr_head; s; s = s->next) {
    if (s->opcode == &hir::OPCODE_SOURCE_OFFSET_info) {
      return uint32_t(s->src1.offset);
    }
  }
  return 0;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_BACK_EDGE_WALK_H_
