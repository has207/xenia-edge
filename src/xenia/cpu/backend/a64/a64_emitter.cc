/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"

#include <cstring>

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_int64(a64_max_stackpoints);
DECLARE_bool(a64_enable_host_guest_stack_synchronization);

DECLARE_bool(log_safepoint_pc);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace Xbyak_aarch64;

// Defined in a64_backend.cc.
extern uint64_t ResolveFunction(void* raw_context, uint64_t target_address);
extern uint64_t ResolveDynamicCall(void* raw_context, uint64_t target_address);
extern uint64_t ResolveDirectDynamicCall(void* raw_context,
                                         uint64_t target_address);
extern uint64_t ResolveCachedReturnSiteCall(void* raw_context,
                                            uint64_t target_address);
extern uint64_t ResolveCachedReturnSiteTail(void* raw_context,
                                            uint64_t target_address);
extern uint64_t ResolveDynamicTailCall(void* raw_context,
                                       uint64_t target_address);

static uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  XELOGE("undefined extern call to {:08X} {}", function->address(),
         function->name());
  return 0;
}

static constexpr size_t kMaxCodeSize = 1_MiB;

// Register maps:
// GPR allocatable registers: x22, x23, x24, x25, x26, x27, x28
// (x19=backend context, x20=context, x21=membase are reserved)
const uint32_t A64Emitter::gpr_reg_map_[GPR_COUNT] = {
    22, 23, 24, 25, 26, 27, 28,
};

// VEC allocatable registers: v4-v15, v16-v31
// (v0-v3 are scratch)
const uint32_t A64Emitter::vec_reg_map_[VEC_COUNT] = {
    4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

A64Emitter::A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator)
    : CodeGenerator(kMaxCodeSize, Xbyak_aarch64::AutoGrow, allocator),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()),
      allocator_(allocator),
      feature_flags_(arm64::GetFeatureFlags()) {}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, hir::HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  guest_module_ = dynamic_cast<XexModule*>(function->module());

  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  coverage_offset_ = function->coverage_offset();
  coverage_start_address_ = function->address();
  coverage_instruction_count_ =
      function->has_end_address()
          ? (function->end_address() - function->address()) / 4 + 1
          : 0;
  coverage_current_index_ = UINT32_MAX;
  coverage_out_of_range_ = false;
  sequence_samples_.clear();

  current_guest_function_ = function->address();
  guest_tlb_ = processor_->memory()->guest_tlbs_active();

  // Reset state.
  stack_size_ = StackLayout::GUEST_STACK_SIZE;
  source_map_arena_.Reset();
  tail_code_.clear();
  label_bind_offsets_.clear();
  fpcr_mode_ = FPCRMode::Fpu;
  remap_kept_label_ids_.clear();
  remap_kept_branch_after_call_ = false;
  emitting_tail_ = false;

  // The prolog, epilog and helpers emit outside the per-opcode guard below, so
  // an unencodable operand needs catching here too.
  EmitFunctionInfo func_info = {};
  bool emitted = false;
  try {
    emitted = Emit(builder, func_info);
  } catch (const Xbyak_aarch64::Error& e) {
    XELOGE("A64: assembler error while emitting guest function {:08X}: {}",
           current_guest_function_, e.what());
    emitted = false;
  }
  if (!emitted) {
    // Emplace only runs on success, so a failed compile has to reset too.
    ResetPerFunctionState();
    return false;
  }

  // Emplace the code into the code cache.
  *out_code_address = Emplace(func_info, function);
  *out_code_size = func_info.code_size.total;

  // Copy source map.
  source_map_arena_.CloneContents(out_source_map);

  if (!sequence_samples_.empty()) {
    processor_->RecordSequenceSamples(function->address(),
                                      std::move(sequence_samples_));
    sequence_samples_.clear();
  }

  return *out_code_address != nullptr;
}

