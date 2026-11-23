/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_shader_translator.h"

#include <cfloat>
#include <cmath>
#include <cstdint>

#include "third_party/glslang/SPIRV/GLSL.std.450.h"
#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/spirv_compatibility.h"

namespace xe {
namespace gpu {

spv::Id SpirvShaderTranslator::ZeroIfAnyOperandIsZero(spv::Id value,
                                                      spv::Id operand_0_abs,
                                                      spv::Id operand_1_abs) {
  EnsureBuildPointAvailable();
  assert_true(builder_->getNumComponents(value) == 1);
  assert_true(builder_->getNumComponents(operand_0_abs) == 1);
  assert_true(builder_->getNumComponents(operand_1_abs) == 1);
  return builder_->createTriOp(
      spv::OpSelect, type_float_,
      builder_->createBinOp(spv::OpFOrdEqual, type_bool_,
                            builder_->createBinBuiltinCall(
                                type_float_, ext_inst_glsl_std_450_,
                                GLSLstd450NMin, operand_0_abs, operand_1_abs),
                            const_float_0_),
      const_float_0_, value);
}

spv::Id SpirvShaderTranslator::ScalarEstimate(ScalarEstimateOp op,
                                              spv::Id value) {
  EnsureBuildPointAvailable();

  if (!alu_op_result_rounding_sensitive_) {
    switch (op) {
      case ScalarEstimateOp::kRcp:
        return builder_->createNoContractionBinOp(spv::OpFDiv, type_float_,
                                                  const_float_1_, value);
      case ScalarEstimateOp::kRsq:
        return builder_->createUnaryBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450InverseSqrt, value);
      case ScalarEstimateOp::kSqrt:
        return builder_->createUnaryBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450Sqrt, value);
      case ScalarEstimateOp::kExp:
        return builder_->createUnaryBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450Exp2, value);
      case ScalarEstimateOp::kLog:
        return builder_->createUnaryBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450Log2, value);
    }
  }

  const xenos::ScalarEstimate* estimate = nullptr;
  const char* coefficients_name = nullptr;
  switch (op) {
    case ScalarEstimateOp::kRcp:
      estimate = &xenos::kRcpEstimate;
      coefficients_name = "xe_var_rcp_coefficients";
      break;
    case ScalarEstimateOp::kRsq:
      estimate = &xenos::kRsqEstimate;
      coefficients_name = "xe_var_rsq_coefficients";
      break;
    case ScalarEstimateOp::kSqrt:
      estimate = &xenos::kSqrtEstimate;
      coefficients_name = "xe_var_sqrt_coefficients";
      break;
    case ScalarEstimateOp::kExp:
      estimate = &xenos::kExpEstimate;
      coefficients_name = "xe_var_exp_coefficients";
      break;
    case ScalarEstimateOp::kLog:
      estimate = &xenos::kLogEstimate;
      coefficients_name = "xe_var_log_coefficients";
      break;
  }
  spv::Id& var_coefficients = var_main_estimate_coefficients_[size_t(op)];
  if (var_coefficients == spv::NoResult) {
    std::vector<spv::Id> segments;
    segments.reserve(xe::countof(estimate->coefficients));
    for (const auto& segment_coefficients : estimate->coefficients) {
      id_vector_temp_.clear();
      for (uint32_t coefficient : segment_coefficients) {
        id_vector_temp_.push_back(builder_->makeUintConstant(coefficient));
      }
      segments.push_back(
          builder_->makeCompositeConstant(type_uint4_, id_vector_temp_));
    }
    spv::Id type_coefficients = builder_->makeArrayType(
        type_uint4_, builder_->makeUintConstant(uint32_t(segments.size())), -1);
    var_coefficients = builder_->createVariable(
        spv::NoPrecision, spv::StorageClassFunction, type_coefficients,
        coefficients_name,
        builder_->makeCompositeConstant(type_coefficients, segments));
  }

  auto uint_constant = [&](uint32_t constant) {
    return builder_->makeUintConstant(constant);
  };
  auto uint_op = [&](spv::Op uint_op_code, spv::Id a, spv::Id b) {
    return builder_->createBinOp(uint_op_code, type_uint_, a, b);
  };
  auto uint_op_constant = [&](spv::Op uint_op_code, spv::Id a, uint32_t b) {
    return uint_op(uint_op_code, a, uint_constant(b));
  };
  auto select = [&](spv::Id condition, spv::Id a, spv::Id b) {
    return builder_->createTriOp(spv::OpSelect, type_uint_, condition, a, b);
  };
  auto compare = [&](spv::Op compare_op, spv::Id a, spv::Id b) {
    return builder_->createBinOp(compare_op, type_bool_, a, b);
  };
  auto round_to_odd_1 = [&](spv::Id a) {
    return uint_op(spv::OpBitwiseOr,
                   uint_op_constant(spv::OpShiftRightLogical, a, 1),
                   uint_op_constant(spv::OpBitwiseAnd, a, 1));
  };
  auto shift_right_sticky = [&](spv::Id a, uint32_t shift) {
    return uint_op(
        spv::OpBitwiseOr,
        uint_op_constant(spv::OpShiftLeftLogical,
                         uint_op_constant(spv::OpShiftRightLogical, a, shift),
                         1),
        builder_->createBinBuiltinCall(
            type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin,
            uint_op_constant(spv::OpBitwiseAnd, a, (uint32_t(1) << shift) - 1),
            uint_constant(1)));
  };
  // floor(C1 * offset / 2^(low_bits + shift)) from 32-bit pieces.
  auto split_linear = [&](spv::Id c1, spv::Id offset, uint32_t low_bits,
                          uint32_t shift) {
    return uint_op_constant(
        spv::OpShiftRightLogical,
        uint_op(spv::OpIAdd,
                uint_op(spv::OpIMul, c1,
                        uint_op_constant(spv::OpShiftRightLogical, offset,
                                         low_bits)),
                uint_op_constant(
                    spv::OpShiftRightLogical,
                    uint_op(spv::OpIMul, c1,
                            uint_op_constant(spv::OpBitwiseAnd, offset,
                                             (uint32_t(1) << low_bits) - 1)),
                    low_bits)),
        shift);
  };

  spv::Id bits = builder_->createUnaryOp(spv::OpBitcast, type_uint_, value);
  spv::Id sign =
      uint_op_constant(spv::OpBitwiseAnd, bits, UINT32_C(0x80000000));
  spv::Id is_negative = compare(spv::OpINotEqual, sign, const_uint_0_);
  spv::Id exponent = uint_op_constant(
      spv::OpBitwiseAnd, uint_op_constant(spv::OpShiftRightLogical, bits, 23),
      0xFF);
  spv::Id is_exponent_0 = compare(spv::OpIEqual, exponent, const_uint_0_);
  spv::Id is_exponent_255 =
      compare(spv::OpIEqual, exponent, uint_constant(0xFF));

  // The segment and the offset within it, of the mantissa or for EXP of the
  // fixed-point fraction.
  spv::Id segment = spv::NoResult, offset = spv::NoResult;
  spv::Id exp_integer = spv::NoResult, exp_fraction = spv::NoResult;
  switch (op) {
    case ScalarEstimateOp::kRcp:
    case ScalarEstimateOp::kLog:
      segment = uint_op_constant(
          spv::OpBitwiseAnd,
          uint_op_constant(spv::OpShiftRightLogical, bits, 18), 0x1F);
      offset = uint_op_constant(spv::OpBitwiseAnd, bits, 0x3FFFF);
      break;
    case ScalarEstimateOp::kRsq:
    case ScalarEstimateOp::kSqrt:
      // Segments 16-31 are for an odd unbiased exponent, an even biased one.
      segment = uint_op_constant(
          spv::OpBitwiseXor,
          uint_op_constant(spv::OpBitwiseAnd,
                           uint_op_constant(spv::OpShiftRightLogical, bits, 19),
                           0x1F),
          0x10);
      offset = uint_op_constant(spv::OpBitwiseAnd, bits, 0x7FFFF);
      break;
    case ScalarEstimateOp::kExp: {
      // |x| truncated to fixed point with 23 fraction bits, with the shifts
      // clamped where the result is replaced anyway.
      spv::Id significand = uint_op_constant(
          spv::OpBitwiseOr, uint_op_constant(spv::OpBitwiseAnd, bits, 0x7FFFFF),
          0x800000);
      spv::Id shift_right = builder_->createTriBuiltinCall(
          type_uint_, ext_inst_glsl_std_450_, GLSLstd450UClamp,
          uint_op(spv::OpISub, uint_constant(127),
                  builder_->createBinBuiltinCall(
                      type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin,
                      exponent, uint_constant(127))),
          const_uint_0_, uint_constant(24));
      spv::Id shift_left = builder_->createBinBuiltinCall(
          type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin,
          uint_op_constant(spv::OpISub,
                           builder_->createBinBuiltinCall(
                               type_uint_, ext_inst_glsl_std_450_,
                               GLSLstd450UMax, exponent, uint_constant(127)),
                           127),
          uint_constant(7));
      spv::Id fixed =
          uint_op(spv::OpShiftLeftLogical,
                  uint_op(spv::OpShiftRightLogical, significand, shift_right),
                  shift_left);
      exp_integer = uint_op_constant(spv::OpShiftRightLogical, fixed, 23);
      exp_fraction = uint_op_constant(spv::OpBitwiseAnd, fixed, 0x7FFFFF);
      // 2^-(i + f) = 2^-(i + 1) * 2^(1 - f), with 1 - f in ones' complement.
      spv::Id mantissa_input =
          select(is_negative,
                 uint_op_constant(spv::OpBitwiseXor, exp_fraction, 0x7FFFFF),
                 exp_fraction);
      segment = uint_op_constant(spv::OpShiftRightLogical, mantissa_input, 18);
      offset = uint_op_constant(spv::OpBitwiseAnd, mantissa_input, 0x3FFFF);
    } break;
  }

  id_vector_temp_.clear();
  id_vector_temp_.push_back(segment);
  spv::Id segment_coefficients = builder_->createLoad(
      builder_->createAccessChain(spv::StorageClassFunction, var_coefficients,
                                  id_vector_temp_),
      spv::NoPrecision);
  spv::Id coefficients[4];
  for (uint32_t i = 0; i < 4; ++i) {
    coefficients[i] =
        builder_->createCompositeExtract(segment_coefficients, type_uint_, i);
  }

  spv::Id square_input =
      shift_right_sticky(offset, estimate->square_input_shift);
  spv::Id square = round_to_odd_1(
      uint_op_constant(spv::OpShiftRightLogical,
                       uint_op(spv::OpIMul, square_input, square_input),
                       estimate->square_shift));
  spv::Id square_coarse = uint_op_constant(
      spv::OpShiftLeftLogical,
      round_to_odd_1(uint_op_constant(spv::OpShiftRightLogical, square,
                                      estimate->square_coarse_shift)),
      estimate->square_coarse_shift + 1);
  spv::Id cubic = uint_op(spv::OpIMul, coefficients[3],
                          shift_right_sticky(offset, estimate->block_shift));
  if (estimate->cubic_rounds_up) {
    cubic = uint_op_constant(spv::OpIAdd, cubic,
                             (uint32_t(1) << estimate->cubic_shift) - 1);
  }
  cubic = round_to_odd_1(
      uint_op_constant(spv::OpShiftRightLogical, cubic, estimate->cubic_shift));
  spv::Id quadratic = uint_op(spv::OpIMul, coefficients[2], square);
  spv::Id cubic_term = uint_op(spv::OpIMul, cubic, square_coarse);

  spv::Id result = spv::NoResult;
  switch (op) {
    case ScalarEstimateOp::kRcp:
    case ScalarEstimateOp::kRsq: {
      uint32_t offset_bits = op == ScalarEstimateOp::kRcp ? 18 : 19;
      spv::Id higher_order =
          uint_op(spv::OpIAdd, coefficients[0],
                  uint_op_constant(spv::OpShiftRightLogical,
                                   uint_op(spv::OpISub, quadratic, cubic_term),
                                   offset_bits - 3));
      // The linear term is subtracted in ones' complement.
      spv::Id mantissa = uint_op_constant(
          spv::OpShiftRightLogical,
          uint_op(spv::OpISub, higher_order,
                  uint_op_constant(spv::OpIAdd,
                                   split_linear(coefficients[1], offset, 12,
                                                offset_bits - 12),
                                   1)),
          1);
      if (op == ScalarEstimateOp::kRsq) {
        result = uint_op(
            spv::OpIAdd,
            uint_op_constant(
                spv::OpShiftLeftLogical,
                uint_op(spv::OpISub, uint_constant(189),
                        uint_op_constant(
                            spv::OpShiftRightLogical,
                            uint_op_constant(spv::OpIAdd, exponent, 1), 1)),
                23),
            mantissa);
        result = select(is_exponent_255, const_uint_0_, result);
        result =
            select(is_negative, uint_constant(UINT32_C(0xFFC00000)), result);
      } else {
        spv::Id magnitude =
            uint_op(spv::OpIAdd,
                    uint_op_constant(
                        spv::OpShiftLeftLogical,
                        uint_op(spv::OpISub, uint_constant(252), exponent), 23),
                    mantissa);
        // Results below the normal range are +0 whatever the sign.
        result = select(
            builder_->createBinOp(
                spv::OpSGreaterThanEqual, type_bool_,
                builder_->createUnaryOp(spv::OpBitcast, type_int_, magnitude),
                builder_->makeIntConstant(1 << 23)),
            uint_op(spv::OpBitwiseOr, sign, magnitude), const_uint_0_);
        result = select(is_exponent_255, sign, result);
      }
      // Denormals are zero.
      result =
          select(is_exponent_0,
                 uint_op_constant(spv::OpBitwiseOr, sign, UINT32_C(0x7F800000)),
                 result);
    } break;
    case ScalarEstimateOp::kSqrt: {
      spv::Id higher_order =
          uint_op(spv::OpISub, coefficients[0],
                  uint_op_constant(
                      spv::OpShiftRightLogical,
                      uint_op_constant(
                          spv::OpIAdd,
                          uint_op(spv::OpISub, quadratic, cubic_term), 0x7FFF),
                      15));
      // The mantissa of a result in [1, 2).
      spv::Id mantissa = uint_op_constant(
          spv::OpBitwiseAnd,
          uint_op_constant(
              spv::OpShiftRightLogical,
              uint_op(spv::OpIAdd, split_linear(coefficients[1], offset, 12, 6),
                      higher_order),
              2),
          0x7FFFFF);
      result = uint_op(
          spv::OpBitwiseOr,
          uint_op_constant(
              spv::OpShiftLeftLogical,
              uint_op_constant(spv::OpShiftRightLogical,
                               uint_op_constant(spv::OpIAdd, exponent, 127), 1),
              23),
          mantissa);
      result = select(is_exponent_255, bits, result);
      result = select(is_negative, uint_constant(UINT32_C(0xFFC00000)), result);
      // Denormals are zero.
      result = select(is_exponent_0, sign, result);
    } break;
    case ScalarEstimateOp::kExp: {
      spv::Id higher_order = uint_op(
          spv::OpIAdd, coefficients[0],
          uint_op_constant(spv::OpShiftRightLogical,
                           uint_op(spv::OpIAdd, quadratic, cubic_term), 14));
      // floor((C1 * offset + higher_order * 2^17) / 2^19) in 32 bits.
      spv::Id linear_low = uint_op(
          spv::OpIAdd,
          uint_op(spv::OpIMul, coefficients[1],
                  uint_op_constant(spv::OpBitwiseAnd, offset, 0x1FF)),
          uint_op_constant(spv::OpShiftLeftLogical,
                           uint_op_constant(spv::OpBitwiseAnd, higher_order, 3),
                           17));
      spv::Id linear_high = uint_op(
          spv::OpIAdd,
          uint_op(spv::OpIMul, coefficients[1],
                  uint_op_constant(spv::OpShiftRightLogical, offset, 9)),
          uint_op_constant(spv::OpShiftRightLogical, linear_low, 9));
      spv::Id mantissa = uint_op_constant(
          spv::OpBitwiseAnd,
          uint_op(spv::OpIAdd,
                  uint_op_constant(spv::OpShiftRightLogical, higher_order, 2),
                  uint_op_constant(spv::OpShiftRightLogical, linear_high, 10)),
          0x7FFFFF);
      spv::Id positive = uint_op(
          spv::OpBitwiseOr,
          uint_op_constant(spv::OpShiftLeftLogical,
                           uint_op_constant(spv::OpIAdd, exp_integer, 127), 23),
          mantissa);
      // Results below the normal range are +0.
      spv::Id negative = select(
          compare(spv::OpUGreaterThanEqual, exp_integer, uint_constant(126)),
          const_uint_0_,
          uint_op(
              spv::OpBitwiseOr,
              uint_op_constant(
                  spv::OpShiftLeftLogical,
                  uint_op(spv::OpISub, uint_constant(126), exp_integer), 23),
              mantissa));
      spv::Id negative_exact =
          select(compare(spv::OpULessThan, exp_integer, uint_constant(127)),
                 uint_op_constant(
                     spv::OpShiftLeftLogical,
                     uint_op(spv::OpISub, uint_constant(127), exp_integer), 23),
                 const_uint_0_);
      result =
          select(is_negative,
                 select(compare(spv::OpIEqual, exp_fraction, const_uint_0_),
                        negative_exact, negative),
                 positive);
      result = select(
          compare(spv::OpUGreaterThanEqual, exponent, uint_constant(127 + 7)),
          select(is_negative, const_uint_0_,
                 uint_constant(UINT32_C(0x7F800000))),
          result);
      // Denormals are zero.
      result =
          select(is_exponent_0, uint_constant(UINT32_C(0x3F800000)), result);
    } break;
    case ScalarEstimateOp::kLog: {
      // log2 of the mantissa with 25 fraction bits.
      spv::Id mantissa_log =
          uint_op(spv::OpISub,
                  uint_op(spv::OpIAdd, coefficients[0],
                          split_linear(coefficients[1], offset, 9, 8)),
                  uint_op_constant(
                      spv::OpShiftRightLogical,
                      uint_op_constant(
                          spv::OpIAdd,
                          uint_op(spv::OpISub, quadratic, cubic_term), 0x7FFF),
                      15));
      // The exponent added, negative sums negated in ones' complement.
      spv::Id is_below_1 =
          compare(spv::OpULessThan, exponent, uint_constant(127));
      spv::Id magnitude = select(
          is_below_1,
          uint_op(spv::OpBitwiseOr,
                  uint_op_constant(
                      spv::OpShiftLeftLogical,
                      uint_op(spv::OpISub, uint_constant(126), exponent), 25),
                  uint_op_constant(spv::OpBitwiseXor, mantissa_log, 0x1FFFFFF)),
          uint_op(spv::OpBitwiseOr,
                  uint_op_constant(spv::OpShiftLeftLogical,
                                   uint_op_constant(spv::OpISub, exponent, 127),
                                   25),
                  mantissa_log));
      // Normalized with the low bits truncated, one of the shifts is 0. A zero
      // magnitude, with no highest bit, is replaced below.
      spv::Id msb = builder_->createBinBuiltinCall(
          type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin,
          builder_->createUnaryOp(spv::OpBitcast, type_uint_,
                                  builder_->createUnaryBuiltinCall(
                                      type_int_, ext_inst_glsl_std_450_,
                                      GLSLstd450FindUMsb, magnitude)),
          uint_constant(31));
      spv::Id significand = uint_op(
          spv::OpShiftLeftLogical,
          uint_op(spv::OpShiftRightLogical, magnitude,
                  uint_op_constant(spv::OpISub,
                                   builder_->createBinBuiltinCall(
                                       type_uint_, ext_inst_glsl_std_450_,
                                       GLSLstd450UMax, msb, uint_constant(23)),
                                   23)),
          uint_op(spv::OpISub, uint_constant(23),
                  builder_->createBinBuiltinCall(
                      type_uint_, ext_inst_glsl_std_450_, GLSLstd450UMin, msb,
                      uint_constant(23))));
      result = uint_op(
          spv::OpBitwiseOr,
          select(is_below_1, uint_constant(UINT32_C(0x80000000)),
                 const_uint_0_),
          uint_op(spv::OpBitwiseOr,
                  uint_op_constant(spv::OpShiftLeftLogical,
                                   uint_op_constant(spv::OpIAdd, msb, 102), 23),
                  uint_op_constant(spv::OpBitwiseAnd, significand, 0x7FFFFF)));
      result = select(compare(spv::OpIEqual, magnitude, const_uint_0_),
                      const_uint_0_, result);
      result = select(is_exponent_255, bits, result);
      result = select(is_negative, uint_constant(UINT32_C(0xFFC00000)), result);
      // Denormals are zero, and the sign of a zero doesn't matter.
      result =
          select(is_exponent_0, uint_constant(UINT32_C(0xFF800000)), result);
    } break;
  }
  // NaNs pass through.
  result = select(
      compare(spv::OpUGreaterThan,
              uint_op_constant(spv::OpBitwiseAnd, bits, UINT32_C(0x7FFFFFFF)),
              uint_constant(UINT32_C(0x7F800000))),
      bits, result);
  return builder_->createUnaryOp(spv::OpBitcast, type_float_, result);
}

