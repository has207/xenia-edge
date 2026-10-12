/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
#define XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_

#include <cstdint>
#include <string>
#include <vector>

struct IRRootSignature;

namespace xe {
namespace gpu {
namespace metal {

// Root parameters the guest DXIL is compiled against, in the register layout
// Mesa's spirv_to_dxil emits for SpirvShaderTranslator's descriptor sets. Each
// occupies one slot of the top-level argument buffer, at the byte offset
// root_parameter_offset() reports.
enum class MetalRootParameter : uint32_t {
  kSystemConstants,       // CBV b0, space1
  kFloatConstantsVertex,  // CBV b1, space1
  kFloatConstantsPixel,   // CBV b2, space1
  kBoolLoopConstants,     // CBV b3, space1
  kFetchConstants,        // CBV b4, space1
  kRuntimeData,           // CBV b0, space31
  kSharedMemorySrv,       // SRV t0, space0
  kSharedMemoryUav,       // UAV u0, space0
  kTextureIndicesVertex,  // SRV t2, space0
  kTextureIndicesPixel,   // SRV t3, space0
  kTextureRangeVertex,    // SRV table, space2
  kTextureRangePixel,     // SRV table, space3
  kSamplerRangeVertex,    // Sampler table, space2
  kSamplerRangePixel,     // Sampler table, space3

  kCount,
};

// Root parameters of the render target cache's internal compute shaders, whose
// SPIR-V the shared emitters produce: descriptor set 0 is the EDRAM buffer,
// which Mesa gives u0 space0, set 1 holds the source textures as t0 and t1 of
// space1, and push constants land in Mesa's push constant CBV at b1 space31.
enum class MetalInternalComputeRootParameter : uint32_t {
  // UAV u0, space0 - the EDRAM buffer, or the resolve destination for a
  // direct resolve.
  kDestUav,
  kSourceTable,    // SRV table, space1
  kPushConstants,  // CBV b1, space31

  kCount,
};

// Root parameters of the render target cache's ownership transfer fragment
// shaders. The emitter numbers its descriptor sets densely per mode, so a
// second source set only exists for the modes that also read a host depth
// source, and Mesa gives each set the space of the same number.
enum class MetalInternalGraphicsRootParameter : uint32_t {
  kSourceTable0,  // SRV table, space0
  kSourceTable1,  // SRV table, space1
  // UAV u0, space0 - the EDRAM buffer, which the host depth copy modes read
  // the previous owner's depth back out of instead of a texture. Declared with
  // the same storage buffer form the dump shader's EDRAM buffer uses, so Mesa
  // gives it a UAV rather than a table entry.
  kHostDepthBufferUav,
  kPushConstants,  // CBV b1, space31

  kCount,
};

enum class MetalShaderStage {
  kVertex,
  kHull,
  kDomain,
  kFragment,
  kCompute,
};

// The reflection values IRRuntimeTessellationPipelineConfig and the hull/domain
// compatibility check need. Only the fields of the converted stage are filled.
struct MetalShaderReflection {
  uint32_t vertex_output_size_in_bytes = 0;

  uint32_t hs_max_patches_per_object_threadgroup = 0;
  uint32_t hs_max_object_threads_per_patch = 0;
  uint32_t hs_input_control_point_count = 0;
  uint32_t hs_output_control_point_count = 0;
  uint32_t hs_output_control_point_size = 0;
  uint32_t hs_patch_constants_size = 0;
  uint32_t hs_tessellator_output_primitive = 0;
  float hs_max_tessellation_factor = 0.0f;

  uint32_t ds_max_input_prims_per_mesh_threadgroup = 0;
  uint32_t ds_input_control_point_count = 0;
  uint32_t ds_input_control_point_size = 0;
  uint32_t ds_patch_constants_size = 0;
};

struct MetalShaderConversionResult {
  bool success = false;
  std::vector<uint8_t> metallib;
  // Entry point to look up in the metallib, from MSC's reflection.
  std::string entry_point_name;
  MetalShaderReflection reflection;
  // For the vertex stage with tessellation emulation, the synthesized stage-in
  // function that the object function links (no vertex attributes, the guest
  // fetches its vertices itself).
  std::vector<uint8_t> stage_in_metallib;
  std::string error_message;
};

// Compiles the DXIL that SpirvToDxilCompiler produces into Metal AIR with
// Apple's Metal Shader Converter, and describes the top-level argument buffer
// the resulting shaders expect.
//
// Thread safe once initialized: the root signature is built during Initialize
// and only read afterwards, and every Convert call uses its own IRCompiler.
class MetalShaderConverter {
 public:
  MetalShaderConverter() = default;
  ~MetalShaderConverter();