bool A64Emitter::Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info) {
  // Count each block's incoming edges for the FPCR tracker, and note whether
  // the function touches VEC128 at all.
  function_has_vmx_ = false;
  ResetSequenceHandoffs();
  expected_preds_.clear();
  incoming_state_.clear();
  size_t hir_instr_count = 0;
  for (auto* b = builder->first_block(); b; b = b->next) {
    // Branches can sit mid-block, so every instruction is scanned.
    for (auto* i = b->instr_head; i; i = i->next) {
      ++hir_instr_count;
      if (const hir::Label* label = i->BranchLabel()) {
        ++expected_preds_[label->block];
      }
      if (function_has_vmx_) {
        continue;
      }
      if (i->dest && i->dest->type == hir::VEC128_TYPE) {
        function_has_vmx_ = true;
        continue;
      }
      uint32_t sig = i->opcode->signature;
      const hir::Instr::Op* ops[3] = {&i->src1, &i->src2, &i->src3};
      for (int k = 0; k < 3; ++k) {
        auto t =
            static_cast<hir::OpcodeSignatureType>((sig >> (3 * (k + 1))) & 0x7);
        if (t == hir::OPCODE_SIG_TYPE_V &&
            ops[k]->value->type == hir::VEC128_TYPE) {
          function_has_vmx_ = true;
          break;
        }
      }
    }
    auto* last = b->instr_tail;
    if (b->next && !(last && last->opcode == &hir::OPCODE_BRANCH_info)) {
      ++expected_preds_[b->next];
    }
  }
  // The function entry is an edge too, and every function is entered in Fpu.
  ++expected_preds_[builder->first_block()];
  RecordIncomingState(builder->first_block(), FPCRMode::Fpu, false);
  // 256 bytes per HIR instruction bounds the body, the tail and the literal
  // pool islands. Should that bound ever be wrong, xbyak_aarch64 rejects the
  // out-of-range branch when the label is bound (ERR_LABEL_IS_TOO_FAR), and
  // the throw fails this function's compile.
  near_tail_branches_safe_ = hir_instr_count * 256 < (768 * 1024);
  near_tbz_branches_safe_ = hir_instr_count * 256 < (24 * 1024);

  // Calculate local variable stack offsets.
  auto locals = builder->locals();
  size_t stack_offset = StackLayout::GUEST_STACK_SIZE;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = hir::GetTypeSize(slot->type);
    // Align to natural size (at least 4 bytes for ARM64 alignment).
    size_t align_size = xe::round_up(type_size, static_cast<size_t>(4));
    stack_offset = xe::align(stack_offset, align_size);
    slot->set_constant(static_cast<uint32_t>(stack_offset));
    stack_offset += type_size;
  }
  // Align total stack offset to 16 bytes (ARM64 ABI requirement).
  stack_offset -= StackLayout::GUEST_STACK_SIZE;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));

  const size_t stack_size = StackLayout::GUEST_STACK_SIZE + stack_offset;
  // ARM64 ABI: SP must always be 16-byte aligned.
  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  func_info.lr_save_offset = StackLayout::HOST_RET_ADDR;
  stack_size_ = stack_size;

  struct {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t prolog_stack_alloc;
  } code_offsets = {};

  // ========================================================================
  // PROLOG
  // ========================================================================
  code_offsets.prolog = getSize();

  // sub sp, sp, #stack_size
  if (stack_size <= 4095) {
    sub(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    sub(sp, sp, x17, UXTX);
  }
  code_offsets.prolog_stack_alloc = getSize();

  // Store host return address (x30/LR) so the epilog can restore it.
  str(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  // Store guest PPC return address (passed in x0 by convention).
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  // Store zero for call return address (we haven't made a call yet).
  str(xzr, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));

  // Record stackpoint for longjmp recovery.
  PushStackpoint();

  // ========================================================================
  // BODY
  // ========================================================================
  code_offsets.body = getSize();

  // FTrace: log guest function entry when the backend was built with
  // function tracing available (gated at runtime by the trace_func flag).
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionEntry));
  }

  // Allocate the epilog label (owned by label_cache_ for cleanup).
  auto epilog_label_ptr = new Label();
  label_cache_.push_back(epilog_label_ptr);
  epilog_label_ = epilog_label_ptr;

  // Walk HIR blocks and emit ARM64 instructions.
  auto block = builder->first_block();
  synchronize_stack_on_next_instruction_ = false;
  while (block) {
    // Start in the meet of the incoming states once every incoming edge has
    // been emitted. A loop header's back edge has not been, so it starts
    // with the FPCR mode untracked and the remap bound not loaded.
    FPCRMode incoming_fpcr = FPCRMode::Unknown;
    bool incoming_remap_bound = false;
    {
      auto exp_it = expected_preds_.find(block);
      auto in_it = incoming_state_.find(block);
      if (exp_it != expected_preds_.end() && in_it != incoming_state_.end() &&
          in_it->second.count == exp_it->second) {
        incoming_fpcr = in_it->second.fpcr_meet;
        incoming_remap_bound = in_it->second.remap_bound_valid;
      }
    }
    fpcr_mode_ = incoming_fpcr == FPCRMode::Unknown ? UntrackedFpcrMode()
                                                    : incoming_fpcr;

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      L(GetLabel(label->id));
      label = label->next;
    }
    if (incoming_remap_bound) {
      set_physical_remap_bound_valid();
    } else {
      DropPhysicalRemapBound();
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute. With debug info on a
      // COMMENT precedes the SOURCE_OFFSET, so skip that too.
      if (synchronize_stack_on_next_instruction_) {
        const hir::Opcode opcode_num = instr->GetOpcodeNum();
        if (opcode_num != hir::OPCODE_SOURCE_OFFSET &&
            opcode_num != hir::OPCODE_COMMENT) {
          synchronize_stack_on_next_instruction_ = false;
          EnsureSynchronizedGuestAndHostStack();
        }
      }
      const hir::Instr* new_tail = instr;
      bool selected = false;
      const bool handoff_pending = sequence_handoff_pending();
      try {
        selected = SelectSequence(this, instr, &new_tail);
        if (const hir::Label* label = instr->BranchLabel()) {
          RecordIncomingState(label->block);
        }
      } catch (const Xbyak_aarch64::Error& e) {
        // Uncaught this aborts the process with no context, so name the opcode
        // and the guest function and fail just this compile.
        XELOGE(
            "A64: assembler rejected HIR opcode {} in guest function {:08X}: "
            "{}",
            hir::GetOpcodeName(instr->GetOpcodeInfo()), current_guest_function_,
            e.what());
        return false;
      }
      if (!selected) {
        // No sequence matched — this is expected in Phase 1 before
        // sequences are implemented.
        XELOGE("A64: Unable to process HIR opcode {}",
               hir::GetOpcodeName(instr->GetOpcodeInfo()));
        return false;
      }
      if (handoff_pending && sequence_handoff_pending() &&
          !PassesHandoffs(instr)) {
        // The previous sequence emitted nothing and left its work to this
        // one, which did not do it: the code would compute a wrong result.
        XELOGE(
            "A64: HIR opcode {} did not take the handoff of the sequence "
            "before it in guest function {:08X}",
            hir::GetOpcodeName(instr->GetOpcodeInfo()),
            current_guest_function_);
        return false;
      }
      instr = new_tail;
    }

    if (!MaybeFlushV128ConstPool()) {
      return false;
    }

    auto* last = block->instr_tail;
    if (block->next && !(last && last->opcode == &hir::OPCODE_BRANCH_info)) {
      RecordIncomingState(block->next);
    }
    block = block->next;
  }

  // ========================================================================
  // EPILOG
  // ========================================================================
  // Falling into the epilog skips RETURN, which leaves the Fpu mode callers
  // assume.
  EnsureFpuFpcrModeForTransition();
  L(*epilog_label_);
  epilog_label_ = nullptr;
  // A call as the last instruction leaves the check unconsumed.
  if (synchronize_stack_on_next_instruction_) {
    synchronize_stack_on_next_instruction_ = false;
    EnsureSynchronizedGuestAndHostStack();
  }
  // FTrace: log the guest return value (r3) on normal return.
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionReturn));
  }
  code_offsets.epilog = getSize();

  // Pop stackpoint before leaving.
  PopStackpoint();

  // Restore host return address and deallocate stack.
  ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  if (stack_size <= 4095) {
    add(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    add(sp, sp, x17, UXTX);
  }
  ret();

  // ========================================================================
  // TAIL CODE
  // ========================================================================
  emitting_tail_ = true;
  for (auto& tail_item : tail_code_) {
    // ARM64 instructions are always 4-byte aligned, so alignment is mostly
    // a no-op unless we want cache-line alignment for hot paths.
    L(tail_item.label);
    tail_item_called_ = false;
    // Tail code runs in whatever mode its branch site held, not the mode the
    // last block ended in.
    fpcr_mode_ = UntrackedFpcrMode();
    try {
      tail_item.func(*this, tail_item.label);
    } catch (const Xbyak_aarch64::Error& e) {
      XELOGE("A64: assembler rejected tail code in guest function {:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
    if (!MaybeFlushV128ConstPool()) {
      return false;
    }
  }
  emitting_tail_ = false;
  if (remap_kept_branch_after_call_) {
    XELOGE(
        "A64: tail code in guest function {:08X} made a call and then "
        "returned through a label that kept the physical remap bound",
        current_guest_function_);
    return false;
  }
  code_offsets.tail = getSize();

  // ========================================================================
  // LITERAL POOL
  // ========================================================================
  if (!FlushV128ConstPool(false)) {
    return false;
  }

  // Fill in EmitFunctionInfo metrics.
  assert_zero(code_offsets.prolog);
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {
  assert_true(func_info.code_size.total == getSize());

  void* new_execute_address;
  void* new_write_address;

  if (function) {
    code_cache_->PlaceGuestCode(
        function->address(),
        const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        function, new_execute_address, new_write_address);
  } else {
    code_cache_->PlaceHostCode(
        0, const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        new_execute_address, new_write_address);
  }

  // In xbyak_aarch64, labels are resolved at define time (backpatching),
  // so all relative offsets are already correct. We just need to reset
  // the codegen state for the next function.
  ResetPerFunctionState();

  return new_execute_address;
}

void A64Emitter::ResetPerFunctionState() {
  reset();
  tail_code_.clear();
  // reset() restarts xbyak label ids from 1, so recorded bind offsets from
  // this function must not leak into the next one.
  label_bind_offsets_.clear();
  remap_kept_label_ids_.clear();
  remap_kept_branch_after_call_ = false;
  emitting_tail_ = false;

  // Clean up cached labels.
  epilog_label_ = nullptr;
  for (auto* cached_label : label_cache_) {
    delete cached_label;
  }
  label_cache_.clear();
  v128_consts_.clear();
  v128_consts_first_use_ = 0;

  // Clean up HIR->xbyak label map. HIR label ids restart at each function, so
  // stale entries would hand the next function this one's labels.
  for (auto& pair : label_map_) {
    delete pair.second;
  }
  label_map_.clear();
}

void A64Emitter::MarkSourceOffset(const hir::Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->hir_offset = uint32_t(i->block->ordinal << 16) | i->ordinal;
  entry->code_offset = static_cast<uint32_t>(getSize());

  if ((debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctionCoverage) &&
      coverage_offset_ != GuestFunction::kInvalidCoverageOffset) {
    // A source offset is not guaranteed to land inside the range the scanner
    // reported, and counting outside the reserved slice writes through a wild
    // displacement into whatever the arena holds next.
    uint32_t instruction_index =
        (entry->guest_address - coverage_start_address_) / 4;
    if (entry->guest_address < coverage_start_address_ ||
        instruction_index >= coverage_instruction_count_) {
      coverage_current_index_ = UINT32_MAX;
      if (!coverage_out_of_range_) {
        coverage_out_of_range_ = true;
        XELOGW(
            "Coverage: {:08X} is outside {:08X} and the {} instructions after "
            "it, not counting it",
            entry->guest_address, coverage_start_address_,
            coverage_instruction_count_);
      }
      return;
    }
    // Everything emitted from here until the next source offset belongs to
    // this guest instruction.
    coverage_current_index_ = instruction_index;
    const size_t byte_offset =
        coverage_offset_ + static_cast<size_t>(instruction_index) * 8;
    // Counters are per thread, so this needs no atomic. x0 and x1 are scratch
    // and outside gpr_reg_map_ (x22-x28), and this sits on its own
    // OPCODE_SOURCE_OFFSET instruction, so no HIR value is live in either.
    //
    // add rather than adds because NZCV has to survive: a guest compare and
    // the branch consuming it are separate guest instructions, so a source
    // offset lands between them.
    ldr(x0, ptr(GetContextReg(),
                static_cast<int32_t>(offsetof(ppc::PPCContext, trace_counts))));
    mov(x1, static_cast<uint64_t>(byte_offset));
    add(x0, x0, x1);
    ldr(x1, ptr(x0));
    add(x1, x1, 1);
    str(x1, ptr(x0));
  }
}

void A64Emitter::RecordSequenceSample(const hir::Instr* i, uint32_t backend_key,
                                      uint32_t host_bytes) {
  if (coverage_current_index_ == UINT32_MAX || !host_bytes) {
    return;
  }
  sequence_samples_.push_back({hir::MakeSequenceSampleKey(i, backend_key),
                               coverage_current_index_, host_bytes});
}

void A64Emitter::DebugBreak() { brk(0xF000); }

static uint64_t TrapDebugBreak(void*) {
  XELOGE("tw/td forced trap hit! This should be a crash!");
  if (cvars::break_on_debugbreak) {
    xe::debugging::Break();
  }
  return 0;
}

void A64Emitter::Trap(uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26:
      CallNative(reinterpret_cast<void*>(&backend::TrapDebugPrint));
      break;
    case 0:
    case 22:
      CallNative(reinterpret_cast<void*>(&TrapDebugBreak));
      break;
    case 25:
      break;
    default:
      XELOGW("Unknown trap type {}", trap_type);
      // Not brk #0: that is the breakpoint encoding.
      brk(0xF002);
      break;
  }
}

void A64Emitter::b(const Xbyak_aarch64::Cond cond,
                   const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tail_branches_safe_ ||
      IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::b(cond, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::b(static_cast<Xbyak_aarch64::Cond>(cond ^ 1), skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::WReg& rt,
                     const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tail_branches_safe_ ||
      IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::XReg& rt,
                     const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tail_branches_safe_ ||
      IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::WReg& rt,
                      const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tail_branches_safe_ ||
      IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::XReg& rt,
                      const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tail_branches_safe_ ||
      IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tbz_branches_safe_ ||
      IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tbz_branches_safe_ ||
      IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tbz_branches_safe_ ||
      IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  CheckRemapKeptBranch(label);
  if (near_tbz_branches_safe_ ||
      IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  LKeepingRemapBound(skip);
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  XELOGE("A64: Unimplemented HIR instruction: {}",
         hir::GetOpcodeName(i->GetOpcodeInfo()));
  DebugBreak();
}

void A64Emitter::EmitEncodedIndirectionLookup() {
  // Encoded path: see A64CodeCache for the entry format.
  static_assert(offsetof(A64BackendContext, indirection_table_bias) < 4096 &&
                offsetof(A64BackendContext, code_execute_base) < 4096 &&
                offsetof(A64BackendContext, external_indirection_table) < 4096);
  ldr(x14, BackendCtxPtr(offsetof(A64BackendContext, indirection_table_bias)));
  add(x14, x14, w16, UXTW);
  ldr(w9, ptr(x14, static_cast<uint32_t>(0)));

  // External: tagged index into the side table. Cold, so it goes to the tail
  // when tbnz can provably reach it.
  auto emit_external = [](A64Emitter& e) {
    e.and_(e.w15, e.w9, A64CodeCache::kIndirectionExternalIndexMask);
    e.ldr(e.x14, e.BackendCtxPtr(
                     offsetof(A64BackendContext, external_indirection_table)));
    e.add(e.x14, e.x14, e.x15, LSL, 3);
    e.ldr(e.x9, ptr(e.x14, static_cast<uint32_t>(0)));
  };
  auto emit_internal = [](A64Emitter& e) {
    // Internal: rel32 from code cache base.
    e.ldr(e.x14,
          e.BackendCtxPtr(offsetof(A64BackendContext, code_execute_base)));
    e.add(e.x9, e.x14, e.w9, UXTW);
  };
  auto& indirection_ready = NewCachedLabel();
  if (near_tbz_branches_safe_) {
    auto& external_target =
        AddToTail([emit_external, &indirection_ready](A64Emitter& e, Label&) {
          emit_external(e);
          e.b(indirection_ready);
        });
    tbnz_near(w9, 31, external_target);
    emit_internal(*this);
  } else {
    // Bound a few instructions later, so the short form reaches it.
    auto& external_target = NewCachedLabel();
    tbnz_near(w9, 31, external_target);
    emit_internal(*this);
    b(indirection_ready);
    L(external_target);
    emit_external(*this);
  }
  L(indirection_ready);
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  EnsureFpuFpcrModeForTransition();
  DropPhysicalRemapBound();
  NoteCallClobbersRemapBound();
  if (TryInlinePPCGprLrSaveRestore(instr, function)) {
    return;
  }

  auto fn = static_cast<A64Function*>(function);

  // Calls to dynamic code look the target up every time, since a direct call
  // would keep reaching the old copy once the code is rewritten.
  const bool rewritable = processor()->dynamic_code_enabled() &&
                          code_cache_->has_indirection_table() &&
                          !code_cache_->HasIndirectionSlot(function->address());
  if (fn->machine_code() && !rewritable) {
    // Direct call — function is already compiled.
    mov(x9, reinterpret_cast<uint64_t>(fn->machine_code()));
    if (!(instr->flags & hir::CALL_TAIL)) {
      // Pass the next call's guest return address in x0.
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
      blr(x9);
      synchronize_stack_on_next_instruction_ = true;
    } else {
      // Tail call: pass our return address to the callee.
      PopStackpoint();
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
      ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
      if (stack_size() <= 4095) {
        add(sp, sp, static_cast<uint32_t>(stack_size()));
      } else {
        mov(x17, static_cast<uint64_t>(stack_size()));
        add(sp, sp, x17, UXTX);
      }
      br(x9);
    }
    return;
  }

  if (code_cache_->has_indirection_table() &&
      code_cache_->HasIndirectionSlot(function->address())) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    mov(w16, function->address());
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      EmitEncodedIndirectionLookup();
    }
  } else if (code_cache_->has_indirection_table()) {
    mov(w16, function->address());
    EmitDynamicCallLookup((instr->flags & hir::CALL_TAIL) != 0, true);
  } else {
    // No indirection table: resolve at runtime.
    mov(x0, x20);  // context
    mov(x1, static_cast<uint64_t>(function->address()));
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address in x9
  }

  if (instr->flags & hir::CALL_TAIL) {
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

bool A64Emitter::TryInlinePPCGprLrSaveRestore(const hir::Instr* instr,
                                              const GuestFunction* function) {
  // The inlined form addresses the stack through the membase.
  if (guest_tlb_ || !function->IsSaverest() ||
      function->SaverestType() != SaveRestoreType::GPR) {
    return false;
  }

  const unsigned first_gpr = function->SaverestIndex();
  if (first_gpr < 14 || first_gpr > 31) {
    return false;
  }

  const bool is_tail_call = (instr->flags & hir::CALL_TAIL) != 0;
  if ((function->IsSave() && is_tail_call) ||
      (function->IsRestore() && !is_tail_call)) {
    return false;
  }

  // Standard PPC helper layout:
  //   std/ld rN, -((33 - N) * 8)(r1), N = first_gpr..31
  //   stw/lwz r12, -8(r1)
  const uint32_t first_slot_offset = (33 - first_gpr) * 8;
  const uint32_t lr_slot_offset = (32 - first_gpr) * 8;

  ldr(w14, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  if (xe::memory::allocation_granularity() > 0x1000) {
    // Branch-free: w15 = w14 + 0x1000, keep it only when w14 >= 0xE0000000.
    mov(w15, 0xE0000000u);
    cmp(w14, w15);
    add(w15, w14, 1, 12);  // w15 = w14 + 0x1000 via LSL #12
    csel(w14, w14, w15, LO);
  }
  add(x14, x21, w14, UXTW);
  sub(x14, x14, first_slot_offset);

  if (function->IsSave()) {
    for (unsigned guest_reg = first_gpr; guest_reg <= 31; ++guest_reg) {
      ldr(x15, ptr(x20, static_cast<int32_t>(
                            offsetof(ppc::PPCContext, r[guest_reg]))));
      rev(x15, x15);
      str(x15, ptr(x14, static_cast<uint32_t>((guest_reg - first_gpr) * 8)));
    }

    ldr(w15, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[12]))));
    rev(w15, w15);
    str(w15, ptr(x14, lr_slot_offset));
    return true;
  }

  for (unsigned guest_reg = first_gpr; guest_reg <= 31; ++guest_reg) {
    ldr(x15, ptr(x14, static_cast<uint32_t>((guest_reg - first_gpr) * 8)));
    rev(x15, x15);
    str(x15, ptr(x20, static_cast<int32_t>(
                          offsetof(ppc::PPCContext, r[guest_reg]))));
  }

  ldr(w16, ptr(x14, lr_slot_offset));
  rev(w16, w16);
  str(x16, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[12]))));
  str(x16, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));

  // __restgprlr_N returns to the reloaded LR: take our epilogue when it is our
  // own return address, otherwise tail-call it. CallIndirect emits the
  // indirection lookup and, for a tail call, the stack teardown and jump.
  ldr(w15, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  cmp(w16, w15);
  b(EQ, epilog_label());
  CallIndirect(instr, 16);
  return true;
}