spv::Id SpirvShaderTranslator::RoundTowardZero(spv::Id rounded, spv::Id error,
                                               spv::Id a, spv::Id b) {
  spv::Id type = builder_->getTypeId(rounded);
  int component_count = builder_->getNumComponents(rounded);
  spv::Id type_bool = type_bool_vectors_[component_count - 1];
  spv::Id type_uint = type_uint_vectors_[component_count - 1];
  spv::Id const_zero = const_float_vectors_0_[component_count - 1];
  auto splat = [&](spv::Id vector_type, spv::Id scalar) {
    if (component_count == 1) {
      return scalar;
    }
    std::vector<spv::Id> components(component_count, scalar);
    return builder_->makeCompositeConstant(vector_type, components);
  };
  auto compare = [&](spv::Op op, spv::Id x, spv::Id y) {
    return builder_->createBinOp(op, type_bool, x, y);
  };
  auto absolute = [&](spv::Id x) {
    return builder_->createUnaryBuiltinCall(type, ext_inst_glsl_std_450_,
                                            GLSLstd450FAbs, x);
  };

  // Opposite signs mean the rounding went away from zero.
  spv::Id step = builder_->createBinOp(
      spv::OpLogicalAnd, type_bool,
      compare(spv::OpFOrdNotEqual, error, const_zero),
      builder_->createBinOp(spv::OpLogicalNotEqual, type_bool,
                            compare(spv::OpFOrdLessThan, error, const_zero),
                            compare(spv::OpFOrdLessThan, rounded, const_zero)));
  step =
      builder_->createBinOp(spv::OpLogicalAnd, type_bool, step,
                            compare(spv::OpFOrdNotEqual, rounded, const_zero));
  // Overflow from finite operands saturates.
  spv::Id const_infinity = splat(type, builder_->makeFloatConstant(INFINITY));
  spv::Id overflowed = builder_->createBinOp(
      spv::OpLogicalAnd, type_bool,
      compare(spv::OpFOrdEqual, absolute(rounded), const_infinity),
      builder_->createBinOp(
          spv::OpLogicalAnd, type_bool,
          compare(spv::OpFOrdNotEqual, absolute(a), const_infinity),
          compare(spv::OpFOrdNotEqual, absolute(b), const_infinity)));
  step = builder_->createBinOp(spv::OpLogicalOr, type_bool, step, overflowed);
  spv::Id stepped = builder_->createUnaryOp(
      spv::OpBitcast, type,
      builder_->createBinOp(
          spv::OpISub, type_uint,
          builder_->createUnaryOp(spv::OpBitcast, type_uint, rounded),
          splat(type_uint, builder_->makeUintConstant(1))));
  return builder_->createTriOp(spv::OpSelect, type, step, stepped, rounded);
}