  MetalShaderConverter(const MetalShaderConverter&) = delete;
  MetalShaderConverter& operator=(const MetalShaderConverter&) = delete;

  // Builds the root signature and verifies the converter is usable.
  bool Initialize();

  bool is_available() const { return is_available_; }

  // Size of the top-level argument buffer the compiled shaders read, bound at
  // kIRArgumentBufferBindPoint.
  uint32_t argument_buffer_size() const { return argument_buffer_size_; }

  // Byte offset of a root parameter within the top-level argument buffer, or
  // UINT32_MAX if the root signature does not expose it.
  uint32_t root_parameter_offset(MetalRootParameter parameter) const {
    return root_parameter_offsets_[uint32_t(parameter)];
  }

  // tessellation_emulation turns on the object/mesh lowering MSC needs for
  // hull and domain stages, and for the vertex stage feeding them.
  MetalShaderConversionResult Convert(
      MetalShaderStage stage, const std::vector<uint8_t>& dxil,
      bool tessellation_emulation = false) const;

  // Converts an internal compute shader against the internal root signature
  // rather than the guest one.
  MetalShaderConversionResult ConvertInternalCompute(
      const std::vector<uint8_t>& dxil) const;

  uint32_t internal_compute_argument_buffer_size() const {
    return internal_compute_argument_buffer_size_;
  }
  uint32_t internal_compute_root_parameter_offset(
      MetalInternalComputeRootParameter parameter) const {
    return internal_compute_root_parameter_offsets_[uint32_t(parameter)];
  }

  // Converts an internal graphics shader against the internal graphics root
  // signature rather than the guest one.
  MetalShaderConversionResult ConvertInternalGraphics(
      MetalShaderStage stage, const std::vector<uint8_t>& dxil) const;

  uint32_t internal_graphics_argument_buffer_size() const {
    return internal_graphics_argument_buffer_size_;
  }
  uint32_t internal_graphics_root_parameter_offset(
      MetalInternalGraphicsRootParameter parameter) const {
    return internal_graphics_root_parameter_offsets_[uint32_t(parameter)];
  }

 private:
  // Builds the Mesa-layout root signature. Returns null on failure.
  IRRootSignature* CreateRootSignature() const;
  IRRootSignature* CreateInternalComputeRootSignature() const;
  IRRootSignature* CreateInternalGraphicsRootSignature() const;
  MetalShaderConversionResult ConvertWithRootSignature(
      MetalShaderStage stage, const std::vector<uint8_t>& dxil,
      IRRootSignature* root_signature, uint32_t compatibility_flags,
      bool tessellation_emulation) const;
  // Fills root_parameter_offsets_ and argument_buffer_size_ from the root
  // signature's reflection, failing if it does not describe the layout the
  // shaders were compiled against.
  bool QueryRootParameterOffsets();
  bool QueryInternalComputeRootParameterOffsets();
  bool QueryInternalGraphicsRootParameterOffsets();

  bool is_available_ = false;
  IRRootSignature* root_signature_ = nullptr;
  uint32_t argument_buffer_size_ = 0;
  uint32_t root_parameter_offsets_[uint32_t(MetalRootParameter::kCount)] = {};
  IRRootSignature* internal_compute_root_signature_ = nullptr;
  uint32_t internal_compute_argument_buffer_size_ = 0;
  uint32_t internal_compute_root_parameter_offsets_[uint32_t(
      MetalInternalComputeRootParameter::kCount)] = {};
  IRRootSignature* internal_graphics_root_signature_ = nullptr;
  uint32_t internal_graphics_argument_buffer_size_ = 0;
  uint32_t internal_graphics_root_parameter_offsets_[uint32_t(
      MetalInternalGraphicsRootParameter::kCount)] = {};
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHADER_CONVERTER_H_