void A64Emitter::CallIndirect(const hir::Instr* instr, int reg_index) {
  EnsureFpuFpcrModeForTransition();
  DropPhysicalRemapBound();
  NoteCallClobbersRemapBound();
  auto target_w = WReg(reg_index);

  // Check if this is a possible return (e.g., PPC blr).
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    // Compare target guest address with our function's return address.
    ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    cmp(target_w, w0);
    b(EQ, epilog_label());
  }

  // Load host code address from indirection table.
  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    if (target_w.getIdx() != w16.getIdx()) {
      mov(w16, target_w);
    }
    // A target without an indirection slot goes through the dynamic call cache.
    // Only dynamic code has such targets, and the check costs code cache space
    // at every indirect call site, so titles without it do not pay for it.
    Label* target_ready = nullptr;
    if (processor()->dynamic_code_enabled()) {
      target_ready = &NewCachedLabel();
      // TODO(has207): while the base is 0x80000000, eor with it gives the same
      // branch outcome in one instruction. Needs a static_assert on the base.
      mov(w14, code_cache_->indirection_guest_base());
      sub(w14, w16, w14);
      mov(w15, code_cache_->indirection_guest_size());
      cmp(w14, w15);
      const bool tail_call = (instr->flags & hir::CALL_TAIL) != 0;
      const bool possible_return =
          tail_call && (instr->flags & hir::CALL_POSSIBLE_RETURN) &&
          cvars::a64_enable_host_guest_stack_synchronization;
      Label& resolve_natively = AddToTail(
          [target_ready, tail_call, possible_return](A64Emitter& e, Label&) {
            if (possible_return) {
              e.str(e.wzr, e.BackendCtxPtr(offsetof(A64BackendContext,
                                                    dynamic_target_in_body)));
            }
            e.EmitDynamicCallLookup(tail_call);
            if (possible_return) {
              e.EmitDropCallingFrame();
            }
            e.b(*target_ready);
          });
      b(HS, resolve_natively);
    }
    // Without dynamic code, a target the table doesn't cover goes to the
    // resolve thunk, since its slot would be read from outside the table.
    // Returns, the most common sites, aren't checked.
    Label* slot_ready = nullptr;
    if (!processor()->dynamic_code_enabled() &&
        !(instr->flags & hir::CALL_POSSIBLE_RETURN)) {
      // The table covers the addresses whose top three bits are 100.
      static_assert(A64CodeCache::indirection_guest_base() == 0x80000000 &&
                    A64CodeCache::indirection_guest_size() == 0x1FFFFFFF);
      slot_ready = &NewCachedLabel();
      eor(w14, w16, 0x80000000);
      tst(w14, 0xE0000000);
      const uint64_t resolve_thunk =
          reinterpret_cast<uint64_t>(backend()->resolve_function_thunk());
      Label& uncovered =
          AddToTail([slot_ready, resolve_thunk](A64Emitter& e, Label&) {
            e.mov(e.x9, resolve_thunk);
            e.b(*slot_ready);
          });
      b(NE, uncovered);
    }
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      EmitEncodedIndirectionLookup();
    }
    if (slot_ready) {
      L(*slot_ready);
    }
    if (target_ready) {
      L(*target_ready);
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(w16, target_w);
    mov(x0, x20);  // context
    mov(x1, x16);  // guest address
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address
  }

  if (instr->flags & hir::CALL_TAIL) {
    // Tail call: pass our return address to the callee.
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    // Regular call: pass the next call's return address.
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

// Whether a float or vector value defined before `call` is read after it.
// Registers hold values only within a block, so only this block can.
static bool FpValueLiveAcross(const hir::Instr* call) {
  const auto is_fp = [](const hir::Value* v) {
    return v->type == hir::FLOAT32_TYPE || v->type == hir::FLOAT64_TYPE ||
           v->type == hir::VEC128_TYPE;
  };
  std::unordered_set<const hir::Value*> before;
  for (const hir::Instr* i = call->block->instr_head; i != call; i = i->next) {
    if (i->dest && is_fp(i->dest)) {
      before.insert(i->dest);
    }
  }
  if (before.empty()) {
    return false;
  }
  bool live = false;
  for (hir::Instr* i = call->next; i && !live; i = i->next) {
    i->VisitValueOperands(
        [&](hir::Value* v, uint32_t) { live |= before.count(v) != 0; });
  }
  return live;
}

void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  EnsureFpuFpcrModeForTransition();
  DropPhysicalRemapBound();
  NoteCallClobbersRemapBound();
  // The no-vec thunk skips the q4-q31 save, which is right as long as no
  // value lives in a vector register across the call.
  const uint32_t thunk_offset = static_cast<uint32_t>(
      FpValueLiveAcross(instr)
          ? offsetof(A64BackendContext, guest_to_host_thunk_address)
          : offsetof(A64BackendContext, guest_to_host_thunk_no_vec_address));
  bool undefined = true;
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0, x2=arg1
      // Thunk rearranges to: x0=context, x1=arg0, x2=arg1, calls target
      mov(x0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      mov(x1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      mov(x2, reinterpret_cast<uint64_t>(builtin_function->arg1()));
      ldr(x9, BackendCtxPtr(thunk_offset));
      blr(x9);
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0
      mov(x0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      ldr(x1, ptr(GetContextReg(), static_cast<int32_t>(offsetof(
                                       ppc::PPCContext, kernel_state))));
      ldr(x9, BackendCtxPtr(thunk_offset));
      blr(x9);
    }
  }
  if (undefined) {
    // Set arg0 = function pointer, then call UndefinedCallExtern via thunk.
    mov(x1, reinterpret_cast<uint64_t>(function));
    CallNativeSafe(reinterpret_cast<void*>(&UndefinedCallExtern));
  }
}

void A64Emitter::EmitDynamicCallLookup(bool tail, bool direct) {
  const bool direct_call = direct && !tail;
  Label miss;
  Label done;
  Label check_kind;
  ldr(x14, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, dynamic_call_cache))));
  cbz(x14, miss);
  // Mirrors DynamicCallCacheIndex, which writes the entries this probes.
  lsr(w15, w16, 2);
  eor(w15, w15, w16, LSR, 14);
  and_(w15, w15, kA64DynamicCallCacheSize - 1);
  add(x14, x14, w15, UXTW, 4);
  ldr(w15, ptr(x14, static_cast<uint32_t>(
                        offsetof(A64DynamicCallCacheEntry, guest_address))));
  cmp(w15, w16);
  b(NE, miss);
  if (!direct_call) {
    static_assert(kA64DynamicCallAny == 0);
    ldr(w15, ptr(x14, static_cast<uint32_t>(
                          offsetof(A64DynamicCallCacheEntry, kind))));
    cbnz(w15, check_kind);
  }
  ldr(x9, ptr(x14, static_cast<uint32_t>(
                       offsetof(A64DynamicCallCacheEntry, host_address))));
  // An entry the cache was initialized with holds no address.
  cbz(x9, miss);
  b(done);
  if (!direct_call) {
    L(check_kind);
    cmp(w15, uint32_t(kA64DynamicCallReturnSite));
    b(NE, miss);
    mov(w1, w16);
    CallNativeSafe(reinterpret_cast<void*>(tail ? ResolveCachedReturnSiteTail
                                                : ResolveCachedReturnSiteCall));
    mov(x9, x0);
    b(done);
  }

  L(miss);
  // The host call can clobber x16, so hand the address over as its argument.
  mov(w1, w16);
  CallNativeSafe(reinterpret_cast<void*>(direct_call ? ResolveDirectDynamicCall
                                         : tail      ? ResolveDynamicTailCall
                                                     : ResolveDynamicCall));
  mov(x9, x0);

  L(done);
}

