/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SPIRV_SHADER_TRANSLATOR_H_
#define XENIA_GPU_SPIRV_SHADER_TRANSLATOR_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shader_translator.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}  // namespace vulkan
}  // namespace ui
}  // namespace xe

namespace xe {
namespace gpu {

class SpirvShaderTranslator : public ShaderTranslator {
 public:
  union Modification {
    // If anything in this structure is changed in a way not compatible with
    // the previous layout, invalidate the pipeline storages by increasing this
    // version number! Backends add it to their dated
    // PipelineDescription::kVersion, so bumping either one is enough. Only
    // ever raise it, a reverted layout change needs another bump.
    static constexpr uint32_t kVersion = 23;

    enum class DepthStencilMode : uint32_t {
      kNoModifiers,
      // Early fragment tests - enable if alpha test and alpha to coverage are
      // disabled; ignored if anything in the shader blocks early Z writing.
      kEarlyHint,
      // Converting the depth to the closest 32-bit float representable exactly
      // as a 20e4 float, truncating towards zero, so SV_DepthLessEqual-style
      // conservative depth output (ExecutionModeDepthLess) can still allow
      // coarse early Z culling. MSAA depth must be per-sample, so the shader
      // runs at sample frequency.
      // Fixed-function viewport depth bounds must be snapped to float24 too.
      kFloat24Truncating,
      // Similar to kFloat24Truncating, but rounding to the nearest even, so
      // plain ExecutionModeDepthReplacing is used rather than DepthLess.
      kFloat24Rounding,
      // Host RT shader polygon offset for suspected coplanar redraws with tiny
      // biases. Writes the biased depth from the pixel shader and zeroes fixed
      // function depth bias to avoid host slope/quantization quirks. This path
      // is controlled by depth_bias_shader_offset.
      kPolygonOffset,
      kFloat24TruncatingPolygonOffset,
      kFloat24RoundingPolygonOffset,
      // TODO(Triang3l): Unorm24 (rounding) output mode.
    };

    struct {
      // uint32_t 0.
      // Interpolators written by the vertex shader and needed by the pixel
      // shader.
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      // For HostVertexShaderType kPointListAsTriangleStrip, whether to output
      // the point coordinates.
      // For other HostVertexShaderTypes (though truly reachable only for
      // kVertex), whether to output the point size.
      uint32_t output_point_parameters : 1;
      // Dynamically indexable register count from SQ_PROGRAM_CNTL.
      uint32_t dynamic_addressable_register_count : 8;
      // Pipeline stage and input configuration.
      Shader::HostVertexShaderType host_vertex_shader_type
          : Shader::kHostVertexShaderTypeBitCount;
      // User clip plane count, number of clip planes enabled (0-6).
      uint32_t user_clip_plane_count : 3;
      // If user_clip_plane_count is non-zero, whether they should be cull
      // distances instead of clip distances.
      uint32_t user_clip_plane_cull : 1;
      // Vertex kill (oPts.z) with the "and" operator - the primitive is culled
      // only when all of its vertices request the kill, emulated with an extra
      // cull distance written after the user clip plane cull distances. The
      // "or" operator sets the position to NaN instead and needs no bit here.
      uint32_t vertex_kill_and : 1;
      // For domain shaders - the tessellation mode, selecting the tessellation
      // evaluation shader spacing (Direct3D 12 sets it in the hull shaders, but
      // in SPIR-V the spacing lives in the domain shader). Discrete uses equal
      // spacing, continuous and adaptive use fractional even.
      xenos::TessellationMode tessellation_mode : 2;
    } vertex;
    struct PixelShaderModification {
      // uint32_t 0.
      // Interpolators written by the vertex shader and needed by the pixel
      // shader.
      uint32_t interpolator_mask : xenos::kMaxInterpolators;
      uint32_t interpolators_centroid : xenos::kMaxInterpolators;
      // uint32_t 1.
      // Dynamically indexable register count from SQ_PROGRAM_CNTL. Max 64.
      uint32_t dynamic_addressable_register_count : 7;
      uint32_t param_gen_enable : 1;
      uint32_t param_gen_interpolator : 4;
      // If param_gen_enable is set, this must be set for point primitives, and
      // must not be set for other primitive types - enables the point sprite
      // coordinates input, and also effects the flag bits in PsParamGen.
      uint32_t param_gen_point : 1;
      // For host render targets - depth / stencil output mode. The FSI path
      // has no such state, so it aliases these bits as fsi_msaa_samples. The
      // two paths are mutually exclusive per device.
      DepthStencilMode depth_stencil_mode : 3;
      // For host render targets with MIN/MAX blend op - the source blend factor
      // to pre-multiply the shader output by (since Vulkan/D3D12 MIN/MAX
      // ignores blend factors, but Xbox 360 applies them). kOne means no
      // pre-multiply. Only RT0 is supported for now.
      xenos::BlendFactor rt0_blend_rgb_factor_for_premult : 5;
      xenos::BlendFactor rt0_blend_a_factor_for_premult : 5;
      // For host render targets - which color render targets are actually
      // bound.
      uint32_t color_targets_used : xenos::kMaxColorRenderTargets;
      // Shared bit, two meanings, one per render target path - a device is on
      // one path or the other, and neither reads the other's meaning.
      // FSI path - set when no render target the shader writes has blending
      // enabled, so the EDRAM ROP skips emitting the blending path entirely.
      // Host render target path - the draw is inside a hybrid occlusion query
      // (occlusion_query_full_counters), so count the coverage before the
      // depth/stencil test into the ZPD counter's Total lane.
      uint32_t fsi_no_blending_or_zpd_total : 1;
      // PsParamGen and the memexport dedup must act like there's no resolution
      // scaling. Doesn't affect fetch offsets, those follow texture scale, not
      // from the draw. This is only set when the draw is native because of a
      // set scale threshold (FBO only).
      uint32_t resolution_scale_native : 1;

      // The host render target path's fields, assembled into one value so
      // the FSI path can carve them up without its own uses colliding. Safe
      // because GetPixelShaderModification only writes the fields themselves
      // on the host path, and a device is on one path or the other.
      static constexpr uint32_t kFsiBitCount = 18;
      static constexpr uint32_t kFsiRtFormatsShift = xenos::kMsaaSamplesBits;
      static_assert(
          kFsiRtFormatsShift + xenos::kMaxColorRenderTargets *
                                   xenos::kColorRenderTargetFormatBits <=
              kFsiBitCount,
          "The FSI fields have to fit in the host render target bits");

      // The shifts assume the widths declared above. Nothing in C++ can
      // measure a bitfield, so set_fsi_bits round-trips as the guard.
      uint32_t fsi_bits() const {
        return uint32_t(depth_stencil_mode) |
               (uint32_t(rt0_blend_rgb_factor_for_premult) << 3) |
               (uint32_t(rt0_blend_a_factor_for_premult) << 8) |
               (color_targets_used << 13) | (resolution_scale_native << 17);
      }
      void set_fsi_bits(uint32_t bits) {
        assert_zero(bits >> kFsiBitCount);
        depth_stencil_mode = DepthStencilMode(bits & 0x7);
        rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor((bits >> 3) & 0x1F);
        rt0_blend_a_factor_for_premult = xenos::BlendFactor((bits >> 8) & 0x1F);
        color_targets_used = (bits >> 13) & 0xF;
        resolution_scale_native = (bits >> 17) & 1;
        // Catches a field having been narrowed under the shifts above.
        assert_true(fsi_bits() == bits);
      }

      xenos::MsaaSamples fsi_msaa_samples() const {
        return xenos::MsaaSamples(
            fsi_bits() & ((uint32_t(1) << xenos::kMsaaSamplesBits) - 1));
      }
      void set_fsi_msaa_samples(xenos::MsaaSamples msaa_samples) {
        // The per-sample code is emitted for 1 << this many samples, and the
        // render target cache rejects the draw above 4x before this is read.
        assert_true(msaa_samples <= xenos::MsaaSamples::k4X);
        constexpr uint32_t kMask = (uint32_t(1) << xenos::kMsaaSamplesBits) - 1;
        set_fsi_bits((fsi_bits() & ~kMask) | uint32_t(msaa_samples));
      }

      // Only meaningful for render targets the shader writes, the rest stay
      // at zero.
      xenos::ColorRenderTargetFormat fsi_rt_format(uint32_t rt) const {
        assert_true(rt < xenos::kMaxColorRenderTargets);
        constexpr uint32_t kMask =
            (uint32_t(1) << xenos::kColorRenderTargetFormatBits) - 1;
        return xenos::ColorRenderTargetFormat(
            (fsi_bits() >>
             (kFsiRtFormatsShift + rt * xenos::kColorRenderTargetFormatBits)) &
            kMask);
      }
      bool fsi_no_blending() const { return fsi_no_blending_or_zpd_total != 0; }
      void set_fsi_no_blending(bool value) {
        fsi_no_blending_or_zpd_total = uint32_t(value);
      }
      bool zpd_total() const { return fsi_no_blending_or_zpd_total != 0; }
      void set_zpd_total(bool value) {
        fsi_no_blending_or_zpd_total = uint32_t(value);
      }