bool SpirvShaderTranslator::RoundsTowardZero() const {
  return cvars::alu_round_toward_zero && is_vertex_shader() &&
         (features_.rounding_mode_rtz_float32 ||
          alu_op_result_rounding_sensitive_);
}

bool SpirvShaderTranslator::RoundsTowardZeroInHardware() const {
  return cvars::alu_round_toward_zero && is_vertex_shader() &&
         features_.rounding_mode_rtz_float32;
}

spv::Id SpirvShaderTranslator::AluAdd(spv::Id a, spv::Id b) {
  spv::Id type = builder_->getTypeId(a);
  spv::Id sum = builder_->createNoContractionBinOp(spv::OpFAdd, type, a, b);
  if (!RoundsTowardZero() || RoundsTowardZeroInHardware()) {
    return sum;
  }
  // Two-sum error of the rounded sum, exact unless it overflowed.
  spv::Id b_rounded =
      builder_->createNoContractionBinOp(spv::OpFSub, type, sum, a);
  spv::Id error = builder_->createNoContractionBinOp(
      spv::OpFAdd, type,
      builder_->createNoContractionBinOp(
          spv::OpFSub, type, a,
          builder_->createNoContractionBinOp(spv::OpFSub, type, sum,
                                             b_rounded)),
      builder_->createNoContractionBinOp(spv::OpFSub, type, b, b_rounded));
  return RoundTowardZero(sum, error, a, b);
}

spv::Id SpirvShaderTranslator::AluSubtract(spv::Id a, spv::Id b) {
  if (!RoundsTowardZero() || RoundsTowardZeroInHardware()) {
    return builder_->createNoContractionBinOp(spv::OpFSub,
                                              builder_->getTypeId(a), a, b);
  }
  return AluAdd(a, builder_->createNoContractionUnaryOp(
                       spv::OpFNegate, builder_->getTypeId(b), b));
}

spv::Id SpirvShaderTranslator::AluMultiply(spv::Id a, spv::Id b) {
  spv::Id type = builder_->getTypeId(a);
  spv::Id product = builder_->createNoContractionBinOp(spv::OpFMul, type, a, b);
  if (!RoundsTowardZero() || RoundsTowardZeroInHardware()) {
    return product;
  }
  // Fma isn't guaranteed to be fused (spirv_to_dxil splits 32-bit Fma into a
  // multiply and an add), so recover the product error with a Veltkamp split
  // and Dekker error sum instead.
  auto mul = [&](spv::Id x, spv::Id y) {
    return builder_->createNoContractionBinOp(spv::OpFMul, type, x, y);
  };
  auto add = [&](spv::Id x, spv::Id y) {
    return builder_->createNoContractionBinOp(spv::OpFAdd, type, x, y);
  };
  auto sub = [&](spv::Id x, spv::Id y) {
    return builder_->createNoContractionBinOp(spv::OpFSub, type, x, y);
  };
  int component_count = builder_->getNumComponents(a);
  spv::Id const_split = builder_->makeFloatConstant(4097.0f);
  if (component_count > 1) {
    std::vector<spv::Id> components(component_count, const_split);
    const_split = builder_->makeCompositeConstant(type, components);
  }
  auto split = [&](spv::Id value, spv::Id& high, spv::Id& low) {
    spv::Id scaled = mul(value, const_split);
    high = sub(scaled, sub(scaled, value));
    low = sub(value, high);
  };
  spv::Id a_high, a_low, b_high, b_low;
  split(a, a_high, a_low);
  split(b, b_high, b_low);
  spv::Id error = sub(mul(a_high, b_high), product);
  error = add(error, mul(a_high, b_low));
  error = add(error, mul(a_low, b_high));
  error = add(error, mul(a_low, b_low));
  return RoundTowardZero(product, error, a, b);
}

void SpirvShaderTranslator::KillPixel(
    spv::Id condition, uint8_t memexport_eM_potentially_written_before) {
  SpirvBuilder::IfBuilder kill_if(condition, spv::SelectionControlMaskNone,
                                  *builder_);
  {
    // Perform outstanding memory exports before the invocation becomes inactive
    // and storage writes are disabled.
    ExportToMemory(memexport_eM_potentially_written_before);
    if (var_main_kill_pixel_ != spv::NoResult) {
      builder_->createStore(builder_->makeBoolConstant(true),
                            var_main_kill_pixel_);
    }
    if (features_.demote_to_helper_invocation) {
      builder_->createNoResultOp(spv::OpDemoteToHelperInvocationEXT);
    }
  }
  kill_if.makeEndIf();
}

void SpirvShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before) {
  if (BisectSkipsInstruction()) {
    return;
  }
  if (instr.IsNop()) {
    // Don't even disassemble or update predication.
    BisectSnapshotAfterInstruction();
    return;
  }

  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  // Floating-point arithmetic operations (addition, subtraction, negation,
  // multiplication, division, modulo - see isArithmeticOperation in
  // propagateNoContraction of glslang; though for some reason it's not applied
  // to SPIR-V OpDot, at least in the February 16, 2020 version installed on
  // http://shader-playground.timjones.io/) must have the NoContraction
  // decoration to prevent reordering to make sure floating-point calculations
  // are optimized predictably and exactly the same in different shaders to
  // allow for multipass rendering (in addition to the Invariant decoration on
  // outputs).

  const Shader& shader = current_shader();
  uint32_t instruction_address = current_instruction_address();

  // Whether the instruction has changed the predicate, and it needs to be
  // checked again later.
  bool predicate_written_vector = false;
  alu_op_result_rounding_sensitive_ =
      shader.IsAluVectorResultRoundingSensitive(instruction_address);
  spv::Id vector_result = ProcessVectorAluOperation(
      instr, memexport_eM_potentially_written_before, predicate_written_vector);

  bool predicate_written_scalar = false;
  alu_op_result_rounding_sensitive_ =
      shader.IsAluScalarResultRoundingSensitive(instruction_address);
  spv::Id scalar_result = ProcessScalarAluOperation(
      instr, memexport_eM_potentially_written_before, predicate_written_scalar);
  if (scalar_result != spv::NoResult) {
    EnsureBuildPointAvailable();
    builder_->createStore(scalar_result, var_main_previous_scalar_);
  } else {
    // Special retain_prev case - load ps only if needed and don't store the
    // same value back to ps.
    if (instr.scalar_result.GetUsedWriteMask()) {
      EnsureBuildPointAvailable();
      scalar_result =
          builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
    }
  }

  StoreResult(instr.vector_and_constant_result, vector_result);
  StoreResult(instr.scalar_result, scalar_result);

  if (predicate_written_vector || predicate_written_scalar) {
    cf_exec_predicate_written_ = true;
    CloseInstructionPredication();
  }

  BisectSnapshotAfterInstruction();
}