void A64Emitter::EmitDropCallingFrame() {
  // A blr that returns into neither this frame nor its caller enters the target
  // in the caller's frame, as if the caller had tail-called it.
  Label keep_frame;
  // Only a function's entry can take a frame.
  ldr(w10, BackendCtxPtr(offsetof(A64BackendContext, dynamic_target_in_body)));
  cbnz(w10, keep_frame);
  ldr(w10,
      BackendCtxPtr(offsetof(A64BackendContext, current_stackpoint_depth)));
  // This frame, its caller, and the frame the caller returns into.
  cmp(w10, 3);
  b(LO, keep_frame);
  static_assert(sizeof(A64BackendStackpoint) == 16);
  static_assert(offsetof(A64BackendStackpoint, host_stack_) == 0);
  // x11 = &stackpoints[depth - 3], the frame the caller returns into. The
  // caller's record follows it.
  ldr(x11, BackendCtxPtr(offsetof(A64BackendContext, stackpoints)));
  sub(w12, w10, 3);
  add(x11, x11, w12, UXTW, 4);
  ldr(x12, ptr(x11, static_cast<uint32_t>(sizeof(A64BackendStackpoint))));
  // The caller called this frame directly, so its stack pointer is just above
  // this frame.
  if (stack_size() <= 4095) {
    add(x13, sp, static_cast<uint32_t>(stack_size()));
  } else {
    mov(x13, static_cast<uint64_t>(stack_size()));
    add(x13, sp, x13, UXTX);
  }
  cmp(x12, x13);
  b(NE, keep_frame);
  ldr(x13, ptr(x11, static_cast<uint32_t>(0)));
  cmp(x13, x12);
  b(LS, keep_frame);
  // A caller entered from host code returns into host code.
  ldr(w14, ptr(x12, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  mov(w15, kHostReturnAddress);
  cmp(w14, w15);
  b(EQ, keep_frame);
  // Pass the caller's return addresses over and return where it would.
  ldr(x0, ptr(x12, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  ldr(x30, ptr(x12, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  mov(sp, x13);
  sub(w10, w10, 2);
  str(w10,
      BackendCtxPtr(offsetof(A64BackendContext, current_stackpoint_depth)));
  br(x9);
  L(keep_frame);
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNativeSafe(void* fn) {
  DropPhysicalRemapBound();
  NoteCallClobbersRemapBound();
  // Sequences may emit this on a conditional path, so the mode after it is the
  // meet of the call path (Fpu) and the mode on entry.
  const FPCRMode entry_mode = fpcr_mode_;
  EnsureFpuFpcrModeForTransition();
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  ldr(x9,
      BackendCtxPtr(offsetof(A64BackendContext, guest_to_host_thunk_address)));
  blr(x9);
  MergeFpcrModeAfterConditional(entry_mode);
}

void A64Emitter::SetReturnAddress(uint64_t value) {
  mov(x0, value);
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
}

void A64Emitter::ReloadMembase() {
  // Reload x21 from context->virtual_membase.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
}

bool A64Emitter::ChangeFpcrMode(FPCRMode new_mode, bool already_set) {
  if (fpcr_mode_ == new_mode) {
    return false;
  }
  const FPCRMode old_mode = fpcr_mode_;
  fpcr_mode_ = new_mode;
  if (!already_set) {
    // Load the pre-computed FPCR value from the backend context.
    // This avoids an expensive MRS + read-modify-write cycle.
    auto bctx = GetBackendCtxReg();
    uint32_t fpcr_offset;
    if (new_mode == FPCRMode::Vmx) {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_vmx));
    } else if (new_mode == FPCRMode::VmxDaz) {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_vmx_daz));
    } else {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu));
    }

    // SET_NJM only toggles FZ in fpcr_vmx and nothing else writes it, so with
    // NJM on it already equals fpcr_vmx_daz and the two vmx modes are the same
    // bits. Skipping then saves the msr, which is the expensive part.
    const bool vmx_variant_switch =
        IsVmxFpcrMode(old_mode) && IsVmxFpcrMode(new_mode);
    Xbyak_aarch64::Label skip;
    if (vmx_variant_switch) {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, flags))));
      tbnz(w0, kA64BackendNJMOn, skip);
    }
    ldr(w0, Xbyak_aarch64::ptr(bctx, fpcr_offset));
    msr(3, 3, 4, 4, 0, x0);  // msr FPCR, x0
    if (vmx_variant_switch) {
      L(skip);
    }
  }
  return true;
}

