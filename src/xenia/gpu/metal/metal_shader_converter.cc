/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader_converter.h"

#include <algorithm>
#include <vector>

#include "metal_irconverter.h"

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

// SpirvShaderTranslator needs real IEEE semantics - NaN position culling, NaN
// flushed to 0 on saturate, ordered alpha test compares, INFINITY clamps in
// rcp/rsq/log - which MSC 4.0 optimizes away unless DisableNanInfOptimization
// keeps its pre-4.0 behavior. ForceTextureArray matches the 2D array textures
// the texture cache creates.
constexpr IRCompatibilityFlags kCompatibilityFlags = IRCompatibilityFlags(
    IRCompatibilityFlagForceTextureArray | IRCompatibilityFlagBoundsCheck |
    IRCompatibilityFlagVertexPositionInfToNan |
    IRCompatibilityFlagDisableNanInfOptimization);

// Descriptor set 0 bindings, which Mesa maps to space0 registers of the
// matching index. 2 and 3 are the per-stage index buffers the bindless lowering
// adds for descriptor sets 2 and 3.
constexpr uint32_t kSpace0SharedMemory = 0;
constexpr uint32_t kSpace0TextureIndicesVertex = 2;
constexpr uint32_t kSpace0TextureIndicesPixel = 3;

// The internal compute shaders put their source textures in descriptor set 1,
// and Mesa parks push constants in a high space so they can't collide with the
// descriptor sets (see MakeRuntimeConf in spirv_to_dxil_compiler.cc).
constexpr uint32_t kSpaceInternalComputeSource = 1;
constexpr uint32_t kSpaceInternalComputePushConstants = 31;
constexpr uint32_t kSlotInternalComputePushConstants = 1;

// Descriptor sets 2 (vertex textures) and 3 (pixel textures) map to the
// register spaces of the same number.
constexpr uint32_t kSpaceTexturesVertex = 2;
constexpr uint32_t kSpaceTexturesPixel = 3;

// Where Mesa parks Dozen's runtime data CBV, per MakeRuntimeConf.
constexpr uint32_t kSpaceRuntimeData = 31;

// SpirvShaderTranslator::ConstantBuffer, mapped to space1 registers of the
// matching index.
constexpr uint32_t kSpaceConstants = 1;

IRShaderStage ToIRShaderStage(MetalShaderStage stage) {
  switch (stage) {
    case MetalShaderStage::kVertex:
      return IRShaderStageVertex;
    case MetalShaderStage::kHull:
      return IRShaderStageHull;
    case MetalShaderStage::kDomain:
      return IRShaderStageDomain;
    case MetalShaderStage::kFragment:
      return IRShaderStageFragment;
    default:
      return IRShaderStageCompute;
  }
}

const char* StageName(MetalShaderStage stage) {
  switch (stage) {
    case MetalShaderStage::kVertex:
      return "vertex";
    case MetalShaderStage::kHull:
      return "hull";
    case MetalShaderStage::kDomain:
      return "domain";
    case MetalShaderStage::kFragment:
      return "fragment";
    default:
      return "compute";
  }
}

