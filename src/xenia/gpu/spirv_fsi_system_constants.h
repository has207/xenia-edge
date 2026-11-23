/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SPIRV_FSI_SYSTEM_CONSTANTS_H_
#define XENIA_GPU_SPIRV_FSI_SYSTEM_CONSTANTS_H_

#include <cstdint>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader_translator.h"

namespace xe {
namespace gpu {

// Fills the fragment shader interlock (EDRAM ROP) subset of the SPIR-V system
// constants from the register file: the FSI-specific flag bits, the EDRAM tile
// pitch / render target / depth base / stencil / blend / polygon offset
// constants and the ZPD counter index. Shared by the Vulkan and the D3D12
// (Mesa spirv_to_dxil) command processors so the single EDRAM ROP encoding is
// not duplicated. Only call when the render target cache is in its pixel shader
// interlock path.
//
// flags is the in-progress system constant flags word. The FSI flag bits are
// OR'd into it (the caller owns the base bits and the final assignment). dirty
// is OR'd with true when any written constant changed, for the Vulkan caller's
// constant buffer invalidation. The D3D12 caller ignores it.
// zpd_fsi_counter_index is computed per backend (UINT32_MAX to disable, with
// XenosZPDReport::kCounterIndexFirstCopyOnly for VIZ segments) and passed in.
void WriteFragmentShaderInterlockSystemConstants(
    SpirvShaderTranslator::SystemConstants& system_constants, uint32_t& flags,
    bool& dirty, const RegisterFile& regs, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, uint32_t zpd_fsi_counter_index,
    int32_t window_offset_tiles);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SPIRV_FSI_SYSTEM_CONSTANTS_H_