Label& A64Emitter::AddToTail(TailEmitCallback callback, uint32_t alignment) {
  TailEmitter tail;
  tail.alignment = alignment;
  tail.func = std::move(callback);
  tail_code_.push_back(std::move(tail));
  return tail_code_.back().label;
}

Label& A64Emitter::NewCachedLabel() {
  auto* label = new Label();
  label_cache_.push_back(label);
  return *label;
}

uint32_t A64Emitter::MapReg(const hir::Value* v, const uint32_t* map, int count,
                            const char* set_name) {
  // reg.index is a signed int32 and is -1 while unassigned, so the unsigned
  // compare catches both "never allocated" and "past the end of the set".
  const uint32_t index = static_cast<uint32_t>(v->reg.index);
  if (index >= static_cast<uint32_t>(count)) {
    XELOGE(
        "A64: value v{} (type {}, def opcode {}) has no {} register assignment "
        "(index {} of {}); codegen would emit a bogus register",
        v->ordinal, static_cast<uint32_t>(v->type),
        v->def ? hir::GetOpcodeName(v->def->GetOpcodeInfo()) : "<none>",
        set_name, index, count);
    assert_always("register allocation missed a value");
    // Clamp so the assembler still produces a decodable instruction.
    return map[0];
  }
  return map[index];
}