// MSC only fills the info block of the stage it compiled.
void CopyStageReflection(const IRShaderReflection* shader_reflection,
                         IRShaderStage ir_stage,
                         MetalShaderReflection& reflection_out) {
  switch (ir_stage) {
    case IRShaderStageVertex: {
      IRVersionedVSInfo info = {};
      if (IRShaderReflectionCopyVertexInfo(shader_reflection,
                                           IRReflectionVersion_1_0, &info)) {
        reflection_out.vertex_output_size_in_bytes =
            info.info_1_0.vertex_output_size_in_bytes;
        IRShaderReflectionReleaseVertexInfo(&info);
      }
      break;
    }
    case IRShaderStageHull: {
      IRVersionedHSInfo info = {};
      if (IRShaderReflectionCopyHullInfo(shader_reflection,
                                         IRReflectionVersion_1_0, &info)) {
        reflection_out.hs_max_patches_per_object_threadgroup =
            info.info_1_0.max_patches_per_object_threadgroup;
        reflection_out.hs_max_object_threads_per_patch =
            info.info_1_0.max_object_threads_per_patch;
        reflection_out.hs_input_control_point_count =
            info.info_1_0.input_control_point_count;
        reflection_out.hs_output_control_point_count =
            info.info_1_0.output_control_point_count;
        reflection_out.hs_output_control_point_size =
            info.info_1_0.output_control_point_size;
        reflection_out.hs_patch_constants_size =
            info.info_1_0.patch_constants_size;
        reflection_out.hs_tessellator_output_primitive =
            uint32_t(info.info_1_0.tessellator_output_primitive);
        reflection_out.hs_max_tessellation_factor =
            info.info_1_0.max_tessellation_factor;
        IRShaderReflectionReleaseHullInfo(&info);
      }
      break;
    }
    case IRShaderStageDomain: {
      IRVersionedDSInfo info = {};
      if (IRShaderReflectionCopyDomainInfo(shader_reflection,
                                           IRReflectionVersion_1_0, &info)) {
        reflection_out.ds_max_input_prims_per_mesh_threadgroup =
            info.info_1_0.max_input_prims_per_mesh_threadgroup;
        reflection_out.ds_input_control_point_count =
            info.info_1_0.input_control_point_count;
        reflection_out.ds_input_control_point_size =
            info.info_1_0.input_control_point_size;
        reflection_out.ds_patch_constants_size =
            info.info_1_0.patch_constants_size;
        IRShaderReflectionReleaseDomainInfo(&info);
      }
      break;
    }
    default:
      break;
  }
}

// What a root parameter should look like in MSC's reflection. Descriptor tables
// report no space or slot, so they carry IRResourceTypeInvalid.
struct RootParameterKey {
  IRResourceType type;
  uint32_t space;
  uint32_t slot;
};

RootParameterKey RootParameterKeyOf(MetalRootParameter parameter) {
  switch (parameter) {
    case MetalRootParameter::kSystemConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 0};
    case MetalRootParameter::kFloatConstantsVertex:
      return {IRResourceTypeCBV, kSpaceConstants, 1};
    case MetalRootParameter::kFloatConstantsPixel:
      return {IRResourceTypeCBV, kSpaceConstants, 2};
    case MetalRootParameter::kBoolLoopConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 3};
    case MetalRootParameter::kFetchConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 4};
    case MetalRootParameter::kRuntimeData:
      return {IRResourceTypeCBV, kSpaceRuntimeData, 0};
    case MetalRootParameter::kSharedMemorySrv:
      return {IRResourceTypeSRV, 0, kSpace0SharedMemory};
    case MetalRootParameter::kSharedMemoryUav:
      return {IRResourceTypeUAV, 0, kSpace0SharedMemory};
    case MetalRootParameter::kTextureIndicesVertex:
      return {IRResourceTypeSRV, 0, kSpace0TextureIndicesVertex};
    case MetalRootParameter::kTextureIndicesPixel:
      return {IRResourceTypeSRV, 0, kSpace0TextureIndicesPixel};
    default:
      return {IRResourceTypeInvalid, 0, 0};
  }
}

}  // namespace

MetalShaderConverter::~MetalShaderConverter() {
  if (root_signature_) {
    IRRootSignatureDestroy(root_signature_);
  }
  if (internal_compute_root_signature_) {
    IRRootSignatureDestroy(internal_compute_root_signature_);
  }
  if (internal_graphics_root_signature_) {
    IRRootSignatureDestroy(internal_graphics_root_signature_);
  }
}

bool MetalShaderConverter::Initialize() {
  IRCompiler* probe = IRCompilerCreate();
  if (!probe) {
    XELOGE("MetalShaderConverter: failed to create an IR compiler");
    return false;
  }
  IRCompilerDestroy(probe);

  root_signature_ = CreateRootSignature();
  if (!root_signature_) {
    return false;
  }
  if (!QueryRootParameterOffsets()) {
    return false;
  }

  internal_compute_root_signature_ = CreateInternalComputeRootSignature();
  if (!internal_compute_root_signature_) {
    return false;
  }
  if (!QueryInternalComputeRootParameterOffsets()) {
    return false;
  }

  internal_graphics_root_signature_ = CreateInternalGraphicsRootSignature();
  if (!internal_graphics_root_signature_) {
    return false;
  }
  if (!QueryInternalGraphicsRootParameterOffsets()) {
    return false;
  }

  is_available_ = true;
  XELOGI("MetalShaderConverter: initialized, {} byte top-level argument buffer",
         argument_buffer_size_);
  return true;
}

