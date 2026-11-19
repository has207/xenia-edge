/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_HIR_BUILDER_H_
#define XENIA_CPU_PPC_PPC_HIR_BUILDER_H_

#include <initializer_list>

#include "xenia/base/string_buffer.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace ppc {

struct PPCBuiltins;
class PPCFrontend;

class PPCHIRBuilder : public hir::HIRBuilder {
  using Instr = xe::cpu::hir::Instr;
  using Label = xe::cpu::hir::Label;
  using Value = xe::cpu::hir::Value;

 public:
  explicit PPCHIRBuilder(PPCFrontend* frontend);
  ~PPCHIRBuilder() override;

  PPCBuiltins* builtins() const;

  void Reset() override;

  enum EmitFlags {
    // Emit comment nodes.
    EMIT_DEBUG_COMMENTS = 1 << 0,
  };
  bool Emit(GuestFunction* function, uint32_t flags);

  GuestFunction* function() const { return function_; }
  // Whether the function is code the guest wrote at runtime.
  bool dynamic_code() const { return dynamic_code_; }
  Function* LookupFunction(uint32_t address);
  Label* LookupLabel(uint32_t address);

  Value* LoadLR();
  void StoreLR(Value* value);
  Value* LoadCTR();
  void StoreCTR(Value* value);
  Value* LoadCR();
  Value* LoadCR(uint32_t n);
  Value* LoadCRField(uint32_t n, uint32_t bit);
  void StoreCR(Value* value);
  void StoreCR(uint32_t n, Value* value);
  void StoreCRField(uint32_t n, uint32_t bit, Value* value);
  void UpdateCR(uint32_t n, Value* lhs, bool is_signed = true);
  void UpdateCR(uint32_t n, Value* lhs, Value* rhs, bool is_signed = true);
  void UpdateCR6(Value* src_value);
  Value* LoadFPSCR();
  void StoreFPSCR(Value* value);
  // For instructions that cannot raise an exception.
  void ClearFPSCRExceptions(bool update_cr1);
  // Call before the arithmetic so the status the host reports afterwards
  // belongs to that operation alone. Only the recording forms pay for it.
  void BeginFPSCRUpdate(bool update_cr1);
  // Derives the summary from what the host raised, plus the invalid a
  // signalling NaN operand always means. Rc=0 clears the exception bits as
  // ClearFPSCRExceptions does.
  // `suppress`, when given, is a condition that clears everything raised.
  void UpdateFPSCR(std::initializer_list<Value*> operands, bool update_cr1,
                   Value* suppress = nullptr);
  // As UpdateFPSCR, plus the 0 x inf the host is allowed to leave unsignalled.
  void UpdateFPSCRForMultiplyAdd(Value* a, Value* c, Value* b, bool update_cr1,
                                 Value* suppress = nullptr);
  // The single-precision quirk: an operand outside the single range answers
  // with the default QNaN and raises nothing. Feed the condition to both of the
  // above.
  Value* SingleOutOfRangeOperand(std::initializer_list<Value*> operands);
  Value* ApplySingleOutOfRangeOperand(Value* quirk, Value* result);
  // ARITHMETIC_SINGLE_RANGE, for the operation to apply the quirk itself,
  // unless it's a recording form, which needs the condition as well.
  uint32_t SingleRangeFlag(bool update_cr1) const;
  // Divide and square root take the other half of the quirk: an operand
  // outside the single range skips the rounding to single instead. Snapshot the
  // status between the arithmetic and the rounding, then hand both to the
  // update.
  Value* SnapshotFpExceptions(bool update_cr1);
  void UpdateFPSCRForUnroundedSingle(std::initializer_list<Value*> operands,
                                     bool update_cr1, Value* quirk,
                                     Value* before_rounding);
  // For the estimates, whose host stand-ins raise nothing of their own.
  void UpdateFPSCRForEstimate(Value* b, bool is_sqrt_estimate, bool update_cr1);
  // For the conversions to integer, whose out of range invalid the host does
  // not report: x64 clamps the operand before converting.
  void UpdateFPSCRForConvertToInteger(Value* b, hir::RoundMode round_mode,
                                      bool to_int64, bool update_cr1);
  void CopyFPSCRToCR1();
  Value* LoadXER();
  void StoreXER(Value* value);
  // XER[25:31] as an INT64
  Value* LoadXERCount();
  // Call before UpdateCR, which copies XER[SO] into the CR field.
  void StoreOV(Value* value);
  Value* LoadCA();
  void StoreCA(Value* value);
  Value* LoadSAT();
  void StoreSAT(Value* value);

  Value* LoadGPR(uint32_t reg);
  void StoreGPR(uint32_t reg, Value* value);
  Value* LoadFPR(uint32_t reg);
  void StoreFPR(uint32_t reg, Value* value);
  Value* LoadVR(uint32_t reg);
  void StoreVR(uint32_t reg, Value* value);

  // calls original impl in hirbuilder, but also records the is_return_site bit
  // into flags in the guestmodule
  void SetReturnAddress(Value* value);

 private:
  void MaybeBreakOnInstruction(uint32_t address);
  void AnnotateLabel(uint32_t address, Label* label);
  void StoreFPSCRSummary(Value* raised, bool update_cr1);
  Value* FpInvalidFromOperands(std::initializer_list<Value*> operands);

  PPCFrontend* frontend_;

  // Reset whenever needed:
  StringBuffer comment_buffer_;

  // Reset each Emit:
  bool with_debug_info_;
  GuestFunction* function_;
  bool dynamic_code_;
  uint64_t start_address_;
  uint64_t instr_count_;
  Instr** instr_offset_list_;
  Label** label_list_;
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_HIR_BUILDER_H_