void A64Emitter::EmitPreemptCheck(uint32_t guest_address) {
  // Only safe at a block head, where no guest value is live in a register.
  //
  // Tests the preempt flag other threads raise. The cold path clears it, a
  // deferred yield re-sets it.
  Label& after = NewCachedLabel();
  // ldrb/strb unsigned-offset encoding caps at 4095.
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  const uint32_t flag_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  // The thunk returns in Fpu, while the hot path may hold a VMX mode.
  const FPCRMode held_mode = fpcr_mode_;
  Label& do_yield =
      AddToTail([&after, flag_offset, held_mode](A64Emitter& e, Label&) {
        e.strb(e.wzr, ptr(e.x20, flag_offset));
        // Null until the scheduler starts, and a stale flag can reach here
        // after it shuts down, so check before calling.
        e.ldr(e.x0, e.BackendCtxPtr(offsetof(A64BackendContext,
                                             preempt_yield_handler_address)));
        e.ldr(e.x0, ptr(e.x0));
        e.cbz(e.x0, after);
        e.ldr(e.x9, e.BackendCtxPtr(offsetof(A64BackendContext,
                                             guest_to_host_thunk_address)));
        e.blr(e.x9);
        e.NoteCallClobbersRemapBound();
        if (held_mode != FPCRMode::Unknown && held_mode != FPCRMode::Fpu) {
          e.ReloadFpcrMode(held_mode);
        }
        e.b(after);
      });
  if (cvars::log_safepoint_pc && guest_address) {
    // Diagnostic only: costs a materialize + store on every loop back-edge, so
    // it stays off unless a wedge is being chased.
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    mov(w9, guest_address);
    str(w9, ptr(x20, static_cast<uint32_t>(
                         offsetof(ppc::PPCContext, last_safepoint_pc))));
  }
  ldrb(w8, ptr(x20, flag_offset));
  cbnz(w8, do_yield);
  L(after);
}