IRRootSignature* MetalShaderConverter::CreateRootSignature() const {
  IRDescriptorRange1 ranges[uint32_t(MetalRootParameter::kCount)] = {};
  IRRootDescriptorTable1 tables[uint32_t(MetalRootParameter::kCount)] = {};
  IRRootParameter1 parameters[uint32_t(MetalRootParameter::kCount)] = {};
  uint32_t range_count = 0;
  uint32_t table_count = 0;
  uint32_t parameter_count = 0;

  auto append_root_descriptor = [&](IRRootParameterType type,
                                    uint32_t shader_register,
                                    uint32_t register_space) {
    IRRootParameter1& parameter = parameters[parameter_count++];
    parameter.ParameterType = type;
    parameter.Descriptor.ShaderRegister = shader_register;
    parameter.Descriptor.RegisterSpace = register_space;
    parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
    parameter.ShaderVisibility = IRShaderVisibilityAll;
  };

  // The bindless lowering leaves the original texture and sampler declarations
  // in the DXIL, so the root signature still has to cover them.
  auto append_unbounded_table = [&](IRDescriptorRangeType type,
                                    uint32_t register_space) {
    IRDescriptorRange1& range = ranges[range_count++];
    range.RangeType = type;
    range.NumDescriptors = UINT32_MAX;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = register_space;
    range.Flags = IRDescriptorRangeFlagNone;
    range.OffsetInDescriptorsFromTableStart = 0;

    IRRootDescriptorTable1& table = tables[table_count++];
    table.NumDescriptorRanges = 1;
    table.pDescriptorRanges = &range;

    IRRootParameter1& parameter = parameters[parameter_count++];
    parameter.ParameterType = IRRootParameterTypeDescriptorTable;
    parameter.DescriptorTable = table;
    parameter.ShaderVisibility = IRShaderVisibilityAll;
  };

  // Order must match MetalRootParameter so the offsets line up one to one.
  append_root_descriptor(IRRootParameterTypeCBV, 0, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 1, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 2, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 3, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 4, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 0, kSpaceRuntimeData);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0SharedMemory, 0);
  append_root_descriptor(IRRootParameterTypeUAV, kSpace0SharedMemory, 0);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0TextureIndicesVertex,
                         0);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0TextureIndicesPixel, 0);
  append_unbounded_table(IRDescriptorRangeTypeSRV, kSpaceTexturesVertex);
  append_unbounded_table(IRDescriptorRangeTypeSRV, kSpaceTexturesPixel);
  append_unbounded_table(IRDescriptorRangeTypeSampler, kSpaceTexturesVertex);
  append_unbounded_table(IRDescriptorRangeTypeSampler, kSpaceTexturesPixel);

  IRRootSignatureDescriptor1 descriptor = {};
  descriptor.NumParameters = parameter_count;
  descriptor.pParameters = parameters;
  descriptor.NumStaticSamplers = 0;
  descriptor.pStaticSamplers = nullptr;
  // The lowered shader indexes ResourceDescriptorHeap / SamplerDescriptorHeap
  // directly, which MSC serves from the heaps bound at
  // kIRDescriptorHeapBindPoint and kIRSamplerHeapBindPoint.
  descriptor.Flags =
      IRRootSignatureFlags(IRRootSignatureFlagCBVSRVUAVHeapDirectlyIndexed |
                           IRRootSignatureFlagSamplerHeapDirectlyIndexed);

  IRVersionedRootSignatureDescriptor versioned = {};
  versioned.version = IRRootSignatureVersion_1_1;
  versioned.desc_1_1 = descriptor;

  IRError* error = nullptr;
  IRRootSignature* root_signature =
      IRRootSignatureCreateFromDescriptor(&versioned, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    XELOGE("MetalShaderConverter: failed to create the root signature: {}",
           message ? message : "unknown error");
    IRErrorDestroy(error);
    return nullptr;
  }
  return root_signature;
}

