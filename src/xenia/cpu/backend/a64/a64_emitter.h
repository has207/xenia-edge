/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
#define XENIA_CPU_BACKEND_A64_A64_EMITTER_H_

#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/arena.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache_base.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#include "xbyak_aarch64.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
using namespace arm64;
class A64Backend;
class A64CodeCache;

// VmxDaz is Vmx with FZ pinned on, for the VMX ops that flush regardless of
// NJM.
enum class FPCRMode : uint32_t { Unknown, Fpu, Vmx, VmxDaz };
inline bool IsVmxFpcrMode(FPCRMode mode) {
  return mode == FPCRMode::Vmx || mode == FPCRMode::VmxDaz;
}

// Unfortunately due to the design of xbyak we have to pass this to the ctor.
class XbyakA64Allocator : public Xbyak_aarch64::Allocator {
 public:
  virtual bool useProtect() const { return false; }
};

class A64Emitter;
using TailEmitCallback =
    std::function<void(A64Emitter& e, Xbyak_aarch64::Label& lbl)>;
struct TailEmitter {
  Xbyak_aarch64::Label label;
  uint32_t alignment;
  TailEmitCallback func;
};

class A64Emitter : public Xbyak_aarch64::CodeGenerator {
 public:
  A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator);
  virtual ~A64Emitter();

  Processor* processor() const { return processor_; }
  A64Backend* backend() const { return backend_; }

  bool Emit(GuestFunction* function, hir::HIRBuilder* builder,
            uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
            void** out_code_address, size_t* out_code_size,
            std::vector<SourceMapEntry>* out_source_map);

 public:
  // Reserved: sp, x19 (backend context), x20 (context), x21 (membase)
  // Scratch: x0-x18 (caller-saved), v0-v3
  // Available GPRs for register allocator: x22-x28
  static constexpr int GPR_COUNT = 7;
  // Available VEC regs: v4-v15, v16-v31
  static constexpr int VEC_COUNT = 28;
  static constexpr size_t kStashOffset = 32;

  // A value that reached codegen without a register assignment indexes the maps
  // out of bounds, which otherwise surfaces far away as an unencodable-register
  // exception. Report the culprit here, where it is still in hand.
  static uint32_t MapReg(const hir::Value* v, const uint32_t* map, int count,
                         const char* set_name);

  static void SetupReg(const hir::Value* v, Xbyak_aarch64::WReg& r) {
    r = Xbyak_aarch64::WReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::XReg& r) {
    r = Xbyak_aarch64::XReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::SReg& r) {
    r = Xbyak_aarch64::SReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::DReg& r) {
    r = Xbyak_aarch64::DReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::QReg& r) {
    r = Xbyak_aarch64::QReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::VReg& r) {
    r = Xbyak_aarch64::VReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }

  void MarkSourceOffset(const hir::Instr* i);

  // Called from SelectSequence once a sequence has emitted. Cheap no-op unless
  // this function is being counted.
  void RecordSequenceSample(const hir::Instr* i, uint32_t backend_key,
                            uint32_t host_bytes);

  void DebugBreak();
  void Trap(uint16_t trap_type = 0);
  void UnimplementedInstr(const hir::Instr* i);

  void Call(const hir::Instr* instr, GuestFunction* function);
  void CallIndirect(const hir::Instr* instr, int reg_index);
  // w16 = guest address in, x9 = host target out; clobbers x14/x15.
  void EmitEncodedIndirectionLookup();
  void CallExtern(const hir::Instr* instr, const Function* function);
  // Emits a PPC __savegprlr_N/__restgprlr_N helper body inline instead of
  // calling it. Returns false when the callee is not a GPR saverest helper.
  bool TryInlinePPCGprLrSaveRestore(const hir::Instr* instr,
                                    const GuestFunction* function);
  bool InlinesPPCGprLrSaveRestore(const hir::Instr* instr,
                                  const GuestFunction* function) const;
  // Whether |instr| calls or branches to guest code, a saverest helper inlined
  // in place aside, or calls host code, traps included.
  bool MakesCall(const hir::Instr* instr) const;
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  // Loads the host address for the guest address in w16 into x9. A direct bl,
  // |direct| without |tail|, takes any entry. Other lookups take Any entries
  // and check a return site's for a live frame.
  void EmitDynamicCallLookup(bool tail, bool direct = false);
  // Jumps to the dynamic blr target in x9 on the caller's frame, or falls
  // through when the records don't allow that.
  void EmitDropCallingFrame();
  void SetReturnAddress(uint64_t value);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Operand for an A64BackendContext field; offset comes from offsetof.
  Xbyak_aarch64::AdrUimm BackendCtxPtr(size_t offset) const {
    return Xbyak_aarch64::ptr(x19, static_cast<uint32_t>(offset));
  }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  static void HandleStackpointOverflowError(ppc::PPCContext* context);

  void MergeFpcrModeAfterConditional(FPCRMode skip_path_mode) {
    if (fpcr_mode_ != skip_path_mode) {
      fpcr_mode_ = UntrackedFpcrMode();
    }
  }
  FPCRMode fpcr_mode() const { return fpcr_mode_; }
  void ForgetFpcrMode() {
    if (IsVmxFpcrMode(fpcr_mode_)) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = UntrackedFpcrMode();
  }
  // The mode to assume where the tracker cannot follow the paths in, such as
  // a loop header. Only a function that touches VEC128 ever enters a VMX
  // mode, so in any other the FPCR holds the Fpu mode throughout.
  FPCRMode UntrackedFpcrMode() const {
    return function_has_vmx_ ? FPCRMode::Unknown : FPCRMode::Fpu;
  }
  // For cold paths whose host call clobbered the mode the tracker still holds.
  void ReloadFpcrMode(FPCRMode mode) {
    fpcr_mode_ = FPCRMode::Unknown;
    ChangeFpcrMode(mode);
    fpcr_mode_ = mode;
  }
  // Every guest function is entered and left, and every host call made, in
  // the scalar (Fpu) mode. Unknown may be VMX at runtime, but only in a
  // function that touches VEC128.
  void EnsureFpuFpcrModeForTransition() {
    if (IsVmxFpcrMode(fpcr_mode_) ||
        (fpcr_mode_ == FPCRMode::Unknown && function_has_vmx_)) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Fpu;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

  // The label stays valid for the whole function, even as more tails are
  // added, and a tail's callback may add tails of its own.
  Xbyak_aarch64::Label& AddToTail(TailEmitCallback callback,
                                  uint32_t alignment = 0);
  Xbyak_aarch64::Label& NewCachedLabel();

  // Emits a cooperative-scheduler preemption safepoint: yields the fiber once
  // the context's preempt_requested flag is raised. Only valid at a block head.
  // guest_address is stamped into the context for wedge diagnosis when
  // log_safepoint_pc is on; 0 means unknown.
  void EmitPreemptCheck(uint32_t guest_address = 0);

  // ARM64 conditional branches (cbz/cbnz: ±1 MiB, tbz/tbnz: ±32 KiB,
  // b.cond: ±1 MiB) can fall short of their target in large guest functions.
  // These shadows emit the safe pattern `<inverse> skip; b target; skip:`,
  // routing the long branch through unconditional b (±128 MiB). The
  // int64_t-immediate overloads remain available via the using-declarations
  // for hand-tuned thunks that pass literal byte offsets.
  //
  // A single direct branch is emitted whenever the target is provably in
  // range: when the whole function is (near_tail_branches_safe_ and
  // near_tbz_branches_safe_), and otherwise when the label is already bound
  // (backward branch, e.g. a loop back-edge) and the distance is known. This
  // halves the hottest branches in guest code and keeps the natural
  // taken/not-taken polarity for the branch predictor.
  using Xbyak_aarch64::CodeGenerator::b;
  using Xbyak_aarch64::CodeGenerator::cbnz;
  using Xbyak_aarch64::CodeGenerator::cbz;
  using Xbyak_aarch64::CodeGenerator::tbnz;
  using Xbyak_aarch64::CodeGenerator::tbz;
  void b(const Xbyak_aarch64::Label& label) {
    CheckRemapKeptBranch(label);
    CodeGenerator::b(label);
  }
  void b(const Xbyak_aarch64::Cond cond, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);

  // Single-instruction conditional branches for forward targets that are
  // PROVABLY within range because the label is bound a bounded number of
  // instructions later within the same sequence/helper emission (e.g.
  // intra-sequence fast-path skips). Callers must guarantee the bound:
  // ±1 MiB for b_near/cbz_near/cbnz_near. Backward targets are handled
  // automatically by the shadows above.
  void b_near(const Xbyak_aarch64::Cond cond,
              const Xbyak_aarch64::Label& label) {
    CodeGenerator::b(cond, label);
  }
  void cbz_near(const Xbyak_aarch64::WReg& rt,
                const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbz(rt, label);
  }
  void cbz_near(const Xbyak_aarch64::XReg& rt,
                const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbz(rt, label);
  }
  void cbnz_near(const Xbyak_aarch64::WReg& rt,
                 const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbnz(rt, label);
  }
  void cbnz_near(const Xbyak_aarch64::XReg& rt,
                 const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbnz(rt, label);
  }

  // +/-32 KiB only; guard with near_tbz_branches_safe_.
  void tbnz_near(const Xbyak_aarch64::WReg& rt, uint32_t bit,
                 const Xbyak_aarch64::Label& label) {
    CodeGenerator::tbnz(rt, bit, label);
  }

  // Shadow of CodeGenerator::L that records the bind offset so later
  // branches to this label can be emitted in single-instruction form.
  void L(Xbyak_aarch64::Label& label) {
    CodeGenerator::L(label);
    label_bind_offsets_.emplace(label.getId(), getSize());
    // A label may be reached from code that never loaded the bound.
    DropPhysicalRemapBound();
  }
  // Binds a label that is reached only by branches emitted in the state the
  // emitter is in now, through code that leaves w7 alone: the skip after a
  // long branch, or the return from a tail stub that works in scratch
  // registers. The physical remap bound stays valid across it.
  void LKeepingRemapBound(Xbyak_aarch64::Label& label) {
    CodeGenerator::L(label);
    label_bind_offsets_.emplace(label.getId(), getSize());
    remap_kept_label_ids_.insert(label.getId());
  }
  // A call clobbers w7, so tail code that made one must not return through a
  // label that kept the bound. Emit fails the function if it does.
  void NoteCallClobbersRemapBound() { tail_item_called_ = true; }
  void CheckRemapKeptBranch(const Xbyak_aarch64::Label& label) {
    if (emitting_tail_ && tail_item_called_ &&
        remap_kept_label_ids_.count(label.getId())) {
      remap_kept_branch_after_call_ = true;
    }
  }

  // A producer that emits nothing hands the next sequence the register it
  // should have read and a 32-bit mask to fold into the address it computes
  // anyway. Keyed on the register the producer would have written; cleared
  // on read. The very next sequence must read it, or Emit fails the function.
  void MarkFusedAddressMask(int dest_reg, int src_reg, uint32_t mask) {
    fused_addr_mask_dest_reg_ = dest_reg;
    fused_addr_mask_src_reg_ = src_reg;
    fused_addr_mask_imm_ = mask;
  }
  bool ConsumeFusedAddressMask(int dest_reg, int* out_src_reg,
                               uint32_t* out_mask) {
    if (dest_reg < 0 || fused_addr_mask_dest_reg_ != dest_reg) {
      return false;
    }
    *out_src_reg = fused_addr_mask_src_reg_;
    *out_mask = fused_addr_mask_imm_;
    fused_addr_mask_dest_reg_ = -1;
    return true;
  }

  // A compare whose only reader is the branch right after it makes no
  // boolean: the branch reads the flags the compare set. Keyed on the
  // register the cset would have written; cleared on read. Only instructions
  // that PassesHandoffs may sit between the two, and the branch must take it,
  // or Emit fails the function.
  void MarkFusedCompareBranch(int dest_reg, Xbyak_aarch64::Cond cond) {
    fused_cmp_branch_reg_ = dest_reg;
    fused_cmp_branch_cond_ = cond;
  }
  bool ConsumeFusedCompareBranch(int dest_reg, Xbyak_aarch64::Cond* out_cond) {
    if (dest_reg < 0 || fused_cmp_branch_reg_ != dest_reg) {
      return false;
    }
    *out_cond = fused_cmp_branch_cond_;
    fused_cmp_branch_reg_ = -1;
    return true;
  }

  // A TO_SINGLE_KEEP_NAN reading the result of the F64 operation right before
  // it leaves the NaN to that operation, when it tests its result anyway: the
  // operation's NaN exits hand the NaN to the TO_SINGLE's dest and jump to
  // exit, past the conversion, where the TO_SINGLE binds it. Keyed on the
  // TO_SINGLE; cleared on read. It must take it, or Emit fails the function.
  void MarkFusedToSingle(const hir::Instr* to_single,
                         Xbyak_aarch64::Label* exit) {
    fused_to_single_ = to_single;
    fused_to_single_exit_ = exit;
  }
  const hir::Instr* fused_to_single() const { return fused_to_single_; }
  Xbyak_aarch64::Label* fused_to_single_exit() const {
    return fused_to_single_exit_;
  }
  Xbyak_aarch64::Label* ConsumeFusedToSingle(const hir::Instr* to_single) {
    if (!fused_to_single_ || fused_to_single_ != to_single) {
      return nullptr;
    }
    fused_to_single_ = nullptr;
    return fused_to_single_exit_;
  }

  // SOURCE_OFFSET, CONTEXT_BARRIER and NOP emit no code that touches NZCV
  // or a register a required handoff names (SOURCE_OFFSET's coverage
  // counter uses add, not adds, and only x0/x1). COMMENT does not pass: it
  // calls TraceString when instruction tracing is on.
  static bool PassesHandoffs(const hir::Instr* instr) {
    switch (instr->GetOpcodeNum()) {
      case hir::OPCODE_SOURCE_OFFSET:
      case hir::OPCODE_CONTEXT_BARRIER:
      case hir::OPCODE_NOP:
        return true;
      default:
        return false;
    }
  }

  // A guest call writes its return address twice from the same immediate:
  // into the host stack slot and into the guest link register. The first
  // leaves it in x0, and `reader`, the HIR instruction right after it, may
  // store it from there. Nothing is emitted between two adjacent sequences.
  void MarkX0HoldsConstant(const hir::Instr* reader, uint64_t value) {
    x0_constant_reader_ = reader;
    x0_constant_ = value;
  }
  bool X0HoldsConstant(const hir::Instr* reader, uint64_t value) const {
    return reader == x0_constant_reader_ && value == x0_constant_;
  }

  // w7 holds 0xE0000000 for the physical remap from its first use until the
  // next host call or label, across a block boundary only when every edge
  // into the block carries it. Nothing else in the backend uses w7.
  bool physical_remap_bound_valid() const {
    return physical_remap_bound_valid_;
  }
  void set_physical_remap_bound_valid() { physical_remap_bound_valid_ = true; }
  void DropPhysicalRemapBound() { physical_remap_bound_valid_ = false; }

  // Whether this function translates guest addresses through the context's
  // tables rather than the membase. Decided when the function is emitted.
  bool guest_tlb() const { return guest_tlb_; }

  // Get or create a xbyak_aarch64 label for a HIR label ID.
  Xbyak_aarch64::Label& GetLabel(uint32_t label_id);

  // Get or create a pool slot for a v128 constant with no immediate form.
  Xbyak_aarch64::Label& GetV128ConstLabel(const vec128_t& value);

  XexModule* GuestModule() { return guest_module_; }

 protected:
  void* Emplace(const EmitFunctionInfo& func_info,
                GuestFunction* function = nullptr);
  // Drops the code buffer, tail entries and both label pools. Both the success
  // path and a failed compile must run it, or stale labels carry over.
  void ResetPerFunctionState();
  bool Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info);

  // Emit the pending v128 literals, branching over them when code follows.
  bool FlushV128ConstPool(bool branch_over);
  // Plant an island, only safe where a branch and data cannot split a sequence.
  bool MaybeFlushV128ConstPool();

 protected:
  Processor* processor_ = nullptr;
  A64Backend* backend_ = nullptr;
  A64CodeCache* code_cache_ = nullptr;
  XbyakA64Allocator* allocator_ = nullptr;
  XexModule* guest_module_ = nullptr;
  uint64_t feature_flags_ = 0;
  uint32_t current_guest_function_ = 0;
  bool guest_tlb_ = false;

  Xbyak_aarch64::Label* epilog_label_ = nullptr;

  hir::Instr* current_instr_ = nullptr;

  FunctionDebugInfo* debug_info_ = nullptr;
  uint32_t debug_info_flags_ = 0;
  size_t coverage_offset_ = 0;
  uint32_t coverage_start_address_ = 0;
  uint32_t coverage_instruction_count_ = 0;
  uint32_t coverage_current_index_ = UINT32_MAX;
  bool coverage_out_of_range_ = false;
  std::vector<SequenceSample> sequence_samples_;
  Arena source_map_arena_;

  size_t stack_size_ = 0;
  // Whether every branch within the function, to the epilog and the tail
  // included, is provably within b.cond/cbnz reach (+/-1 MiB), and within
  // tbz/tbnz reach (+/-32 KiB).
  bool near_tail_branches_safe_ = false;
  bool near_tbz_branches_safe_ = false;
  // Whether the near-branch flags come from a typical code size rather than
  // the largest one, so a branch out of range calls for another try.
  bool estimate_typical_code_size_ = false;
  bool IsRetryableBranchRangeError(const Xbyak_aarch64::Error& e) const {
    return estimate_typical_code_size_ &&
           static_cast<int>(e) == Xbyak_aarch64::ERR_LABEL_IS_TOO_FAR;
  }

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  // A deque, so adding a tail doesn't move the labels already handed out
  std::deque<TailEmitter> tail_code_;
  std::vector<Xbyak_aarch64::Label*> label_cache_;

  // v128 constants needing a literal, with labels owned by label_cache_.
  std::vector<std::pair<vec128_t, Xbyak_aarch64::Label*>> v128_consts_;
  // Code offset of the ldr that opened the pending pool.
  size_t v128_consts_first_use_ = 0;

  // Map from HIR label IDs to xbyak_aarch64 Labels.
  std::unordered_map<uint32_t, Xbyak_aarch64::Label*> label_map_;

  // Byte offsets at which labels were bound (keyed by xbyak label id).
  // Used to emit short-form backward branches when the distance is known
  // to be in range. Must be cleared whenever the code generator is reset
  // (xbyak reuses label ids after reset()).
  std::unordered_map<int, size_t> label_bind_offsets_;
  // Labels bound with LKeepingRemapBound, keyed like label_bind_offsets_.
  std::unordered_set<int> remap_kept_label_ids_;
  bool emitting_tail_ = false;
  bool tail_item_called_ = false;
  bool remap_kept_branch_after_call_ = false;

  // True if `label` is bound at most `max_backward_bytes` behind the
  // current emission offset.
  bool IsBoundLabelInRange(const Xbyak_aarch64::Label& label,
                           int64_t max_backward_bytes) const {
    const int id = label.getId();
    if (id == 0) {
      return false;
    }
    const auto it = label_bind_offsets_.find(id);
    if (it == label_bind_offsets_.end()) {
      return false;
    }
    const int64_t distance =
        static_cast<int64_t>(it->second) - static_cast<int64_t>(getSize());
    return distance >= -max_backward_bytes;
  }

  // Conservative reach limits (exact architectural ranges are ±1 MiB for
  // b.cond/cbz/cbnz and ±32 KiB for tbz/tbnz; leave one instruction of
  // margin).
  static constexpr int64_t kCondBranchBackwardRange = (1ll << 20) - 8;
  static constexpr int64_t kTestBranchBackwardRange = (1ll << 15) - 8;

  FPCRMode fpcr_mode_ = FPCRMode::Unknown;
  bool function_has_vmx_ = false;
  // Whether this function records a stackpoint. One that makes no call has no
  // return sites, so no unwind ever restores its frame, and it is never below
  // another frame, so a pseudo stack trace, taken from the frames that called
  // out, has nothing to show for it either. Every tail branch, and so every
  // dynamic one, leaves from a frame that has its record.
  bool function_has_stackpoint_ = true;
  // FPCR tracking across blocks: lattice {Unknown, Fpu, Vmx, VmxDaz}, where
  // meet(a, b) = a if a == b else Unknown. A block starts in the meet of its
  // incoming edges once all of them have been emitted, else Unknown. The
  // remap bound in w7 is tracked the same way: a block starts with it loaded
  // only when every incoming edge left it loaded.
  struct IncomingState {
    FPCRMode fpcr_meet = FPCRMode::Unknown;
    bool remap_bound_valid = false;
    uint32_t count = 0;
  };
  std::unordered_map<const hir::Block*, uint32_t> expected_preds_;
  std::unordered_map<const hir::Block*, IncomingState> incoming_state_;
  void RecordIncomingState(const hir::Block* target, FPCRMode mode,
                           bool remap_bound_valid) {
    auto& in = incoming_state_[target];
    if (in.count == 0) {
      in.fpcr_meet = mode;
      in.remap_bound_valid = remap_bound_valid;
    } else {
      if (in.fpcr_meet != mode) {
        in.fpcr_meet = FPCRMode::Unknown;
      }
      in.remap_bound_valid = in.remap_bound_valid && remap_bound_valid;
    }
    ++in.count;
  }
  // An edge leaving from where the emitter is now.
  void RecordIncomingState(const hir::Block* target) {
    RecordIncomingState(target, fpcr_mode_, physical_remap_bound_valid_);
  }
  bool physical_remap_bound_valid_ = false;
  // Handoffs between adjacent sequences; see MarkFusedAddressMask,
  // MarkFusedCompareBranch and MarkFusedToSingle.
  void ResetSequenceHandoffs() {
    fused_addr_mask_dest_reg_ = -1;
    fused_cmp_branch_reg_ = -1;
    fused_to_single_ = nullptr;
    x0_constant_reader_ = nullptr;
  }
  bool sequence_handoff_pending() const {
    return fused_addr_mask_dest_reg_ >= 0 || fused_cmp_branch_reg_ >= 0 ||
           fused_to_single_;
  }
  int fused_addr_mask_dest_reg_ = -1;
  int fused_addr_mask_src_reg_ = -1;
  uint32_t fused_addr_mask_imm_ = 0;
  int fused_cmp_branch_reg_ = -1;
  Xbyak_aarch64::Cond fused_cmp_branch_cond_ = Xbyak_aarch64::EQ;
  const hir::Instr* fused_to_single_ = nullptr;
  Xbyak_aarch64::Label* fused_to_single_exit_ = nullptr;
  const hir::Instr* x0_constant_reader_ = nullptr;
  uint64_t x0_constant_ = 0;
  bool synchronize_stack_on_next_instruction_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