spv::Id SpirvShaderTranslator::ProcessVectorAluOperation(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before, bool& predicate_written) {
  predicate_written = false;

  uint32_t used_result_components =
      instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return spv::NoResult;
  }
  uint32_t used_result_component_count = xe::bit_count(used_result_components);

  // Load operand storage without swizzle and sign modifiers.
  // A small shortcut, operands of cube are the same, but swizzled.
  uint32_t operand_count;
  if (instr.vector_opcode == ucode::AluVectorOpcode::kCube) {
    operand_count = 1;
  } else {
    operand_count = instr.vector_operand_count;
  }
  spv::Id operand_storage[3] = {};
  for (uint32_t i = 0; i < operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.vector_operands[i]);
  }
  spv::Id result_type =
      used_result_component_count
          ? type_float_vectors_[used_result_component_count - 1]
          : spv::NoType;

  // In case the paired scalar instruction (if processed first) terminates the
  // block.
  EnsureBuildPointAvailable();

  // Lookup table for variants of instructions with similar structure.
  static constexpr unsigned int kOps[] = {
      static_cast<unsigned int>(spv::OpNop),                   // kAdd
      static_cast<unsigned int>(spv::OpNop),                   // kMul
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kMax
      static_cast<unsigned int>(spv::OpFOrdLessThan),          // kMin
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kSeq
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kSgt
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kSge
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kSne
      static_cast<unsigned int>(GLSLstd450Fract),              // kFrc
      static_cast<unsigned int>(GLSLstd450Trunc),              // kTrunc
      static_cast<unsigned int>(GLSLstd450Floor),              // kFloor
      static_cast<unsigned int>(spv::OpNop),                   // kMad
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kCndEq
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kCndGe
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kCndGt
      static_cast<unsigned int>(spv::OpNop),                   // kDp4
      static_cast<unsigned int>(spv::OpNop),                   // kDp3
      static_cast<unsigned int>(spv::OpNop),                   // kDp2Add
      static_cast<unsigned int>(spv::OpNop),                   // kCube
      static_cast<unsigned int>(spv::OpNop),                   // kMax4
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kSetpEqPush
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kSetpNePush
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kSetpGtPush
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kSetpGePush
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kKillEq
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kKillGt
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kKillGe
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kKillNe
      static_cast<unsigned int>(spv::OpNop),                   // kDst
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kMaxA
  };

  switch (instr.vector_opcode) {
    case ucode::AluVectorOpcode::kAdd: {
      return AluAdd(
          GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                               used_result_components),
          GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                               used_result_components));
    }
    case ucode::AluVectorOpcode::kMul:
    case ucode::AluVectorOpcode::kMad: {
      spv::Id multiplicands[2];
      for (uint32_t i = 0; i < 2; ++i) {
        multiplicands[i] =
            GetOperandComponents(operand_storage[i], instr.vector_operands[i],
                                 used_result_components);
      }
      spv::Id result = AluMultiply(multiplicands[0], multiplicands[1]);
      uint32_t multiplicands_different =
          used_result_components &
          ~instr.vector_operands[0].GetIdenticalComponents(
              instr.vector_operands[1]);
      if (multiplicands_different) {
        // Shader Model 3: +0 or denormal * anything = +-0.
        spv::Id different_operands[2] = {multiplicands[0], multiplicands[1]};
        spv::Id different_result = result;
        uint32_t different_count = xe::bit_count(multiplicands_different);
        spv::Id different_type = type_float_vectors_[different_count - 1];
        // Extract the different components, if not all are different.
        if (multiplicands_different != used_result_components) {
          uint_vector_temp_.clear();
          uint32_t components_remaining = used_result_components;
          for (uint32_t i = 0; i < used_result_component_count; ++i) {
            uint32_t component;
            xe::bit_scan_forward(components_remaining, &component);
            components_remaining &= ~(uint32_t(1) << component);
            if (multiplicands_different & (1 << component)) {
              uint_vector_temp_.push_back(i);
            }
          }
          assert_true(uint_vector_temp_.size() == different_count);
          if (different_count > 1) {
            for (uint32_t i = 0; i < 2; ++i) {
              different_operands[i] = builder_->createRvalueSwizzle(
                  spv::NoPrecision, different_type, different_operands[i],
                  uint_vector_temp_);
            }
            different_result = builder_->createRvalueSwizzle(
                spv::NoPrecision, different_type, different_result,
                uint_vector_temp_);
          } else {
            for (uint32_t i = 0; i < 2; ++i) {
              different_operands[i] = builder_->createCompositeExtract(
                  different_operands[i], different_type, uint_vector_temp_[0]);
            }
            different_result = builder_->createCompositeExtract(
                different_result, different_type, uint_vector_temp_[0]);
          }
        }
        // Check if the different components in any of the operands are zero,
        // even if the other is NaN - if min(|a|, |b|) is 0.
        for (uint32_t i = 0; i < 2; ++i) {
          different_operands[i] = GetAbsoluteOperand(different_operands[i],
                                                     instr.vector_operands[i]);
        }
        spv::Id different_zero = builder_->createBinOp(
            spv::OpFOrdEqual, type_bool_vectors_[different_count - 1],
            builder_->createBinBuiltinCall(
                different_type, ext_inst_glsl_std_450_, GLSLstd450NMin,
                different_operands[0], different_operands[1]),
            const_float_vectors_0_[different_count - 1]);
        // Replace with +0.
        different_result = builder_->createTriOp(
            spv::OpSelect, different_type, different_zero,
            const_float_vectors_0_[different_count - 1], different_result);
        // Insert the different components back to the result.
        if (multiplicands_different != used_result_components) {
          if (different_count > 1) {
            std::unique_ptr<spv::Instruction> shuffle_op =
                std::make_unique<spv::Instruction>(
                    builder_->getUniqueId(), result_type, spv::OpVectorShuffle);
            shuffle_op->addIdOperand(result);
            shuffle_op->addIdOperand(different_result);
            uint32_t components_remaining = used_result_components;
            unsigned int different_shuffle_index = used_result_component_count;
            for (uint32_t i = 0; i < used_result_component_count; ++i) {
              uint32_t component;
              xe::bit_scan_forward(components_remaining, &component);
              components_remaining &= ~(uint32_t(1) << component);
              shuffle_op->addImmediateOperand(
                  (multiplicands_different & (1 << component))
                      ? different_shuffle_index++
                      : i);
            }
            result = shuffle_op->getResultId();
            builder_->getBuildPoint()->addInstruction(std::move(shuffle_op));
          } else {
            result = builder_->createCompositeInsert(
                different_result, result, result_type,
                xe::bit_count(used_result_components &
                              (multiplicands_different - 1)));
          }
        } else {
          result = different_result;
        }
      }
      if (instr.vector_opcode == ucode::AluVectorOpcode::kMad) {
        // Not replacing true `0 + term` with conditional selection of the term
        // because +0 + -0 should result in +0, not -0.
        result = AluAdd(result, GetOperandComponents(operand_storage[2],
                                                     instr.vector_operands[2],
                                                     used_result_components));
      }
      return result;
    }

    case ucode::AluVectorOpcode::kMax:
    case ucode::AluVectorOpcode::kMin:
    case ucode::AluVectorOpcode::kMaxA: {
      bool is_maxa = instr.vector_opcode == ucode::AluVectorOpcode::kMaxA;
      spv::Id operand_0 = GetOperandComponents(
          operand_storage[0], instr.vector_operands[0],
          used_result_components | (is_maxa ? 0b1000 : 0b0000));
      spv::Id maxa_operand_0_w = spv::NoResult;
      if (is_maxa) {
        // a0 = (int)clamp(floor(src0.w + 0.5), -256.0, 255.0)
        int operand_0_num_components = builder_->getNumComponents(operand_0);
        if (operand_0_num_components > 1) {
          maxa_operand_0_w = builder_->createCompositeExtract(
              operand_0, type_float_,
              static_cast<unsigned int>(operand_0_num_components - 1));
        } else {
          maxa_operand_0_w = operand_0;
        }
        builder_->createStore(
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int_,
                builder_->createTriBuiltinCall(
                    type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                    builder_->createUnaryBuiltinCall(
                        type_float_, ext_inst_glsl_std_450_, GLSLstd450Floor,
                        builder_->createNoContractionBinOp(
                            spv::OpFAdd, type_float_, maxa_operand_0_w,
                            builder_->makeFloatConstant(0.5f))),
                    builder_->makeFloatConstant(-256.0f),
                    builder_->makeFloatConstant(255.0f))),
            var_main_address_register_);
      }
      if (!used_result_components) {
        // maxa returning nothing - can't load src1.
        return spv::NoResult;
      }
      // max is commonly used as mov.
      uint32_t identical = instr.vector_operands[0].GetIdenticalComponents(
                               instr.vector_operands[1]) &
                           used_result_components;
      spv::Id operand_0_per_component;
      if (is_maxa && !(used_result_components & 0b1000) &&
          (identical == used_result_components || !identical)) {
        // operand_0 and operand_1 have different lengths though if src0.w is
        // forced without W being in the write mask for maxa purposes -
        // shuffle/extract the needed part if src0.w is only needed for setting
        // a0.
        // This is only needed for cases without mixed identical and different
        // components - the mixed case uses CompositeExtract, which works fine.
        if (used_result_component_count > 1) {
          // Need all but the last (W) element of operand_0 as a vector.
          uint_vector_temp_.clear();
          for (unsigned int i = 0; i < used_result_component_count; ++i) {
            uint_vector_temp_.push_back(i);
          }
          operand_0_per_component = builder_->createRvalueSwizzle(
              spv::NoPrecision,
              type_float_vectors_[used_result_component_count - 1], operand_0,
              uint_vector_temp_);
        } else {
          // Need the non-W component as scalar.
          operand_0_per_component =
              builder_->createCompositeExtract(operand_0, type_float_, 0);
        }
      } else {
        operand_0_per_component = operand_0;
      }
      if (identical == used_result_components) {
        // All components are identical - mov (with the correct length in case
        // of maxa). Don't access operand_1 at all in this case (operand_0 is
        // already accessed for W in case of maxa).
        assert_true(builder_->getNumComponents(operand_0_per_component) ==
                    used_result_component_count);
        return operand_0_per_component;
      }
      spv::Id operand_1 = GetOperandComponents(
          operand_storage[1], instr.vector_operands[1], used_result_components);
      // Shader Model 3 NaN behavior (a op b ? a : b, not SPIR-V FMax/FMin which
      // are undefined for NaN or NMax/NMin which return the non-NaN operand).
      spv::Op op = spv::Op(kOps[size_t(instr.vector_opcode)]);
      if (!identical) {
        // All components are different - max/min of the scalars or the entire
        // vectors (with the correct length in case of maxa).
        assert_true(builder_->getNumComponents(operand_0_per_component) ==
                    used_result_component_count);
        return builder_->createTriOp(
            spv::OpSelect, result_type,
            builder_->createBinOp(
                op, type_bool_vectors_[used_result_component_count - 1],
                operand_0_per_component, operand_1),
            operand_0_per_component, operand_1);
      }
      // Mixed identical and different components.
      assert_true(used_result_component_count > 1);
      id_vector_temp_.clear();
      uint32_t components_remaining = used_result_components;
      for (uint32_t i = 0; i < used_result_component_count; ++i) {
        // Composite extraction of operand_0[i] works fine even it's maxa with
        // src0.w forced without W being in the write mask - src0.w would be the
        // last, so all indices before it are still valid. Don't extract twice
        // if already extracted though.
        spv::Id result_component =
            ((used_result_components & 0b1000) &&
             i + 1 >= used_result_component_count &&
             maxa_operand_0_w != spv::NoResult)
                ? maxa_operand_0_w
                : builder_->createCompositeExtract(operand_0, type_float_, i);
        uint32_t component_index;
        xe::bit_scan_forward(components_remaining, &component_index);
        components_remaining &= ~(uint32_t(1) << component_index);
        if (!(identical & (1 << component_index))) {
          spv::Id operand_1_component =
              builder_->createCompositeExtract(operand_1, type_float_, i);
          result_component = builder_->createTriOp(
              spv::OpSelect, type_float_,
              builder_->createBinOp(op, type_bool_, result_component,
                                    operand_1_component),
              result_component, operand_1_component);
        }
        id_vector_temp_.push_back(result_component);
      }
      return builder_->createCompositeConstruct(result_type, id_vector_temp_);
    }

    case ucode::AluVectorOpcode::kSeq:
    case ucode::AluVectorOpcode::kSgt:
    case ucode::AluVectorOpcode::kSge:
    case ucode::AluVectorOpcode::kSne:
      return builder_->createTriOp(
          spv::OpSelect, result_type,
          builder_->createBinOp(
              spv::Op(kOps[size_t(instr.vector_opcode)]),
              type_bool_vectors_[used_result_component_count - 1],
              GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                   used_result_components),
              GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                   used_result_components)),
          const_float_vectors_1_[used_result_component_count - 1],
          const_float_vectors_0_[used_result_component_count - 1]);

    case ucode::AluVectorOpcode::kFrc:
    case ucode::AluVectorOpcode::kTrunc:
    case ucode::AluVectorOpcode::kFloor: {
      spv::Id operand = GetOperandComponents(
          operand_storage[0], instr.vector_operands[0], used_result_components);
      if (instr.vector_opcode == ucode::AluVectorOpcode::kFrc &&
          RoundsTowardZero()) {
        return AluSubtract(operand, builder_->createUnaryBuiltinCall(
                                        result_type, ext_inst_glsl_std_450_,
                                        GLSLstd450Floor, operand));
      }
      return builder_->createUnaryBuiltinCall(
          result_type, ext_inst_glsl_std_450_,
          GLSLstd450(kOps[size_t(instr.vector_opcode)]), operand);
    }

    case ucode::AluVectorOpcode::kCndEq:
    case ucode::AluVectorOpcode::kCndGe:
    case ucode::AluVectorOpcode::kCndGt:
      return builder_->createTriOp(
          spv::OpSelect, result_type,
          builder_->createBinOp(
              spv::Op(kOps[size_t(instr.vector_opcode)]),
              type_bool_vectors_[used_result_component_count - 1],
              GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                   used_result_components),
              const_float_vectors_0_[used_result_component_count - 1]),
          GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                               used_result_components),
          GetOperandComponents(operand_storage[2], instr.vector_operands[2],
                               used_result_components));

    case ucode::AluVectorOpcode::kDp4:
    case ucode::AluVectorOpcode::kDp3:
    case ucode::AluVectorOpcode::kDp2Add: {
      // Not using OpDot for predictable optimization (especially addition
      // order) and NoContraction (which, for some reason, isn't placed on dot
      // in glslang as of the February 16, 2020 version). Each product and sum
      // is rounded separately.
      uint32_t component_count;
      if (instr.vector_opcode == ucode::AluVectorOpcode::kDp2Add) {
        component_count = 2;
      } else if (instr.vector_opcode == ucode::AluVectorOpcode::kDp3) {
        component_count = 3;
      } else {
        component_count = 4;
      }
      uint32_t component_mask = (1 << component_count) - 1;
      spv::Id operands[2];
      for (uint32_t i = 0; i < 2; ++i) {
        operands[i] = GetOperandComponents(
            operand_storage[i], instr.vector_operands[i], component_mask);
      }
      uint32_t different =
          component_mask & ~instr.vector_operands[0].GetIdenticalComponents(
                               instr.vector_operands[1]);
      spv::Id result = spv::NoResult;
      for (uint32_t i = 0; i < component_count; ++i) {
        spv::Id operand_components[2];
        for (unsigned int j = 0; j < 2; ++j) {
          operand_components[j] =
              builder_->createCompositeExtract(operands[j], type_float_, i);
        }
        spv::Id product =
            AluMultiply(operand_components[0], operand_components[1]);
        if (different & (1 << i)) {
          // Shader Model 3: +0 or denormal * anything = +-0.
          product = ZeroIfAnyOperandIsZero(
              product,
              GetAbsoluteOperand(operand_components[0],
                                 instr.vector_operands[0]),
              GetAbsoluteOperand(operand_components[1],
                                 instr.vector_operands[1]));
        }
        if (!i) {
          result = product;
          continue;
        }
        result = AluAdd(result, product);
      }
      if (instr.vector_opcode == ucode::AluVectorOpcode::kDp2Add) {
        result = AluAdd(result,
                        GetOperandComponents(operand_storage[2],
                                             instr.vector_operands[2], 0b0001));
      }
      return result;
    }

    case ucode::AluVectorOpcode::kCube: {
      // operands[0] is .z_xy.
      // Result is T coordinate, S coordinate, 2 * major axis, face ID.
      // Skipping the second component of the operand, so 120, not 230.
      spv::Id operand_vector = GetOperandComponents(
          operand_storage[0], instr.vector_operands[0], 0b1101);
      // Remapped from ZXY (Z_XY without the skipped component) to XYZ.
      spv::Id operand[3];
      for (unsigned int i = 0; i < 3; ++i) {
        operand[i] = builder_->createCompositeExtract(operand_vector,
                                                      type_float_, (i + 1) % 3);
      }
      spv::Id operand_abs[3];
      if (!instr.vector_operands[0].is_absolute_value ||
          instr.vector_operands[0].is_negated) {
        for (unsigned int i = 0; i < 3; ++i) {
          operand_abs[i] = builder_->createUnaryBuiltinCall(
              type_float_, ext_inst_glsl_std_450_, GLSLstd450FAbs, operand[i]);
        }
      } else {
        for (unsigned int i = 0; i < 3; ++i) {
          operand_abs[i] = operand[i];
        }
      }
      spv::Id operand_neg[3] = {};
      if (used_result_components & 0b0001) {
        operand_neg[1] = builder_->createNoContractionUnaryOp(
            spv::OpFNegate, type_float_, operand[1]);
      }
      if (used_result_components & 0b0010) {
        operand_neg[0] = builder_->createNoContractionUnaryOp(
            spv::OpFNegate, type_float_, operand[0]);
        operand_neg[2] = builder_->createNoContractionUnaryOp(
            spv::OpFNegate, type_float_, operand[2]);
      }

      spv::Id ma_z_result[4] = {}, ma_yx_result[4] = {};

      // Check if the major axis is Z (abs(z) >= abs(x) && abs(z) >= abs(y)).
      SpirvBuilder::IfBuilder ma_z_if(
          builder_->createBinOp(
              spv::OpLogicalAnd, type_bool_,
              builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                    operand_abs[2], operand_abs[0]),
              builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                    operand_abs[2], operand_abs[1])),
          spv::SelectionControlMaskNone, *builder_);
      {
        // The major axis is Z.
        // tc = -y
        ma_z_result[0] = operand_neg[1];
        // ma/2 = z
        ma_z_result[2] = operand[2];
        if (used_result_components & 0b1010) {
          spv::Id z_is_neg = builder_->createBinOp(
              spv::OpFOrdLessThan, type_bool_, operand[2], const_float_0_);
          if (used_result_components & 0b0010) {
            // sc = z < 0.0 ? -x : x
            ma_z_result[1] =
                builder_->createTriOp(spv::OpSelect, type_float_, z_is_neg,
                                      operand_neg[0], operand[0]);
          }
          if (used_result_components & 0b1000) {
            // id = z < 0.0 ? 5.0 : 4.0
            ma_z_result[3] =
                builder_->createTriOp(spv::OpSelect, type_float_, z_is_neg,
                                      builder_->makeFloatConstant(5.0f),
                                      builder_->makeFloatConstant(4.0f));
          }
        }
      }
      ma_z_if.makeBeginElse();
      {
        spv::Id ma_y_result[4] = {}, ma_x_result[4] = {};

        // The major axis is not Z - create an inner conditional to check if the
        // major axis is Y (abs(y) >= abs(x)).
        SpirvBuilder::IfBuilder ma_y_if(
            builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                  operand_abs[1], operand_abs[0]),
            spv::SelectionControlMaskNone, *builder_);
        {
          // The major axis is Y.
          // sc = x
          ma_y_result[1] = operand[0];
          // ma/2 = y
          ma_y_result[2] = operand[1];
          if (used_result_components & 0b1001) {
            spv::Id y_is_neg = builder_->createBinOp(
                spv::OpFOrdLessThan, type_bool_, operand[1], const_float_0_);
            if (used_result_components & 0b0001) {
              // tc = y < 0.0 ? -z : z
              ma_y_result[0] =
                  builder_->createTriOp(spv::OpSelect, type_float_, y_is_neg,
                                        operand_neg[2], operand[2]);
              // id = y < 0.0 ? 3.0 : 2.0
              ma_y_result[3] =
                  builder_->createTriOp(spv::OpSelect, type_float_, y_is_neg,
                                        builder_->makeFloatConstant(3.0f),
                                        builder_->makeFloatConstant(2.0f));
            }
          }
        }
        ma_y_if.makeBeginElse();
        {
          // The major axis is X.
          // tc = -y
          ma_x_result[0] = operand_neg[1];
          // ma/2 = x
          ma_x_result[2] = operand[0];
          if (used_result_components & 0b1010) {
            spv::Id x_is_neg = builder_->createBinOp(
                spv::OpFOrdLessThan, type_bool_, operand[0], const_float_0_);
            if (used_result_components & 0b0010) {
              // sc = x < 0.0 ? z : -z
              ma_x_result[1] =
                  builder_->createTriOp(spv::OpSelect, type_float_, x_is_neg,
                                        operand[2], operand_neg[2]);
            }
            if (used_result_components & 0b1000) {
              // id = x < 0.0 ? 1.0 : 0.0
              ma_x_result[3] =
                  builder_->createTriOp(spv::OpSelect, type_float_, x_is_neg,
                                        const_float_1_, const_float_0_);
            }
          }
        }
        ma_y_if.makeEndIf();

        // The major axis is Y or X - choose the options of the result from Y
        // and X.
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(used_result_components & (1 << i))) {
            continue;
          }
          ma_yx_result[i] =
              ma_y_if.createMergePhi(ma_y_result[i], ma_x_result[i]);
        }
      }
      ma_z_if.makeEndIf();

      // Choose the result options from Z and YX cases.
      id_vector_temp_.clear();
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(used_result_components & (1 << i))) {
          continue;
        }
        id_vector_temp_.push_back(
            ma_z_if.createMergePhi(ma_z_result[i], ma_yx_result[i]));
      }
      assert_true(id_vector_temp_.size() == used_result_component_count);
      if (used_result_components & 0b0100) {
        // Multiply the major axis by 2.
        spv::Id& ma2 = id_vector_temp_[xe::bit_count(used_result_components &
                                                     ((1 << 2) - 1))];
        ma2 = builder_->createNoContractionBinOp(
            spv::OpFMul, type_float_, builder_->makeFloatConstant(2.0f), ma2);
      }
      if (used_result_component_count == 1) {
        // Only one component - not composite.
        return id_vector_temp_[0];
      }
      return builder_->createCompositeConstruct(
          type_float_vectors_[used_result_component_count - 1],
          id_vector_temp_);
    }

    case ucode::AluVectorOpcode::kMax4: {
      // Find max of all different components of the first operand.
      // FIXME(Triang3l): Not caring about NaN because no info about the
      // correct order, just using NMax here, which replaces them with the
      // non-NaN component (however, there's one nice thing about it is that it
      // may be compiled into max3 + max on GCN).
      uint32_t components_remaining = 0b0000;
      for (uint32_t i = 0; i < 4; ++i) {
        SwizzleSource swizzle_source = instr.vector_operands[0].GetComponent(i);
        assert_true(swizzle_source >= SwizzleSource::kX &&
                    swizzle_source <= SwizzleSource::kW);
        components_remaining |=
            1 << (uint32_t(swizzle_source) - uint32_t(SwizzleSource::kX));
      }
      assert_not_zero(components_remaining);
      spv::Id operand =
          ApplyOperandModifiers(operand_storage[0], instr.vector_operands[0]);
      uint32_t component;
      xe::bit_scan_forward(components_remaining, &component);
      components_remaining &= ~(uint32_t(1) << component);
      spv::Id result = builder_->createCompositeExtract(
          operand, type_float_, static_cast<unsigned int>(component));
      while (xe::bit_scan_forward(components_remaining, &component)) {
        components_remaining &= ~(uint32_t(1) << component);
        result = builder_->createBinBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450NMax, result,
            builder_->createCompositeExtract(
                operand, type_float_, static_cast<unsigned int>(component)));
      }
      return result;
    }

    case ucode::AluVectorOpcode::kSetpEqPush:
    case ucode::AluVectorOpcode::kSetpNePush:
    case ucode::AluVectorOpcode::kSetpGtPush:
    case ucode::AluVectorOpcode::kSetpGePush: {
      // X is only needed for the result, W is needed for the predicate.
      spv::Id operands[2];
      spv::Id operands_w[2];
      for (uint32_t i = 0; i < 2; ++i) {
        operands[i] =
            GetOperandComponents(operand_storage[i], instr.vector_operands[i],
                                 used_result_components ? 0b1001 : 0b1000);
        if (used_result_components) {
          operands_w[i] =
              builder_->createCompositeExtract(operands[i], type_float_, 1);
        } else {
          operands_w[i] = operands[i];
        }
      }
      spv::Op op = spv::Op(kOps[size_t(instr.vector_opcode)]);
      // p0 = src0.w == 0.0 && src1.w op 0.0
      builder_->createStore(
          builder_->createBinOp(
              spv::OpLogicalAnd, type_bool_,
              builder_->createBinOp(spv::OpFOrdEqual, type_bool_, operands_w[0],
                                    const_float_0_),
              builder_->createBinOp(op, type_bool_, operands_w[1],
                                    const_float_0_)),
          var_main_predicate_);
      predicate_written = true;
      if (!used_result_components) {
        return spv::NoResult;
      }
      // result = (src0.x == 0.0 && src1.x op 0.0) ? 0.0 : src0.x + 1.0
      // Or:
      // result = ((src0.x == 0.0 && src1.x op 0.0) ? -1.0 : src0.x) + 1.0
      spv::Id operands_x[2];
      for (uint32_t i = 0; i < 2; ++i) {
        operands_x[i] =
            builder_->createCompositeExtract(operands[i], type_float_, 0);
      }
      spv::Id condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, operands_x[0],
                                const_float_0_),
          builder_->createBinOp(op, type_bool_, operands_x[1], const_float_0_));
      return builder_->createNoContractionBinOp(
          spv::OpFAdd, type_float_,
          builder_->createTriOp(spv::OpSelect, type_float_, condition,
                                builder_->makeFloatConstant(-1.0f),
                                operands_x[0]),
          const_float_1_);
    }

    case ucode::AluVectorOpcode::kKillEq:
    case ucode::AluVectorOpcode::kKillGt:
    case ucode::AluVectorOpcode::kKillGe:
    case ucode::AluVectorOpcode::kKillNe: {
      spv::Id condition = builder_->createUnaryOp(
          spv::OpAny, type_bool_,
          builder_->createBinOp(
              spv::Op(kOps[size_t(instr.vector_opcode)]), type_bool4_,
              GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                   0b1111),
              GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                   0b1111)));
      KillPixel(condition, memexport_eM_potentially_written_before);
      // Kills write their destination: 1.0 when the kill condition is true,
      // 0.0 otherwise (ucode.h). KillPixel demotes, so execution continues.
      return builder_->createTriOp(spv::OpSelect, type_float_, condition,
                                   const_float_1_, const_float_0_);
    }

    case ucode::AluVectorOpcode::kDst: {
      spv::Id operands[2] = {};
      if (used_result_components & 0b0110) {
        // result.yz is needed: [0] = y, [1] = z.
        // resuly.y is needed: scalar = y.
        // resuly.z is needed: scalar = z.
        operands[0] =
            GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                 used_result_components & 0b0110);
      }
      if (used_result_components & 0b1010) {
        // result.yw is needed: [0] = y, [1] = w.
        // resuly.y is needed: scalar = y.
        // resuly.w is needed: scalar = w.
        operands[1] =
            GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                 used_result_components & 0b1010);
      }
      // y = src0.y * src1.y
      spv::Id result_y = spv::NoResult;
      if (used_result_components & 0b0010) {
        spv::Id operands_y[2];
        operands_y[0] =
            (used_result_components & 0b0100)
                ? builder_->createCompositeExtract(operands[0], type_float_, 0)
                : operands[0];
        operands_y[1] =
            (used_result_components & 0b1000)
                ? builder_->createCompositeExtract(operands[1], type_float_, 0)
                : operands[1];
        result_y = AluMultiply(operands_y[0], operands_y[1]);
        if (!(instr.vector_operands[0].GetIdenticalComponents(
                  instr.vector_operands[1]) &
              0b0010)) {
          // Shader Model 3: +0 or denormal * anything = +-0.
          result_y = ZeroIfAnyOperandIsZero(
              result_y,
              GetAbsoluteOperand(operands_y[0], instr.vector_operands[0]),
              GetAbsoluteOperand(operands_y[1], instr.vector_operands[1]));
        }
      }
      id_vector_temp_.clear();
      if (used_result_components & 0b0001) {
        // x = 1.0
        id_vector_temp_.push_back(const_float_1_);
      }
      if (used_result_components & 0b0010) {
        // y = src0.y * src1.y
        id_vector_temp_.push_back(result_y);
      }
      if (used_result_components & 0b0100) {
        // z = src0.z
        id_vector_temp_.push_back(
            (used_result_components & 0b0010)
                ? builder_->createCompositeExtract(operands[0], type_float_, 1)
                : operands[0]);
      }
      if (used_result_components & 0b1000) {
        // w = src1.w
        id_vector_temp_.push_back(
            (used_result_components & 0b0010)
                ? builder_->createCompositeExtract(operands[1], type_float_, 1)
                : operands[1]);
      }
      assert_true(id_vector_temp_.size() == used_result_component_count);
      if (used_result_component_count == 1) {
        // Only one component - not composite.
        return id_vector_temp_[0];
      }
      return builder_->createCompositeConstruct(
          type_float_vectors_[used_result_component_count - 1],
          id_vector_temp_);
    }
  }

  assert_unhandled_case(instr.vector_opcode);
  EmitTranslationError("Unknown ALU vector operation");
  return spv::NoResult;
}