IRRootSignature* MetalShaderConverter::CreateInternalComputeRootSignature()
    const {
  // Two source textures: the color or depth source, and the stencil source of
  // a depth key. A shader that declares only the first still matches - the
  // table is a range, not a per-shader declaration.
  IRDescriptorRange1 source_range = {};
  source_range.RangeType = IRDescriptorRangeTypeSRV;
  source_range.NumDescriptors = 2;
  source_range.BaseShaderRegister = 0;
  source_range.RegisterSpace = kSpaceInternalComputeSource;
  source_range.Flags = IRDescriptorRangeFlagNone;
  source_range.OffsetInDescriptorsFromTableStart = 0;

  IRRootDescriptorTable1 source_table = {};
  source_table.NumDescriptorRanges = 1;
  source_table.pDescriptorRanges = &source_range;

  IRRootParameter1
      parameters[uint32_t(MetalInternalComputeRootParameter::kCount)] = {};

  // Order must match MetalInternalComputeRootParameter - table entries come
  // back from the reflection without a space or slot to match on.
  IRRootParameter1& edram_parameter =
      parameters[uint32_t(MetalInternalComputeRootParameter::kDestUav)];
  edram_parameter.ParameterType = IRRootParameterTypeUAV;
  edram_parameter.Descriptor.ShaderRegister = 0;
  edram_parameter.Descriptor.RegisterSpace = 0;
  edram_parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
  edram_parameter.ShaderVisibility = IRShaderVisibilityAll;

  IRRootParameter1& source_parameter =
      parameters[uint32_t(MetalInternalComputeRootParameter::kSourceTable)];
  source_parameter.ParameterType = IRRootParameterTypeDescriptorTable;
  source_parameter.DescriptorTable = source_table;
  source_parameter.ShaderVisibility = IRShaderVisibilityAll;

  IRRootParameter1& push_constant_parameter =
      parameters[uint32_t(MetalInternalComputeRootParameter::kPushConstants)];
  push_constant_parameter.ParameterType = IRRootParameterTypeCBV;
  push_constant_parameter.Descriptor.ShaderRegister =
      kSlotInternalComputePushConstants;
  push_constant_parameter.Descriptor.RegisterSpace =
      kSpaceInternalComputePushConstants;
  push_constant_parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
  push_constant_parameter.ShaderVisibility = IRShaderVisibilityAll;

  IRRootSignatureDescriptor1 descriptor = {};
  descriptor.NumParameters =
      uint32_t(MetalInternalComputeRootParameter::kCount);
  descriptor.pParameters = parameters;
  descriptor.NumStaticSamplers = 0;
  descriptor.pStaticSamplers = nullptr;
  descriptor.Flags = IRRootSignatureFlagNone;

  IRVersionedRootSignatureDescriptor versioned = {};
  versioned.version = IRRootSignatureVersion_1_1;
  versioned.desc_1_1 = descriptor;

  IRError* error = nullptr;
  IRRootSignature* root_signature =
      IRRootSignatureCreateFromDescriptor(&versioned, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    XELOGE(
        "MetalShaderConverter: failed to create the internal compute root "
        "signature: {}",
        message ? message : "unknown error");
    IRErrorDestroy(error);
    return nullptr;
  }
  return root_signature;
}

bool MetalShaderConverter::QueryInternalComputeRootParameterOffsets() {
  constexpr uint32_t kParameterCount =
      uint32_t(MetalInternalComputeRootParameter::kCount);
  size_t location_count =
      IRRootSignatureGetResourceCount(internal_compute_root_signature_);
  if (location_count != kParameterCount) {
    XELOGE(
        "MetalShaderConverter: the internal compute root signature reports {} "
        "top-level resources, expected {}",
        location_count, kParameterCount);
    return false;
  }
  std::vector<IRResourceLocation> locations(location_count);
  IRRootSignatureGetResourceLocations(internal_compute_root_signature_,
                                      locations.data());
  for (uint32_t i = 0; i < kParameterCount; ++i) {
    const IRResourceLocation& location = locations[i];
    internal_compute_root_parameter_offsets_[i] = location.topLevelOffset;
    internal_compute_argument_buffer_size_ =
        std::max(internal_compute_argument_buffer_size_,
                 uint32_t(location.topLevelOffset + location.sizeBytes));
  }
  return true;
}

