/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/dead_cr_store_elimination_pass.h"

#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/ppc/ppc_context.h"

DECLARE_bool(debug);
DECLARE_bool(disable_context_promotion);
DECLARE_bool(store_all_context_values);
DECLARE_bool(full_optimization_even_with_debug);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;

namespace {

// cr0..cr7 are the first 32 context bytes; one bit per byte here.
constexpr uint32_t kCRBase =
    static_cast<uint32_t>(offsetof(ppc::PPCContext, cr0));
constexpr uint32_t kCRBytes = 8 * 4;
constexpr uint32_t kAllCR = ~uint32_t(0);

uint32_t MaskForRange(uint32_t offset, uint32_t size) {
  uint32_t mask = 0;
  for (uint32_t byte = offset; byte < offset + size; ++byte) {
    if (byte >= kCRBase && byte < kCRBase + kCRBytes) {
      mask |= uint32_t(1) << (byte - kCRBase);
    }
  }
  return mask;
}

// Blocks are numbered by Block::ordinal.
uint32_t TransferBlock(Block* block, uint32_t live, bool apply,
                       const std::vector<uint32_t>& live_in) {
  Instr* i = block->instr_tail;
  while (i) {
    Instr* prev = i->prev;
    const Opcode num = i->GetOpcodeNum();
    if (const Label* target = i->BranchLabel()) {
      // Branches are VOLATILE but read no context: only the target's live-in.
      // A block outside the list (never numbered) is assumed to read all CR.
      const uint16_t n = target->block->ordinal;
      const uint32_t target_live = n < live_in.size() ? live_in[n] : kAllCR;
      if (num == OPCODE_BRANCH) {
        live = target_live;
      } else {
        live |= target_live;
      }
    } else if (num == OPCODE_LOAD_CONTEXT) {
      live |= MaskForRange(static_cast<uint32_t>(i->src1.offset),
                           static_cast<uint32_t>(GetTypeSize(i->dest->type)));
    } else if (num == OPCODE_STORE_CONTEXT) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size =
          static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
      const uint32_t mask = MaskForRange(offset, size);
      if (mask && offset >= kCRBase && offset + size <= kCRBase + kCRBytes) {
        if (!(mask & live)) {
          if (apply) {
            i->UnlinkAndNOP();
          }
        } else {
          live &= ~mask;
        }
      }
    } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
      // Calls included: guest code need not follow the ABI's volatile fields.
      live = kAllCR;
    } else if (i->opcode->flags & OPCODE_FLAG_MEMORY) {
      // A guest access can fault, and the guest's handlers see the CR in the
      // context.
      live = kAllCR;
    }
    i = prev;
  }
  return live;
}

}  // namespace

DeadCRStoreEliminationPass::DeadCRStoreEliminationPass() : CompilerPass() {}

DeadCRStoreEliminationPass::~DeadCRStoreEliminationPass() = default;

bool DeadCRStoreEliminationPass::Run(HIRBuilder* builder) {
  SCOPE_profile_cpu_f("cpu");

  if (cvars::disable_context_promotion ||
      !(cvars::full_optimization_even_with_debug ||
        (!cvars::debug && !cvars::store_all_context_values)) ||
      (builder->attributes() & FUNCTION_ATTRIB_KEEP_CONTEXT_STORES)) {
    return true;
  }

  // Block::ordinal is 16 bits.
  size_t block_count = 0;
  for (auto block = builder->first_block(); block; block = block->next) {
    if (block_count > UINT16_MAX) {
      return true;
    }
    block->ordinal = static_cast<uint16_t>(block_count++);
  }
  if (!block_count) {
    return true;
  }

  std::vector<uint32_t> live_in(block_count, 0);
  auto live_out_of = [&](Block* block) -> uint32_t {
    if (!block->outgoing_edge_head) {
      return kAllCR;
    }
    uint32_t live_out = 0;
    for (auto edge = block->outgoing_edge_head; edge;
         edge = edge->outgoing_next) {
      live_out |= live_in[edge->dest->ordinal];
    }
    return live_out;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (Block* block = builder->last_block(); block; block = block->prev) {
      const uint32_t result =
          TransferBlock(block, live_out_of(block), false, live_in);
      if (result != live_in[block->ordinal]) {
        live_in[block->ordinal] = result;
        changed = true;
      }
    }
  }

  for (auto block = builder->first_block(); block; block = block->next) {
    TransferBlock(block, live_out_of(block), true, live_in);
  }

  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
