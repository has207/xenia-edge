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

#include <functional>
#include <unordered_map>
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
  // True when branches to tail labels may use the single-instruction near
  // forms (see Emit); sequences use this to place cold paths out of line.
  bool near_tail_branches_safe() const { return near_tail_branches_safe_; }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }

  // NZCV fusion between adjacent HIR instructions. The classic producer is
  // ANDS (condition NE); a producer may instead declare any condition code
  // that is true at runtime exactly when the register is nonzero, on every
  // path reaching the next instruction.
  void DeclareFlagsZeroTest(int gpr_reg, bool is64) {
    DeclareFlagsNonzeroCond(gpr_reg, is64, Xbyak_aarch64::NE);
  }
  void DeclareFlagsNonzeroCond(int gpr_reg, bool is64,
                               Xbyak_aarch64::Cond cond) {
    flags_zero_fresh_reg_ = gpr_reg;
    flags_zero_fresh_is64_ = is64;
    flags_zero_fresh_cond_ = cond;
  }
  bool FlagsNonzeroCondHeld(int gpr_reg, bool is64,
                            Xbyak_aarch64::Cond* out_cond) const {
    if (flags_zero_armed_reg_ == gpr_reg && flags_zero_armed_is64_ == is64 &&
        gpr_reg >= 0) {
      *out_cond = flags_zero_armed_cond_;
      return true;
    }
    return false;
  }
  bool FlagsHoldZeroTest(int gpr_reg, bool is64) const {
    return flags_zero_armed_reg_ == gpr_reg && flags_zero_armed_is64_ == is64 &&
           gpr_reg >= 0 && flags_zero_armed_cond_ == Xbyak_aarch64::NE;
  }
  // Reset and Shift govern every adjacent-sequence handoff, not just the
  // flags: the w16 forwarding below rides on the same fresh/armed pair.
  void ResetFlagsZeroTest() {
    flags_zero_fresh_reg_ = flags_zero_armed_reg_ = -1;
    w16_holds_fresh_ = w16_holds_armed_ = nullptr;
  }
  void ShiftFlagsZeroTest() {
    flags_zero_armed_reg_ = flags_zero_fresh_reg_;
    flags_zero_armed_is64_ = flags_zero_fresh_is64_;
    flags_zero_armed_cond_ = flags_zero_fresh_cond_;
    flags_zero_fresh_reg_ = -1;
    w16_holds_armed_ = w16_holds_fresh_;
    w16_holds_fresh_ = nullptr;
  }

  // One-shot forwarding of an indirect-call target into w16: LOAD_CONTEXT
  // declares that it loaded this HIR value directly into w16, and only the
  // immediately following instruction may consume it (same shift/reset rules
  // as the flags fusion; nothing between the two can touch w16).
  void DeclareW16Holds(const hir::Value* value) { w16_holds_fresh_ = value; }
  bool W16Holds(const hir::Value* value) const {
    return value && w16_holds_armed_ == value;
  }

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
  // Shared by Call and CallIndirect: w16 (guest address) -> x9 (host target).
  void EmitEncodedIndirectionLookup();
  void CallExtern(const hir::Instr* instr, const Function* function);
  // Emits a PPC __savegprlr_N/__restgprlr_N helper body inline instead of
  // calling it. Returns false when the callee is not a GPR saverest helper.
  bool TryInlinePPCGprLrSaveRestore(const hir::Instr* instr,
                                    const GuestFunction* function);
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  void SetReturnAddress(uint64_t value);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  // After a conditional region (TRAP_TRUE / CALL_*_TRUE), the taken path's
  // tracker state must meet the skip path's entry state: keep it only when
  // both agree.
  void MergeFpcrModeAfterConditional(FPCRMode skip_path_mode) {
    if (fpcr_mode_ != skip_path_mode) {
      fpcr_mode_ = FPCRMode::Unknown;
    }
  }
  FPCRMode fpcr_mode() const { return fpcr_mode_; }
  // Switches to Fpu if a VMX mode is tracked and drops to Unknown. Only
  // SET_NJM needs this: it rewrites the VMX FPCR images, so a tracked VMX
  // mode no longer describes the register.
  void ForgetFpcrMode() {
    if (IsVmxFpcrMode(fpcr_mode_)) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Unknown;
  }
  // Re-emits the switch to |mode| even though the tracker already claims it:
  // for a cold path whose host call clobbered FPCR at runtime while the hot
  // path's static mode stays |mode|.
  void ReloadFpcrMode(FPCRMode mode) {
    fpcr_mode_ = FPCRMode::Unknown;
    ChangeFpcrMode(mode);
    fpcr_mode_ = mode;
  }
  // Host and cross-function transitions must run in FPU mode. The switch is
  // emitted when a VMX mode is tracked, and also when the tracker holds
  // Unknown in a function that touches VEC128 at all, since Unknown may be
  // VMX at runtime; a function with no VEC128 can never be in a VMX mode and
  // skips the guard entirely.
  void EnsureFpuFpcrModeForTransition() {
    if (IsVmxFpcrMode(fpcr_mode_) ||
        (fpcr_mode_ == FPCRMode::Unknown && function_has_vmx_)) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    // The transition this guards returns with FPCR back in the scalar FPU
    // state: a guest callee restores it before returning, and host calls
    // come back through the guest-to-host or resolve thunk, both of which
    // reload fpcr_fpu. The post-call code may rely on that.
    fpcr_mode_ = FPCRMode::Fpu;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

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
  // When the target label is already bound (backward branch — e.g. loop
  // back-edges) the distance is known exactly, so a single direct branch is
  // emitted whenever it is in range. This halves the hottest branches in
  // guest code and keeps the natural taken/not-taken polarity for the
  // branch predictor.
  using Xbyak_aarch64::CodeGenerator::b;
  using Xbyak_aarch64::CodeGenerator::cbnz;
  using Xbyak_aarch64::CodeGenerator::cbz;
  using Xbyak_aarch64::CodeGenerator::tbnz;
  using Xbyak_aarch64::CodeGenerator::tbz;
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
  }

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
  // True when every tail label of the current function provably sits within
  // the +/-1 MiB reach of cbnz/b.cond (set per function in Emit).
  bool near_tail_branches_safe_ = false;
  // Same, for tbnz's +/-32 KiB reach.
  bool near_tbz_branches_safe_ = false;
  // NZCV fusion: the previous HIR instruction's sequence declared that the
  // flags currently hold a zero-test of this GPR (ANDS). -1 = nothing.
  // `fresh` is what the current sequence declares; the emit loop shifts it
  // into `armed` between instructions, so `armed` can never leak past one
  // instruction, a label bind, or a block boundary.
  int flags_zero_fresh_reg_ = -1;
  Xbyak_aarch64::Cond flags_zero_fresh_cond_ = Xbyak_aarch64::NE;
  Xbyak_aarch64::Cond flags_zero_armed_cond_ = Xbyak_aarch64::NE;
  bool flags_zero_fresh_is64_ = false;
  int flags_zero_armed_reg_ = -1;
  bool flags_zero_armed_is64_ = false;
  const hir::Value* w16_holds_fresh_ = nullptr;
  const hir::Value* w16_holds_armed_ = nullptr;

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  std::vector<TailEmitter> tail_code_;
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
  // Whether the current function contains any VEC128-typed instruction (set
  // per function in Emit); gates the Unknown-mode transition guard.
  bool function_has_vmx_ = false;
  // FPCR tracking contract. Lattice {Unknown, Fpu, Vmx, VmxDaz} with
  // meet(a, b) = a if a == b else Unknown. Every guest function is entered
  // in Fpu: the host-to-guest thunk restores fpcr_fpu on entry, every call
  // site runs EnsureFpuFpcrModeForTransition, every callee returns in Fpu,
  // the guest-to-host thunks restore fpcr_fpu after host callbacks and the
  // resolve thunk restores it after the host resolve. Within a function,
  // every branch records the tracker's mode at the branch point on its
  // target's incoming edges, block ends record the fall-through mode unless
  // the last instruction is an unconditional BRANCH, and the function entry
  // records Fpu on the first block; a block is seeded with the meet only
  // when its recorded count equals the expected count from the pre-scan,
  // otherwise it starts Unknown. RETURN/RETURN_TRUE do not participate and
  // the epilog has no guard: PPC blr lowers to CALL_INDIRECT, which carries
  // the guard, so they are unreachable from the frontend today.
  // FPCR seeding state, per function: how many predecessor edges each block
  // expects (from the final-HIR pre-scan), the meet of the modes recorded so
  // far on its incoming edges, and the authoritative label->block mapping.
  struct IncomingFpcr {
    FPCRMode meet = FPCRMode::Unknown;
    uint32_t count = 0;
  };
  std::unordered_map<const hir::Block*, uint32_t> expected_preds_;
  std::unordered_map<const hir::Block*, IncomingFpcr> incoming_fpcr_;
  std::unordered_map<const hir::Label*, const hir::Block*> label_block_;
  void RecordIncomingFpcr(const hir::Block* target, FPCRMode mode) {
    auto& in = incoming_fpcr_[target];
    if (in.count == 0) {
      in.meet = mode;
    } else if (in.meet != mode) {
      in.meet = FPCRMode::Unknown;
    }
    ++in.count;
  }
  bool synchronize_stack_on_next_instruction_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