bool MetalShaderConverter::QueryRootParameterOffsets() {
  std::fill(std::begin(root_parameter_offsets_),
            std::end(root_parameter_offsets_), UINT32_MAX);

  constexpr uint32_t kParameterCount = uint32_t(MetalRootParameter::kCount);
  size_t location_count = IRRootSignatureGetResourceCount(root_signature_);
  if (location_count != kParameterCount) {
    XELOGE(
        "MetalShaderConverter: the root signature reports {} top-level "
        "resources, expected {}",
        location_count, kParameterCount);
    return false;
  }
  std::vector<IRResourceLocation> locations(location_count);
  IRRootSignatureGetResourceLocations(root_signature_, locations.data());

  // Locations come back in declaration order, which MetalRootParameter mirrors,
  // so position is what identifies them - table entries carry no space or slot
  // to match on. Checking the rest against their declared register turns a
  // reordering on either side into a failure here rather than a misbind.
  for (uint32_t i = 0; i < kParameterCount; ++i) {
    const IRResourceLocation& location = locations[i];
    RootParameterKey key = RootParameterKeyOf(MetalRootParameter(i));
    if (key.type != IRResourceTypeInvalid &&
        (location.resourceType != key.type || location.space != key.space ||
         location.slot != key.slot)) {
      XELOGE(
          "MetalShaderConverter: root parameter {} is type {} space {} slot "
          "{}, "
          "expected type {} space {} slot {}",
          i, uint32_t(location.resourceType), location.space, location.slot,
          uint32_t(key.type), key.space, key.slot);
      return false;
    }
    root_parameter_offsets_[i] = location.topLevelOffset;
    argument_buffer_size_ =
        std::max(argument_buffer_size_,
                 uint32_t(location.topLevelOffset + location.sizeBytes));
  }
  return true;
}

MetalShaderConversionResult MetalShaderConverter::Convert(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil,
    bool tessellation_emulation) const {
  return ConvertWithRootSignature(stage, dxil, root_signature_,
                                  kCompatibilityFlags, tessellation_emulation);
}

