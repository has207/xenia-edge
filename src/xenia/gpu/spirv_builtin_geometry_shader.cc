/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_builtin_geometry_shader.h"

#include <array>
#include <cstddef>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/glslang/SPIRV/GLSL.std.450.h"
#include "xenia/base/assert.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

std::vector<unsigned int> BuildGuestPrimitiveGeometryShaderSpirv(
    BuiltinGeometryShaderType type, uint32_t interpolator_count,
    uint32_t user_clip_plane_count, bool user_clip_plane_cull,
    bool has_vertex_kill_and, bool has_point_size, bool has_point_coordinates,
    unsigned int spirv_version, bool denorm_flush_to_zero_float32,
    bool signed_zero_inf_nan_preserve_float32, bool rounding_mode_rte_float32) {
  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  spv::ExecutionMode input_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t output_max_vertices = 0;
  switch (type) {
    case BuiltinGeometryShaderType::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case BuiltinGeometryShaderType::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case BuiltinGeometryShaderType::kQuadList:
      // 4 vertices passed via a line list with adjacency to a strip of 2
      // triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case BuiltinGeometryShaderType::kLineList:
      // Line (of a list or a strip) to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLines;
      input_primitive_vertex_count = 2;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      assert_unhandled_case(type);
  }

  uint32_t clip_distance_count = 0;
  uint32_t cull_distance_count = 0;
  if (user_clip_plane_cull) {
    cull_distance_count = user_clip_plane_count;
  } else {
    clip_distance_count = user_clip_plane_count;
  }
  cull_distance_count += has_vertex_kill_and;

  SpirvBuilder builder(spirv_version,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityGeometry);
  if (clip_distance_count) {
    builder.addCapability(spv::CapabilityClipDistance);
  }
  if (cull_distance_count) {
    builder.addCapability(spv::CapabilityCullDistance);
  }
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  // Match the vertex and pixel shaders' float controls. NaN preservation most
  // importantly keeps the NaN-position primitive discard below (used for the
  // vertex kill "or" operator and degenerate rectangles) from being folded
  // away. The execution modes are added once the entry point exists.
  if (spirv_version < spv::Spv_1_4 &&
      (denorm_flush_to_zero_float32 || signed_zero_inf_nan_preserve_float32 ||
       rounding_mode_rte_float32)) {
    builder.addExtension("SPV_KHR_float_controls");
  }
  if (denorm_flush_to_zero_float32) {
    builder.addCapability(spv::CapabilityDenormFlushToZero);
  }
  if (signed_zero_inf_nan_preserve_float32) {
    builder.addCapability(spv::CapabilitySignedZeroInfNanPreserve);
  }
  if (rounding_mode_rte_float32) {
    builder.addCapability(spv::CapabilityRoundingModeRTE);
  }

  std::vector<spv::Id> main_interface;

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_bool4 = builder.makeVectorType(type_bool, 4);
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(
                type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(
                type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  // System constants.
  // For points (lines only need point_screen_diameter_to_ndc_radius, as the
  // NDC size of a guest pixel):
  // - float2 point_constant_diameter
  // - float2 point_screen_diameter_to_ndc_radius
  enum PointConstant : uint32_t {
    kPointConstantConstantDiameter,
    kPointConstantScreenDiameterToNdcRadius,
    kPointConstantCount,
  };
  spv::Id type_system_constants = spv::NoType;
  if (type == BuiltinGeometryShaderType::kPointList ||
      type == BuiltinGeometryShaderType::kLineList) {
    id_vector_temp.clear();
    id_vector_temp.resize(kPointConstantCount);
    id_vector_temp[kPointConstantConstantDiameter] = type_float2;
    id_vector_temp[kPointConstantScreenDiameterToNdcRadius] = type_float2;
    type_system_constants =
        builder.makeStructType(id_vector_temp, "XeSystemConstants");
    builder.addMemberName(type_system_constants, kPointConstantConstantDiameter,
                          "point_constant_diameter");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantConstantDiameter,
        spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants,
                     point_constant_diameter)));
    builder.addMemberName(type_system_constants,
                          kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantScreenDiameterToNdcRadius,
        spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants,
                     point_screen_diameter_to_ndc_radius)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
        "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants,
                          spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));
    // SPIR-V 1.4+ lists every global the entry point references in its
    // interface, not just inputs and outputs.
    if (spirv_version >= spv::Spv_1_4) {
      main_interface.push_back(uniform_system_constants);
    }
  }

  // Inputs and outputs - matching glslang order, in gl_PerVertex gl_in[],
  // user-defined outputs, user-defined inputs, out gl_PerVertex.
  // TODO(Triang3l): Point parameters from the system uniform buffer.

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  // in gl_PerVertex gl_in[].
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_in_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_in_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_in_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_clip_distance));
  }
  // gl_CullDistance.
  uint32_t member_in_gl_per_vertex_cull_distance = UINT32_MAX;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
  // Structure and array.
  spv::Id type_struct_in_gl_per_vertex =
      builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_in_gl_per_vertex,
                        member_in_gl_per_vertex_position, "gl_Position");
  builder.addMemberDecoration(
      type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
      spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::Position));
  if (clip_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex,
                          member_in_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(
        type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::ClipDistance));
  }
  if (cull_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex,
                          member_in_gl_per_vertex_cull_distance,
                          "gl_CullDistance");
    builder.addMemberDecoration(
        type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::CullDistance));
  }
  builder.addDecoration(type_struct_in_gl_per_vertex, spv::DecorationBlock);
  spv::Id type_array_in_gl_per_vertex = builder.makeArrayType(
      type_struct_in_gl_per_vertex, const_input_primitive_vertex_count, 0);
  spv::Id in_gl_per_vertex =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_array_in_gl_per_vertex, "gl_in");
  main_interface.push_back(in_gl_per_vertex);

  uint32_t output_location = 0;

  // Interpolators outputs.
  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (uint32_t i = 0; i < interpolator_count; ++i) {
    spv::Id out_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassOutput, type_float4,
        fmt::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation,
                          int(output_location));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
    ++output_location;
  }

  // Point coordinate output.
  spv::Id out_point_coordinates = spv::NoResult;
  if (has_point_coordinates) {
    out_point_coordinates =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                               type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation,
                          int(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  uint32_t input_location = 0;

  // Interpolator inputs.
  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (uint32_t i = 0; i < interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count,
                              0),
        fmt::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation,
                          int(input_location));
    main_interface.push_back(in_interpolator);
    ++input_location;
  }

  // Point size input.
  spv::Id in_point_size = spv::NoResult;
  if (has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count,
                              0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation,
                          int(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  // out gl_PerVertex.
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_out_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_out_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_out_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance = builder.makeIntConstant(
        int32_t(member_out_gl_per_vertex_clip_distance));
  }
  // Structure.
  spv::Id type_struct_out_gl_per_vertex =
      builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_out_gl_per_vertex,
                        member_out_gl_per_vertex_position, "gl_Position");
  builder.addMemberDecoration(
      type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
      spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::Position));
  if (clip_distance_count) {
    builder.addMemberName(type_struct_out_gl_per_vertex,
                          member_out_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(
        type_struct_out_gl_per_vertex, member_out_gl_per_vertex_clip_distance,
        spv::DecorationBuiltIn, static_cast<int>(spv::BuiltIn::ClipDistance));
  }
  builder.addDecoration(type_struct_out_gl_per_vertex, spv::DecorationBlock);
  spv::Id out_gl_per_vertex =
      builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                             type_struct_out_gl_per_vertex, "");
  builder.addDecoration(out_gl_per_vertex, spv::DecorationInvariant);
  main_interface.push_back(out_gl_per_vertex);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGeometry, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }
  builder.addExecutionMode(main_function, input_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeInvocations, 1);
  builder.addExecutionMode(main_function, output_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeOutputVertices,
                           int(output_max_vertices));
  if (denorm_flush_to_zero_float32) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDenormFlushToZero,
                             32);
  }
  if (signed_zero_inf_nan_preserve_float32) {
    builder.addExecutionMode(main_function,
                             spv::ExecutionModeSignedZeroInfNanPreserve, 32);
  }
  if (rounding_mode_rte_float32) {
    builder.addExecutionMode(main_function, spv::ExecutionModeRoundingModeRTE,
                             32);
  }

  // Note that after every OpEmitVertex, all output variables are undefined.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
    id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
    spv::Id position_is_nan = builder.createUnaryOp(
        spv::OpAny, type_bool,
        builder.createUnaryOp(
            spv::OpIsNan, type_bool4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision)));
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block,
                                 spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(position_is_nan);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // TODO(Triang3l): For points, handle ps_ucp_mode (transform the host clip
  // space to the guest one, calculate the distances to the user clip planes,
  // cull using the distance from the center for modes 0, 1 and 2, cull and clip
  // per-vertex for modes 2 and 3) - except for the vertex kill flag.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_cull_distance));
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    for (uint32_t i = 0; i < cull_distance_count; ++i) {
      for (uint32_t j = 0; j < input_primitive_vertex_count; ++j) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
        id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
        spv::Id cull_distance_is_negative = builder.createBinOp(
            spv::OpFOrdLessThan, type_bool,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            const_float_0);
        if (cull_condition != spv::NoResult) {
          cull_condition =
              builder.createBinOp(spv::OpLogicalAnd, type_bool, cull_condition,
                                  cull_distance_is_negative);
        } else {
          cull_condition = cull_distance_is_negative;
        }
      }
    }
    assert_true(cull_condition != spv::NoResult);
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block,
                                 spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(cull_condition);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  switch (type) {
    case BuiltinGeometryShaderType::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      // Load the point diameter in guest pixels.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(int32_t(kPointConstantConstantDiameter)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_guest_diameter_x = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id point_guest_diameter_y = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);
      if (has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        spv::Id point_vertex_diameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_point_size,
                                      id_vector_temp),
            spv::NoPrecision);
        spv::Id point_vertex_diameter_written =
            builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                point_vertex_diameter, const_float_0);
        point_guest_diameter_x = builder.createTriOp(
            spv::OpSelect, type_float, point_vertex_diameter_written,
            point_vertex_diameter, point_guest_diameter_x);
        point_guest_diameter_y = builder.createTriOp(
            spv::OpSelect, type_float, point_vertex_diameter_written,
            point_vertex_diameter, point_guest_diameter_y);
      }

      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      spv::Id point_size_not_zero = builder.createBinOp(
          spv::OpLogicalAnd, type_bool,
          builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                              point_guest_diameter_x, const_float_0),
          builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                              point_guest_diameter_y, const_float_0));
      spv::Block& point_size_zero_predecessor = *builder.getBuildPoint();
      spv::Block& point_size_zero_then_block = builder.makeNewBlock();
      spv::Block& point_size_zero_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&point_size_zero_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(point_size_not_zero);
        branch_conditional_op->addIdOperand(
            point_size_zero_merge_block.getId());
        branch_conditional_op->addIdOperand(point_size_zero_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        point_size_zero_predecessor.addInstruction(
            std::move(branch_conditional_op));
      }
      point_size_zero_then_block.addPredecessor(&point_size_zero_predecessor);
      point_size_zero_merge_block.addPredecessor(&point_size_zero_predecessor);
      builder.setBuildPoint(&point_size_zero_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&point_size_zero_merge_block);

      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(
          int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants,
                                                       id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants,
                                                       id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                    id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w =
          builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_radius_y, point_w);

      // Load the inputs for the guest point.
      // Interpolators.
      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      for (uint32_t i = 0; i < interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput,
                                      in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      // Positions.
      spv::Id point_x =
          builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y =
          builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.createNoContractionBinOp(
            point_radius_add_op, type_float, point_x, point_radius_x);
        point_edge_y[i] = builder.createNoContractionBinOp(
            point_radius_add_op, type_float, point_y, point_radius_y);
      };
      spv::Id point_z =
          builder.createCompositeExtract(point_position, type_float, 2);
      // Clip distances.
      spv::Id point_clip_distances = spv::NoResult;
      if (clip_distance_count) {
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top-left, bottom-left, top-right, bottom-right order (chosen
        // arbitrarily, simply based on counterclockwise meaning front with
        // frontFace = VkFrontFace(0), but faceness is ignored for non-polygon
        // primitive types).
        uint32_t point_vertex_x = i >> 1;
        uint32_t point_vertex_y = i & 1;
        // Point coordinates.
        if (has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeFloatConstant(float(point_vertex_x)));
          id_vector_temp.push_back(
              builder.makeFloatConstant(float(point_vertex_y)));
          builder.createStore(
              builder.makeCompositeConstant(type_float2, id_vector_temp),
              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(point_edge_x[point_vertex_x]);
        id_vector_temp.push_back(point_edge_y[point_vertex_y]);
        id_vector_temp.push_back(point_z);
        id_vector_temp.push_back(point_w);
        spv::Id point_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            point_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        // TODO(Triang3l): Handle ps_ucp_mode properly, clip expanded points if
        // needed.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              point_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case BuiltinGeometryShaderType::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Get squares of edge lengths to choose the longest edge.
      // [0] - 12, [1] - 20, [2] - 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (uint32_t i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(int32_t((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(int32_t((2 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_1_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_1_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        spv::Id edge_x =
            builder.createBinOp(spv::OpFSub, type_float, edge_1_x, edge_0_x);
        spv::Id edge_y =
            builder.createBinOp(spv::OpFSub, type_float, edge_1_y, edge_0_y);
        edge_lengths[i] = builder.createBinOp(
            spv::OpFAdd, type_float,
            builder.createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
            builder.createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
      }

      // Choose the index of the first vertex in the strip based on which edge
      // is the longest, and calculate the indices of the other vertices.
      spv::Id vertex_indices[3];
      // If 12 > 20 && 12 > 01, then 12 is the longest edge, and the strip is
      // 0123. Otherwise, if 20 > 01, then 20 is the longest, and the strip is
      // 1203, but if not, 01 is the longest, and the strip is 2013.
      vertex_indices[0] = builder.createTriOp(
          spv::OpSelect, type_int,
          builder.createBinOp(
              spv::OpLogicalAnd, type_bool,
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[0], edge_lengths[1]),
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[0], edge_lengths[2])),
          const_int_0,
          builder.createTriOp(
              spv::OpSelect, type_int,
              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                  edge_lengths[1], edge_lengths[2]),
              const_int_1, const_int_2));
      for (uint32_t i = 1; i < 3; ++i) {
        // vertex_indices[i] = (vertex_indices[0] + i) % 3
        spv::Id vertex_index_without_wrapping =
            builder.createBinOp(spv::OpIAdd, type_int, vertex_indices[0],
                                builder.makeIntConstant(int32_t(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool,
                                vertex_index_without_wrapping, const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int,
                                vertex_index_without_wrapping, const_int_3));
      }

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero =
            builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(
                                     spv::StorageClassInput,
                                     in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero,
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              vertex_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      // Construct the fourth vertex.
      // Interpolators.
      for (uint32_t i = 0; i < interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      // Point coordinates.
      if (has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero,
                            out_point_coordinates);
      }
      // Position.
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                    id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.createNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.createNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                    id_vector_temp));
      // Clip distances.
      for (uint32_t i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(int32_t(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float, vertex_clip_distance_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput,
                                          in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        builder.createStore(
            vertex_clip_distance_v3,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
      }
      // Emit the vertex.
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case BuiltinGeometryShaderType::kQuadList: {
      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero =
            builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        spv::Id const_vertex_index =
            builder.makeIntConstant(int32_t(i ^ (i >> 1)));
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(
                                     spv::StorageClassInput,
                                     in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero,
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              vertex_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case BuiltinGeometryShaderType::kLineList: {
      // Host lines are rasterized 1 host pixel wide, but a guest line covers 1
      // guest pixel, which is draw_resolution_scale host pixels. Expand the
      // segment into a quad 1 guest pixel wide centered on the line, each end
      // keeping its own attributes.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
      spv::Id const_float_1 = builder.makeFloatConstant(1.0f);

      // Half of a guest pixel in the NDC along each axis - the constant is the
      // NDC radius of a 1 guest pixel diameter.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(
          int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id half_pixel_ndc_x = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id half_pixel_ndc_y = builder.createLoad(
          builder.createAccessChain(spv::StorageClassUniform,
                                    uniform_system_constants, id_vector_temp),
          spv::NoPrecision);

      // Load the positions, and get the vertices in half guest pixel units
      // (NDC divided by the NDC size of half a guest pixel) so the direction
      // is measured in screen space regardless of the viewport aspect.
      std::array<spv::Id, 2> line_vertex_x, line_vertex_y, line_vertex_z,
          line_vertex_w, line_vertex_pixels_x, line_vertex_pixels_y;
      for (uint32_t i = 0; i < 2; ++i) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id line_vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                      id_vector_temp),
            spv::NoPrecision);
        line_vertex_x[i] =
            builder.createCompositeExtract(line_vertex_position, type_float, 0);
        line_vertex_y[i] =
            builder.createCompositeExtract(line_vertex_position, type_float, 1);
        line_vertex_z[i] =
            builder.createCompositeExtract(line_vertex_position, type_float, 2);
        line_vertex_w[i] =
            builder.createCompositeExtract(line_vertex_position, type_float, 3);
        spv::Id line_vertex_inv_w = builder.createBinOp(
            spv::OpFDiv, type_float, const_float_1, line_vertex_w[i]);
        line_vertex_pixels_x[i] = builder.createBinOp(
            spv::OpFDiv, type_float,
            builder.createBinOp(spv::OpFMul, type_float, line_vertex_x[i],
                                line_vertex_inv_w),
            half_pixel_ndc_x);
        line_vertex_pixels_y[i] = builder.createBinOp(
            spv::OpFDiv, type_float,
            builder.createBinOp(spv::OpFMul, type_float, line_vertex_y[i],
                                line_vertex_inv_w),
            half_pixel_ndc_y);
      }

      // Direction of the line in screen space.
      spv::Id line_direction_x =
          builder.createBinOp(spv::OpFSub, type_float, line_vertex_pixels_x[1],
                              line_vertex_pixels_x[0]);
      spv::Id line_direction_y =
          builder.createBinOp(spv::OpFSub, type_float, line_vertex_pixels_y[1],
                              line_vertex_pixels_y[0]);
      spv::Id line_length = builder.createUnaryBuiltinCall(
          type_float, ext_inst_glsl_std_450, GLSLstd450Sqrt,
          builder.createBinOp(
              spv::OpFAdd, type_float,
              builder.createBinOp(spv::OpFMul, type_float, line_direction_x,
                                  line_direction_x),
              builder.createBinOp(spv::OpFMul, type_float, line_direction_y,
                                  line_direction_y)));

      // Drop zero-length lines (also NaN-safe), there's nothing to expand and
      // the normal would be undefined.
      spv::Id line_length_positive = builder.createBinOp(
          spv::OpFOrdGreaterThan, type_bool, line_length, const_float_0);
      spv::Block& line_degenerate_predecessor = *builder.getBuildPoint();
      spv::Block& line_degenerate_then_block = builder.makeNewBlock();
      spv::Block& line_degenerate_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&line_degenerate_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(line_length_positive);
        branch_conditional_op->addIdOperand(
            line_degenerate_merge_block.getId());
        branch_conditional_op->addIdOperand(line_degenerate_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        line_degenerate_predecessor.addInstruction(
            std::move(branch_conditional_op));
      }
      line_degenerate_then_block.addPredecessor(&line_degenerate_predecessor);
      line_degenerate_merge_block.addPredecessor(&line_degenerate_predecessor);
      builder.setBuildPoint(&line_degenerate_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&line_degenerate_merge_block);

      // Unit normal in screen space, then half a guest pixel along it in the
      // NDC (back to the per-axis NDC size of half a guest pixel).
      spv::Id line_inv_length = builder.createBinOp(spv::OpFDiv, type_float,
                                                    const_float_1, line_length);
      spv::Id line_offset_ndc_x = builder.createBinOp(
          spv::OpFMul, type_float,
          builder.createBinOp(spv::OpFMul, type_float,
                              builder.createUnaryOp(spv::OpFNegate, type_float,
                                                    line_direction_y),
                              line_inv_length),
          half_pixel_ndc_x);
      spv::Id line_offset_ndc_y = builder.createBinOp(
          spv::OpFMul, type_float,
          builder.createBinOp(spv::OpFMul, type_float, line_direction_x,
                              line_inv_length),
          half_pixel_ndc_y);

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (has_point_coordinates) {
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero =
            builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the strip: both sides of the first vertex, then of the second.
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t line_vertex_index = i >> 1;
        spv::Id const_line_vertex_index =
            builder.makeIntConstant(int32_t(line_vertex_index));
        spv::Op line_offset_add_op = (i & 1) ? spv::OpFAdd : spv::OpFSub;
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_line_vertex_index);
        for (uint32_t j = 0; j < interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(
                                     spv::StorageClassInput,
                                     in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero,
                              out_point_coordinates);
        }
        // Position - the NDC offset is transformed to the clip space by
        // multiplying by W.
        spv::Id line_vertex_w_value = line_vertex_w[line_vertex_index];
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.createNoContractionBinOp(
            line_offset_add_op, type_float, line_vertex_x[line_vertex_index],
            builder.createBinOp(spv::OpFMul, type_float, line_offset_ndc_x,
                                line_vertex_w_value)));
        id_vector_temp.push_back(builder.createNoContractionBinOp(
            line_offset_add_op, type_float, line_vertex_y[line_vertex_index],
            builder.createBinOp(spv::OpFMul, type_float, line_offset_ndc_y,
                                line_vertex_w_value)));
        id_vector_temp.push_back(line_vertex_z[line_vertex_index]);
        id_vector_temp.push_back(line_vertex_w_value);
        spv::Id line_quad_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            line_quad_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput,
                                      out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_line_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id line_vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput,
                                        in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(
              line_vertex_clip_distances,
              builder.createAccessChain(spv::StorageClassOutput,
                                        out_gl_per_vertex, id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      assert_unhandled_case(type);
  }

  // End the main function.
  builder.leaveFunction();

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  return shader_code;
}

}  // namespace gpu
}  // namespace xe
