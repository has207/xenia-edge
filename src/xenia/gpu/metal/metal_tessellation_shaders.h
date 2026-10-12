/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TESSELLATION_SHADERS_H_
#define XENIA_GPU_METAL_METAL_TESSELLATION_SHADERS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace metal {

// The host tessellation vertex and hull shaders a tessellated draw links with,
// as SPIR-V. Same sources the Vulkan and D3D12 backends use.
struct MetalTessellationHostShaders {
  const uint32_t* vertex_spirv = nullptr;
  size_t vertex_word_count = 0;
  const uint32_t* hull_spirv = nullptr;
  size_t hull_word_count = 0;
  // Control points per patch the hull shader takes from the vertex stage.
  uint32_t input_control_point_count = 0;
};

// Returns false for a mode/domain combination with no host hull shader.
bool GetMetalTessellationHostShaders(
    xenos::TessellationMode tessellation_mode,
    Shader::HostVertexShaderType host_vertex_shader_type,
    MetalTessellationHostShaders& shaders_out);

// SPIR-V of a fragment shader that writes nothing, for tessellated draws
// without a pixel shader: the emulation pipeline always links a fragment stage.
std::vector<uint32_t> GetMetalTessellationDepthOnlyFragmentSpirv();

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TESSELLATION_SHADERS_H_