IRRootSignature* MetalShaderConverter::CreateInternalGraphicsRootSignature()
    const {
  // Two textures per set: a depth source also binds its stencil view. A shader
  // that declares fewer, or only the first set, still matches - a table is a
  // range, not a per-shader declaration.
  IRDescriptorRange1 ranges[2] = {};
  IRRootDescriptorTable1 tables[2] = {};
  IRRootParameter1
      parameters[uint32_t(MetalInternalGraphicsRootParameter::kCount)] = {};

  // Order must match MetalInternalGraphicsRootParameter - table entries come
  // back from the reflection without a space or slot to match on.
  for (uint32_t i = 0; i < 2; ++i) {
    IRDescriptorRange1& range = ranges[i];
    range.RangeType = IRDescriptorRangeTypeSRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = i;
    range.Flags = IRDescriptorRangeFlagNone;
    range.OffsetInDescriptorsFromTableStart = 0;

    IRRootDescriptorTable1& table = tables[i];
    table.NumDescriptorRanges = 1;
    table.pDescriptorRanges = &range;

    IRRootParameter1& parameter = parameters[i];
    parameter.ParameterType = IRRootParameterTypeDescriptorTable;
    parameter.DescriptorTable = table;
    parameter.ShaderVisibility = IRShaderVisibilityAll;
  }

  IRRootParameter1& host_depth_buffer_parameter = parameters[uint32_t(
      MetalInternalGraphicsRootParameter::kHostDepthBufferUav)];
  host_depth_buffer_parameter.ParameterType = IRRootParameterTypeUAV;
  host_depth_buffer_parameter.Descriptor.ShaderRegister = 0;
  host_depth_buffer_parameter.Descriptor.RegisterSpace = 0;
  host_depth_buffer_parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
  host_depth_buffer_parameter.ShaderVisibility = IRShaderVisibilityAll;

  IRRootParameter1& push_constant_parameter =
      parameters[uint32_t(MetalInternalGraphicsRootParameter::kPushConstants)];
  push_constant_parameter.ParameterType = IRRootParameterTypeCBV;
  push_constant_parameter.Descriptor.ShaderRegister =
      kSlotInternalComputePushConstants;
  push_constant_parameter.Descriptor.RegisterSpace =
      kSpaceInternalComputePushConstants;
  push_constant_parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
  push_constant_parameter.ShaderVisibility = IRShaderVisibilityAll;

  IRRootSignatureDescriptor1 descriptor = {};
  descriptor.NumParameters =
      uint32_t(MetalInternalGraphicsRootParameter::kCount);
  descriptor.pParameters = parameters;
  descriptor.NumStaticSamplers = 0;
  descriptor.pStaticSamplers = nullptr;
  descriptor.Flags = IRRootSignatureFlagNone;

  IRVersionedRootSignatureDescriptor versioned = {};
  versioned.version = IRRootSignatureVersion_1_1;
  versioned.desc_1_1 = descriptor;

  IRError* error = nullptr;
  IRRootSignature* root_signature =
      IRRootSignatureCreateFromDescriptor(&versioned, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    XELOGE(
        "MetalShaderConverter: failed to create the internal graphics root "
        "signature: {}",
        message ? message : "unknown error");
    IRErrorDestroy(error);
    return nullptr;
  }
  return root_signature;
}

bool MetalShaderConverter::QueryInternalGraphicsRootParameterOffsets() {
  constexpr uint32_t kParameterCount =
      uint32_t(MetalInternalGraphicsRootParameter::kCount);
  size_t location_count =
      IRRootSignatureGetResourceCount(internal_graphics_root_signature_);
  if (location_count != kParameterCount) {
    XELOGE(
        "MetalShaderConverter: the internal graphics root signature reports {} "
        "top-level resources, expected {}",
        location_count, kParameterCount);
    return false;
  }
  std::vector<IRResourceLocation> locations(location_count);
  IRRootSignatureGetResourceLocations(internal_graphics_root_signature_,
                                      locations.data());
  for (uint32_t i = 0; i < kParameterCount; ++i) {
    const IRResourceLocation& location = locations[i];
    internal_graphics_root_parameter_offsets_[i] = location.topLevelOffset;
    internal_graphics_argument_buffer_size_ =
        std::max(internal_graphics_argument_buffer_size_,
                 uint32_t(location.topLevelOffset + location.sizeBytes));
  }
  return true;
}

MetalShaderConversionResult MetalShaderConverter::ConvertInternalGraphics(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil) const {
  // No compatibility flags, for the same reason internal compute uses none.
  return ConvertWithRootSignature(stage, dxil,
                                  internal_graphics_root_signature_,
                                  IRCompatibilityFlagNone, false);
}

MetalShaderConversionResult MetalShaderConverter::ConvertInternalCompute(
    const std::vector<uint8_t>& dxil) const {
  // No compatibility flags: they exist for guest shader semantics, and
  // ForceTextureArray in particular would compile the sources as array
  // textures, which the render target cache does not bind.
  return ConvertWithRootSignature(MetalShaderStage::kCompute, dxil,
                                  internal_compute_root_signature_,
                                  IRCompatibilityFlagNone, false);
}