Label& A64Emitter::GetLabel(uint32_t label_id) {
  auto it = label_map_.find(label_id);
  if (it != label_map_.end()) {
    return *it->second;
  }
  auto* label = new Label();
  label_map_[label_id] = label;
  return *label;
}

// Half of LDR (literal)'s +-1MB reach, leaving room for the item in flight.
static constexpr size_t kV128ConstPoolReach = 512 * 1024;

Label& A64Emitter::GetV128ConstLabel(const vec128_t& value) {
  for (auto& entry : v128_consts_) {
    if (entry.first == value) {
      return *entry.second;
    }
  }
  if (v128_consts_.empty()) {
    v128_consts_first_use_ = getSize();
  }
  auto* label = new Label();
  label_cache_.push_back(label);
  v128_consts_.emplace_back(value, label);
  return *label;
}

bool A64Emitter::FlushV128ConstPool(bool branch_over) {
  if (v128_consts_.empty()) {
    return true;
  }
  Label* skip = nullptr;
  if (branch_over) {
    skip = new Label();
    label_cache_.push_back(skip);
    b(*skip);
  }
  // Functions start 16-byte aligned in the code cache.
  while (getSize() % 16) {
    dd(0);
  }
  try {
    for (auto& entry : v128_consts_) {
      L(*entry.second);
      for (int word = 0; word < 4; ++word) {
        dd(entry.first.u32[word]);
      }
    }
  } catch (const Xbyak_aarch64::Error& e) {
    XELOGE(
        "A64: v128 literal pool out of range in guest function {:08X} "
        "({} constants, {} bytes since the first use): {}",
        current_guest_function_, v128_consts_.size(),
        getSize() - v128_consts_first_use_, e.what());
    return false;
  }
  v128_consts_.clear();
  if (skip) {
    L(*skip);
  }
  return true;
}

