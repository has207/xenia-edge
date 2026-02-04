/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_flags.h"

#include "xenia/base/logging.h"
#include "xenia/ui/renderdoc_api.h"

DEFINE_bool(use_50Hz_mode, false, "Enables usage of PAL-50 mode.", "Video");

DEFINE_path(trace_gpu_prefix, "scratch/gpu/",
            "Prefix path for GPU trace files.", "GPU");
DEFINE_bool(trace_gpu_stream, false, "Trace all GPU packets.", "GPU");

DEFINE_path(
    dump_shaders, "",
    "For shader debugging, path to dump GPU shaders to as they are compiled.",
    "GPU");

DEFINE_bool(guest_display_refresh_cap, true,
            "Control guest vblank timing.\n"
            "  true: Fixed rate vblanks (50Hz PAL, 60Hz NTSC based on "
            "use_50Hz_mode).\n"
            "  false: Unlimited vblanks, allows the guest to run as fast as "
            "possible.",
            "GPU");

DEFINE_uint64(
    framerate_limit, 0,
    "Host frame rate limit in FPS. 0 = unlimited.\n"
    "Throttles presentation without affecting guest vblank timing.\n"
    "Guest vblanks are controlled by use_50Hz_mode (50Hz PAL, 60Hz NTSC).",
    "GPU");
UPDATE_from_uint64(framerate_limit, 2024, 8, 31, 20, 60);

void SetGuestDisplayRefreshCap(bool value) {
  OVERRIDE_bool(guest_display_refresh_cap, value);
}

void SetFramerateLimit(uint64_t value) {
  OVERRIDE_uint64(framerate_limit, value);
}

DEFINE_bool(
    gpu_allow_invalid_fetch_constants, true,
    "Allow texture and vertex fetch constants with invalid type - generally "
    "unsafe because the constant may contain completely invalid values, but "
    "may be used to bypass fetch constant type errors in certain games until "
    "the real reason why they're invalid is found.",
    "GPU");
DEFINE_bool(
    gpu_allow_invalid_upload_range, false,
    "Allows games to read data from pages that are marked as no access.",
    "GPU");

DEFINE_bool(
    non_seamless_cube_map, true,
    "Disable filtering between cube map faces near edges where possible "
    "(Vulkan with VK_EXT_non_seamless_cube_map) to reproduce the Direct3D 9 "
    "behavior.",
    "GPU");

// Extremely bright screen borders in 4D5307E6.
// Reading between texels with half-pixel offset in 58410954.
DEFINE_bool(
    half_pixel_offset, true,
    "Enable support of vertex half-pixel offset (D3D9 PA_SU_VTX_CNTL "
    "PIX_CENTER). Generally games are aware of the half-pixel offset, and "
    "having this enabled is the correct behavior (disabling this may "
    "significantly break post-processing in some games), but in certain games "
    "it might have been ignored, resulting in slight blurriness of UI "
    "textures, for instance, when they are read between texels rather than "
    "at texel centers, or the leftmost/topmost pixels may not be fully covered "
    "when MSAA is used with fullscreen passes.",
    "GPU");

DEFINE_int32(query_occlusion_sample_lower_threshold, 80,
             "If set to -1 no sample counts are written, games may hang. Else, "
             "the sample count of every tile will be incremented on every "
             "EVENT_WRITE_ZPD by this number. Setting this to 0 means "
             "everything is reported as occluded.",
             "GPU");
DEFINE_int32(
    query_occlusion_sample_upper_threshold, 100,
    "Set to higher number than query_occlusion_sample_lower_threshold. This "
    "value is ignored if query_occlusion_sample_lower_threshold is set to -1.",
    "GPU");

DEFINE_bool(metal_shader_disk_cache, true,
            "Cache compiled Metal shader libraries (metallib) to disk when "
            "store_shaders is enabled.",
            "GPU");

DEFINE_bool(metal_pipeline_binary_archive, true,
            "Use MTLBinaryArchive for Metal pipeline compilation caching. "
            "Requires store_shaders and a compatible OS/driver.",
            "GPU");

DEFINE_bool(metal_pipeline_disk_cache, true,
            "Store Metal render pipeline descriptor keys to disk so the binary "
            "archive can be prewarmed on the next run.",
            "GPU");

DEFINE_int32(
    metal_draw_ring_count, 128,
    "Metal per-command-buffer draw ring size (descriptor-table pages). "
    "Higher reduces ring churn but uses more memory.",
    "GPU");

DEFINE_bool(metal_use_heaps, true,
            "Use MTLHeap-backed texture allocations in Metal to reduce "
            "allocation overhead and fragmentation.",
            "GPU");
DEFINE_bool(metal_shared_memory_zero_copy, true,
            "Use MTLBuffer bytes-no-copy for guest memory on unified memory "
            "devices when possible.",
            "GPU");