      void set_fsi_rt_format(uint32_t rt,
                             xenos::ColorRenderTargetFormat format) {
        assert_true(rt < xenos::kMaxColorRenderTargets);
        assert_zero(uint32_t(format) >> xenos::kColorRenderTargetFormatBits);
        uint32_t shift =
            kFsiRtFormatsShift + rt * xenos::kColorRenderTargetFormatBits;
        uint32_t mask =
            ((uint32_t(1) << xenos::kColorRenderTargetFormatBits) - 1) << shift;
        set_fsi_bits((fsi_bits() & ~mask) |
                     ((uint32_t(format) << shift) & mask));
      }
    } pixel;
    uint64_t value = 0;

    explicit Modification(uint64_t modification_value = 0)
        : value(modification_value) {
      static_assert_size(*this, sizeof(value));
    }
  };

  enum : uint32_t {
    kSysFlag_VertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit_Shift,
    kSysFlag_XYDividedByW_Shift,
    kSysFlag_ZDividedByW_Shift,
    kSysFlag_WNotReciprocal_Shift,
    kSysFlag_PrimitivePolygonal_Shift,
    kSysFlag_PrimitiveLine_Shift,
    kSysFlag_MsaaSamples_Shift,
    kSysFlag_DepthFloat24_Shift =
        kSysFlag_MsaaSamples_Shift + xenos::kMsaaSamplesBits,
    kSysFlag_AlphaPassIfLess_Shift,
    kSysFlag_AlphaPassIfEqual_Shift,
    kSysFlag_AlphaPassIfGreater_Shift,
    kSysFlag_ConvertColor0ToGamma_Shift,
    kSysFlag_ConvertColor1ToGamma_Shift,
    kSysFlag_ConvertColor2ToGamma_Shift,
    kSysFlag_ConvertColor3ToGamma_Shift,

    kSysFlag_FSIDepthStencil_Shift,
    kSysFlag_FSIDepthPassIfLess_Shift,
    kSysFlag_FSIDepthPassIfEqual_Shift,
    kSysFlag_FSIDepthPassIfGreater_Shift,
    // 1 to write new depth to the depth buffer, 0 to keep the old one if the
    // depth test passes.
    kSysFlag_FSIDepthWrite_Shift,
    kSysFlag_FSIStencilTest_Shift,
    // If the depth / stencil test has failed, but resulted in a stencil value
    // that is different than the one currently in the depth buffer, write it
    // anyway and don't run the rest of the shader (to check if the sample may
    // be discarded some way) - use when alpha test and alpha to coverage are
    // disabled. Ignored by the shader if not applicable to it (like if it has
    // kill instructions or writes the depth output).
    // TODO(Triang3l): Investigate replacement with an alpha-to-mask flag,
    // checking `(flags & (alpha test | alpha to mask)) == (always | disabled)`,
    // taking into account the potential relation with occlusion queries (but
    // should be safe at least temporarily).
    kSysFlag_FSIDepthStencilEarlyWrite_Shift,

    kSysFlag_Count,

    // For HostVertexShaderType kVertex, if fullDrawIndexUint32 is not
    // supported (ignored otherwise), whether to fetch the index manually
    // (32-bit only - 16-bit indices are always fetched via the Vulkan index
    // buffer).
    kSysFlag_VertexIndexLoad = 1u << kSysFlag_VertexIndexLoad_Shift,
    // For HostVertexShaderTypes kMemExportCompute, kPointListAsTriangleStrip,
    // kRectangleListAsTriangleStrip, whether the vertex index needs to be
    // loaded from the index buffer (rather than using autogenerated indices),
    // and whether it's 32-bit. This is separate from kSysFlag_VertexIndexLoad
    // because the same system constants may be used for the memexporting
    // compute shader and the vertex shader for the same draw, but
    // kSysFlag_VertexIndexLoad may be not needed.
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad =
        1u << kSysFlag_ComputeOrPrimitiveVertexIndexLoad_Shift,
    kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit =
        1u << kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit_Shift,
    kSysFlag_XYDividedByW = 1u << kSysFlag_XYDividedByW_Shift,
    kSysFlag_ZDividedByW = 1u << kSysFlag_ZDividedByW_Shift,
    kSysFlag_WNotReciprocal = 1u << kSysFlag_WNotReciprocal_Shift,
    kSysFlag_PrimitivePolygonal = 1u << kSysFlag_PrimitivePolygonal_Shift,
    kSysFlag_PrimitiveLine = 1u << kSysFlag_PrimitiveLine_Shift,
    kSysFlag_DepthFloat24 = 1u << kSysFlag_DepthFloat24_Shift,
    kSysFlag_AlphaPassIfLess = 1u << kSysFlag_AlphaPassIfLess_Shift,
    kSysFlag_AlphaPassIfEqual = 1u << kSysFlag_AlphaPassIfEqual_Shift,
    kSysFlag_AlphaPassIfGreater = 1u << kSysFlag_AlphaPassIfGreater_Shift,
    kSysFlag_ConvertColor0ToGamma = 1u << kSysFlag_ConvertColor0ToGamma_Shift,
    kSysFlag_ConvertColor1ToGamma = 1u << kSysFlag_ConvertColor1ToGamma_Shift,
    kSysFlag_ConvertColor2ToGamma = 1u << kSysFlag_ConvertColor2ToGamma_Shift,
    kSysFlag_ConvertColor3ToGamma = 1u << kSysFlag_ConvertColor3ToGamma_Shift,
    kSysFlag_FSIDepthStencil = 1u << kSysFlag_FSIDepthStencil_Shift,
    kSysFlag_FSIDepthPassIfLess = 1u << kSysFlag_FSIDepthPassIfLess_Shift,
    kSysFlag_FSIDepthPassIfEqual = 1u << kSysFlag_FSIDepthPassIfEqual_Shift,
    kSysFlag_FSIDepthPassIfGreater = 1u << kSysFlag_FSIDepthPassIfGreater_Shift,
    kSysFlag_FSIDepthWrite = 1u << kSysFlag_FSIDepthWrite_Shift,
    kSysFlag_FSIStencilTest = 1u << kSysFlag_FSIStencilTest_Shift,
    kSysFlag_FSIDepthStencilEarlyWrite =
        1u << kSysFlag_FSIDepthStencilEarlyWrite_Shift,
  };
  static_assert(kSysFlag_Count <= 32, "Too many flags in the system constants");

  // IF SYSTEM CONSTANTS ARE CHANGED OR ADDED, THE FOLLOWING MUST BE UPDATED:
  // - SystemConstantIndex enum.
  // - Structure members in BeginTranslation.
  //
  // Using the std140 layout - vec2 must be aligned to 8 bytes, vec3 and vec4 to
  // 16 bytes.
  struct SystemConstants {
    uint32_t flags;
    uint32_t vertex_index_load_address;
    uint32_t vertex_index_count;
    xenos::Endian vertex_index_endian;
    int32_t vertex_base_index;
    uint32_t padding_after_vertex_index[3];

    float ndc_scale[3];
    float point_vertex_diameter_min;

    float ndc_offset[3];
    float point_vertex_diameter_max;

    float point_constant_diameter[2];
    // Diameter in guest screen coordinates > radius (0.5 * diameter) in the NDC
    // for the host viewport.
    float point_screen_diameter_to_ndc_radius[2];

    // Each byte contains post-swizzle TextureSign values for each of the needed
    // components of each of the 32 used texture fetch constants.
    uint32_t texture_swizzled_signs[8];

    // If the imageViewFormatSwizzle portability subset is not supported, the
    // component swizzle (taking both guest and host swizzles into account) to
    // apply to the result directly in the shader code. In each uint32_t,
    // swizzles for 2 texture fetch constants (in bits 0:11 and 12:23).
    uint32_t texture_swizzles[16];

    // Whether the contents of each texture in fetch constants comes from a
    // resolve operation (bit per texture, 32 textures max).
    uint32_t textures_resolved;

    float alpha_test_reference;
    // If alpha to mask is disabled, the entire alpha_to_mask value must be 0.
    // If alpha to mask is enabled, bits 0:7 are sample offsets, and bit 8 must
    // be 1.
    uint32_t alpha_to_mask;

    // UINT32_MAX when the draw is outside an active ZPD segment, which is used
    // as a skip writing sentinel to the FSI counter buffer.
    uint32_t zpd_fsi_counter_index;

    uint32_t edram_32bpp_tile_pitch_dwords_scaled;
    uint32_t edram_depth_base_dwords_scaled;
    uint32_t padding_after_depth_info[2];

    float color_exp_bias[4];

    float edram_poly_offset_front_scale;
    float edram_poly_offset_back_scale;
    float edram_poly_offset_front_offset;
    float edram_poly_offset_back_offset;

    union {
      struct {
        uint32_t edram_stencil_front_reference_masks;
        uint32_t edram_stencil_front_func_ops;

