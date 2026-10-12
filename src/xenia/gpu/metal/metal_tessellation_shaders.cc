/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_tessellation_shaders.h"

// Same array names as the other bytecode trees, different type (uint32_t
// SPIR-V words), so they live in their own namespace.
namespace shaders_spirv {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_indexed_vs.h"
}  // namespace shaders_spirv

namespace xe {
namespace gpu {
namespace metal {

bool GetMetalTessellationHostShaders(
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type,
    MetalTessellationHostShaders& shaders_out) {
  // Adaptive reads edge factors from the index buffer, the other modes pass
  // indices.
  auto set_vs = [&](const auto& arr) {
    shaders_out.vertex_spirv = arr;
    shaders_out.vertex_word_count = sizeof(arr) / sizeof(uint32_t);
  };
  if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
    set_vs(shaders_spirv::tessellation_adaptive_vs);
  } else {
    set_vs(shaders_spirv::tessellation_indexed_vs);
  }

  auto set_hs = [&](const auto& arr) {
    shaders_out.hull_spirv = arr;
    shaders_out.hull_word_count = sizeof(arr) / sizeof(uint32_t);
  };
  using HVS = Shader::HostVertexShaderType;
  switch (tessellation_mode) {
    case xenos::TessellationMode::kDiscrete:
      switch (host_vertex_shader_type) {
        case HVS::kTriangleDomainCPIndexed:
          set_hs(shaders_spirv::discrete_triangle_3cp_hs);
          break;
        case HVS::kTriangleDomainPatchIndexed:
          set_hs(shaders_spirv::discrete_triangle_1cp_hs);
          break;
        case HVS::kQuadDomainCPIndexed:
          set_hs(shaders_spirv::discrete_quad_4cp_hs);
          break;
        case HVS::kQuadDomainPatchIndexed:
          set_hs(shaders_spirv::discrete_quad_1cp_hs);
          break;
        default:
          return false;
      }
      break;
    case xenos::TessellationMode::kContinuous:
      switch (host_vertex_shader_type) {
        case HVS::kTriangleDomainCPIndexed:
          set_hs(shaders_spirv::continuous_triangle_3cp_hs);
          break;
        case HVS::kTriangleDomainPatchIndexed:
          set_hs(shaders_spirv::continuous_triangle_1cp_hs);
          break;
        case HVS::kQuadDomainCPIndexed:
          set_hs(shaders_spirv::continuous_quad_4cp_hs);
          break;
        case HVS::kQuadDomainPatchIndexed:
          set_hs(shaders_spirv::continuous_quad_1cp_hs);
          break;
        default:
          return false;
      }
      break;
    case xenos::TessellationMode::kAdaptive:
      switch (host_vertex_shader_type) {
        case HVS::kTriangleDomainPatchIndexed:
          set_hs(shaders_spirv::adaptive_triangle_hs);
          break;
        case HVS::kQuadDomainPatchIndexed:
          set_hs(shaders_spirv::adaptive_quad_hs);
          break;
        default:
          return false;
      }
      break;
    default:
      return false;
  }

  // Adaptive patches take one edge factor per edge, the other patch-indexed
  // modes one patch index, and control-point-indexed modes one index per
  // corner.
  bool triangle = host_vertex_shader_type == HVS::kTriangleDomainCPIndexed ||
                  host_vertex_shader_type == HVS::kTriangleDomainPatchIndexed;
  bool per_corner = tessellation_mode == xenos::TessellationMode::kAdaptive ||
                    host_vertex_shader_type == HVS::kTriangleDomainCPIndexed ||
                    host_vertex_shader_type == HVS::kQuadDomainCPIndexed;
  shaders_out.input_control_point_count = per_corner ? (triangle ? 3 : 4) : 1;
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