spv::Id SpirvShaderTranslator::ProcessScalarAluOperation(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before, bool& predicate_written) {
  predicate_written = false;

  spv::Id operand_storage[2] = {};
  for (uint32_t i = 0; i < instr.scalar_operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.scalar_operands[i]);
  }

  // In case the paired vector instruction (if processed first) terminates the
  // block.
  EnsureBuildPointAvailable();

  // Lookup table for variants of instructions with similar structure.
  static constexpr unsigned int kOps[] = {
      static_cast<unsigned int>(spv::OpNop),                   // kAdds
      static_cast<unsigned int>(spv::OpNop),                   // kAddsPrev
      static_cast<unsigned int>(spv::OpNop),                   // kMuls
      static_cast<unsigned int>(spv::OpNop),                   // kMulsPrev
      static_cast<unsigned int>(spv::OpNop),                   // kMulsPrev2
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kMaxs
      static_cast<unsigned int>(spv::OpFOrdLessThan),          // kMins
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kSeqs
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kSgts
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kSges
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kSnes
      static_cast<unsigned int>(GLSLstd450Fract),              // kFrcs
      static_cast<unsigned int>(GLSLstd450Trunc),              // kTruncs
      static_cast<unsigned int>(GLSLstd450Floor),              // kFloors
      static_cast<unsigned int>(GLSLstd450Exp2),               // kExp
      static_cast<unsigned int>(spv::OpNop),                   // kLogc
      static_cast<unsigned int>(GLSLstd450Log2),               // kLog
      static_cast<unsigned int>(spv::OpNop),                   // kRcpc
      static_cast<unsigned int>(spv::OpNop),                   // kRcpf
      static_cast<unsigned int>(spv::OpNop),                   // kRcp
      static_cast<unsigned int>(spv::OpNop),                   // kRsqc
      static_cast<unsigned int>(spv::OpNop),                   // kRsqf
      static_cast<unsigned int>(spv::OpNop),                   // kRsq
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kMaxAs
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kMaxAsf
      static_cast<unsigned int>(spv::OpNop),                   // kSubs
      static_cast<unsigned int>(spv::OpNop),                   // kSubsPrev
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kSetpEq
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kSetpNe
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kSetpGt
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kSetpGe
      static_cast<unsigned int>(spv::OpNop),                   // kSetpInv
      static_cast<unsigned int>(spv::OpNop),                   // kSetpPop
      static_cast<unsigned int>(spv::OpNop),                   // kSetpClr
      static_cast<unsigned int>(spv::OpNop),                   // kSetpRstr
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kKillsEq
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),       // kKillsGt
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),  // kKillsGe
      static_cast<unsigned int>(spv::OpFUnordNotEqual),        // kKillsNe
      static_cast<unsigned int>(spv::OpFOrdEqual),             // kKillsOne
      static_cast<unsigned int>(GLSLstd450Sqrt),               // kSqrt
      static_cast<unsigned int>(spv::OpNop),                   // Invalid
      static_cast<unsigned int>(spv::OpNop),                   // kMulsc0
      static_cast<unsigned int>(spv::OpNop),                   // kMulsc1
      static_cast<unsigned int>(spv::OpNop),                   // kAddsc0
      static_cast<unsigned int>(spv::OpNop),                   // kAddsc1
      static_cast<unsigned int>(spv::OpNop),                   // kSubsc0
      static_cast<unsigned int>(spv::OpNop),                   // kSubsc1
      static_cast<unsigned int>(GLSLstd450Sin),                // kSin
      static_cast<unsigned int>(GLSLstd450Cos),                // kCos
      static_cast<unsigned int>(spv::OpNop),                   // kRetainPrev
  };

  switch (instr.scalar_opcode) {
    case ucode::AluScalarOpcode::kAdds:
    case ucode::AluScalarOpcode::kSubs: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      return instr.scalar_opcode == ucode::AluScalarOpcode::kAdds
                 ? AluAdd(a, b)
                 : AluSubtract(a, b);
    }
    case ucode::AluScalarOpcode::kAddsPrev:
    case ucode::AluScalarOpcode::kSubsPrev: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      spv::Id ps =
          builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
      return instr.scalar_opcode == ucode::AluScalarOpcode::kAddsPrev
                 ? AluAdd(a, ps)
                 : AluSubtract(a, ps);
    }
    case ucode::AluScalarOpcode::kMuls: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      spv::Id result = AluMultiply(a, b);
      if (a != b) {
        // Shader Model 3: +0 or denormal * anything = +-0.
        result = ZeroIfAnyOperandIsZero(
            result, GetAbsoluteOperand(a, instr.scalar_operands[0]),
            GetAbsoluteOperand(b, instr.scalar_operands[0]));
      }
      return result;
    }
    case ucode::AluScalarOpcode::kMulsPrev: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      spv::Id ps =
          builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
      spv::Id result = AluMultiply(a, ps);
      // Shader Model 3: +0 or denormal * anything = +-0.
      return ZeroIfAnyOperandIsZero(
          result, GetAbsoluteOperand(a, instr.scalar_operands[0]),
          builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,
                                           GLSLstd450FAbs, ps));
    }
    case ucode::AluScalarOpcode::kMulsPrev2: {
      // Check if need to select the src0.a * ps case.
      // Selection merge must be the penultimate instruction in the block, check
      // the condition before it.
      spv::Id ps =
          builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
      // ps != -FLT_MAX.
      spv::Id const_float_max_neg = builder_->makeFloatConstant(-FLT_MAX);
      spv::Id condition = builder_->createBinOp(
          spv::OpFUnordNotEqual, type_bool_, ps, const_float_max_neg);
      // isfinite(ps), or |ps| <= FLT_MAX, or -|ps| >= -FLT_MAX, since -FLT_MAX
      // is already loaded to an SGPR, this is also false if it's NaN.
      spv::Id ps_abs = builder_->createUnaryBuiltinCall(
          type_float_, ext_inst_glsl_std_450_, GLSLstd450FAbs, ps);
      spv::Id ps_abs_neg = builder_->createNoContractionUnaryOp(
          spv::OpFNegate, type_float_, ps_abs);
      condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool_, condition,
          builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                ps_abs_neg, const_float_max_neg));
      // isfinite(src0.b), or -|src0.b| >= -FLT_MAX for the same reason.
      spv::Id b = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0010);
      spv::Id b_abs_neg = b;
      if (!instr.scalar_operands[0].is_absolute_value) {
        b_abs_neg = builder_->createUnaryBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450FAbs, b_abs_neg);
      }
      if (!instr.scalar_operands[0].is_absolute_value ||
          !instr.scalar_operands[0].is_negated) {
        b_abs_neg = builder_->createNoContractionUnaryOp(
            spv::OpFNegate, type_float_, b_abs_neg);
      }
      condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool_, condition,
          builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool_,
                                b_abs_neg, const_float_max_neg));
      // src0.b > 0 (need !(src0.b <= 0), but src0.b has already been checked
      // for NaN).
      condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool_, condition,
          builder_->createBinOp(spv::OpFOrdGreaterThan, type_bool_, b,
                                const_float_0_));
      SpirvBuilder::IfBuilder multiply_if(
          condition, spv::SelectionControlMaskNone, *builder_);
      spv::Id product;
      {
        // Multiplication case.
        spv::Id a = instr.scalar_operands[0].GetComponent(0) !=
                            instr.scalar_operands[0].GetComponent(1)
                        ? GetOperandComponents(operand_storage[0],
                                               instr.scalar_operands[0], 0b0001)
                        : b;
        product = AluMultiply(a, ps);
        // Shader Model 3: +0 or denormal * anything = +-0.
        product = ZeroIfAnyOperandIsZero(
            product, GetAbsoluteOperand(a, instr.scalar_operands[0]), ps_abs);
      }
      multiply_if.makeEndIf();
      // Merge - choose between the product and -FLT_MAX.
      return multiply_if.createMergePhi(product, const_float_max_neg);
    }

    case ucode::AluScalarOpcode::kMaxs:
    case ucode::AluScalarOpcode::kMins:
    case ucode::AluScalarOpcode::kMaxAs:
    case ucode::AluScalarOpcode::kMaxAsf: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      if (instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAs ||
          instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAsf) {
        // Scalar maxas/maxasf clamp a0 to [0, 255] (non-negative), unlike the
        // vector maxa which allows [-256, 255].
        // maxas: a0 = (int)clamp(floor(src0.a + 0.5), 0.0, 255.0)
        // maxasf: a0 = (int)clamp(floor(src0.a), 0.0, 255.0)
        spv::Id maxa_address;
        if (instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAs) {
          maxa_address = builder_->createNoContractionBinOp(
              spv::OpFAdd, type_float_, a, builder_->makeFloatConstant(0.5f));
        } else {
          maxa_address = a;
        }
        builder_->createStore(
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int_,
                builder_->createTriBuiltinCall(
                    type_float_, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                    builder_->createUnaryBuiltinCall(
                        type_float_, ext_inst_glsl_std_450_, GLSLstd450Floor,
                        maxa_address),
                    builder_->makeFloatConstant(0.0f),
                    builder_->makeFloatConstant(255.0f))),
            var_main_address_register_);
      }
      if (a == b) {
        // max is commonly used as mov.
        return a;
      }
      // Shader Model 3 NaN behavior (a op b ? a : b, not SPIR-V FMax/FMin which
      // are undefined for NaN or NMax/NMin which return the non-NaN operand).
      return builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::Op(kOps[size_t(instr.scalar_opcode)]),
                                type_bool_, a, b),
          a, b);
    }

    case ucode::AluScalarOpcode::kSeqs:
    case ucode::AluScalarOpcode::kSgts:
    case ucode::AluScalarOpcode::kSges:
    case ucode::AluScalarOpcode::kSnes:
      return builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(
              spv::Op(kOps[size_t(instr.scalar_opcode)]), type_bool_,
              GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                                   0b0001),
              const_float_0_),
          const_float_1_, const_float_0_);

    case ucode::AluScalarOpcode::kFrcs:
    case ucode::AluScalarOpcode::kTruncs:
    case ucode::AluScalarOpcode::kFloors:
    case ucode::AluScalarOpcode::kSin:
    case ucode::AluScalarOpcode::kCos: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      if (instr.scalar_opcode == ucode::AluScalarOpcode::kFrcs &&
          RoundsTowardZero()) {
        return AluSubtract(
            a, builder_->createUnaryBuiltinCall(
                   type_float_, ext_inst_glsl_std_450_, GLSLstd450Floor, a));
      }
      return builder_->createUnaryBuiltinCall(
          type_float_, ext_inst_glsl_std_450_,
          GLSLstd450(kOps[size_t(instr.scalar_opcode)]), a);
    }
    case ucode::AluScalarOpcode::kExp: {
      return ScalarEstimate(
          ScalarEstimateOp::kExp,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
    }
    case ucode::AluScalarOpcode::kLog: {
      return ScalarEstimate(
          ScalarEstimateOp::kLog,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
    }
    case ucode::AluScalarOpcode::kSqrt: {
      return ScalarEstimate(
          ScalarEstimateOp::kSqrt,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
    }
    case ucode::AluScalarOpcode::kRsq: {
      return ScalarEstimate(
          ScalarEstimateOp::kRsq,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
    }
    case ucode::AluScalarOpcode::kLogc: {
      spv::Id result = ScalarEstimate(
          ScalarEstimateOp::kLog,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
      return builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(-INFINITY)),
          builder_->makeFloatConstant(-FLT_MAX), result);
    }
    case ucode::AluScalarOpcode::kRcpc: {
      spv::Id result = ScalarEstimate(
          ScalarEstimateOp::kRcp,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
      result = builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(-INFINITY)),
          builder_->makeFloatConstant(-FLT_MAX), result);
      return builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(INFINITY)),
          builder_->makeFloatConstant(FLT_MAX), result);
    }
    case ucode::AluScalarOpcode::kRcpf: {
      spv::Id result = ScalarEstimate(
          ScalarEstimateOp::kRcp,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
      result = builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(INFINITY)),
          const_float_0_, result);
      // Can't create -0.0f with makeFloatConstant due to float comparison
      // internally, cast to bit pattern.
      result = builder_->createTriOp(
          spv::OpSelect, type_uint_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(-INFINITY)),
          builder_->makeUintConstant(uint32_t(INT32_MIN)),
          builder_->createUnaryOp(spv::OpBitcast, type_uint_, result));
      return builder_->createUnaryOp(spv::OpBitcast, type_float_, result);
    }
    case ucode::AluScalarOpcode::kRcp: {
      return ScalarEstimate(
          ScalarEstimateOp::kRcp,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
    }
    case ucode::AluScalarOpcode::kRsqc: {
      spv::Id result = ScalarEstimate(
          ScalarEstimateOp::kRsq,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
      result = builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(-INFINITY)),
          builder_->makeFloatConstant(-FLT_MAX), result);
      return builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(INFINITY)),
          builder_->makeFloatConstant(FLT_MAX), result);
    }
    case ucode::AluScalarOpcode::kRsqf: {
      spv::Id result = ScalarEstimate(
          ScalarEstimateOp::kRsq,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001));
      result = builder_->createTriOp(
          spv::OpSelect, type_float_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(INFINITY)),
          const_float_0_, result);
      // Can't create -0.0f with makeFloatConstant due to float comparison
      // internally, cast to bit pattern.
      result = builder_->createTriOp(
          spv::OpSelect, type_uint_,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool_, result,
                                builder_->makeFloatConstant(-INFINITY)),
          builder_->makeUintConstant(uint32_t(INT32_MIN)),
          builder_->createUnaryOp(spv::OpBitcast, type_uint_, result));
      return builder_->createUnaryOp(spv::OpBitcast, type_float_, result);
    }

    case ucode::AluScalarOpcode::kSetpEq:
    case ucode::AluScalarOpcode::kSetpNe:
    case ucode::AluScalarOpcode::kSetpGt:
    case ucode::AluScalarOpcode::kSetpGe: {
      spv::Id predicate = builder_->createBinOp(
          spv::Op(kOps[size_t(instr.scalar_opcode)]), type_bool_,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001),
          const_float_0_);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float_, predicate,
                                   const_float_0_, const_float_1_);
    }
    case ucode::AluScalarOpcode::kSetpInv: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      spv::Id predicate = builder_->createBinOp(spv::OpFOrdEqual, type_bool_, a,
                                                const_float_1_);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(
          spv::OpSelect, type_float_, predicate, const_float_0_,
          builder_->createTriOp(
              spv::OpSelect, type_float_,
              builder_->createBinOp(spv::OpFOrdEqual, type_bool_, a,
                                    const_float_0_),
              const_float_1_, a));
    }
    case ucode::AluScalarOpcode::kSetpPop: {
      spv::Id a_minus_1 =
          AluSubtract(GetOperandComponents(operand_storage[0],
                                           instr.scalar_operands[0], 0b0001),
                      const_float_1_);
      spv::Id predicate = builder_->createBinOp(
          spv::OpFOrdLessThanEqual, type_bool_, a_minus_1, const_float_0_);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float_, predicate,
                                   const_float_0_, a_minus_1);
    }
    case ucode::AluScalarOpcode::kSetpClr:
      builder_->createStore(builder_->makeBoolConstant(false),
                            var_main_predicate_);
      return builder_->makeFloatConstant(FLT_MAX);
    case ucode::AluScalarOpcode::kSetpRstr: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      spv::Id predicate = builder_->createBinOp(spv::OpFOrdEqual, type_bool_, a,
                                                const_float_0_);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float_, predicate,
                                   const_float_0_, a);
    }

    case ucode::AluScalarOpcode::kKillsEq:
    case ucode::AluScalarOpcode::kKillsGt:
    case ucode::AluScalarOpcode::kKillsGe:
    case ucode::AluScalarOpcode::kKillsNe:
    case ucode::AluScalarOpcode::kKillsOne: {
      spv::Id condition = builder_->createBinOp(
          spv::Op(kOps[size_t(instr.scalar_opcode)]), type_bool_,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0],
                               0b0001),
          instr.scalar_opcode == ucode::AluScalarOpcode::kKillsOne
              ? const_float_1_
              : const_float_0_);
      KillPixel(condition, memexport_eM_potentially_written_before);
      // Kills write ps: 1.0 when the kill condition is true, 0.0 otherwise
      // (ucode.h). KillPixel demotes, so execution continues.
      return builder_->createTriOp(spv::OpSelect, type_float_, condition,
                                   const_float_1_, const_float_0_);
    }

    case ucode::AluScalarOpcode::kMulsc0:
    case ucode::AluScalarOpcode::kMulsc1: {
      spv::Id operand_0 = GetOperandComponents(
          operand_storage[0], instr.scalar_operands[0], 0b0001);
      spv::Id operand_1 = GetOperandComponents(
          operand_storage[1], instr.scalar_operands[1], 0b0001);
      spv::Id result = AluMultiply(operand_0, operand_1);
      if (!(instr.scalar_operands[0].GetIdenticalComponents(
                instr.scalar_operands[1]) &
            0b0001)) {
        // Shader Model 3: +0 or denormal * anything = +-0.
        result = ZeroIfAnyOperandIsZero(
            result, GetAbsoluteOperand(operand_0, instr.scalar_operands[0]),
            GetAbsoluteOperand(operand_1, instr.scalar_operands[1]));
      }
      return result;
    }
    case ucode::AluScalarOpcode::kAddsc0:
    case ucode::AluScalarOpcode::kAddsc1:
    case ucode::AluScalarOpcode::kSubsc0:
    case ucode::AluScalarOpcode::kSubsc1: {
      spv::Id a = GetOperandComponents(operand_storage[0],
                                       instr.scalar_operands[0], 0b0001);
      spv::Id b = GetOperandComponents(operand_storage[1],
                                       instr.scalar_operands[1], 0b0001);
      return (instr.scalar_opcode == ucode::AluScalarOpcode::kAddsc0 ||
              instr.scalar_opcode == ucode::AluScalarOpcode::kAddsc1)
                 ? AluAdd(a, b)
                 : AluSubtract(a, b);
    }

    case ucode::AluScalarOpcode::kRetainPrev:
      // Special case in ProcessAluInstruction - loading ps only if writing to
      // anywhere.
      return spv::NoResult;
  }

  assert_unhandled_case(instr.scalar_opcode);
  EmitTranslationError("Unknown ALU scalar operation");
  return spv::NoResult;
}

}  // namespace gpu
}  // namespace xe