        uint32_t edram_stencil_back_reference_masks;
        uint32_t edram_stencil_back_func_ops;
      };
      struct {
        uint32_t edram_stencil_front[2];
        uint32_t edram_stencil_back[2];
      };
    };

    uint32_t edram_rt_base_dwords_scaled[4];

    // xenos_draw.glsli reads the tessellation fields below at fixed std140
    // offsets, so these bytes can't be reclaimed.
    uint32_t padding_after_rt_base_dwords_scaled[4];

    // Render target blending options - RB_BLENDCONTROL, with only the relevant
    // options (factors and operations - AND 0x1FFF1FFF). If 0x00010001
    // (1 * src + 0 * dst), blending is disabled for the render target.
    uint32_t edram_rt_blend_factors_ops[4];

    // Format info - mask to apply to the old packed RT data, and to apply as
    // inverted to the new packed data, before storing (more or less the inverse
    // of the write mask packed like render target channels). This can be used
    // to bypass unpacking if blending is not used. If 0 and not blending,
    // reading the old data from the EDRAM buffer is not required.
    uint32_t edram_rt_keep_mask[4][2];

    // Format info - values to clamp the color to before blending or storing.
    // Low color, low alpha, high color, high alpha.
    float edram_rt_clamp[4][4];

    // The constant blend factor for the respective modes.
    float edram_blend_constant[4];

    // User clip planes. Also read by the GLSL tessellation helpers in
    // xenos_draw.glsli at fixed std140 offsets, guarded by the static_assert
    // below.
    float user_clip_planes[6][4];
    // Tessellation factor range: [0] = min, [1] = max. 1.0 is added on the CPU
    // per Xbox 360 docs. fractional_even partitioning needs min >= 2.0.
    float tessellation_factor_range[2];
    float tessellation_padding0[2];
    uint32_t tessellation_vertex_index_endian;
    uint32_t tessellation_vertex_index_offset;
    uint32_t tessellation_vertex_index_min_max[2];

    // Ucode interpreter VS placeholder. Guest VS ucode location in shared
    // memory as a dword address, and its control-flow instruction count. Zero
    // unless the interpreter is bound for this draw. Appended after the
    // tessellation tail so the static_assert offsets above are unaffected.
    // Read as std140 uint4 [34].xy by ucode_interpreter.vs.slang.
    uint32_t interpreter_ucode_base_dwords;
    uint32_t interpreter_cf_instr_count;
    uint32_t texture_integer_scale_pad[2];

    // Packed fixed texture conversion (see GetIntegerScaleBits).
    // Every component occupies 6 bits in bits 0:23
    //   bits 0:3 = component_bits - 1
    //   bits 4:5 = xenos::TextureSign
    // bit 24 = normalized num_format
    // bit 26 = point sampled fetch constant
    // Zero means no conversion.
    // Appended at the very tail (std140 uint4 [35]) so it disturbs neither the
    // xenos_draw.glsli tessellation offsets nor the interpreter [34] slot.
    uint32_t texture_integer_scale_bits[32];

    // PA_SC_WINDOW_OFFSET the PsParamGen position needs added when the offset
    // is carried in the EDRAM bases rather than the viewport.
    // 0 when it's in the viewport.
    float param_gen_window_offset[2];
    uint32_t param_gen_window_offset_padding[2];
  };

  // xenos_draw.glsli reads these tessellation fields from the system constants
  // UBO at these fixed std140 offsets. Keep them in sync.
  static_assert(
      offsetof(SystemConstants, tessellation_factor_range) == 512 &&
          offsetof(SystemConstants, tessellation_vertex_index_endian) == 528 &&
          offsetof(SystemConstants, tessellation_vertex_index_offset) == 532 &&
          offsetof(SystemConstants, tessellation_vertex_index_min_max) == 536,
      "Keep xenos_draw.glsli tessellation offsets in sync with "
      "SystemConstants");

  enum ConstantBuffer : uint32_t {
    kConstantBufferSystem,
    kConstantBufferFloatVertex,
    kConstantBufferFloatPixel,
    kConstantBufferBoolLoop,
    kConstantBufferFetch,

    kConstantBufferCount,
  };

  // The minimum limit for maxPerStageDescriptorStorageBuffers is 4, and for
  // maxStorageBufferRange it's 128 MB. These are the values of those limits on
  // Arm Mali as of November 2020. Xenia needs 512 MB shared memory to be bound,
  // therefore SSBOs must only be used for shared memory - all other storage
  // resources must be images or texel buffers.
  enum DescriptorSet : uint32_t {
    // According to the "Pipeline Layout Compatibility" section of the Vulkan
    // specification:
    // "Two pipeline layouts are defined to be "compatible for set N" if they
    //  were created with identically defined descriptor set layouts for sets
    //  zero through N, and if they were created with identical push constant
    //  ranges."
    // "Place the least frequently changing descriptor sets near the start of
    //  the pipeline layout, and place the descriptor sets representing the most
    //  frequently changing resources near the end. When pipelines are switched,
    //  only the descriptor set bindings that have been invalidated will need to
    //  be updated and the remainder of the descriptor set bindings will remain
    //  in place."
    // This is partially the reverse of the Direct3D 12's rule of placing the
    // most frequently changed descriptor sets in the beginning. Here all
    // descriptor sets with an immutable layout are placed first, in reverse
    // frequency of changing, and sets that may be different for different
    // pipeline states last.

    // Always the same descriptor set layouts for all pipeline layouts:

    // Never changed.
    kDescriptorSetSharedMemoryAndEdram,
    // Changed in case of changes in the data.
    kDescriptorSetConstants,

    // Mutable part of the pipeline layout:
    kDescriptorSetMutableLayoutsStart,

    // Rarely used at all, but may be changed at an unpredictable rate when
    // vertex textures are used (for example, for bones of an object, which may
    // consist of multiple draw commands with different materials).
    kDescriptorSetTexturesVertex = kDescriptorSetMutableLayoutsStart,
    // Per-material textures.
    kDescriptorSetTexturesPixel,

    kDescriptorSetCount,
  };
  static_assert(
      kDescriptorSetCount <= 4,
      "The number of descriptor sets used by translated shaders must be within "
      "the minimum Vulkan maxBoundDescriptorSets requirement of 4, which is "
      "the limit on most GPUs used in Android devices - Arm Mali, Imagination "
      "PowerVR, Qualcomm Adreno 6xx and older, as well as on old PC Nvidia "
      "drivers");

  // "Xenia Emulator Microcode Translator".
  // https://github.com/KhronosGroup/SPIRV-Headers/blob/c43a43c7cc3af55910b9bec2a71e3e8a622443cf/include/spirv/spir-v.xml#L79
  static constexpr uint32_t kSpirvMagicToolId = 26;

  struct Features {
    explicit Features(const ui::vulkan::VulkanDevice* vulkan_device);
    explicit Features(bool all = false);

    unsigned int spirv_version;

    uint32_t max_storage_buffer_range;

    bool full_draw_index_uint32;

    bool vertex_pipeline_stores_and_atomics;
    bool fragment_stores_and_atomics;

    bool clip_distance;
    bool cull_distance;

    bool image_view_format_swizzle;

    bool signed_zero_inf_nan_preserve_float32;
    bool denorm_flush_to_zero_float32;
    bool rounding_mode_rte_float32;
    bool rounding_mode_rtz_float32 = false;

    bool fragment_shader_sample_interlock;

    bool demote_to_helper_invocation;

    bool fragment_shader_barycentric;

    bool allow_float_contraction = false;
  };

  SpirvShaderTranslator(const Features& features,
                        bool native_2x_msaa_with_attachments,
                        bool native_2x_msaa_no_attachments,
                        bool edram_fragment_shader_interlock,
                        bool precise_interpolation,
                        // No defaults - a missing argument would shift the
                        // ones after it and silently take a wrong scale.
                        uint32_t draw_resolution_scale_x,
                        uint32_t draw_resolution_scale_y)
      : features_(features),
        native_2x_msaa_with_attachments_(native_2x_msaa_with_attachments),
        native_2x_msaa_no_attachments_(native_2x_msaa_no_attachments),
        edram_fragment_shader_interlock_(edram_fragment_shader_interlock),
        precise_interpolation_(precise_interpolation),
        zpd_full_counters_(cvars::occlusion_query_full_counters),
        draw_resolution_scale_x_(draw_resolution_scale_x),
        draw_resolution_scale_y_(draw_resolution_scale_y) {}

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

  // Feature set the translator emits for, so host helper shaders (e.g. the
  // built-in geometry shader) can match the SPIR-V version and float controls.
  const Features& features() const { return features_; }

  static constexpr uint32_t GetSharedMemoryStorageBufferCountLog2(
      uint32_t max_storage_buffer_range) {
    if (max_storage_buffer_range >= 512 * 1024 * 1024) {
      return 0;
    }
    if (max_storage_buffer_range >= 256 * 1024 * 1024) {
      return 1;
    }
    return 2;
  }
  uint32_t GetSharedMemoryStorageBufferCountLog2() const {
    return GetSharedMemoryStorageBufferCountLog2(
        features_.max_storage_buffer_range);
  }

