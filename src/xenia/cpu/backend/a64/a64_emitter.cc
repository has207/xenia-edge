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
#include <string>

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
#include "xenia/cpu/thread_state.h"

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

  // Reset state.
  stack_size_ = StackLayout::GUEST_STACK_SIZE;
  source_map_arena_.Reset();
  tail_code_.clear();
  label_bind_offsets_.clear();
  fpcr_mode_ = FPCRMode::Unknown;

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
    // Reset FPCR tracking on each block entry (we don't know which
    // predecessor ran, so mode is unknown).
    ForgetFpcrMode();

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      L(GetLabel(label->id));
      label = label->next;
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute.
      if (synchronize_stack_on_next_instruction_) {
        if (instr->GetOpcodeNum() != hir::OPCODE_SOURCE_OFFSET) {
          synchronize_stack_on_next_instruction_ = false;
          EnsureSynchronizedGuestAndHostStack();
        }
      }
      const hir::Instr* new_tail = instr;
      bool selected = false;
      try {
        selected = SelectSequence(this, instr, &new_tail);
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
      instr = new_tail;
    }

    if (!MaybeFlushV128ConstPool()) {
      return false;
    }

    block = block->next;
  }

  // ========================================================================
  // EPILOG
  // ========================================================================
  L(*epilog_label_);
  epilog_label_ = nullptr;
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
  for (auto& tail_item : tail_code_) {
    // ARM64 instructions are always 4-byte aligned, so alignment is mostly
    // a no-op unless we want cache-line alignment for hot paths.
    L(tail_item.label);
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
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::b(cond, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::b(static_cast<Xbyak_aarch64::Cond>(cond ^ 1), skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::WReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::XReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::WReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::XReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  XELOGE("A64: Unimplemented HIR instruction: {}",
         hir::GetOpcodeName(i->GetOpcodeInfo()));
  DebugBreak();
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  ForgetFpcrMode();
  if (TryInlinePPCGprLrSaveRestore(instr, function)) {
    return;
  }

  auto fn = static_cast<A64Function*>(function);

  if (fn->machine_code()) {
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

  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    mov(w16, function->address());
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format.
      Label external_target;
      Label indirection_ready;

      mov(x14, code_cache_->indirection_table_base_bias());
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));
      tbnz(w9, 31, external_target);

      // Internal: rel32 from code cache base.
      mov(x14, code_cache_->execute_base_address());
      add(x9, x14, w9, UXTW);
      b(indirection_ready);

      // External: tagged index into the side table.
      L(external_target);
      and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x14, code_cache_->external_indirection_table_base_address());
      lsl(x15, x15, 3);
      add(x14, x14, x15);
      ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

      L(indirection_ready);
    }
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
  if (!function->IsSaverest() ||
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
  ForgetFpcrMode();
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
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format.
      Label external_target;
      Label indirection_ready;

      mov(x14, code_cache_->indirection_table_base_bias());
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));
      tbnz(w9, 31, external_target);

      // Internal: rel32 from code cache base.
      mov(x14, code_cache_->execute_base_address());
      add(x9, x14, w9, UXTW);
      b(indirection_ready);

      // External: tagged index into the side table.
      L(external_target);
      and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x14, code_cache_->external_indirection_table_base_address());
      lsl(x15, x15, 3);
      add(x14, x14, x15);
      ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

      L(indirection_ready);
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

void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  ForgetFpcrMode();
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
      mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
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
      mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
      blr(x9);
    }
  }
  if (undefined) {
    // Set arg0 = function pointer, then call UndefinedCallExtern via thunk.
    mov(x1, reinterpret_cast<uint64_t>(function));
    CallNativeSafe(reinterpret_cast<void*>(&UndefinedCallExtern));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNativeSafe(void* fn) {
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
  blr(x9);
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
  // Only safe at a block head, where the per-block register allocator leaves no
  // guest value live and ForgetFpcrMode has already run, so the unannounced
  // guest->host call cannot lose a register or desync the mode tracking.
  //
  // Tests the preempt flag other threads raise. The cold path clears it, a
  // deferred yield re-sets it.
  Label& after = NewCachedLabel();
  // ldrb/strb unsigned-offset encoding caps at 4095.
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  const uint32_t flag_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  Label& do_yield = AddToTail([&after, flag_offset](A64Emitter& e, Label&) {
    e.strb(e.wzr, ptr(e.x20, flag_offset));
    // Null until the scheduler starts, and a stale flag can reach here after
    // it shuts down, so check before calling.
    e.mov(e.x0,
          reinterpret_cast<uint64_t>(&xe::cpu::backend::preempt_yield_handler));
    e.ldr(e.x0, ptr(e.x0));
    e.cbz(e.x0, after);
    e.mov(e.x9, reinterpret_cast<uint64_t>(e.backend()->guest_to_host_thunk()));
    e.blr(e.x9);
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
    e.mov(e.x10, reinterpret_cast<uint64_t>(
                     e.backend()->synchronize_guest_and_host_stack_helper()));
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