MetalShaderConversionResult MetalShaderConverter::ConvertWithRootSignature(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil,
    IRRootSignature* root_signature, uint32_t compatibility_flags,
    bool tessellation_emulation) const {
  MetalShaderConversionResult result;
  if (!is_available_ || !root_signature) {
    result.error_message = "MetalShaderConverter is not initialized";
    return result;
  }
  if (dxil.empty()) {
    result.error_message = "Empty DXIL";
    return result;
  }

  IRObject* dxil_object =
      IRObjectCreateFromDXIL(dxil.data(), dxil.size(), IRBytecodeOwnershipNone);
  if (!dxil_object) {
    result.error_message = "Failed to create the DXIL object";
    return result;
  }
  IRCompiler* compiler = IRCompilerCreate();
  if (!compiler) {
    IRObjectDestroy(dxil_object);
    result.error_message = "Failed to create an IR compiler";
    return result;
  }

  IRCompilerSetCompatibilityFlags(compiler,
                                  IRCompatibilityFlags(compatibility_flags));
  IRCompilerSetGlobalRootSignature(compiler, root_signature);
  // Mesa embeds no root signature, but the flag also keeps MSC from inferring
  // one and disagreeing with ours.
  IRCompilerIgnoreRootSignature(compiler, true);
  bool synthesize_stage_in =
      tessellation_emulation && stage == MetalShaderStage::kVertex;
  if (tessellation_emulation) {
    IRCompilerEnableGeometryAndTessellationEmulation(compiler, true);
  }
  if (synthesize_stage_in) {
    // The tessellation emulation pipeline links a separate stage-in function
    // into the object function.
    IRCompilerSetStageInGenerationMode(
        compiler, IRStageInCodeGenerationModeUseSeparateStageInFunction);
  }

  IRError* error = nullptr;
  IRObject* metal_object =
      IRCompilerAllocCompileAndLink(compiler, nullptr, dxil_object, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    result.error_message = std::string("MSC compilation failed: ") +
                           (message ? message : "unknown error");
    IRErrorDestroy(error);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxil_object);
    return result;
  }
  if (!metal_object) {
    result.error_message = "MSC returned no object and no error";
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxil_object);
    return result;
  }

  IRShaderStage ir_stage = ToIRShaderStage(stage);
  IRMetalLibBinary* metallib = IRMetalLibBinaryCreate();
  if (metallib) {
    if (IRObjectGetMetalLibBinary(metal_object, ir_stage, metallib)) {
      size_t size = IRMetalLibGetBytecodeSize(metallib);
      if (size) {
        result.metallib.resize(size);
        IRMetalLibGetBytecode(metallib, result.metallib.data());
      }
    }
    IRMetalLibBinaryDestroy(metallib);
  }

  IRShaderReflection* reflection = IRShaderReflectionCreate();
  if (reflection) {
    if (IRObjectGetReflection(metal_object, ir_stage, reflection)) {
      const char* entry_point_name =
          IRShaderReflectionGetEntryPointFunctionName(reflection);
      if (entry_point_name) {
        result.entry_point_name = entry_point_name;
      }
      CopyStageReflection(reflection, ir_stage, result.reflection);
      if (synthesize_stage_in) {
        IRVersionedInputLayoutDescriptor input_layout = {};
        input_layout.version = IRInputLayoutDescriptorVersion_1;
        input_layout.desc_1_0.numElements = 0;
        IRMetalLibBinary* stage_in_metallib = IRMetalLibBinaryCreate();
        if (stage_in_metallib) {
          if (IRMetalLibSynthesizeStageInFunction(
                  compiler, reflection, &input_layout, stage_in_metallib)) {
            size_t size = IRMetalLibGetBytecodeSize(stage_in_metallib);
            if (size) {
              result.stage_in_metallib.resize(size);
              IRMetalLibGetBytecode(stage_in_metallib,
                                    result.stage_in_metallib.data());
            }
          }
          IRMetalLibBinaryDestroy(stage_in_metallib);
        }
      }
    }
    IRShaderReflectionDestroy(reflection);
  }

  IRObjectDestroy(metal_object);
  IRCompilerDestroy(compiler);
  IRObjectDestroy(dxil_object);

  if (result.metallib.empty()) {
    result.error_message =
        std::string("MSC produced an empty ") + StageName(stage) + " metallib";
    return result;
  }
  if (result.entry_point_name.empty()) {
    result.error_message = std::string("MSC reported no ") + StageName(stage) +
                           " entry point name";
    return result;
  }
  if (synthesize_stage_in && result.stage_in_metallib.empty()) {
    result.error_message = "MSC could not synthesize the stage-in function";
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