  // Creates a special fragment shader without color outputs - this resets the
  // state of the translator.
  // Creates a synthetic depth-only fragment shader. When depth_stencil_mode is
  // a float24 mode, the shader reads gl_FragCoord.z, converts to float24, and
  // writes the result to gl_FragDepth - matching the substitute pixel shader
  // the DXBC backend uses when a guest draw has no pixel shader.
  // input_modification is the modification of the pixel shader stood in for.
  // Its inputs are declared without being read so the signature links with
  // the stage before it.
  std::vector<uint8_t> CreateDepthOnlyFragmentShader(
      Modification::DepthStencilMode depth_stencil_mode =
          Modification::DepthStencilMode::kNoModifiers,
      bool zpd_total = false, bool viz_survey = false,
      uint64_t input_modification = 0);
  // FSI variant - specialized for one guest sample count instead of a host
  // depth / stencil mode.
  std::vector<uint8_t> CreateDepthOnlyFragmentShader(
      xenos::MsaaSamples fsi_msaa_samples, bool viz_survey = false,
      uint64_t input_modification = 0);
  // The part of a pixel shader modification that decides its input signature,
  // the interpolator count and point coordinates, in a canonical form.
  static uint64_t GetPixelShaderInputModification(uint64_t modification);

  // Common functions useful not only for the translator, but also for EDRAM
  // emulation via conventional render targets.

  // Converts the color value externally clamped to [0, 31.875] to 7e3 floating
  // point, with zeros in bits 10:31, rounding to the nearest even.
  static spv::Id PreClampedFloat32To7e3(SpirvBuilder& builder,
                                        spv::Id f32_scalar,
                                        spv::Id ext_inst_glsl_std_450);
  // Same as PreClampedFloat32To7e3, but clamps the input to [0, 31.875].
  static spv::Id UnclampedFloat32To7e3(SpirvBuilder& builder,
                                       spv::Id f32_scalar,
                                       spv::Id ext_inst_glsl_std_450);
  // Converts the 7e3 number in bits [f10_shift, f10_shift + 10) to a 32-bit
  // float.
  static spv::Id Float7e3To32(SpirvBuilder& builder, spv::Id f10_uint_scalar,
                              uint32_t f10_shift, bool result_as_uint,
                              spv::Id ext_inst_glsl_std_450);
  // Converts the depth value externally clamped to the representable [0, 2)
  // range to 20e4 floating point, with zeros in bits 24:31, rounding to the
  // nearest even or towards zero. If remap_from_0_to_0_5 is true, it's assumed
  // that 0...1 is pre-remapped to 0...0.5 in the input.
  static spv::Id PreClampedDepthTo20e4(SpirvBuilder& builder,
                                       spv::Id f32_scalar,
                                       bool round_to_nearest_even,
                                       bool remap_from_0_to_0_5,
                                       spv::Id ext_inst_glsl_std_450);
  // Converts the 20e4 number in bits [f24_shift, f24_shift + 24) to a 32-bit
  // float.
  static spv::Id Depth20e4To32(SpirvBuilder& builder, spv::Id f24_uint_scalar,
                               uint32_t f24_shift, bool remap_to_0_to_0_5,
                               bool result_as_uint,
                               spv::Id ext_inst_glsl_std_450);
  // Piecewise-linear gamma conversions for k_8_8_8_8_GAMMA values stored as
  // linear UNORM16. Values may be scalars or vectors of up to 3 components.
  // Unless pre_saturated is true, inputs are clamped to [0, 1] (NaN to 0).
  static spv::Id PWLGammaToLinear(SpirvBuilder* builder_, spv::Id value,
                                  bool pre_saturated,
                                  spv::Id ext_inst_glsl_std_450);
  static spv::Id LinearToPWLGamma(SpirvBuilder* builder_, spv::Id value,
                                  bool pre_saturated,
                                  spv::Id ext_inst_glsl_std_450);

 protected:
  void Reset() override;

  uint32_t GetModificationRegisterCount() const override;

  void StartTranslation() override;

  std::vector<uint8_t> CompleteTranslation() override;

  void PostTranslation() override;

