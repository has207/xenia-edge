/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/dead_cr_store_elimination_pass.h"

#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(eliminate_dead_cr_stores, true,
            "Drop condition-register stores that no path through the function "
            "can observe.",
            "CPU");

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

// PowerPC ABI: cr2-cr4 survive a call, the rest are the callee's to destroy.
constexpr uint32_t kVolatileCR = 0x000000FFu     // cr0, cr1
                                 | 0xFFF00000u;  // cr5, cr6, cr7

uint32_t MaskForRange(uint32_t offset, uint32_t size) {
  uint32_t mask = 0;
  for (uint32_t byte = offset; byte < offset + size; ++byte) {
    if (byte >= kCRBase && byte < kCRBase + kCRBytes) {
      mask |= uint32_t(1) << (byte - kCRBase);
    }
  }
  return mask;
}

Block* BranchTarget(const Instr* i) {
  switch (i->GetOpcodeNum()) {
    case OPCODE_BRANCH:
      return i->src1.label ? i->src1.label->block : nullptr;
    case OPCODE_BRANCH_TRUE:
    case OPCODE_BRANCH_FALSE:
      return i->src2.label ? i->src2.label->block : nullptr;
    default:
      return nullptr;
  }
}

enum class CallKind {
  kNotACall,
  kUnconditional,
  kConditional,
};

CallKind ClassifyCall(const Instr* i) {
  switch (i->GetOpcodeNum()) {
    case OPCODE_CALL:
    case OPCODE_CALL_INDIRECT:
    case OPCODE_CALL_EXTERN:
      // A tail call is an exit, not a clobber.
      return (i->flags & CALL_TAIL) ? CallKind::kNotACall
                                    : CallKind::kUnconditional;
    case OPCODE_CALL_TRUE:
    case OPCODE_CALL_INDIRECT_TRUE:
      return (i->flags & CALL_TAIL) ? CallKind::kNotACall
                                    : CallKind::kConditional;
    default:
      return CallKind::kNotACall;
  }
}

uint32_t TransferBlock(Block* block, uint32_t live, bool apply,
                       const std::unordered_map<Block*, size_t>& index,
                       const std::vector<uint32_t>& live_in) {
  auto live_in_of = [&](Block* dest) -> uint32_t {
    if (!dest) {
      return kAllCR;
    }
    auto it = index.find(dest);
    return it == index.end() ? kAllCR : live_in[it->second];
  };

  Instr* i = block->instr_tail;
  while (i) {
    Instr* prev = i->prev;
    const Opcode num = i->GetOpcodeNum();
    if (Block* target = BranchTarget(i)) {
      // Branches are VOLATILE but read no context: only the target's live-in.
      if (num == OPCODE_BRANCH) {
        live = live_in_of(target);
      } else {
        live |= live_in_of(target);
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
    } else if (const CallKind kind = ClassifyCall(i);
               kind != CallKind::kNotACall) {
      if (kind == CallKind::kUnconditional) {
        live &= ~kVolatileCR;
      }
    } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
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

  if (!cvars::eliminate_dead_cr_stores || cvars::disable_context_promotion ||
      !(cvars::full_optimization_even_with_debug ||
        (!cvars::debug && !cvars::store_all_context_values))) {
    return true;
  }

  std::vector<Block*> blocks;
  std::unordered_map<Block*, size_t> index;
  for (auto block = builder->first_block(); block; block = block->next) {
    index.emplace(block, blocks.size());
    blocks.push_back(block);
  }
  if (blocks.empty()) {
    return true;
  }

  std::vector<uint32_t> live_in(blocks.size(), 0);
  auto live_out_of = [&](Block* block) -> uint32_t {
    if (!block->outgoing_edge_head) {
      return kAllCR;
    }
    uint32_t live_out = 0;
    for (auto edge = block->outgoing_edge_head; edge;
         edge = edge->outgoing_next) {
      auto it = index.find(edge->dest);
      if (it == index.end()) {
        return kAllCR;
      }
      live_out |= live_in[it->second];
    }
    return live_out;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t n = blocks.size(); n-- > 0;) {
      const uint32_t result = TransferBlock(blocks[n], live_out_of(blocks[n]),
                                            false, index, live_in);
      if (result != live_in[n]) {
        live_in[n] = result;
        changed = true;
      }
    }
  }

  for (Block* block : blocks) {
    TransferBlock(block, live_out_of(block), true, index, live_in);
  }

  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
