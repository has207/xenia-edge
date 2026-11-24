/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/guest_spirv_shader_cache.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/spirv_shader_translator.h"

namespace xe {
namespace gpu {

GuestSpirvShaderCache::GuestSpirvShaderCache(
    Host& host, const RegisterFile& register_file,
    const RenderTargetCache& render_target_cache)
    : host_(host),
      register_file_(register_file),
      render_target_cache_(render_target_cache) {}

GuestSpirvShaderCache::~GuestSpirvShaderCache() { Shutdown(); }

bool GuestSpirvShaderCache::Initialize() {
  translator_ = host_.CreateTranslator();
  return translator_ != nullptr;
}

void GuestSpirvShaderCache::Shutdown() { translator_.reset(); }

std::unique_ptr<SpirvShaderTranslator>
GuestSpirvShaderCache::CreateWorkerTranslator() const {
  return host_.CreateTranslator();
}

uint64_t GuestSpirvShaderCache::GetVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  // Tessellation mode selects the domain shader spacing.
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode =
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  }

  // User clip planes.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);

  // Vertex kill via the kill flag (oPts.z). The "and" operator (kill only when
  // all vertices of the primitive request it) is emulated with a cull distance.
  // The "or" operator sets the position to NaN in the translator.
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  if (draw_util::IsZClippedWithClipDistances(regs,
                                             host_.raw_clipping_supported())) {
    modification.vertex.z_clip_distances = 1;
    modification.vertex.z_clip_near_at_minus_w =
        uint32_t(!pa_cl_clip_cntl.dx_clip_space_def);
  }
  modification.vertex.guard_band_clip_distances =
      uint32_t(draw_util::IsGuardBandClippedWithClipDistances(
          regs, host_.raw_clipping_supported()));
  modification.vertex.discard_band_cull_distances =
      uint32_t(draw_util::IsDiscardBandCulledWithCullDistances(
          regs, host_.cull_distances_supported()));

  // For kPointListAsTriangleStrip the bit means output coordinates (only the
  // Vulkan fallback uses that host type). Otherwise it means output point size.
  if (host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                     xenos::PrimitiveType::kPointList);
  }

  return modification.value;
}

uint64_t GuestSpirvShaderCache::GetPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  // Depth/stencil mode, blend pre-multiply and the color target mask are host
  // render target path state, left at their defaults on the FSI path.
  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Whether this draw is native res due to a scale threshold.
    modification.pixel.resolution_scale_native =
        uint32_t(render_target_cache_.IsDrawScaleNative());

    using DepthStencilMode =
        SpirvShaderTranslator::Modification::DepthStencilMode;
    if (host_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      modification.pixel.depth_stencil_mode =
          apply_polygon_offset_in_shader
              ? (host_.depth_float24_round()
                     ? DepthStencilMode::kFloat24RoundingPolygonOffset
                     : DepthStencilMode::kFloat24TruncatingPolygonOffset)
              : (host_.depth_float24_round()
                     ? DepthStencilMode::kFloat24Rounding
                     : DepthStencilMode::kFloat24Truncating);
    } else {
      if (apply_polygon_offset_in_shader) {
        modification.pixel.depth_stencil_mode =
            DepthStencilMode::kPolygonOffset;
      } else {
        // kEarlyHint triggers a GPU fault on nvidia (Alan Wake gameplay), so
        // use the safe alternative.
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }

    // MIN/MAX blend ignores fixed-function factors on the host, but the Xbox
    // 360 applies them. When the destination factor is ONE we pre-multiply the
    // shader output by the source factor. Only RT0 is supported for now.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;

    if (shader.writes_color_target(0)) {
      auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[0]);
      if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.color_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
      if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_a_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
    }

    modification.pixel.color_targets_used =
        (((normalized_color_mask >> 0) & 0xF) ? 1 : 0) |
        (((normalized_color_mask >> 4) & 0xF) ? 2 : 0) |
        (((normalized_color_mask >> 8) & 0xF) ? 4 : 0) |
        (((normalized_color_mask >> 12) & 0xF) ? 8 : 0);
  } else {
    // The FSI shader runs the EDRAM ROP itself, so specialize it for the
    // sample count. No new pipeline permutations, they already vary by it.
    modification.pixel.set_fsi_msaa_samples(
        regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
    // Per render target, the format that drives the pack and unpack trees
    // and whether it blends. Unlike the sample count these add pipeline
    // permutations - the FSI render pass has no attachments to vary by - so
    // skip render targets the draw masks off, which the shader skips anyway.
    bool any_blending = false;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!shader.writes_color_target(i) ||
          !((normalized_color_mask >> (i * 4)) & 0xF)) {
        continue;
      }
      modification.pixel.set_fsi_rt_format(
          i, regs.Get<reg::RB_COLOR_INFO>(
                     reg::RB_COLOR_INFO::rt_register_indices[i])
                 .color_format);
      // The shader treats 1 * source + 0 * destination as no blending.
      if ((regs.Get<reg::RB_BLENDCONTROL>(
                   reg::RB_BLENDCONTROL::rt_register_indices[i])
               .value &
           0x1FFF1FFF) != 0x00010001) {
        any_blending = true;
      }
    }
    // With no blending anywhere, the whole blending path can be left out.
    modification.pixel.set_fsi_no_blending(!any_blending);
  }

  return modification.value;
}

bool GuestSpirvShaderCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    uint64_t vertex_shader_modification, uint64_t pixel_shader_modification,
    GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  SpirvShaderTranslator::Modification vertex_mod(vertex_shader_modification);
  SpirvShaderTranslator::Modification pixel_mod(pixel_shader_modification);
  // The *AsTriangleStrip host vertex shader types are the fallback for when
  // geometry shaders are unsupported. There geometry_shader_type is kNone and
  // this is not called. output_point_parameters means coordinates, not size,
  // for those, so reject them.
  if (vertex_mod.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_mod.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    XELOGE(
        "GetGeometryShaderKey: AsTriangleStrip vertex shader types should not "
        "be used with geometry shaders");
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = xe::bit_count(vertex_mod.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_mod.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_mod.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_mod.vertex.vertex_kill_and;
  key.has_point_size = vertex_mod.vertex.output_point_parameters;
  key.has_point_coordinates = pixel_mod.pixel.param_gen_point;
  key.z_clip_distances = vertex_mod.vertex.z_clip_distances;
  key.guard_band_clip_distances = vertex_mod.vertex.guard_band_clip_distances;
  key.discard_band_cull_distances =
      vertex_mod.vertex.discard_band_cull_distances;
  key_out = key;
  return true;
}

}  // namespace gpu
}  // namespace xe