  void ProcessLabel(uint32_t cf_index) override;

  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(
      const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(
      const ParsedLoopEndInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                               uint8_t export_eM) override;

  void ProcessVertexFetchInstruction(
      const ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before) override;

 private:
  struct TextureBinding {
    uint32_t fetch_constant;
    // Stacked and 3D are separate TextureBindings.
    xenos::FetchOpDimension dimension;
    bool is_signed;

    spv::Id variable;
  };

  struct SamplerBinding {
    uint32_t fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;

    spv::Id variable;
  };

  // Builder helpers.
  spv::Id SpirvSmearScalarResultOrConstant(spv::Id scalar, spv::Id vector_type);

  Modification GetSpirvShaderModification() const {
    return Modification(current_translation().modification());
  }

  bool IsSpirvVertexShader() const {
    return is_vertex_shader() &&
           !Shader::IsHostVertexShaderTypeDomain(
               GetSpirvShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsSpirvTessEvalShader() const {
    return is_vertex_shader() &&
           Shader::IsHostVertexShaderTypeDomain(
               GetSpirvShaderModification().vertex.host_vertex_shader_type);
  }
  bool IsSpirvComputeShader() const {
    return is_vertex_shader() &&
           GetSpirvShaderModification().vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kMemExportCompute;
  }
  bool IsSpirvRectListAsTriangleStrip() const {
    return IsSpirvVertexShader() &&
           GetSpirvShaderModification().vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  }

  // The host render target path's fields of the pixel modification. The FSI
  // path keeps the EDRAM ROP's specialization in those same bits, so reading
  // them there is a bug - assert rather than leave it to review.
  Modification GetHostRtShaderModification() const {
    assert_false(edram_fragment_shader_interlock_);
    return GetSpirvShaderModification();
  }

  // The modification bit is shared with fsi_no_blending, so it only means
  // zpd_total on the host render target path.
  bool IsZpdTotal() const {
    return !edram_fragment_shader_interlock_ &&
           GetSpirvShaderModification().pixel.zpd_total();
  }

  bool IsExecutionModeEarlyFragmentTests() const {
    return !edram_fragment_shader_interlock_ && is_pixel_shader() &&
           GetHostRtShaderModification().pixel.depth_stencil_mode ==
               Modification::DepthStencilMode::kEarlyHint &&
           !IsZpdTotal() && current_shader().implicit_early_z_write_allowed();
  }

  // Whether the current non-FSI pixel shader should convert the depth to 20e4.
  bool DSV_IsWritingFloat24Depth() const {
    if (edram_fragment_shader_interlock_) {
      return false;
    }
    Modification::DepthStencilMode depth_stencil_mode =
        GetHostRtShaderModification().pixel.depth_stencil_mode;
    return depth_stencil_mode ==
               Modification::DepthStencilMode::kFloat24Truncating ||
           depth_stencil_mode == Modification::DepthStencilMode::
                                     kFloat24TruncatingPolygonOffset ||
           depth_stencil_mode ==
               Modification::DepthStencilMode::kFloat24Rounding ||
           depth_stencil_mode ==
               Modification::DepthStencilMode::kFloat24RoundingPolygonOffset;
  }
  // Whether the current non-FSI pixel shader applies polygon offset via shader
  // depth output instead of fixed function bias.
  bool DSV_IsApplyingPolygonOffset() const {
    if (edram_fragment_shader_interlock_) {
      return false;
    }
    Modification::DepthStencilMode depth_stencil_mode =
        GetHostRtShaderModification().pixel.depth_stencil_mode;
    return depth_stencil_mode ==
               Modification::DepthStencilMode::kPolygonOffset ||
           depth_stencil_mode == Modification::DepthStencilMode::
                                     kFloat24TruncatingPolygonOffset ||
           depth_stencil_mode ==
               Modification::DepthStencilMode::kFloat24RoundingPolygonOffset;
  }
  // Whether the shader runs at sample frequency - when converting depth to
  // float24 from the rasterizer's own depth (not guest oDepth), each sample
  // needs its own depth value for intersections to be antialiased.
  bool IsSampleRate() const {
    return is_pixel_shader() && DSV_IsWritingFloat24Depth() &&
           !current_shader().writes_depth();
  }

  uint32_t GetModificationInterpolatorMask() const {
    Modification modification = GetSpirvShaderModification();
    return is_vertex_shader() ? modification.vertex.interpolator_mask
                              : modification.pixel.interpolator_mask;
  }

  // Returns UINT32_MAX if PsParamGen doesn't need to be written.
  uint32_t GetPsParamGenInterpolator() const;

  // Must be called before emitting any SPIR-V operations that must be in a
  // block in translator callbacks to ensure that if the last instruction added
  // was something like OpBranch - in this case, an unreachable block is
  // created.
  void EnsureBuildPointAvailable();

  void StartVertexOrTessEvalShaderBeforeMain();
  void StartVertexOrTessEvalShaderInMain();
  void CompleteVertexOrTessEvalShaderInMain();
  void ResetUcodeInvocationStateInMain();
  void ResetVertexShaderInvocationStateInMain();
  void WriteVertexIndexToRegister0(spv::Id vertex_index);

  void StartFragmentShaderBeforeMain();
  void StartFragmentShaderInMain();
  void CompleteFragmentShaderInMain();

  // Writes gl_FragDepth for FBO shaders that need explicit depth: guest oDepth,
  // float24 conversion, or the host RT decal bias path.
  void CompleteFragmentShader_DSV_DepthTo24Bit();

  // Updates the current flow control condition (to be called in the beginning
  // of exec and in jumps), closing the previous conditionals if needed.
  // However, if the condition is not different, the instruction-level predicate
  // conditional also won't be closed - this must be checked separately if
  // needed (for example, in jumps).
  void UpdateExecConditionals(ParsedExecInstruction::Type type,
                              uint32_t bool_constant_index, bool condition);
  // Opens or reopens the predicate check conditional for the instruction.
  // Should be called before processing a non-control-flow instruction.
  void UpdateInstructionPredication(bool predicated, bool condition);
  // Closes the instruction-level predicate conditional if it's open, useful if
  // a control flow instruction needs to do some code which needs to respect the
  // current exec conditional, but can't itself be predicated.
  void CloseInstructionPredication();
  // Closes conditionals opened by exec and instructions within them (but not by
  // labels) and updates the state accordingly.
  void CloseExecConditionals();

  spv::Id GetStorageAddressingIndex(
      InstructionStorageAddressingMode addressing_mode, uint32_t storage_index,
      bool is_float_constant = false);
  // Loads unswizzled operand without sign modifiers as float4.
  spv::Id LoadOperandStorage(const InstructionOperand& operand);
  spv::Id ApplyOperandModifiers(spv::Id operand_value,
                                const InstructionOperand& original_operand,
                                bool invert_negate = false,
                                bool force_absolute = false);
  // Returns the requested components, with the operand's swizzle applied, in a
  // condensed form, but without negation / absolute value modifiers. The
  // storage is float4, no matter what the component count of original_operand
  // is (the storage will be either r# or c#, but the instruction may be
  // scalar).
  spv::Id GetUnmodifiedOperandComponents(
      spv::Id operand_storage, const InstructionOperand& original_operand,
      uint32_t components);
  spv::Id GetOperandComponents(spv::Id operand_storage,
                               const InstructionOperand& original_operand,
                               uint32_t components, bool invert_negate = false,
                               bool force_absolute = false) {
    return ApplyOperandModifiers(
        GetUnmodifiedOperandComponents(operand_storage, original_operand,
                                       components),
        original_operand, invert_negate, force_absolute);
  }
  // If components are identical, the same Id will be written to both outputs.
  void GetOperandScalarXY(spv::Id operand_storage,
                          const InstructionOperand& original_operand,
                          spv::Id& a_out, spv::Id& b_out,
                          bool invert_negate = false,
                          bool force_absolute = false);
  // Gets the absolute value of the loaded operand if it's not absolute already.
  spv::Id GetAbsoluteOperand(spv::Id operand_storage,
                             const InstructionOperand& original_operand);
  // The type of the value must be a float vector consisting of
  // xe::bit_count(result.GetUsedResultComponents()) elements, or (to replicate
  // a scalar into all used components) float, or the value can be spv::NoResult
  // if there's no result to store (like constants only).
  void StoreResult(const InstructionResult& result, spv::Id value);

  // For Shader Model 3 multiplication (+-0 or denormal * anything = +0),
  // replaces the value with +0 if the minimum of the two operands is 0. This
  // must be called with absolute values of operands - use GetAbsoluteOperand!
  spv::Id ZeroIfAnyOperandIsZero(spv::Id value, spv::Id operand_0_abs,
                                 spv::Id operand_1_abs);
  enum class ScalarEstimateOp : uint32_t {
    kRcp,
    kRsq,
    kSqrt,
    kExp,
    kLog,
  };
  // The scalar op of a float, the same as xenos::Rcp, Rsq, Sqrt, Exp or Log for
  // rounding-sensitive vertex shader results, and the host's elsewhere where
  // the extra instructions cost too much.
  spv::Id ScalarEstimate(ScalarEstimateOp op, spv::Id value);
  // Whether alu_round_toward_zero applies to the ALU operation being
  // translated. Only in vertex shaders since the extra instructions per
  // operation are too slow per pixel, and unless the rounding mode does it,
  // only to rounding-sensitive results since they're too slow for all.
  bool RoundsTowardZero() const;
  // Whether the shader's rounding mode does it for the whole vertex shader,
  // with no extra instructions.
  bool RoundsTowardZeroInHardware() const;
  // Guest ALU sums and products of floats or float vectors, rounded toward
  // zero if RoundsTowardZero.
  spv::Id AluAdd(spv::Id a, spv::Id b);
  spv::Id AluSubtract(spv::Id a, spv::Id b);
  spv::Id AluMultiply(spv::Id a, spv::Id b);
  // Steps a sum or product of a and b rounded to nearest toward zero if its
  // exact error shows it rounded away, and saturates overflow.
  spv::Id RoundTowardZero(spv::Id rounded, spv::Id error, spv::Id a, spv::Id b);
  // Pack/unpack two floats as Xbox 360 extended-range float16, where exponent
  // 31 is a large finite value (up to +-131008), not Inf/NaN.
  spv::Id PackFloat16x2ExtendedRange(spv::Id float2_value);
  spv::Id UnpackFloat16x2ExtendedRange(spv::Id packed_uint);
  // Conditionally discard the current fragment. Changes the build point.
  void KillPixel(spv::Id condition,
                 uint8_t memexport_eM_potentially_written_before);
  // Return type is a xe::bit_count(result.GetUsedResultComponents())-component
  // float vector or a single float, depending on whether it's a reduction
  // instruction (check getTypeId of the result), or returns spv::NoResult if
  // nothing to store.
  spv::Id ProcessVectorAluOperation(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before, bool& predicate_written);
  // Returns a float value to write to the previous scalar register and to the
  // destination. If the return value is ps itself (in the retain_prev case),
  // returns spv::NoResult (handled as a special case, so if it's retain_prev,
  // but don't need to write to anywhere, no OpLoad(ps) will be done).
  spv::Id ProcessScalarAluOperation(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before, bool& predicate_written);

  // Perform endian swap of a uint scalar or vector.
  spv::Id EndianSwap32Uint(spv::Id value, spv::Id endian);
  // Perform endian swap of a uint4 vector.
  spv::Id EndianSwap128Uint4(spv::Id value, spv::Id endian);

  spv::Id LoadUint32FromSharedMemory(spv::Id address_dwords_int);
  // If `replace_mask` is provided, the bits specified in the mask will be
  // replaced with those from the value via OpAtomicAnd/Or.
  // Bits of `value` not in `replace_mask` will be ignored.
  void StoreUint32ToSharedMemory(spv::Id value, spv::Id address_dwords_int,
                                 spv::Id replace_mask = spv::NoResult);

  bool IsMemoryExportSupported() const {
    if (is_pixel_shader()) {
      return features_.fragment_stores_and_atomics;
    }
    return features_.vertex_pipeline_stores_and_atomics ||
           IsSpirvComputeShader();
  }

  bool IsMemoryExportUsed() const {
    return current_shader().memexport_eM_written() && IsMemoryExportSupported();
  }

  void ExportToMemory(uint8_t export_eM);

  size_t FindOrAddTextureBinding(uint32_t fetch_constant,
                                 xenos::FetchOpDimension dimension,
                                 bool is_signed);
  size_t FindOrAddSamplerBinding(uint32_t fetch_constant,
                                 xenos::TextureFilter mag_filter,
                                 xenos::TextureFilter min_filter,
                                 xenos::TextureFilter mip_filter,
                                 xenos::AnisoFilter aniso_filter);
  // `texture_parameters` need to be set up except for `sampler`, which will be
  // set internally, optionally doing linear interpolation between the an
  // existing value and the new one (the result location may be the same as for
  // the first lerp endpoint, but not across signedness).
  void SampleTexture(spv::Builder::TextureParameters& texture_parameters,
                     spv::ImageOperandsMask image_operands_mask,
                     spv::Id image_unsigned, spv::Id image_signed,
                     spv::Id sampler, spv::Id is_any_unsigned,
                     spv::Id is_any_signed, spv::Id& result_unsigned_out,
                     spv::Id& result_signed_out,
                     spv::Id lerp_factor = spv::NoResult,
                     spv::Id lerp_first_unsigned = spv::NoResult,
                     spv::Id lerp_first_signed = spv::NoResult);
  // `texture_parameters` need to be set up except for `sampler`, which will be
  // set internally.
  spv::Id QueryTextureLod(spv::Builder::TextureParameters& texture_parameters,
                          spv::Id image_unsigned, spv::Id image_signed,
                          spv::Id sampler, spv::Id is_all_signed);
  // The explicit LOD, or the bias of a computed one: the fetch constant's, the
  // register's and the instruction's.
  spv::Id TextureFetchLod(const ParsedTextureFetchInstruction& instr,
                          spv::Id fetch_constant_word_4_signed);
  // getBCF's lane 0 and lanes 1 to 3.
  void TextureBorderColorFrac(const ParsedTextureFetchInstruction& instr,
                              spv::Id& lane_0_out, spv::Id& lanes_1_3_out);

  spv::Id LoadMsaaSamplesFromFlags();
  // The guest sample count baked into the modification, so the MSAA dependent
  // code is emitted for that count alone rather than selected at runtime.
  xenos::MsaaSamples FSI_GetMsaaSamples() const {
    assert_true(edram_fragment_shader_interlock_);
    return GetSpirvShaderModification().pixel.fsi_msaa_samples();
  }
  // Set when nothing the shader writes blends, so the blending path can be
  // left out entirely.
  bool FSI_GetNoBlending() const {
    assert_true(edram_fragment_shader_interlock_);
    return GetSpirvShaderModification().pixel.fsi_no_blending();
  }
  // Only call for render targets the shader writes.
  xenos::ColorRenderTargetFormat FSI_GetRtFormat(uint32_t rt) const {
    assert_true(edram_fragment_shader_interlock_);
    return GetSpirvShaderModification().pixel.fsi_rt_format(rt);
  }
  // Guest samples per pixel - how many of main_fsi_sample_mask_'s per-sample
  // bits and of the EDRAM ROP code are meaningful.
  uint32_t FSI_GetSampleCount() const {
    return uint32_t(1) << uint32_t(FSI_GetMsaaSamples());
  }
  // Whether it's possible and worth skipping running the translated shader for
  // 2x2 quads.
  bool FSI_IsDepthStencilEarly() const {
    assert_true(edram_fragment_shader_interlock_);
    return !is_depth_only_fragment_shader_ &&
           !current_shader().writes_depth() &&
           !current_shader().memexport_eM_written();
  }
  void FSI_LoadSampleMask();
  void FSI_LoadEdramOffsets();
  // The address must be a signed int. Whether the render target is 64bpp, if
  // present at all, must be a bool (if it's NoResult, 32bpp will be assumed).
  spv::Id FSI_AddSampleOffset(spv::Id sample_0_address, uint32_t sample_index,
                              spv::Id is_64bpp = spv::NoResult);
  // Updates main_fsi_sample_mask_. Unless the shader writes oDepth, tests the
  // depth and derivatives taken at the start of the shader.
  void FSI_DepthStencilTest(bool sample_mask_potentially_narrowed_previouly);
  // Adds the selected depth/stencil outcomes to the active ZPD counter slot.
  void FSI_AddMSAASamplesToZPD(bool count_passed, bool count_failed);
  // Adds the coverage before the depth/stencil test to the Total lane of the
  // active ZPD counter slot.
  void FBO_AddMSAASamplesToZPDTotal();

  // Alpha to coverage helper - tests one sample.
  // coverage_out is modified to include this sample if it passes.
  void FSI_AlphaToMaskSample(bool initialize, uint32_t sample_index,
                             float threshold_base, spv::Id threshold_offset,
                             float threshold_offset_scale, spv::Id alpha,
                             spv::Id& coverage_out);

  // Alpha to coverage main function.
  void FSI_AlphaToMask();
  // Returns the first and the second 32 bits as two uints.
  // Both are specialized for the render target's format through the
  // modification - only that format's path is emitted.
  std::array<spv::Id, 2> FSI_ClampAndPackColor(
      spv::Id color_float4, xenos::ColorRenderTargetFormat format);
  std::array<spv::Id, 4> FSI_UnpackColor(std::array<spv::Id, 2> color_packed,
                                         xenos::ColorRenderTargetFormat format);
  // The bounds must have the same number of components as the color or alpha.
  spv::Id FSI_FlushNaNClampAndInBlending(spv::Id color_or_alpha,
                                         spv::Id is_fixed_point,
                                         spv::Id min_value, spv::Id max_value);
  spv::Id FSI_ApplyColorBlendFactor(spv::Id value, spv::Id is_fixed_point,
                                    spv::Id clamp_min_value,
                                    spv::Id clamp_max_value, spv::Id factor,
                                    spv::Id source_color, spv::Id source_alpha,
                                    spv::Id dest_color, spv::Id dest_alpha,
                                    spv::Id constant_color,
                                    spv::Id constant_alpha);
  spv::Id FSI_ApplyAlphaBlendFactor(spv::Id value, spv::Id is_fixed_point,
                                    spv::Id clamp_min_value,
                                    spv::Id clamp_max_value, spv::Id factor,
                                    spv::Id source_alpha, spv::Id dest_alpha,
                                    spv::Id constant_alpha);
  // If source_color_clamped, dest_color, constant_color_clamped are
  // spv::NoResult, will blend the alpha. Otherwise, will blend the color.
  // The result will be unclamped (color packing is supposed to clamp it).
  spv::Id FSI_BlendColorOrAlphaWithUnclampedResult(
      spv::Id is_fixed_point, spv::Id clamp_min_value, spv::Id clamp_max_value,
      spv::Id source_color_clamped, spv::Id source_alpha_clamped,
      spv::Id dest_color, spv::Id dest_alpha, spv::Id constant_color_clamped,
      spv::Id constant_alpha_clamped, spv::Id equation, spv::Id source_factor,
      spv::Id dest_factor);

  // 8_8_8_8, 8_8_8_8_GAMMA, 2_10_10_10 and 2_10_10_10_AS_10_10_10_10 blend in
  // the render backend's 10-bit fixed point (FSI_BlendFixed10), the other
  // formats in floats.
  static bool FSI_BlendsInFixed10(xenos::ColorRenderTargetFormat format);
  // Of those, the formats with 8-bit fields rather than 10:10:10:2.
  static bool FSI_Fixed10Is8bpc(xenos::ColorRenderTargetFormat format);
  // A float scalar or vector to n-bit unorm (uint) the way the render backend
  // converts it: clamped, truncated to 23 fraction bits, then rounded half up.
  // NaN converts by its sign like infinity.
  spv::Id FSI_FloatToUnorm(spv::Id value, uint32_t bits);
  // A uint or uint3 8-bit unorm widened to 10 bits by bit replication.
  spv::Id FSI_Widen8To10(spv::Id value);
  // uint3 10-bit linear to 8-bit gamma, the render target's curve.
  spv::Id FSI_LinearToGamma8(spv::Id linear);
  // uint3 8-bit gamma to 10-bit linear, the exact inverse of the curve.
  spv::Id FSI_Gamma8ToLinear10(spv::Id gamma);
  // The blending operands (uint4, 10 bits) of the exported color, quantized
  // to what the format stores first.
  spv::Id FSI_SourceToFixed10(spv::Id color,
                              xenos::ColorRenderTargetFormat format);
  // The blending operands (uint4, 10 bits) of a stored pixel.
  spv::Id FSI_UnpackToFixed10(spv::Id packed,
                              xenos::ColorRenderTargetFormat format);
  // A blend factor of 10-bit operands before widening, for the color (uint3)
  // or the alpha (uint).
  spv::Id FSI_BlendFactorFixed10(spv::Id factor, bool is_alpha, spv::Id source,
                                 spv::Id dest, spv::Id constant);
  // Packs uint3 color and uint alpha fields, at the widths the format stores,
  // into one of the FSI_BlendsInFixed10 formats.
  spv::Id FSI_PackUnitFields(spv::Id rgb, spv::Id alpha,
                             xenos::ColorRenderTargetFormat format);
  // Converts and packs a color into one of the FSI_BlendsInFixed10 formats.
  spv::Id FSI_PackUnitColor(spv::Id color,
                            xenos::ColorRenderTargetFormat format);
  // Blends 10-bit operands and packs the result into the format.
  spv::Id FSI_BlendFixed10(xenos::ColorRenderTargetFormat format,
                           spv::Id source, spv::Id dest, spv::Id constant,
                           spv::Id color_equation, spv::Id color_source_factor,
                           spv::Id color_dest_factor, spv::Id alpha_equation,
                           spv::Id alpha_source_factor,
                           spv::Id alpha_dest_factor);

  Features features_;
  bool native_2x_msaa_with_attachments_;
  bool native_2x_msaa_no_attachments_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  // Scale of the draw being translated. All position-dependent paths use
  // these. Only fetch offset scaling uses draw_resolution_scale_x_/y_ directly.
  // The scale threshold is host render target state - the FSI path has no
  // host targets and stores RT formats in that bit, so it always scales.
  bool IsCurrentDrawScaleNative() const {
    return !edram_fragment_shader_interlock_ && is_pixel_shader() &&
           GetHostRtShaderModification().pixel.resolution_scale_native;
  }
  uint32_t GetCurrentDrawResolutionScaleX() const {
    return IsCurrentDrawScaleNative() ? 1 : draw_resolution_scale_x_;
  }
  uint32_t GetCurrentDrawResolutionScaleY() const {
    return IsCurrentDrawScaleNative() ? 1 : draw_resolution_scale_y_;
  }
  // Whether point sampled fetches at an interpolated coordinate pick the guest
  // texel sampled at the guest pixel center, rather than at the host pixel.
  bool IsGuestPixelCenterFetchNeeded() const;

  // For safety with different drivers (even though fragment shader interlock in
  // SPIR-V only has one control flow requirement - that both begin and end must
  // be dynamically executed exactly once in this order), adhering to the more
  // strict control flow limitations of OpenGL (GLSL) fragment shader interlock,
  // that begin and end are called only on the outermost level of the control
  // flow of the main function, and that there are no returns before either
  // (there's a single return from the shader).
  bool edram_fragment_shader_interlock_;
  // A device and cvar constant, not draw state.
  bool precise_interpolation_;
  // occlusion_query_full_counters - FSI shaders also count ZFail and
  // StencilFail. Part of the pipeline storage key.
  bool zpd_full_counters_;

  // Is currently writing the empty depth-only pixel shader, such as for depth
  // and stencil testing with fragment shader interlock.
  bool is_depth_only_fragment_shader_ = false;
  bool is_viz_survey_fragment_shader_ = false;

  std::unique_ptr<SpirvBuilder> builder_;

  std::vector<spv::Id> id_vector_temp_;
  // For helper functions like operand loading, so they don't conflict with
  // id_vector_temp_ usage in bigger callbacks.
  std::vector<spv::Id> id_vector_temp_util_;
  std::vector<unsigned int> uint_vector_temp_;
  std::vector<unsigned int> uint_vector_temp_util_;

  spv::Id ext_inst_glsl_std_450_;

  spv::Id type_void_;

  union {
    struct {
      spv::Id type_bool_;
      spv::Id type_bool2_;
      spv::Id type_bool3_;
      spv::Id type_bool4_;
    };
    // Index = component count - 1.
    spv::Id type_bool_vectors_[4];
  };
  union {
    struct {
      spv::Id type_int_;
      spv::Id type_int2_;
      spv::Id type_int3_;
      spv::Id type_int4_;
    };
    spv::Id type_int_vectors_[4];
  };
  union {
    struct {
      spv::Id type_uint_;
      spv::Id type_uint2_;
      spv::Id type_uint3_;
      spv::Id type_uint4_;
    };
    spv::Id type_uint_vectors_[4];
  };
  union {
    struct {
      spv::Id type_float_;
      spv::Id type_float2_;
      spv::Id type_float3_;
      spv::Id type_float4_;
    };
    spv::Id type_float_vectors_[4];
  };

  spv::Id const_int_0_;
  spv::Id const_int4_0_;
  spv::Id const_uint_0_;
  spv::Id const_uint4_0_;
  union {
    struct {
      spv::Id const_float_0_;
      spv::Id const_float2_0_;
      spv::Id const_float3_0_;
      spv::Id const_float4_0_;
    };
    spv::Id const_float_vectors_0_[4];
  };
  union {
    struct {
      spv::Id const_float_1_;
      spv::Id const_float2_1_;
      spv::Id const_float3_1_;
      spv::Id const_float4_1_;
    };
    spv::Id const_float_vectors_1_[4];
  };
  // vec2(0.0, 1.0), to arbitrarily VectorShuffle non-constant and constant
  // components.
  spv::Id const_float2_0_1_;

  enum SystemConstantIndex : unsigned int {
    kSystemConstantFlags,
    kSystemConstantVertexIndexLoadAddress,
    kSystemConstantVertexIndexCount,
    kSystemConstantVertexIndexEndian,
    kSystemConstantVertexBaseIndex,
    kSystemConstantNdcScale,
    kSystemConstantPointVertexDiameterMin,
    kSystemConstantNdcOffset,
    kSystemConstantPointVertexDiameterMax,
    kSystemConstantPointConstantDiameter,
    kSystemConstantPointScreenDiameterToNdcRadius,
    kSystemConstantTextureSwizzledSigns,
    kSystemConstantTextureSwizzles,
    kSystemConstantTexturesResolved,
    kSystemConstantAlphaTestReference,
    kSystemConstantAlphaToMask,
    kSystemConstantZpdFsiCounterIndex,
    kSystemConstantEdram32bppTilePitchDwordsScaled,
    kSystemConstantEdramDepthBaseDwordsScaled,
    kSystemConstantColorExpBias,
    kSystemConstantEdramPolyOffsetFrontScale,
    kSystemConstantEdramPolyOffsetBackScale,
    kSystemConstantEdramPolyOffsetFrontOffset,
    kSystemConstantEdramPolyOffsetBackOffset,
    kSystemConstantEdramStencilFront,
    kSystemConstantEdramStencilBack,
    kSystemConstantEdramRTBaseDwordsScaled,
    kSystemConstantEdramRTBlendFactorsOps,
    // Accessed as float4[2], not float2[4], due to std140 array stride
    // alignment.
    kSystemConstantEdramRTKeepMask,
    kSystemConstantEdramRTClamp,
    kSystemConstantEdramBlendConstant,
    kSystemConstantUserClipPlanes,
    kSystemConstantTessellationFactorRange,
    kSystemConstantTessellationVertexIndexEndian,
    kSystemConstantTessellationVertexIndexOffset,
    kSystemConstantTessellationVertexIndexMinMax,
    kSystemConstantInterpreterUcodeBaseDwords,
    kSystemConstantInterpreterCfInstrCount,
    kSystemConstantTextureIntegerScaleBits,
    kSystemConstantParamGenWindowOffset,
  };
  spv::Id uniform_system_constants_;
  spv::Id uniform_float_constants_;
  spv::Id uniform_bool_loop_constants_;
  spv::Id uniform_fetch_constants_;

  spv::Id buffers_shared_memory_;
  spv::Id buffer_edram_;
  spv::Id buffer_zpd_counter_;

  // Not using combined images and samplers because
  // maxPerStageDescriptorSamplers is often lower than
  // maxPerStageDescriptorSampledImages, and for every fetch constant, there
  // are, for regular fetches, two bindings (unsigned and signed).
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;

  // VS as VS only - int.
  spv::Id input_vertex_index_;
  // VS as TES only - per-control-point float array carrying the patch/control
  // point index computed by the host vertex and hull shaders.
  spv::Id input_control_point_index_;
  // VS as TES only - float3 (barycentric coordinates).
  spv::Id input_tess_coord_;
  // PS, only when needed - float2.
  spv::Id input_point_coordinates_;
  // PS, only when needed - float4.
  spv::Id input_fragment_coordinates_;
  // PS, only when needed - bool.
  spv::Id input_front_facing_;
  // PS, only when needed - int[1].
  spv::Id input_sample_mask_;

  // PS, barycentric coordinate inputs (when fragment_shader_barycentric is
  // enabled) - float3.
  spv::Id input_barycentric_coord_;
  spv::Id input_barycentric_coord_no_persp_;

  // PS, per-vertex interpolator arrays for barycentric interpolation (when
  // fragment_shader_barycentric is enabled). Stores the array variable
  // (float4[3]) for each interpolator.
  std::array<spv::Id, xenos::kMaxInterpolators> input_interpolators_per_vertex_;

  // VS output or PS input, only the ones that are needed (spv::NoResult for the
  // unneeded interpolators), indexed by the guest interpolator index - float4.
  // The Qualcomm Adreno driver has strict requirements for stage linkage - as
  // Xenia uses separate variables, not an array (so the interpolation
  // qualifiers can be applied to each element separately), the interpolators
  // must also be separate variables in the other stage, including the geometry
  // shader (not just an array assuming that consecutive locations will be
  // linked as consecutive array elements, on Qualcomm, they won't be linked at
  // all).
  std::array<spv::Id, xenos::kMaxInterpolators> input_output_interpolators_;

  // VS, only for HostVertexShaderType::kPointListAsTriangleStrip when needed
  // for the PS - float2.
  spv::Id output_point_coordinates_;
  // VS, only when needed - float.
  spv::Id output_point_size_;

  enum OutputPerVertexMember : unsigned int {
    kOutputPerVertexMemberPosition,
    kOutputPerVertexMemberCount,
  };
  spv::Id output_per_vertex_;
  unsigned int output_per_vertex_clip_distance_member_index_ = 0;
  unsigned int output_per_vertex_cull_distance_member_index_ = 0;

  // VS, only for HostVertexShaderType::kRectangleListAsTriangleStrip.
  bool main_vertex_rect_list_as_triangle_strip_ = false;
  // uint (lower 2 bits of the expanded host vertex index).
  spv::Id var_main_rect_list_strip_vertex_;
  // int3 (guest indices for the 3 rectangle vertices after base addition).
  spv::Id var_main_rect_list_guest_vertex_indices_;
  // float4[3] (guest clip-space positions for the 3 rectangle vertices).
  spv::Id var_main_rect_list_guest_positions_;
  // For used interpolators only: float4[3].
  std::array<spv::Id, xenos::kMaxInterpolators>
      var_main_rect_list_guest_interpolators_;

  // Function-scoped variables for fragment color data.
  // Used by both FSI and FBO paths so that color values can be read back
  // (e.g., for alpha test). For FBO, these are copied to output_fragment_data_
  // at the end of the shader.
  std::array<spv::Id, xenos::kMaxColorRenderTargets>
      output_or_var_fragment_data_;

  // FBO only: Actual framebuffer color attachment outputs (Output storage).
  // These are write-only and populated at the end of the shader from
  // output_or_var_fragment_data_.
  std::array<spv::Id, xenos::kMaxColorRenderTargets> output_fragment_data_;

  // Function-scoped staging variable for guest oDepth writes. Used by both
  // FSI (which writes the value to the EDRAM buffer inside the interlock)
  // and FBO (copied to output_fragment_depth_ at the end of the shader,
  // remapping guest 0...1 to host 0...0.5 when the depth format is float24).
  spv::Id output_or_var_fragment_depth_;

  // FBO only: actual gl_FragDepth Output.
  // Written at the end of the pixel shader from output_or_var_fragment_depth_.
  spv::Id output_fragment_depth_;
  // Raster depth and derivatives taken at the start of the shader for the host
  // RT decal path and the FSI depth / stencil test.
  spv::Id main_fbo_depth_unbiased_;
  std::array<spv::Id, 2> main_fbo_depth_derivatives_;

  // Fragment shader sample mask output (gl_SampleMask).
  // Only used for alpha-to-coverage in non-FSI mode.
  // For FSI mode, sample mask is handled via main_fsi_sample_mask_.
  spv::Id output_fragment_sample_mask_;

  std::vector<spv::Id> main_interface_;
  spv::Function* function_main_;
  spv::Id main_system_constant_flags_;
  // bool.
  spv::Id var_main_predicate_;
  // uint4.
  spv::Id var_main_loop_count_;
  // int4.
  spv::Id var_main_loop_address_;
  // int.
  spv::Id var_main_address_register_;
  // float.
  spv::Id var_main_previous_scalar_;
  // `base + index * stride` in dwords from the last vfetch_full as it may be
  // needed by vfetch_mini - int.
  spv::Id var_main_vfetch_address_;
  // Exclusive end (base + size) in dwords of the last vfetch_full's buffer, for
  // clamping out-of-bounds words to 0 in both it and its vfetch_mini - int.
  spv::Id var_main_vfetch_bound_;
  // float.
  spv::Id var_main_tfetch_lod_;
  // float3.
  spv::Id var_main_tfetch_gradients_h_;
  spv::Id var_main_tfetch_gradients_v_;
  // uint4[32] coefficients for ScalarEstimate, created on first use.
  std::array<spv::Id, size_t(ScalarEstimateOp::kLog) + 1>
      var_main_estimate_coefficients_;
  // float4[register_count()].
  spv::Id var_main_registers_;
  // Components of registers 0-15, 4 bits per register, that still hold the
  // interpolant they were initialized with.
  uint64_t main_interpolators_unmodified_;
  // float4 each, the interpolant at the guest pixel center minus at the host
  // pixel, for IsGuestPixelCenterFetchNeeded.
  std::array<spv::Id, xenos::kMaxInterpolators>
      var_main_interpolator_guest_center_deltas_;

  // Guest instruction bisect, snapshotting a register to color 0.
  bool BisectTargetsCurrentShader() const;
  bool BisectSkipsInstruction();
  void BisectSnapshotAfterInstruction();
  void BisectStoreSnapshot();
  void BisectOverrideColorOutput();
  uint32_t bisect_instruction_index_;
  uint32_t bisect_current_instruction_;
  bool bisect_snapshot_emitted_;
  // float4 holding the watched register at the chosen instruction.
  spv::Id var_main_bisect_snapshot_;
  // Memory export variables are created only when needed.
  // float4.
  spv::Id var_main_memexport_address_;
  // Each is float4.
  spv::Id var_main_memexport_data_[ucode::kMaxMemExportElementCount];
  // Bit field of which eM# elements have been written so far by the invocation
  // since the last memory write - uint.
  spv::Id var_main_memexport_data_written_;
  // If memory export is disabled in certain invocations or (if emulating some
  // primitive types without a geometry shader) at specific guest vertex loop
  // iterations because the translated shader is executed multiple times for the
  // same guest vertex or pixel, this contains whether memory export is allowed
  // in the current execution of the translated code.
  // bool.
  spv::Id main_memexport_allowed_;
  // VS only - float3 (special exports).
  spv::Id var_main_point_size_edge_flag_kill_vertex_;
  // PS, only when needed - bool.
  spv::Id var_main_kill_pixel_;
  // PS, when writing to color render targets - uint.
  // Whether color buffers have been written to, if not written on the taken
  // execution path, don't export according to Direct3D 9 register documentation
  // (some games rely on this behavior).
  // Used by both FSI and FBO paths for proper alpha test / alpha-to-coverage
  // behavior.
  spv::Id var_main_fsi_color_written_;
  // Hybrid ZPD query coverage before the depth/stencil test, from
  // SampleMaskIn, narrowed by alpha to coverage.
  spv::Id var_main_zpd_coverage_;
  // Loaded by FSI_LoadSampleMask.
  // Can be modified on the outermost control flow level in the main function.
  // 0:3 - Per-sample coverage at the current stage of the shader's execution.
  //       Affected by things like gl_SampleMaskIn, early or late depth /
  //       stencil (always resets bits for failing, no matter if need to defer
  //       writing), alpha to coverage.
  // 4:7 - Depth write deferred mask - when early depth / stencil resulted in a
  //       different value for the sample (like different stencil if the test
  //       failed), but can't write it before running the shader because it's
  //       not known if the sample will be discarded by the shader, alphatest or
  //       AtoC.
  // Early depth / stencil rejection of the pixel is possible when both 0:3 and
  // 4:7 are zero.
  spv::Id main_fsi_sample_mask_;
  // Per-sample depth/stencil test failures from FSI_DepthStencilTest, zero
  // unless occlusion_query_full_counters is enabled. A sample is in at most one
  // of these, stencil failure taking precedence.
  spv::Id main_fsi_z_fail_sample_mask_;
  spv::Id main_fsi_stencil_fail_sample_mask_;
  // Loaded by FSI_LoadEdramOffsets.
  // Including the depth render target base.
  spv::Id main_fsi_address_depth_;
  // Not including the render target base.
  spv::Id main_fsi_offset_32bpp_;
  spv::Id main_fsi_offset_64bpp_;
  // Loaded by FSI_DepthStencilTest for early depth / stencil, the depth /
  // stencil values to write at the end of the shader if the specified in
  // main_fsi_sample_mask_ and if the samples were not discarded later after the
  // early test.
  std::array<spv::Id, 4> main_fsi_late_write_depth_stencil_;
  spv::Block* main_fsi_early_depth_stencil_execute_quad_merge_;
  spv::Block* main_loop_header_;
  spv::Block* main_loop_continue_;
  spv::Block* main_loop_merge_;
  spv::Id main_loop_pc_next_;
  // VS only, for HostVertexShaderType::kRectangleListAsTriangleStrip.
  spv::Block* main_rect_list_loop_header_;
  spv::Block* main_rect_list_loop_continue_;
  spv::Block* main_rect_list_loop_merge_;
  // int (0..2), OpPhi in main_rect_list_loop_header_.
  spv::Id main_rect_list_loop_vertex_index_;
  // int, produced in main_rect_list_loop_continue_ and consumed by the OpPhi in
  // main_rect_list_loop_header_.
  spv::Id main_rect_list_loop_vertex_index_next_;
  spv::Block* main_switch_header_;
  std::unique_ptr<spv::Instruction> main_switch_op_;
  spv::Block* main_switch_merge_;
  std::vector<spv::Id> main_switch_next_pc_phi_operands_;

  // If the exec bool constant / predicate conditional is open, block after it
  // (not added to the function yet).
  spv::Block* cf_exec_conditional_merge_;
  // If the instruction-level predicate conditional is open, block after it (not
  // added to the function yet).
  spv::Block* cf_instruction_predicate_merge_;
  // When cf_exec_conditional_merge_ is not null:
  // If the current exec conditional is based on a bool constant: the number of
  // the bool constant.
  // If it's based on the predicate value: kCfExecBoolConstantPredicate.
  uint32_t cf_exec_bool_constant_or_predicate_;
  static constexpr uint32_t kCfExecBoolConstantPredicate = UINT32_MAX;
  // When cf_exec_conditional_merge_ is not null, the expected bool constant or
  // predicate value for the current exec conditional.
  bool cf_exec_condition_;
  // When cf_instruction_predicate_merge_ is not null, the expected predicate
  // value for the current or the last instruction.
  bool cf_instruction_predicate_condition_;
  // Whether there was a `setp` in the current exec before the current
  // instruction, thus instruction-level predicate value can be different than
  // the exec-level predicate value, and can't merge two execs with the same
  // predicate condition anymore.
  bool cf_exec_predicate_written_;

  // Whether the result of the ALU operation being translated is
  // rounding-sensitive (see Shader::IsAluVectorResultRoundingSensitive).
  bool alu_op_result_rounding_sensitive_ = false;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SPIRV_SHADER_TRANSLATOR_H_
