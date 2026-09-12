/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/spin_wait_injection_pass.h"

#include <algorithm>
#include <unordered_set>

#include "xenia/cpu/compiler/passes/back_edge_walk.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

SpinWaitInjectionPass::SpinWaitInjectionPass() : CompilerPass() {}

SpinWaitInjectionPass::~SpinWaitInjectionPass() {}

namespace {

struct LoopShape {
  uint32_t guest_lo = UINT32_MAX;
  uint32_t guest_hi = 0;
  bool has_memory_store = false;
  bool has_memory_load = false;
  bool has_call = false;
  bool has_indirect_branch = false;
  bool has_induction_variable = false;
};

// store_context(O, add(load_context(O), constant)): the loop is scanning.
bool IsInductionStore(Instr* instr) {
  if (instr->GetOpcodeNum() != OPCODE_STORE_CONTEXT) {
    return false;
  }
  Value* stored = instr->src2.value;
  if (!stored || !stored->def) {
    return false;
  }
  Instr* def = stored->def;
  if (def->GetOpcodeNum() != OPCODE_ADD && def->GetOpcodeNum() != OPCODE_SUB) {
    return false;
  }
  Value* counted = nullptr;
  if (def->src2.value && def->src2.value->IsConstant()) {
    counted = def->src1.value;
  } else if (def->src1.value && def->src1.value->IsConstant()) {
    counted = def->src2.value;
  }
  if (!counted || !counted->def) {
    return false;
  }
  Instr* load = counted->def;
  return load->GetOpcodeNum() == OPCODE_LOAD_CONTEXT &&
         load->src1.offset == instr->src1.offset;
}

LoopShape ScanLoop(Block* header, Block* latch) {
  LoopShape shape;
  for (Block* b = header; b != nullptr; b = b->next) {
    for (auto instr = b->instr_head; instr != nullptr; instr = instr->next) {
      const Opcode num = instr->GetOpcodeNum();
      switch (num) {
        case OPCODE_SOURCE_OFFSET: {
          const uint32_t addr = uint32_t(instr->src1.offset);
          shape.guest_lo = std::min(shape.guest_lo, addr);
          shape.guest_hi = std::max(shape.guest_hi, addr);
          break;
        }
        case OPCODE_STORE:
        case OPCODE_STORE_OFFSET:
        case OPCODE_STORE_MMIO:
        case OPCODE_MEMSET:
        case OPCODE_ATOMIC_COMPARE_EXCHANGE:
          shape.has_memory_store = true;
          break;
        case OPCODE_LOAD:
        case OPCODE_LOAD_OFFSET:
        case OPCODE_LOAD_MMIO:
          shape.has_memory_load = true;
          break;
        case OPCODE_CALL:
        case OPCODE_CALL_TRUE:
          shape.has_call = true;
          break;
        case OPCODE_STORE_CONTEXT:
          if (IsInductionStore(instr)) {
            shape.has_induction_variable = true;
          }
          break;
        case OPCODE_CALL_INDIRECT:
        case OPCODE_CALL_INDIRECT_TRUE:
        case OPCODE_CALL_EXTERN:
          shape.has_indirect_branch = true;
          break;
        default:
          break;
      }
    }
    if (b == latch) {
      break;
    }
  }
  return shape;
}

}  // namespace

bool SpinWaitInjectionPass::Run(HIRBuilder* builder) {
  if (!cvars::spin_wait_yield_after || !builder->first_block()) {
    return true;
  }
  std::unordered_set<Block*> tagged;
  ForEachBackEdge(builder, [&](Block* header, Block* latch) {
    if (tagged.count(header)) {
      return;
    }
    LoopShape shape = ScanLoop(header, latch);
    if (shape.guest_lo > shape.guest_hi) {
      return;  // no source offsets to size the loop with
    }
    const uint32_t span = (shape.guest_hi - shape.guest_lo) / 4 + 1;
    if (span > kMaxBodyGuestInstrs || shape.has_memory_store ||
        !shape.has_memory_load || !shape.has_call ||
        shape.has_indirect_branch || shape.has_induction_variable) {
      return;
    }
    Instr* first = FirstRealInstr(header);
    if (!first) {
      return;
    }
    if (first->GetOpcodeNum() != OPCODE_DELAY_EXECUTION) {
      Instr* delay = builder->DelayExecution(DELAY_EXECUTION_INJECTED);
      delay->src1.offset = BlockGuestAddress(header);
      delay->MoveBefore(first);
    }
    tagged.insert(header);
  });
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
