/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/context_promotion_pass.h"

#include "xenia/apu/apu_flags.h"
#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_bool(debug);

DEFINE_bool(store_all_context_values, false,
            "Don't strip dead context stores to aid in debugging.", "CPU");

DEFINE_bool(full_optimization_even_with_debug, false,
            "For developer use to analyze the quality of the generated code, "
            "not intended for actual debugging of the code",
            "CPU");

DEFINE_bool(
    context_promote_vec128, true,
    "Promote VMX (VEC128) context loads/stores to SSA values and strip dead "
    "VEC128 context stores, letting the backend keep vectors in host "
    "registers. This restores upstream behavior from before 2026-01-25, when "
    "upstream excluded VEC128 to work around undiagnosed instability in some "
    "games. The range-keyed validity tracking here fixes a real offset "
    "aliasing hazard, but whether that was upstream's problem was never "
    "established. Disabling trades performance for current upstream's "
    "conservative behavior: every vector register access round-trips "
    "through guest context memory. Forced off by disable_context_promotion.",
    "CPU");

DECLARE_bool(disable_context_promotion);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::Value;

ContextPromotionPass::ContextPromotionPass() : CompilerPass() {}

ContextPromotionPass::~ContextPromotionPass() {}

bool ContextPromotionPass::Initialize(Compiler* compiler) {
  if (!CompilerPass::Initialize(compiler)) {
    return false;
  }

  // This is a terrible implementation.
  context_values_.resize(sizeof(ppc::PPCContext));
  context_value_size_.resize(sizeof(ppc::PPCContext));
  context_value_base_.resize(sizeof(ppc::PPCContext));
  context_validity_.resize(static_cast<uint32_t>(sizeof(ppc::PPCContext)));
  context_kill_.resize(static_cast<uint32_t>(sizeof(ppc::PPCContext)));
  context_kill_scratch_.resize(static_cast<uint32_t>(sizeof(ppc::PPCContext)));
  context_kill_read_.resize(static_cast<uint32_t>(sizeof(ppc::PPCContext)));

  return true;
}

bool ContextPromotionPass::Run(HIRBuilder* builder) {
  // Like mem2reg, but because context memory is unaliasable it's easier to
  // check and convert LoadContext/StoreContext into value operations.
  // Example of load->value promotion:
  //   v0 = load_context +100
  //   store_context +200, v0
  //   v1 = load_context +100  <-- replace with v1 = v0
  //   store_context +200, v1
  //
  // It'd be possible in this stage to also remove redundant context stores:
  // Example of dead store elimination:
  //   store_context +100, v0  <-- removed due to following store
  //   store_context +100, v1
  // This is more generally done by DSE, however if it could be done here
  // instead as it may be faster (at least on the block-level).

  // Promote loads to values.
  // Process each block independently, for now.
  auto block = builder->first_block();
  while (block) {
    PromoteBlock(block);
    block = block->next;
  }

  // Remove all dead stores.
  // This will break debugging as we can't recover this information when
  // trying to extract stack traces/register values, so we don't do that.
  if (cvars::full_optimization_even_with_debug ||
      (!cvars::debug && !cvars::store_all_context_values)) {
    block = builder->first_block();
    while (block) {
      RemoveDeadStoresBlock(block);
      block = block->next;
    }
  }

  return true;
}

// Range-keyed validity invariants (shared by the helpers below):
// - context_validity_ bit b is set iff some tracked value's range covers
//   byte b;
// - context_value_base_[b] is that value's base offset (meaningful only
//   while bit b is set);
// - context_value_size_[base] is its size in bytes, and whenever the bit at
//   `base` is set, every bit of [base, base + size) is set - ranges are
//   tracked and invalidated only as whole units.
// Stale Value*/size/base entries are gated solely by the validity bits, so
// the wholesale validity.reset() on volatile instructions remains correct.

void ContextPromotionPass::InvalidateTrackedRange(uint32_t offset,
                                                  uint32_t size) {
  auto& validity = context_validity_;
  const uint32_t end = offset + size;
  uint32_t b = offset;
  while (b < end) {
    if (validity.test(b)) {
      // Drop the whole tracked value covering this byte, whether it sits
      // narrower inside the new range or is wider and merely overlaps it.
      const uint32_t base = context_value_base_[b];
      const uint32_t base_size = context_value_size_[base];
      validity.reset(base, base + base_size);
      b = base + base_size;
    } else {
      ++b;
    }
  }
}

void ContextPromotionPass::TrackValue(uint32_t offset, uint32_t size,
                                      Value* value) {
  InvalidateTrackedRange(offset, size);
  context_values_[offset] = value;
  context_value_size_[offset] = static_cast<uint8_t>(size);
  for (uint32_t b = offset; b < offset + size; ++b) {
    context_value_base_[b] = offset;
  }
  context_validity_.set(offset, offset + size);
}

