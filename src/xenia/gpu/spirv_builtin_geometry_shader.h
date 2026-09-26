/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SPIRV_BUILTIN_GEOMETRY_SHADER_H_
#define XENIA_GPU_SPIRV_BUILTIN_GEOMETRY_SHADER_H_

#include <cstdint>
#include <vector>

namespace xe {
namespace gpu {

// Built-in geometry shader variants for guest primitive types that need host
// expansion (the Xbox 360 has no geometry shaders). The values match the
// per-backend PipelineGeometryShader enums so a backend key can be cast in.
enum class BuiltinGeometryShaderType : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
  // Line list or strip expanded to quads 1 guest pixel wide, for
  // resolution-scaled draws (host lines are always 1 host pixel wide).
  kLineList,
};

// Builds the SPIR-V for a built-in primitive-expansion geometry shader, shared
// by the Vulkan backend (used directly) and the D3D12 backend (fed through
// spirv_to_dxil). The shader reads the SpirvShaderTranslator vertex output
// signature and SystemConstants layout, so it pairs with that translator's
// vertex and pixel shaders on either backend. Returns the SPIR-V words.
std::vector<unsigned int> BuildGuestPrimitiveGeometryShaderSpirv(
    BuiltinGeometryShaderType type, uint32_t interpolator_count,
    uint32_t user_clip_plane_count, bool user_clip_plane_cull,
    bool has_vertex_kill_and, bool has_point_size, bool has_point_coordinates,
    unsigned int spirv_version, bool denorm_flush_to_zero_float32,
    bool signed_zero_inf_nan_preserve_float32, bool rounding_mode_rte_float32);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SPIRV_BUILTIN_GEOMETRY_SHADER_H_