bool A64Emitter::MaybeFlushV128ConstPool() {
  if (v128_consts_.empty() ||
      getSize() - v128_consts_first_use_ < kV128ConstPoolReach) {
    return true;
  }
  return FlushV128ConstPool(true);
}

void A64Emitter::HandleStackpointOverflowError(ppc::PPCContext* context) {
  if (debugging::IsDebuggerAttached()) {
    debugging::Break();
  }
  xe::FatalError(
      "Overflowed stackpoints! Please report this error for this title to "
      "Xenia developers.");
}

void A64Emitter::PushStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // x8 = stackpoints array, w9 = current depth
  ldr(x8, ptr(x19,
              static_cast<uint32_t>(offsetof(A64BackendContext, stackpoints))));
  ldr(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));

  // x8 += w9 * sizeof(A64BackendStackpoint) via scaled extended-register add.
  static_assert(sizeof(A64BackendStackpoint) == 16,
                "stackpoint indexing relies on a 16-byte element size");
  add(x8, x8, w9, UXTW, 4);

  // Store host SP.
  mov(x10, sp);
  str(x10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, host_stack_))));
  // Store guest r1 (32-bit).
  ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  str(w10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, guest_stack_))));
  // Store guest LR (32-bit).
  ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));
  str(w10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, guest_return_address_))));

  // Increment depth.
  add(w9, w9, 1);
  str(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));

  // Check for overflow.
  mov(w10, static_cast<uint32_t>(cvars::a64_max_stackpoints));
  cmp(w9, w10);
  auto& overflow_label = AddToTail([](A64Emitter& e, Label& lbl) {
    e.CallNativeSafe(
        reinterpret_cast<void*>(A64Emitter::HandleStackpointOverflowError));
  });
  b(GE, overflow_label);
}

void A64Emitter::PopStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // Decrement current_stackpoint_depth.
  ldr(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
  sub(w8, w8, 1);
  str(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
}

void A64Emitter::EnsureSynchronizedGuestAndHostStack() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // ResolveFunction marks this when it returns a return-site address inside an
  // existing frame. The marker lives in backend context because native SP can
  // still point at a skipped frame here.
  auto& return_from_sync = NewCachedLabel();

  ldr(w16, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  // Bound forward target (adr + b below) — short form is safe.
  cbz_near(w16, return_from_sync);

  auto& sync_label = AddToTail([](A64Emitter& e, Label& lbl) {
    // x8 was set up in the body to point at return_from_sync; do that there
    // instead of here because adr's ±1 MiB range can't span body+tail in
    // large functions.
    //   x8 = return address (where to resume after fixup)
    e.ldr(e.x10, e.BackendCtxPtr(offsetof(
                     A64BackendContext,
                     synchronize_guest_and_host_stack_helper_address)));
    e.br(e.x10);
  });
  adr(x8, return_from_sync);
  b(sync_label);

  L(return_from_sync);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