Value* ContextPromotionPass::LookupTrackedValue(uint32_t offset, uint32_t size,
                                                TypeName type) {
  // Fold only on an exact range and type match: same base offset, same size
  // (full range coverage is implied by the whole-unit invariant above), and
  // same type. The type check is insurance against same-base-offset unions
  // in PPCContext (e.g. cr0.value as INT32 vs cr0.cr0_lt as INT8).
  if (!context_validity_.test(offset) ||
      context_value_base_[offset] != offset ||
      context_value_size_[offset] != size) {
    return nullptr;
  }
  Value* value = context_values_[offset];
  if (!value || value->type != type) {
    return nullptr;
  }
  return value;
}

void ContextPromotionPass::PromoteBlock(Block* block) {
  auto& validity = context_validity_;
  validity.reset();
  const bool promote_vec128 =
      cvars::context_promote_vec128 && !cvars::disable_context_promotion;

  Instr* i = block->instr_head;
  while (i) {
    auto next = i->next;
    if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
      // Volatile instruction - requires all context values be flushed.
      validity.reset();
    } else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const TypeName type = i->dest->type;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(type));
      Value* previous_value = LookupTrackedValue(offset, size, type);
      if (previous_value) {
        // Legit previous value, reuse.
        i->opcode = &hir::OPCODE_ASSIGN_info;
        i->set_src1(previous_value);
      } else if (promote_vec128 || type != TypeName::VEC128_TYPE) {
        // Track the loaded value so later loads of this range reuse it.
        // (Loads don't modify memory, but TrackValue evicts any tracked
        // value overlapping a range we now know with a different shape.)
        TrackValue(offset, size, i->dest);
      }
    } else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      Value* value = i->src2.value;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(value->type));
      if (promote_vec128 || value->type != TypeName::VEC128_TYPE) {
        // Track the stored value for later loads. TrackValue first drops
        // every tracked value overlapping the bytes this store clobbers.
        TrackValue(offset, size, value);
      } else {
        // VEC128 promotion disabled: the value isn't tracked, but the store
        // still clobbers these bytes, so overlapping tracked values die.
        InvalidateTrackedRange(offset, size);
      }
    }
    i = next;
  }
}

void ContextPromotionPass::ComputeKillSet(Block* block, llvm::BitVector& kill) {
  kill.reset();
  auto& read = context_kill_read_;
  read.reset();
  Instr* i = block->instr_head;
  while (i) {
    if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
      return;
    }
    if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(i->dest->type));
      for (uint32_t b = offset; b < offset + size; ++b) {
        if (!kill.test(b)) {
          read.set(b);
        }
      }
    } else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size =
          static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
      for (uint32_t b = offset; b < offset + size; ++b) {
        if (!read.test(b)) {
          kill.set(b);
        }
      }
    }
    i = i->next;
  }
}

void ContextPromotionPass::ComputeOutgoingKillSet(Block* block,
                                                  llvm::BitVector& out) {
  out.reset();
  auto edge = block->outgoing_edge_head;
  if (!edge) {
    return;
  }
  bool first = true;
  while (edge) {
    ComputeKillSet(edge->dest, context_kill_scratch_);
    if (first) {
      out = context_kill_scratch_;
      first = false;
    } else {
      out &= context_kill_scratch_;
    }
    edge = edge->outgoing_next;
  }
}

void ContextPromotionPass::RemoveDeadStoresBlock(Block* block) {
  // In this walk a validity bit means "this byte is fully overwritten by a
  // later store in this block, with no barrier or load in between".
  auto& validity = context_validity_;
  ComputeOutgoingKillSet(block, context_kill_);
  validity = context_kill_;
  const bool promote_vec128 =
      cvars::context_promote_vec128 && !cvars::disable_context_promotion;

  // Walk backwards and mark byte ranges that are written to.
  // If a store's whole range was already written to later, it is dead.
  Instr* i = block->instr_tail;
  while (i) {
    Instr* prev = i->prev;
    if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
      // Volatile instruction - requires all context values be flushed.
      validity.reset();
    } else if (i->opcode->flags & OPCODE_FLAG_BRANCH) {
      // Tested after VOLATILE so a call or a return still flushes.
      validity = context_kill_;
    } else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      // A load that survived PromoteBlock is a live use of these bytes:
      // earlier stores overlapping it must be kept. (PromoteBlock folds
      // exact-match loads to values, but a load whose range/type mismatches
      // the tracked value - or that follows a volatile reset - survives.)
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(i->dest->type));
      validity.reset(offset, offset + size);
    } else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const Value* value = i->src2.value;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(value->type));
      if (promote_vec128 || value->type != TypeName::VEC128_TYPE) {
        bool fully_overwritten = true;
        for (uint32_t b = offset; b < offset + size; ++b) {
          if (!validity.test(b)) {
            fully_overwritten = false;
            break;
          }
        }
        if (!fully_overwritten) {
          // Some byte still escapes - keep the store; all bytes it writes
          // are now dead for earlier stores.
          validity.set(offset, offset + size);
        } else {
          // Every byte is overwritten later. Remove this store.
          i->UnlinkAndNOP();
        }
      }
    }
    i = prev;
  }
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