DEFINE_int32(metal_heap_min_bytes, 33554432,
             "Minimum heap size (bytes) for Metal heap allocations.", "GPU");

DEFINE_bool(metal_texture_cache_use_private, true,
            "Use MTLStorageModePrivate for Metal texture cache textures when "
            "GPU upload paths support it.",
            "GPU");
DEFINE_bool(metal_texture_upload_via_blit, true,
            "Upload textures via staging buffers and GPU blit copies instead "
            "of CPU replaceRegion.",
            "GPU");

DEFINE_bool(occlusion_query_enable, false,
            "Use hardware occlusion queries instead of fake results. More "
            "accurate but causes GPU stalls and performance issues.",
            "GPU");

void SetOcclusionQueryEnable(bool value) {
  OVERRIDE_bool(occlusion_query_enable, value);
}

uint32_t GetGuestVblankRateHz() { return cvars::use_50Hz_mode ? 50 : 60; }

DEFINE_bool(
    gpu_debug_markers, false,
    "Insert debug markers into GPU command streams for tools like RenderDoc. "
    "Annotates draw calls with Xbox 360 GPU context (primitive type, shader "
    "hashes, vertex count, etc). Automatically enabled when RenderDoc is "
    "detected. Has minimal overhead when disabled.",
    "GPU");

bool IsGpuDebugMarkersEnabled() {
  // Cache the result - RenderDoc detection only needs to happen once.
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (cvars::gpu_debug_markers) {
      result = true;
      XELOGI("GPU debug markers enabled via CVAR");
    } else {
#if XE_PLATFORM_LINUX || XE_PLATFORM_WIN32
      auto renderdoc_api = xe::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        XELOGI("GPU debug markers auto-enabled (RenderDoc detected)");
      }
#endif
    }
  }
  return result;
}

// TODO(Triang3l): Make accuracy (ROV/FSI) the default when it's optimized
// better (for instance, using static shader modifications to pass render
// target parameters).
DEFINE_string(
    render_target_path, "performance",
    "Render target emulation path to use across all GPU backends.\n"
    "Use: [performance, accuracy]\n"
    " performance:\n"
    "  Host render targets and fixed-function blending and depth/stencil "
    "testing, copying between render targets when needed.\n"
    "  Lower accuracy (limited pixel format support).\n"
    "  Performance limited primarily by render target layout changes requiring "
    "copying, but generally higher.\n"
    "  Maps to 'fbo' on Vulkan and 'rtv' on D3D12.\n"
    " accuracy:\n"
    "  Manual pixel packing, blending and depth/stencil testing, with free "
    "render target layout changes.\n"
    "  Requires GPU supporting fragment shader interlock (Vulkan) or "
    "rasterizer-ordered views (D3D12).\n"
    "  Highest accuracy (all pixel formats handled in software).\n"
    "  Performance limited primarily by overdraw.\n"
    "  Maps to 'fsi' on Vulkan and 'rov' on D3D12.",
    "GPU");

DEFINE_bool(submit_on_primary_buffer_end, true,
            "Submit the command buffer when a PM4 primary buffer ends if it's "
            "possible to submit immediately to try to reduce frame latency.",
            "GPU");

DEFINE_bool(no_discard_stencil_in_transfer_pipelines, false,
            "Skip stencil bit discard in render target transfer pipelines. "
            "May improve performance on some GPUs.",
            "GPU");

DEFINE_bool(
    async_shader_compilation, true,
    "Compile shaders and create pipelines asynchronously in background "
    "threads. "
    "Eliminates shader compilation stutter but may cause brief rendering "
    "artifacts while pipelines are being created. When disabled, pipelines are "
    "created synchronously which causes stutter but no visual artifacts.",
    "GPU");

DEFINE_bool(
    readback_resolve_half_pixel_offset, false,
    "When resolution scaling is active, sample from the center of each scaled "
    "pixel block during readback resolve instead of the top-left corner. May "
    "improve image quality in some cases but can break games that rely on "
    "reading back specific pixel values (e.g., for gamma detection).",
    "GPU");

DEFINE_bool(gpu_3d_to_2d_texture, true,
            "Handle shaders that sample 3D textures as 2D by creating a 2D "
            "texture from slice 0 of the guest memory.",
            "GPU");

DEFINE_int32(anisotropic_override, -1,
             "Level of anisotropic filtering enforced on all texture fetch "
             "instructions.\n"
             " -1 = No override\n"
             "  0 = Disable anisotropic filtering\n"
             "  1 = Force 1x anisotropic filtering\n"
             "  2 = Force 2x anisotropic filtering\n"
             "  3 = Force 4x anisotropic filtering\n"
             "  4 = Force 8x anisotropic filtering\n"
             "  5 = Force 16x anisotropic filtering",
             "GPU");
