/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/gpu_flags.h"

#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"
#include "third_party/metal-cpp/Metal/MTLEvent.hpp"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/metal/metal_shader_cache.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/metal/metal_gpu_completion_timeline.h"
using BYTE = uint8_t;
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_downscale_cs.h"
#include "xenia/ui/metal/metal_presenter.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

// Metal IR Converter Runtime - defines IRDescriptorTableEntry and bind points
#define IR_RUNTIME_METALCPP
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

// IR Converter bind points (from metal_irconverter_runtime.h)
// kIRDescriptorHeapBindPoint = 0

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(submit_on_primary_buffer_end);
// kIRSamplerHeapBindPoint = 1
// kIRArgumentBufferBindPoint = 2
// kIRArgumentBufferDrawArgumentsBindPoint = 4
// kIRArgumentBufferUniformsBindPoint = 5
// kIRVertexBufferBindPoint = 6

namespace xe {
namespace gpu {
namespace metal {

namespace {
void LogMetalErrorDetails(const char* label, NS::Error* error) {
  if (!error) {
    return;
  }
  const char* desc = error->localizedDescription()
                         ? error->localizedDescription()->utf8String()
                         : nullptr;
  const char* failure = error->localizedFailureReason()
                            ? error->localizedFailureReason()->utf8String()
                            : nullptr;
  const char* recovery =
      error->localizedRecoverySuggestion()
          ? error->localizedRecoverySuggestion()->utf8String()
          : nullptr;
  const char* domain =
      error->domain() ? error->domain()->utf8String() : nullptr;
  int64_t code = error->code();
  XELOGE("{}: domain={} code={} desc='{}' failure='{}' recovery='{}'", label,
         domain ? domain : "<null>", code, desc ? desc : "<null>",
         failure ? failure : "<null>", recovery ? recovery : "<null>");
  NS::Dictionary* user_info = error->userInfo();
  if (user_info) {
    auto* info_desc = user_info->description();
    XELOGE("{}: userInfo={}", label,
           info_desc ? info_desc->utf8String() : "<null>");
  }
}

MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name) {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }

  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  // XeSL compute entrypoint name used in the embedded metallibs.
  NS::String* fn_name = NS::String::string("entry_xe", NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    XELOGE("Metal: {} missing entry_xe", debug_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineState* pipeline =
      device->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

constexpr uint32_t kPipelineDiskCacheMagic = 0x43504D58;  // 'XMPC'
constexpr uint32_t kPipelineDiskCacheVersion = 2;
constexpr size_t kPipelineDiskCacheMaxEntrySize = 1 << 20;

XEPACKEDSTRUCT(PipelineDiskCacheHeader, {
  uint32_t magic;
  uint32_t version;
  uint32_t reserved[2];
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryHeader, {
  uint32_t entry_size;
  uint32_t reserved;
});

XEPACKEDSTRUCT(PipelineDiskCacheEntryBase, {
  uint64_t pipeline_key;
  uint64_t vertex_shader_cache_key;
  uint64_t pixel_shader_cache_key;
  uint32_t sample_count;
  uint32_t depth_format;
  uint32_t stencil_format;
  uint32_t color_formats[4];
  uint32_t normalized_color_mask;
  uint32_t alpha_to_mask_enable;
  uint32_t blendcontrol[4];
  uint32_t vertex_attribute_count;
  uint32_t vertex_layout_count;
});

static_assert(sizeof(PipelineDiskCacheHeader) == 16,
              "Unexpected pipeline disk cache header size.");
static_assert(sizeof(PipelineDiskCacheEntryHeader) == 8,
              "Unexpected pipeline disk cache entry header size.");

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

MTL::ColorWriteMask ToMetalColorWriteMask(uint32_t write_mask) {
  MTL::ColorWriteMask mtl_mask = MTL::ColorWriteMaskNone;
  if (write_mask & 0x1) {
    mtl_mask |= MTL::ColorWriteMaskRed;
  }
  if (write_mask & 0x2) {
    mtl_mask |= MTL::ColorWriteMaskGreen;
  }
  if (write_mask & 0x4) {
    mtl_mask |= MTL::ColorWriteMaskBlue;
  }
  if (write_mask & 0x8) {
    mtl_mask |= MTL::ColorWriteMaskAlpha;
  }
  return mtl_mask;
}

MTL::BlendOperation ToMetalBlendOperation(xenos::BlendOp blend_op) {
  // 8 entries for safety since 3 bits from the guest are passed directly.
  static const MTL::BlendOperation kBlendOpMap[8] = {
      MTL::BlendOperationAdd,              // 0
      MTL::BlendOperationSubtract,         // 1
      MTL::BlendOperationMin,              // 2
      MTL::BlendOperationMax,              // 3
      MTL::BlendOperationReverseSubtract,  // 4
      MTL::BlendOperationAdd,              // 5
      MTL::BlendOperationAdd,              // 6
      MTL::BlendOperationAdd,              // 7
  };
  return kBlendOpMap[uint32_t(blend_op) & 0x7];
}

MTL::BlendFactor ToMetalBlendFactorRgb(xenos::BlendFactor blend_factor) {
  // 32 because of 0x1F mask, for safety (all unknown to zero).
  static const MTL::BlendFactor kBlendFactorMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceColor,
      /*  5 */ MTL::BlendFactorOneMinusSourceColor,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationColor,
      /*  9 */ MTL::BlendFactorOneMinusDestinationColor,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendColor,  // CONSTANT_COLOR
      /* 13 */ MTL::BlendFactorOneMinusBlendColor,
      /* 14 */ MTL::BlendFactorBlendAlpha,  // CONSTANT_ALPHA
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorMap[uint32_t(blend_factor) & 0x1F];
}

MTL::BlendFactor ToMetalBlendFactorAlpha(xenos::BlendFactor blend_factor) {
  // Like the RGB map, but with color modes changed to alpha.
  static const MTL::BlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceAlpha,
      /*  5 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationAlpha,
      /*  9 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendAlpha,
      /* 13 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorAlphaMap[uint32_t(blend_factor) & 0x1F];
}

void DownscaleResolveTileData(const uint8_t* source, uint8_t* dest,
                              uint32_t tile_count, uint32_t pixel_size_log2,
                              uint32_t scale_x, uint32_t scale_y,
                              bool half_pixel_offset) {
  if (!source || !dest || tile_count == 0) {
    return;
  }
  const uint32_t pixel_size = 1u << pixel_size_log2;
  const uint32_t scale_xy = scale_x * scale_y;
  const uint32_t tile_size_1x = 32u * 32u * pixel_size;
  const uint32_t tile_size_scaled = tile_size_1x * scale_xy;
  const uint32_t block_sample_offset =
      (half_pixel_offset && scale_xy > 1u)
          ? ((scale_x >> 1u) + (scale_y >> 1u) * scale_x)
          : 0u;
  const uint32_t block_sample_offset_bytes = block_sample_offset * pixel_size;
  const uint32_t src_pixel_stride = pixel_size * scale_xy;

  for (uint32_t tile_index = 0; tile_index < tile_count; ++tile_index) {
    const uint8_t* src_tile =
        source + tile_index * tile_size_scaled + block_sample_offset_bytes;
    uint8_t* dst_tile = dest + tile_index * tile_size_1x;
    for (uint32_t pixel_index = 0; pixel_index < 32u * 32u; ++pixel_index) {
      const uint32_t src_offset = pixel_index * src_pixel_stride;
      const uint32_t dst_offset = pixel_index * pixel_size;
      switch (pixel_size_log2) {
        case 0: {
          dst_tile[dst_offset] = src_tile[src_offset];
          break;
        }
        case 1: {
          uint16_t value;
          std::memcpy(&value, src_tile + src_offset, sizeof(value));
          std::memcpy(dst_tile + dst_offset, &value, sizeof(value));
          break;
        }
        case 2: {
          uint32_t value;
          std::memcpy(&value, src_tile + src_offset, sizeof(value));
          std::memcpy(dst_tile + dst_offset, &value, sizeof(value));
          break;
        }
        case 3: {
          uint64_t value;
          std::memcpy(&value, src_tile + src_offset, sizeof(value));
          std::memcpy(dst_tile + dst_offset, &value, sizeof(value));
          break;
        }
        default:
          break;
      }
    }
  }
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(
    MetalGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

MetalCommandProcessor::~MetalCommandProcessor() {
  // End any active render encoder before releasing
  // Note: Only call endEncoding if the encoder is still active
  // (not already ended by a committed command buffer)
  if (current_render_encoder_) {
    // The encoder may already be ended if the command buffer was committed
    // In that case, just release it
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  if (render_pass_descriptor_) {
    render_pass_descriptor_->release();
    render_pass_descriptor_ = nullptr;
  }
  if (render_target_texture_) {
    render_target_texture_->release();
    render_target_texture_ = nullptr;
  }
  if (depth_stencil_texture_) {
    depth_stencil_texture_->release();
    depth_stencil_texture_ = nullptr;
  }

  // Release pipeline cache
  for (auto& pair : pipeline_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  pipeline_cache_.clear();

  for (auto& pair : geometry_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
  }
  geometry_pipeline_cache_.clear();

  for (auto& pair : geometry_vertex_stage_cache_) {
    if (pair.second.library) {
      pair.second.library->release();
    }
    if (pair.second.stage_in_library) {
      pair.second.stage_in_library->release();
    }
  }
  geometry_vertex_stage_cache_.clear();

  for (auto& pair : geometry_shader_stage_cache_) {
    if (pair.second.library) {
      pair.second.library->release();
    }
  }
  geometry_shader_stage_cache_.clear();

  for (auto& pair : depth_stencil_state_cache_) {
    if (pair.second) {
      pair.second->release();
    }
  }
  depth_stencil_state_cache_.clear();

  // Release IR Converter runtime buffers and resources
  if (null_buffer_) {
    null_buffer_->release();
    null_buffer_ = nullptr;
  }
  if (null_texture_) {
    null_texture_->release();
    null_texture_ = nullptr;
  }
  if (null_sampler_) {
    null_sampler_->release();
    null_sampler_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(draw_ring_mutex_);
    active_draw_ring_.reset();
    draw_ring_pool_.clear();
    command_buffer_draw_rings_.clear();
  }
  res_heap_ab_ = nullptr;
  smp_heap_ab_ = nullptr;
  cbv_heap_ab_ = nullptr;
  uniforms_buffer_ = nullptr;
  top_level_ab_ = nullptr;
  draw_args_buffer_ = nullptr;

  ShutdownShaderStorage();
}

MetalCommandProcessor::DrawRingBuffers::~DrawRingBuffers() {
  if (res_heap_ab) {
    res_heap_ab->release();
    res_heap_ab = nullptr;
  }
  if (smp_heap_ab) {
    smp_heap_ab->release();
    smp_heap_ab = nullptr;
  }
  if (cbv_heap_ab) {
    cbv_heap_ab->release();
    cbv_heap_ab = nullptr;
  }
  if (uniforms_buffer) {
    uniforms_buffer->release();
    uniforms_buffer = nullptr;
  }
  if (top_level_ab) {
    top_level_ab->release();
    top_level_ab = nullptr;
  }
  if (draw_args_buffer) {
    draw_args_buffer->release();
    draw_args_buffer = nullptr;
  }
}

void MetalCommandProcessor::UpdateDebugMarkersEnabled() {
  // Enable debug markers if the CVAR is set (RenderDoc auto-detect disabled on
  // macOS).
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void MetalCommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  auto* ns_label = NS::String::string(label, NS::UTF8StringEncoding);
  if (current_render_encoder_) {
    current_render_encoder_->pushDebugGroup(ns_label);
    debug_marker_stack_.push_back(DebugMarkerTarget::kRenderEncoder);
  } else if (current_command_buffer_) {
    current_command_buffer_->pushDebugGroup(ns_label);
    debug_marker_stack_.push_back(DebugMarkerTarget::kCommandBuffer);
  }
}

void MetalCommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_ || debug_marker_stack_.empty()) {
    return;
  }
  DebugMarkerTarget target = debug_marker_stack_.back();
  debug_marker_stack_.pop_back();
  if (target == DebugMarkerTarget::kRenderEncoder) {
    if (current_render_encoder_) {
      current_render_encoder_->popDebugGroup();
    }
  } else {
    if (current_command_buffer_) {
      current_command_buffer_->popDebugGroup();
    }
  }
}

void MetalCommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  auto* ns_label = NS::String::string(label, NS::UTF8StringEncoding);
  if (current_render_encoder_) {
    current_render_encoder_->insertDebugSignpost(ns_label);
  } else if (current_command_buffer_) {
    current_command_buffer_->pushDebugGroup(ns_label);
    current_command_buffer_->popDebugGroup();
  }
}

void MetalCommandProcessor::RequestCapture() {
  capture_requested_.store(true, std::memory_order_release);
}

void MetalCommandProcessor::MaybeStartCapture() {
  if (!capture_requested_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (!command_queue_) {
    XELOGW("Metal capture requested but command queue is not ready");
    return;
  }
  capture_manager_ = MTL::CaptureManager::sharedCaptureManager();
  if (!capture_manager_) {
    XELOGW("Metal capture manager not available");
    return;
  }
  auto* descriptor = MTL::CaptureDescriptor::alloc()->init();
  descriptor->setCaptureObject(command_queue_);
  descriptor->setDestination(MTL::CaptureDestinationGPUTraceDocument);

  const char* capture_dir = std::getenv("XENIA_GPU_CAPTURE_DIR");
  std::string filename =
      capture_dir ? (std::string(capture_dir) + "/metal_capture.gputrace")
                  : std::string("./metal_capture.gputrace");
  auto* url = NS::URL::fileURLWithPath(
      NS::String::string(filename.c_str(), NS::UTF8StringEncoding));
  descriptor->setOutputURL(url);

  NS::Error* error = nullptr;
  if (capture_manager_->startCapture(descriptor, &error)) {
    capture_active_ = true;
    XELOGI("Metal capture started: {}", filename);
  } else {
    XELOGE("Metal capture start failed: {} (set MTL_CAPTURE_ENABLED=1)",
           error ? error->localizedDescription()->utf8String() : "unknown");
  }
  descriptor->release();
}

void MetalCommandProcessor::StopCaptureIfActive() {
  if (!capture_active_ || !capture_manager_) {
    return;
  }
  capture_manager_->stopCapture();
  capture_active_ = false;
  XELOGI("Metal capture completed");
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  if (primitive_processor_) {
    primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Restore the guest EDRAM snapshot captured in the trace into the Metal
  // render-target cache so that subsequent host render targets created from
  // EDRAM (via LoadTiledData) see the same initial contents as other
  // backends like D3D12.
  if (!snapshot) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called with null "
        "snapshot");
    return;
  }
  if (!render_target_cache_) {
    XELOGW(
        "MetalCommandProcessor::RestoreEdramSnapshot called before render "
        "target "
        "cache initialization");
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // TODO(wmarti): Add cache_clear_requested_ flag like D3D12 for deferred
  // clearing of pipeline caches, texture caches, etc.
}

void MetalCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void MetalCommandProcessor::ClearReadbackBuffers() {
  for (auto& entry : readback_buffers_) {
    ReadbackBuffer& rb = entry.second;
    for (size_t i = 0; i < 2; ++i) {
      if (rb.buffers[i]) {
        rb.buffers[i]->release();
        rb.buffers[i] = nullptr;
      }
      rb.sizes[i] = 0;
      rb.submission_ids[i] = 0;
    }
    rb.current_index = 0;
    rb.last_used_frame = 0;
  }
  readback_buffers_.clear();
}

void MetalCommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (frame_current_ <= kReadbackBufferEvictionAgeFrames) {
    return;
  }

  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    if (it->second.last_used_frame <
        frame_current_ - kReadbackBufferEvictionAgeFrames) {
      for (int i = 0; i < 2; ++i) {
        if (it->second.buffers[i]) {
          it->second.buffers[i]->release();
        }
      }
      it = buffer_map.erase(it);
    } else {
      ++it;
    }
  }
}

ui::metal::MetalProvider& MetalCommandProcessor::GetMetalProvider() const {
  return *static_cast<ui::metal::MetalProvider*>(graphics_system_->provider());
}

uint64_t MetalCommandProcessor::GetCurrentSubmission() const {
  return completion_timeline_ ? completion_timeline_->GetUpcomingSubmission()
                              : 1;
}

uint64_t MetalCommandProcessor::GetCompletedSubmission() const {
  return completion_timeline_
             ? completion_timeline_->GetCompletedSubmissionFromLastUpdate()
             : 0;
}

void MetalCommandProcessor::MarkResolvedMemory(uint32_t base_ptr,
                                               uint32_t length) {
  if (length == 0) return;
  resolved_memory_ranges_.push_back({base_ptr, length});
}

bool MetalCommandProcessor::IsResolvedMemory(uint32_t base_ptr,
                                             uint32_t length) const {
  uint32_t end_ptr = base_ptr + length;
  for (const auto& range : resolved_memory_ranges_) {
    uint32_t range_end = range.base + range.length;
    // Check if ranges overlap
    if (base_ptr < range_end && end_ptr > range.base) {
      return true;
    }
  }
  return false;
}

void MetalCommandProcessor::ClearResolvedMemory() {
  resolved_memory_ranges_.clear();
}

void MetalCommandProcessor::ForceIssueSwap() {
  // Force a swap to push any pending render target to presenter
  // This is used by trace dumps to capture output when there's no explicit swap
  if (saw_swap_) {
    return;
  }
  IssueSwap(0, render_target_width_, render_target_height_);
}

void MetalCommandProcessor::SetSwapDestSwap(uint32_t dest_base, bool swap) {
  if (!dest_base) {
    return;
  }
  if (swap_dest_swaps_by_base_.size() > 256) {
    swap_dest_swaps_by_base_.clear();
  }
  swap_dest_swaps_by_base_[dest_base] = swap;
}

bool MetalCommandProcessor::ConsumeSwapDestSwap(uint32_t dest_base,
                                                bool* swap_out) {
  if (!swap_out || !dest_base) {
    return false;
  }
  auto it = swap_dest_swaps_by_base_.find(dest_base);
  if (it == swap_dest_swaps_by_base_.end()) {
    return false;
  }
  *swap_out = it->second;
  swap_dest_swaps_by_base_.erase(it);
  return true;
}

bool MetalCommandProcessor::SetupContext() {
  saw_swap_ = false;
  last_swap_ptr_ = 0;
  last_swap_width_ = 0;
  last_swap_height_ = 0;
  swap_dest_swaps_by_base_.clear();
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  // Check if debug markers should be enabled (CVAR).
  UpdateDebugMarkersEnabled();
  if (debug_markers_enabled_) {
    XELOGI("GPU debug markers enabled for Metal debug tools");
  }

  const ui::metal::MetalProvider& provider = GetMetalProvider();
  device_ = provider.GetDevice();
  command_queue_ = provider.GetCommandQueue();

  if (!device_ || !command_queue_) {
    XELOGE("MetalCommandProcessor: No Metal device or command queue available");
    return false;
  }

  wait_shared_event_ = device_->newSharedEvent();
  if (wait_shared_event_) {
    wait_shared_event_->setLabel(
        NS::String::string("XeniaWaitEvent", NS::UTF8StringEncoding));
    wait_shared_event_value_ = 0;
  } else {
    XELOGW(
        "MetalCommandProcessor: SharedEvent unavailable; falling back to "
        "waitUntilCompleted");
  }

  completion_timeline_ = ui::metal::MetalGPUCompletionTimeline::Create(device_);
  if (!completion_timeline_) {
    XELOGE("MetalCommandProcessor: Failed to create completion timeline");
    return false;
  }
  submission_open_ = false;
  submission_completed_processed_ = 0;
  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::fill_n(closed_frame_submissions_, kQueueFrames, 0);

  bool supports_apple7 = device_->supportsFamily(MTL::GPUFamilyApple7);
  bool supports_mac2 = device_->supportsFamily(MTL::GPUFamilyMac2);
  mesh_shader_supported_ = supports_apple7 || supports_mac2;

  draw_ring_count_ = std::max<int32_t>(1, ::cvars::metal_draw_ring_count);

  // Initialize shared memory
  shared_memory_ = std::make_unique<MetalSharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Initialize primitive processor (index/primitive conversion like D3D12).
  primitive_processor_ = std::make_unique<MetalPrimitiveProcessor>(
      *this, *register_file_, *memory_, trace_writer_, *shared_memory_);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize Metal primitive processor");
    return false;
  }

  // Get the draw resolution scale for the render target cache and the texture
  // cache (match D3D12/Vulkan behavior).
  uint32_t draw_resolution_scale_x = 1;
  uint32_t draw_resolution_scale_y = 1;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);
  if (!draw_resolution_scale_not_clamped) {
    XELOGW(
        "The requested draw resolution scale is not supported by the emulator "
        "or config, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }
  XELOGI("Metal: draw resolution scale {}x{} (supported={})",
         draw_resolution_scale_x, draw_resolution_scale_y,
         draw_resolution_scale_not_clamped);

  texture_cache_ = std::make_unique<MetalTextureCache>(
      this, *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y);
  if (!texture_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal texture cache");
    return false;
  }

  // Initialize render target cache
  render_target_cache_ = std::make_unique<MetalRenderTargetCache>(
      *register_file_, *memory_, &trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize Metal render target cache");
    return false;
  }

  resolve_downscale_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_downscale_cs_metallib,
      sizeof(resolve_downscale_cs_metallib), "resolve_downscale");
  if (!resolve_downscale_pipeline_) {
    XELOGW("MetalCommandProcessor: resolve downscale pipeline unavailable");
  }

  // Initialize shader translation pipeline
  if (!InitializeShaderTranslation()) {
    XELOGE("Failed to initialize shader translation");
    return false;
  }
  if (mesh_shader_supported_) {
    uint64_t tess_tables_size = IRRuntimeTessellatorTablesSize();
    tessellator_tables_buffer_ =
        device_->newBuffer(tess_tables_size, MTL::ResourceStorageModeShared);
    if (!tessellator_tables_buffer_) {
      XELOGE("Failed to allocate tessellator tables buffer ({} bytes)",
             tess_tables_size);
      return false;
    }
    tessellator_tables_buffer_->setLabel(
        NS::String::string("XeniaTessellatorTables", NS::UTF8StringEncoding));
    IRRuntimeLoadTessellatorTables(tessellator_tables_buffer_);
  }

  // Create render target texture for offscreen rendering
  MTL::TextureDescriptor* color_desc = MTL::TextureDescriptor::alloc()->init();
  color_desc->setTextureType(MTL::TextureType2D);
  color_desc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
  color_desc->setWidth(render_target_width_);
  color_desc->setHeight(render_target_height_);
  color_desc->setStorageMode(MTL::StorageModePrivate);
  color_desc->setUsage(MTL::TextureUsageRenderTarget |
                       MTL::TextureUsageShaderRead);

  render_target_texture_ = device_->newTexture(color_desc);
  color_desc->release();

  if (!render_target_texture_) {
    XELOGE("Failed to create render target texture");
    return false;
  }
  render_target_texture_->setLabel(
      NS::String::string("XeniaRenderTarget", NS::UTF8StringEncoding));

  // Create depth/stencil texture
  MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::alloc()->init();
  depth_desc->setTextureType(MTL::TextureType2D);
  depth_desc->setPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
  depth_desc->setWidth(render_target_width_);
  depth_desc->setHeight(render_target_height_);
  depth_desc->setStorageMode(MTL::StorageModePrivate);
  depth_desc->setUsage(MTL::TextureUsageRenderTarget);

  depth_stencil_texture_ = device_->newTexture(depth_desc);
  depth_desc->release();

  if (!depth_stencil_texture_) {
    XELOGE("Failed to create depth/stencil texture");
    return false;
  }
  depth_stencil_texture_->setLabel(
      NS::String::string("XeniaDepthStencil", NS::UTF8StringEncoding));

  // Create render pass descriptor
  render_pass_descriptor_ = MTL::RenderPassDescriptor::alloc()->init();

  auto color_attachment =
      render_pass_descriptor_->colorAttachments()->object(0);
  color_attachment->setTexture(render_target_texture_);
  color_attachment->setLoadAction(MTL::LoadActionClear);
  color_attachment->setStoreAction(MTL::StoreActionStore);
  color_attachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

  auto depth_attachment = render_pass_descriptor_->depthAttachment();
  depth_attachment->setTexture(depth_stencil_texture_);
  depth_attachment->setLoadAction(MTL::LoadActionClear);
  depth_attachment->setStoreAction(MTL::StoreActionDontCare);
  depth_attachment->setClearDepth(1.0);

  auto stencil_attachment = render_pass_descriptor_->stencilAttachment();
  stencil_attachment->setTexture(depth_stencil_texture_);
  stencil_attachment->setLoadAction(MTL::LoadActionClear);
  stencil_attachment->setStoreAction(MTL::StoreActionDontCare);
  stencil_attachment->setClearStencil(0);

  // Create a null buffer for unused descriptor entries
  // This prevents shader validation errors when accessing unpopulated
  // descriptors
  null_buffer_ =
      device_->newBuffer(kNullBufferSize, MTL::ResourceStorageModeShared);
  if (!null_buffer_) {
    XELOGE("Failed to create null buffer");
    return false;
  }
  null_buffer_->setLabel(
      NS::String::string("NullBuffer", NS::UTF8StringEncoding));
  std::memset(null_buffer_->contents(), 0, kNullBufferSize);

  // Create a 1x1x1 placeholder 2D array texture for unbound texture slots
  // Xbox 360 textures are typically 2D arrays (for texture atlases, cubemaps)
  // Using 2DArray prevents "Invalid texture type" validation errors
  MTL::TextureDescriptor* null_tex_desc =
      MTL::TextureDescriptor::alloc()->init();
  null_tex_desc->setTextureType(MTL::TextureType2DArray);
  null_tex_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  null_tex_desc->setWidth(1);
  null_tex_desc->setHeight(1);
  null_tex_desc->setArrayLength(1);  // Single slice in the array
  null_tex_desc->setStorageMode(MTL::StorageModeShared);
  null_tex_desc->setUsage(MTL::TextureUsageShaderRead);

  null_texture_ = device_->newTexture(null_tex_desc);
  null_tex_desc->release();

  if (!null_texture_) {
    XELOGE("Failed to create null texture");
    return false;
  }
  null_texture_->setLabel(
      NS::String::string("NullTexture2DArray", NS::UTF8StringEncoding));

  // Fill the 1x1x1 texture with opaque white (helps debug if sampled)
  uint32_t white_pixel = 0xFFFFFFFF;
  MTL::Region region =
      MTL::Region(0, 0, 0, 1, 1, 1);  // x,y,z origin, w,h,d size
  null_texture_->replaceRegion(region, 0, 0, &white_pixel, 4, 0);  // slice 0

  // Create a default sampler for unbound sampler slots
  // Must set supportsArgumentBuffers=YES for use in argument buffers
  MTL::SamplerDescriptor* null_smp_desc =
      MTL::SamplerDescriptor::alloc()->init();
  null_smp_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
  null_smp_desc->setMipFilter(MTL::SamplerMipFilterLinear);
  null_smp_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
  null_smp_desc->setSupportArgumentBuffers(true);

  null_sampler_ = device_->newSamplerState(null_smp_desc);
  null_smp_desc->release();

  if (!null_sampler_) {
    XELOGE("Failed to create null sampler");
    return false;
  }

  auto ring = CreateDrawRingBuffers();
  if (!ring) {
    return false;
  }
  SetActiveDrawRing(ring);

  return true;
}

bool MetalCommandProcessor::InitializeShaderTranslation() {
  // Initialize DXBC shader translator (use Apple vendor ID for Metal)
  // Must query render_target_cache_ for actual runtime parameters.
  // Metal doesn't use ROV (rasterizer ordered views) path.
  bool edram_rov_used = false;

  // gamma_render_target_as_unorm8: When true, shaders include code to convert
  // linear -> gamma for 8-bit gamma render targets. When false, we use 16-bit
  // UNORM format where hardware handles gamma implicitly.
  bool gamma_render_target_as_unorm8 = !(
      edram_rov_used || render_target_cache_->gamma_render_target_as_unorm16());

  XELOGI(
      "DxbcShaderTranslator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
      gamma_render_target_as_unorm8, render_target_cache_->msaa_2x_supported(),
      render_target_cache_->draw_resolution_scale_x(),
      render_target_cache_->draw_resolution_scale_y());

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      ui::GraphicsProvider::GpuVendorID::kApple,
      false,  // bindless_resources_used - not using bindless for now
      edram_rov_used, gamma_render_target_as_unorm8,
      render_target_cache_->msaa_2x_supported(),
      render_target_cache_->draw_resolution_scale_x(),
      render_target_cache_->draw_resolution_scale_y(),
      false);  // force_emit_source_map

  // Initialize DXBC to DXIL converter
  dxbc_to_dxil_converter_ = std::make_unique<DxbcToDxilConverter>();
  if (!dxbc_to_dxil_converter_->Initialize()) {
    XELOGE("Failed to initialize DXBC to DXIL converter");
    return false;
  }

  // Initialize Metal Shader Converter
  metal_shader_converter_ = std::make_unique<MetalShaderConverter>();
  if (!metal_shader_converter_->Initialize()) {
    XELOGE("Failed to initialize Metal Shader Converter");
    return false;
  }

  // Configure MSC minimum targets to avoid materialization failures on older
  // GPUs/OS versions.
  if (device_) {
    IRGPUFamily min_family = IRGPUFamilyMetal3;
    if (device_->supportsFamily(MTL::GPUFamilyApple10)) {
      min_family = IRGPUFamilyApple10;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple9)) {
      min_family = IRGPUFamilyApple9;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple8)) {
      min_family = IRGPUFamilyApple8;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple7)) {
      min_family = IRGPUFamilyApple7;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple6)) {
      min_family = IRGPUFamilyApple6;
    } else if (device_->supportsFamily(MTL::GPUFamilyMac2) ||
               device_->supportsFamily(MTL::GPUFamilyMetal4) ||
               device_->supportsFamily(MTL::GPUFamilyMetal3)) {
      min_family = IRGPUFamilyMetal3;
    }

    NS::OperatingSystemVersion os_version =
        NS::ProcessInfo::processInfo()->operatingSystemVersion();
    std::ostringstream version_stream;
    version_stream << os_version.majorVersion << "." << os_version.minorVersion
                   << "." << os_version.patchVersion;
    metal_shader_converter_->SetMinimumTarget(
        min_family, IROperatingSystem_macOS, version_stream.str());
  }

  return true;
}

void MetalCommandProcessor::PrepareForWait() {
  // Flush any pending Metal command buffers before entering wait state.
  // This is critical because:
  // 1. The worker thread's autorelease pool will be drained when it exits
  // 2. Metal objects in that pool might still be referenced by in-flight
  // commands
  // 3. Releasing those objects during pool drain can hang waiting for GPU
  // completion
  //
  // By submitting and waiting for all GPU work now, we ensure clean pool
  // drainage.

  EndRenderEncoder();

  if (submission_open_ || current_command_buffer_) {
    uint64_t submission_to_wait =
        current_command_buffer_ ? GetCurrentSubmission() : 0;
    if (!submission_open_) {
      XELOGW(
          "MetalCommandProcessor::PrepareForWait: command buffer without "
          "open submission");
      submission_open_ = true;
    }
    EndSubmission(false);
    if (submission_to_wait) {
      CheckSubmissionCompletion(submission_to_wait);
    }
  }
  DrainCommandBufferAutoreleasePool();

  // Even if we have no active command buffer, there might be GPU work from
  // previously submitted command buffers that autoreleased objects depend on.
  // Submit and wait for a dummy command buffer to ensure ALL GPU work
  // completes.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    // Note: commandBuffer() returns an autoreleased object per metal-cpp docs.
    // We do NOT call release() since we didn't retain() it.
    // The autorelease pool will handle cleanup.
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
      // Don't release - it's autoreleased and will be cleaned up by the pool
    }
    pool->release();
  }

  // Also call the base class to flush trace writer
  CommandProcessor::PrepareForWait();
}

void MetalCommandProcessor::ShutdownContext() {
  EndRenderEncoder();

  if (submission_open_ || current_command_buffer_) {
    uint64_t submission_to_wait =
        current_command_buffer_ ? GetCurrentSubmission() : 0;
    if (!submission_open_) {
      XELOGW(
          "MetalCommandProcessor::ShutdownContext: command buffer without "
          "open submission");
      submission_open_ = true;
    }
    EndSubmission(false);
    if (submission_to_wait) {
      CheckSubmissionCompletion(submission_to_wait);
    }
  }

  // Even if we have no active command buffer at this point, there may be
  // previously committed command buffers still in flight. Submit and wait for
  // a dummy command buffer to ensure all GPU work on this queue has completed
  // before tearing down resources on thread exit.
  if (command_queue_) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* sync_cmd = command_queue_->commandBuffer();
    if (sync_cmd) {
      uint64_t wait_value = 0;
      if (wait_shared_event_) {
        wait_value = ++wait_shared_event_value_;
        sync_cmd->encodeSignalEvent(wait_shared_event_, wait_value);
      }
      sync_cmd->commit();
      if (wait_shared_event_) {
        wait_shared_event_->waitUntilSignaledValue(
            wait_value, std::numeric_limits<uint64_t>::max());
      } else {
        sync_cmd->waitUntilCompleted();
      }
    }
    pool->release();
  }

  // Now safe to release encoder and command buffer
  if (current_render_encoder_) {
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }
  if (current_command_buffer_) {
    current_command_buffer_->release();
    current_command_buffer_ = nullptr;
  }
  DrainCommandBufferAutoreleasePool();

  ClearReadbackBuffers();
  if (resolve_downscale_buffer_) {
    resolve_downscale_buffer_->release();
    resolve_downscale_buffer_ = nullptr;
    resolve_downscale_buffer_size_ = 0;
  }
  if (resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_->release();
    resolve_downscale_pipeline_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(draw_ring_mutex_);
    active_draw_ring_.reset();
    draw_ring_pool_.clear();
    command_buffer_draw_rings_.clear();
  }

  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }

  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  if (tessellator_tables_buffer_) {
    tessellator_tables_buffer_->release();
    tessellator_tables_buffer_ = nullptr;
  }
  if (depth_only_pixel_library_) {
    depth_only_pixel_library_->release();
    depth_only_pixel_library_ = nullptr;
  }
  depth_only_pixel_function_name_.clear();
  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::fill_n(closed_frame_submissions_, kQueueFrames, 0);
  submission_open_ = false;
  submission_completed_processed_ = 0;
  completion_timeline_.reset();

  shader_cache_.clear();
  shared_memory_.reset();
  shader_translator_.reset();
  dxbc_to_dxil_converter_.reset();
  metal_shader_converter_.reset();
  if (wait_shared_event_) {
    wait_shared_event_->release();
    wait_shared_event_ = nullptr;
  }

  ShutdownShaderStorage();

  CommandProcessor::ShutdownContext();
}

void MetalCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  InitializeShaderStorageInternal(cache_root, title_id, blocking);
  if (completion_callback) {
    completion_callback();
  }
}

bool MetalCommandProcessor::InitializeShaderStorageInternal(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();

  if (!device_) {
    XELOGW("Metal shader storage init skipped (no device)");
    return false;
  }

  shader_storage_root_ = cache_root / "shaders" / "metal";
  shader_storage_local_root_ =
      shader_storage_root_ / "local" / GetShaderStorageDeviceTag();
  shader_storage_title_root_ =
      shader_storage_local_root_ / fmt::format("{:08X}", title_id);

  std::error_code ec;
  std::filesystem::create_directories(shader_storage_title_root_, ec);
  if (ec) {
    XELOGW("Metal shader storage: Failed to create {}: {}",
           shader_storage_title_root_.string(), ec.message());
    return false;
  }

  metallib_cache_dir_ = shader_storage_title_root_ / "metallib";
  if (::cvars::metal_shader_disk_cache && g_metal_shader_cache) {
    g_metal_shader_cache->Initialize(metallib_cache_dir_);
  }

  const char* path_suffix = "rtv";

  pipeline_disk_cache_path_ =
      shader_storage_title_root_ /
      fmt::format("{:08X}.{}.metal.pipelines", title_id, path_suffix);
  pipeline_binary_archive_path_ =
      shader_storage_title_root_ /
      fmt::format("{:08X}.{}.metal.binarchive", title_id, path_suffix);

  if (::cvars::metal_pipeline_disk_cache) {
    LoadPipelineDiskCache(pipeline_disk_cache_path_,
                          &pipeline_disk_cache_entries_);
  }

  if (::cvars::metal_pipeline_binary_archive) {
    InitializePipelineBinaryArchive(pipeline_binary_archive_path_);
  }

  if (blocking && pipeline_binary_archive_ &&
      !pipeline_disk_cache_entries_.empty()) {
    PrewarmPipelineBinaryArchive(pipeline_disk_cache_entries_);
  }

  return true;
}

void MetalCommandProcessor::ShutdownShaderStorage() {
  if (pipeline_binary_archive_) {
    SerializePipelineBinaryArchive();
    pipeline_binary_archive_->release();
    pipeline_binary_archive_ = nullptr;
  }
  if (pipeline_disk_cache_file_) {
    std::fclose(pipeline_disk_cache_file_);
    pipeline_disk_cache_file_ = nullptr;
  }
  pipeline_disk_cache_keys_.clear();
  pipeline_disk_cache_entries_.clear();

  if (g_metal_shader_cache) {
    g_metal_shader_cache->Shutdown();
  }
}

std::string MetalCommandProcessor::GetShaderStorageDeviceTag() const {
  std::string tag = "unknown";
  if (device_ && device_->name()) {
    tag = device_->name()->utf8String();
  }

  for (char& ch : tag) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      ch = '_';
    }
  }
  return tag;
}

bool MetalCommandProcessor::LoadPipelineDiskCache(
    const std::filesystem::path& path,
    std::vector<PipelineDiskCacheEntry>* entries) {
  if (!entries) {
    return false;
  }
  entries->clear();
  pipeline_disk_cache_keys_.clear();

  pipeline_disk_cache_file_ = xe::filesystem::OpenFile(path, "a+b");
  if (!pipeline_disk_cache_file_) {
    XELOGW("Metal pipeline disk cache: Failed to open {}", path.string());
    return false;
  }

  PipelineDiskCacheHeader header = {};
  if (std::fread(&header, sizeof(header), 1, pipeline_disk_cache_file_) != 1 ||
      header.magic != kPipelineDiskCacheMagic ||
      header.version != kPipelineDiskCacheVersion) {
    header.magic = kPipelineDiskCacheMagic;
    header.version = kPipelineDiskCacheVersion;
    header.reserved[0] = 0;
    header.reserved[1] = 0;
    xe::filesystem::Seek(pipeline_disk_cache_file_, 0, SEEK_SET);
    std::fwrite(&header, sizeof(header), 1, pipeline_disk_cache_file_);
    std::fflush(pipeline_disk_cache_file_);
    xe::filesystem::Seek(pipeline_disk_cache_file_, 0, SEEK_END);
    return true;
  }

  while (true) {
    PipelineDiskCacheEntryHeader entry_header = {};
    if (std::fread(&entry_header, sizeof(entry_header), 1,
                   pipeline_disk_cache_file_) != 1) {
      break;
    }

    if (entry_header.entry_size < sizeof(PipelineDiskCacheEntryBase) ||
        entry_header.entry_size > kPipelineDiskCacheMaxEntrySize) {
      break;
    }

    PipelineDiskCacheEntryBase base = {};
    if (std::fread(&base, sizeof(base), 1, pipeline_disk_cache_file_) != 1) {
      break;
    }

    size_t expected_size = sizeof(PipelineDiskCacheEntryBase) +
                           size_t(base.vertex_attribute_count) *
                               sizeof(PipelineDiskCacheVertexAttribute) +
                           size_t(base.vertex_layout_count) *
                               sizeof(PipelineDiskCacheVertexLayout);
    if (entry_header.entry_size != expected_size) {
      xe::filesystem::Seek(
          pipeline_disk_cache_file_,
          entry_header.entry_size - sizeof(PipelineDiskCacheEntryBase),
          SEEK_CUR);
      continue;
    }

    PipelineDiskCacheEntry entry = {};
    entry.pipeline_key = base.pipeline_key;
    entry.vertex_shader_cache_key = base.vertex_shader_cache_key;
    entry.pixel_shader_cache_key = base.pixel_shader_cache_key;
    entry.sample_count = base.sample_count;
    entry.depth_format = base.depth_format;
    entry.stencil_format = base.stencil_format;
    std::memcpy(entry.color_formats, base.color_formats,
                sizeof(base.color_formats));
    entry.normalized_color_mask = base.normalized_color_mask;
    entry.alpha_to_mask_enable = base.alpha_to_mask_enable;
    std::memcpy(entry.blendcontrol, base.blendcontrol,
                sizeof(base.blendcontrol));

    entry.vertex_attributes.resize(base.vertex_attribute_count);
    if (base.vertex_attribute_count) {
      if (std::fread(entry.vertex_attributes.data(),
                     sizeof(PipelineDiskCacheVertexAttribute),
                     base.vertex_attribute_count, pipeline_disk_cache_file_) !=
          base.vertex_attribute_count) {
        break;
      }
    }
    entry.vertex_layouts.resize(base.vertex_layout_count);
    if (base.vertex_layout_count) {
      if (std::fread(entry.vertex_layouts.data(),
                     sizeof(PipelineDiskCacheVertexLayout),
                     base.vertex_layout_count,
                     pipeline_disk_cache_file_) != base.vertex_layout_count) {
        break;
      }
    }

    entries->push_back(std::move(entry));
    pipeline_disk_cache_keys_.insert(base.pipeline_key);
  }

  xe::filesystem::Seek(pipeline_disk_cache_file_, 0, SEEK_END);
  return true;
}

bool MetalCommandProcessor::AppendPipelineDiskCacheEntry(
    const PipelineDiskCacheEntry& entry) {
  if (!pipeline_disk_cache_file_) {
    return false;
  }
  if (!pipeline_disk_cache_keys_.insert(entry.pipeline_key).second) {
    return false;
  }

  PipelineDiskCacheEntryBase base = {};
  base.pipeline_key = entry.pipeline_key;
  base.vertex_shader_cache_key = entry.vertex_shader_cache_key;
  base.pixel_shader_cache_key = entry.pixel_shader_cache_key;
  base.sample_count = entry.sample_count;
  base.depth_format = entry.depth_format;
  base.stencil_format = entry.stencil_format;
  std::memcpy(base.color_formats, entry.color_formats,
              sizeof(base.color_formats));
  base.normalized_color_mask = entry.normalized_color_mask;
  base.alpha_to_mask_enable = entry.alpha_to_mask_enable;
  std::memcpy(base.blendcontrol, entry.blendcontrol, sizeof(base.blendcontrol));
  base.vertex_attribute_count =
      static_cast<uint32_t>(entry.vertex_attributes.size());
  base.vertex_layout_count = static_cast<uint32_t>(entry.vertex_layouts.size());

  size_t entry_size =
      sizeof(PipelineDiskCacheEntryBase) +
      entry.vertex_attributes.size() *
          sizeof(PipelineDiskCacheVertexAttribute) +
      entry.vertex_layouts.size() * sizeof(PipelineDiskCacheVertexLayout);
  if (entry_size > kPipelineDiskCacheMaxEntrySize) {
    return false;
  }

  PipelineDiskCacheEntryHeader entry_header = {};
  entry_header.entry_size = static_cast<uint32_t>(entry_size);

  std::fwrite(&entry_header, sizeof(entry_header), 1,
              pipeline_disk_cache_file_);
  std::fwrite(&base, sizeof(base), 1, pipeline_disk_cache_file_);
  if (!entry.vertex_attributes.empty()) {
    std::fwrite(entry.vertex_attributes.data(),
                sizeof(PipelineDiskCacheVertexAttribute),
                entry.vertex_attributes.size(), pipeline_disk_cache_file_);
  }
  if (!entry.vertex_layouts.empty()) {
    std::fwrite(entry.vertex_layouts.data(),
                sizeof(PipelineDiskCacheVertexLayout),
                entry.vertex_layouts.size(), pipeline_disk_cache_file_);
  }
  std::fflush(pipeline_disk_cache_file_);
  pipeline_disk_cache_entries_.push_back(entry);
  return true;
}

bool MetalCommandProcessor::InitializePipelineBinaryArchive(
    const std::filesystem::path& archive_path) {
  if (!device_) {
    return false;
  }
  if (pipeline_binary_archive_) {
    pipeline_binary_archive_->release();
    pipeline_binary_archive_ = nullptr;
  }

  MTL::BinaryArchiveDescriptor* desc =
      MTL::BinaryArchiveDescriptor::alloc()->init();
  NS::String* path_string =
      NS::String::string(archive_path.string().c_str(), NS::UTF8StringEncoding);
  NS::URL* url = NS::URL::fileURLWithPath(path_string);
  desc->setUrl(url);

  NS::Error* error = nullptr;
  pipeline_binary_archive_ = device_->newBinaryArchive(desc, &error);
  desc->release();
  if (!pipeline_binary_archive_) {
    if (error) {
      XELOGW("Metal binary archive init failed: {}",
             error->localizedDescription()->utf8String());
    }
    return false;
  }
  pipeline_binary_archive_path_ = archive_path;
  pipeline_binary_archive_dirty_ = false;
  return true;
}

void MetalCommandProcessor::SerializePipelineBinaryArchive() {
  if (!pipeline_binary_archive_ || !pipeline_binary_archive_dirty_) {
    return;
  }
  NS::String* path_string = NS::String::string(
      pipeline_binary_archive_path_.string().c_str(), NS::UTF8StringEncoding);
  NS::URL* url = NS::URL::fileURLWithPath(path_string);
  NS::Error* error = nullptr;
  if (!pipeline_binary_archive_->serializeToURL(url, &error)) {
    if (error) {
      XELOGW("Metal binary archive serialize failed: {}",
             error->localizedDescription()->utf8String());
    }
  }
  pipeline_binary_archive_dirty_ = false;
}

void MetalCommandProcessor::PrewarmPipelineBinaryArchive(
    const std::vector<PipelineDiskCacheEntry>& entries) {
  if (!pipeline_binary_archive_ || entries.empty()) {
    return;
  }
  if (!g_metal_shader_cache || !g_metal_shader_cache->IsInitialized()) {
    return;
  }

  size_t prewarmed = 0;
  for (const auto& entry : entries) {
    MetalShaderCache::CachedMetallib vs_cached;
    if (!g_metal_shader_cache->Load(entry.vertex_shader_cache_key,
                                    &vs_cached)) {
      continue;
    }

    NS::Error* error = nullptr;
    dispatch_data_t vs_data = dispatch_data_create(
        vs_cached.metallib_data.data(), vs_cached.metallib_data.size(), nullptr,
        DISPATCH_DATA_DESTRUCTOR_NONE);
    MTL::Library* vs_library = device_->newLibrary(vs_data, &error);
    dispatch_release(vs_data);
    if (!vs_library) {
      continue;
    }
    NS::String* vs_name = NS::String::string(vs_cached.function_name.c_str(),
                                             NS::UTF8StringEncoding);
    MTL::Function* vs_function = vs_library->newFunction(vs_name);
    if (!vs_function) {
      vs_library->release();
      continue;
    }

    MTL::Library* ps_library = nullptr;
    MTL::Function* ps_function = nullptr;
    if (entry.pixel_shader_cache_key) {
      MetalShaderCache::CachedMetallib ps_cached;
      if (g_metal_shader_cache->Load(entry.pixel_shader_cache_key,
                                     &ps_cached)) {
        dispatch_data_t ps_data = dispatch_data_create(
            ps_cached.metallib_data.data(), ps_cached.metallib_data.size(),
            nullptr, DISPATCH_DATA_DESTRUCTOR_NONE);
        ps_library = device_->newLibrary(ps_data, &error);
        dispatch_release(ps_data);
        if (ps_library) {
          NS::String* ps_name = NS::String::string(
              ps_cached.function_name.c_str(), NS::UTF8StringEncoding);
          ps_function = ps_library->newFunction(ps_name);
        }
      }
    }

    MTL::RenderPipelineDescriptor* desc =
        MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs_function);
    if (ps_function) {
      desc->setFragmentFunction(ps_function);
    }

    for (uint32_t i = 0; i < 4; ++i) {
      desc->colorAttachments()->object(i)->setPixelFormat(
          static_cast<MTL::PixelFormat>(entry.color_formats[i]));
    }
    desc->setDepthAttachmentPixelFormat(
        static_cast<MTL::PixelFormat>(entry.depth_format));
    desc->setStencilAttachmentPixelFormat(
        static_cast<MTL::PixelFormat>(entry.stencil_format));
    desc->setSampleCount(entry.sample_count);
    desc->setAlphaToCoverageEnabled(entry.alpha_to_mask_enable != 0);

    for (uint32_t i = 0; i < 4; ++i) {
      auto* color_attachment = desc->colorAttachments()->object(i);
      if (entry.color_formats[i] ==
          static_cast<uint32_t>(MTL::PixelFormatInvalid)) {
        color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
        color_attachment->setBlendingEnabled(false);
        continue;
      }
      uint32_t rt_write_mask = (entry.normalized_color_mask >> (i * 4)) & 0xF;
      color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
      if (!rt_write_mask) {
        color_attachment->setBlendingEnabled(false);
        continue;
      }

      reg::RB_BLENDCONTROL blendcontrol = {};
      blendcontrol.value = entry.blendcontrol[i];
      MTL::BlendFactor src_rgb =
          ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
      MTL::BlendFactor dst_rgb =
          ToMetalBlendFactorRgb(blendcontrol.color_destblend);
      MTL::BlendOperation op_rgb =
          ToMetalBlendOperation(blendcontrol.color_comb_fcn);
      MTL::BlendFactor src_alpha =
          ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
      MTL::BlendFactor dst_alpha =
          ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
      MTL::BlendOperation op_alpha =
          ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

      bool blending_enabled = src_rgb != MTL::BlendFactorOne ||
                              dst_rgb != MTL::BlendFactorZero ||
                              op_rgb != MTL::BlendOperationAdd ||
                              src_alpha != MTL::BlendFactorOne ||
                              dst_alpha != MTL::BlendFactorZero ||
                              op_alpha != MTL::BlendOperationAdd;
      color_attachment->setBlendingEnabled(blending_enabled);
      if (blending_enabled) {
        color_attachment->setSourceRGBBlendFactor(src_rgb);
        color_attachment->setDestinationRGBBlendFactor(dst_rgb);
        color_attachment->setRgbBlendOperation(op_rgb);
        color_attachment->setSourceAlphaBlendFactor(src_alpha);
        color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
        color_attachment->setAlphaBlendOperation(op_alpha);
      }
    }

    if (!entry.vertex_attributes.empty() || !entry.vertex_layouts.empty()) {
      MTL::VertexDescriptor* vertex_desc =
          MTL::VertexDescriptor::alloc()->init();
      for (const auto& attr : entry.vertex_attributes) {
        auto* attr_desc =
            vertex_desc->attributes()->object(attr.attribute_index);
        attr_desc->setFormat(static_cast<MTL::VertexFormat>(attr.format));
        attr_desc->setOffset(attr.offset);
        attr_desc->setBufferIndex(attr.buffer_index);
      }
      for (const auto& layout : entry.vertex_layouts) {
        auto* layout_desc = vertex_desc->layouts()->object(layout.buffer_index);
        layout_desc->setStride(layout.stride);
        layout_desc->setStepFunction(
            static_cast<MTL::VertexStepFunction>(layout.step_function));
        layout_desc->setStepRate(layout.step_rate);
      }
      desc->setVertexDescriptor(vertex_desc);
      vertex_desc->release();
    }

    NS::Array* archives = NS::Array::array(pipeline_binary_archive_);
    desc->setBinaryArchives(archives);
    if (pipeline_binary_archive_->addRenderPipelineFunctions(desc, &error)) {
      pipeline_binary_archive_dirty_ = true;
      ++prewarmed;
    }
    desc->release();
    vs_function->release();
    vs_library->release();
    if (ps_function) {
      ps_function->release();
    }
    if (ps_library) {
      ps_library->release();
    }
  }
}

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ProcessCompletedSubmissions();
  saw_swap_ = true;
  last_swap_ptr_ = frontbuffer_ptr;
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;
  EndSubmission(true);

  // Push the rendered frame to the presenter's guest output mailbox
  // This is required for trace dumps to capture the output. Use the
  // MetalRenderTargetCache color target (like D3D12) rather than the
  // legacy standalone render_target_texture_.
  auto* presenter =
      static_cast<ui::metal::MetalPresenter*>(graphics_system_->presenter());
  if (presenter && render_target_cache_) {
    uint32_t output_width =
        frontbuffer_width ? frontbuffer_width : render_target_width_;
    uint32_t output_height =
        frontbuffer_height ? frontbuffer_height : render_target_height_;

    MTL::Texture* source_texture = nullptr;
    bool use_pwl_gamma_ramp = false;
    if (texture_cache_) {
      uint32_t swap_width = 0;
      uint32_t swap_height = 0;
      xenos::TextureFormat swap_format = xenos::TextureFormat::k_8_8_8_8;
      source_texture = texture_cache_->RequestSwapTexture(
          swap_width, swap_height, swap_format);
      if (source_texture) {
        output_width = swap_width;
        output_height = swap_height;
        use_pwl_gamma_ramp =
            swap_format == xenos::TextureFormat::k_2_10_10_10 ||
            swap_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        static MTL::PixelFormat last_format = MTL::PixelFormatInvalid;
        static uint32_t last_samples = 0;
        static uint32_t last_width = 0;
        static uint32_t last_height = 0;
        static int last_swap_format = -1;
        MTL::PixelFormat src_format = source_texture->pixelFormat();
        uint32_t src_samples = source_texture->sampleCount();
        uint32_t src_width = uint32_t(source_texture->width());
        uint32_t src_height = uint32_t(source_texture->height());
        int swap_format_int = static_cast<int>(swap_format);
        if (src_format != last_format || src_samples != last_samples ||
            src_width != last_width || src_height != last_height ||
            swap_format_int != last_swap_format) {
          last_format = src_format;
          last_samples = src_samples;
          last_width = src_width;
          last_height = src_height;
          last_swap_format = swap_format_int;
        }
        if (presenter) {
          if (!gamma_ramp_256_entry_table_up_to_date_ ||
              !gamma_ramp_pwl_up_to_date_) {
            constexpr size_t kGammaRampTableBytes =
                sizeof(reg::DC_LUT_30_COLOR) * 256;
            constexpr size_t kGammaRampPwlBytes =
                sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3;
            if (presenter->UpdateGammaRamp(
                    gamma_ramp_256_entry_table(), kGammaRampTableBytes,
                    gamma_ramp_pwl_rgb(), kGammaRampPwlBytes)) {
              gamma_ramp_256_entry_table_up_to_date_ = true;
              gamma_ramp_pwl_up_to_date_ = true;
            } else {
              XELOGW("Metal IssueSwap: gamma ramp upload failed");
            }
          }
        }
      }
    }

    bool swap_dest_swap = false;
    const bool has_swap_dest_swap =
        ConsumeSwapDestSwap(frontbuffer_ptr, &swap_dest_swap);
    if (!has_swap_dest_swap && frontbuffer_ptr) {
      static uint32_t swap_dest_miss_count = 0;
      if (swap_dest_miss_count < 8) {
        ++swap_dest_miss_count;
      }
    }
    bool force_swap_rb = has_swap_dest_swap && swap_dest_swap;

    if (!source_texture) {
      static bool missing_swap_logged = false;
      if (!missing_swap_logged) {
        missing_swap_logged = true;
        XELOGW(
            "MetalCommandProcessor::IssueSwap: swap texture unavailable; "
            "presenting inactive (black) output");
      }
      presenter->RefreshGuestOutput(
          0, 0, 0, 0, [](ui::Presenter::GuestOutputRefreshContext&) -> bool {
            return false;
          });
      return;
    }

    if (source_texture) {
      ui::metal::MetalPresenter* metal_presenter = presenter;
      uint32_t source_width = output_width;
      uint32_t source_height = output_height;
      bool force_swap_rb_copy = force_swap_rb;
      bool use_pwl_gamma_ramp_copy = use_pwl_gamma_ramp;
      auto aspect = graphics_system_->GetScaledAspectRatio();
      presenter->RefreshGuestOutput(
          output_width, output_height, aspect.first, aspect.second,
          [source_texture, metal_presenter, source_width, source_height,
           force_swap_rb_copy, use_pwl_gamma_ramp_copy](
              ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& metal_context =
                static_cast<ui::metal::MetalGuestOutputRefreshContext&>(
                    context);
            context.SetIs8bpc(!use_pwl_gamma_ramp_copy);
            uint64_t submission_id = 0;
            bool copy_ok = metal_presenter->CopyTextureToGuestOutput(
                source_texture, metal_context.resource_uav_capable(),
                source_width, source_height, force_swap_rb_copy,
                use_pwl_gamma_ramp_copy, &submission_id);
            if (submission_id) {
              metal_context.SetSubmissionId(submission_id);
            }
            return copy_ok;
          });
    }
  }

  StopCaptureIfActive();
}

void MetalCommandProcessor::OnPrimaryBufferEnd() {
  if (cvars::submit_on_primary_buffer_end && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          uint32_t guest_address,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  // Create hash for caching using XXH3 (same as D3D12)
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));

  // Check cache
  auto it = shader_cache_.find(hash);
  if (it != shader_cache_.end()) {
    return it->second.get();
  }

  // Create new shader - analysis and translation happen later when the shader
  // is actually used in a draw call (matching D3D12 pattern)
  auto shader = std::make_unique<MetalShader>(shader_type, hash, host_address,
                                              dword_count);

  MetalShader* result = shader.get();
  shader_cache_[hash] = std::move(shader);

  XELOGD("Loaded {} shader at {:08X} ({} dwords, hash {:016X})",
         shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
         guest_address, dword_count, hash);

  return result;
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  const RegisterFile& regs = *register_file_;
  uint32_t normalized_color_mask = 0;

  if (!BeginSubmission(true)) {
    return false;
  }

  // Check for copy mode
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  // Vertex shader analysis.
  auto vertex_shader = static_cast<MetalShader*>(active_vertex_shader());
  if (!vertex_shader) {
    XELOGW("IssueDraw: No vertex shader");
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  MetalShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<MetalShader*>(active_pixel_shader());
      if (pixel_shader) {
        if (!pixel_shader->is_ucode_analyzed()) {
          pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
        }
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  // Primitive/index processing (like D3D12/Vulkan).
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_) {
    XELOGE("IssueDraw: primitive processor is not initialized");
    return false;
  }
  if (!primitive_processor_->Process(primitive_processing_result)) {
    XELOGE("IssueDraw: primitive processing failed");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kMemExportCompute) {
    primitive_processing_result.host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  }

  bool use_tessellation_emulation = false;
  if (primitive_processing_result.IsTessellated()) {
    if (!mesh_shader_supported_) {
      static bool tess_mesh_logged = false;
      if (!tess_mesh_logged) {
        tess_mesh_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested but mesh shaders are not "
            "supported on this device");
      }
      return true;
    }
    if (!pixel_shader) {
      static bool tess_no_ps_logged = false;
      if (!tess_no_ps_logged) {
        tess_no_ps_logged = true;
        XELOGW(
            "Metal: Tessellation emulation requested without a pixel shader; "
            "using depth-only PS fallback");
      }
    }
    use_tessellation_emulation = true;
  }

  // Configure render targets via MetalRenderTargetCache, similar to D3D12.
  if (render_target_cache_) {
    auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
    uint32_t ps_writes_color_targets =
        pixel_shader ? pixel_shader->writes_color_targets() : 0;
    normalized_color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(
                                               regs, ps_writes_color_targets)
                                         : 0;
    if (!render_target_cache_->Update(is_rasterization_done,
                                      normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      XELOGE(
          "MetalCommandProcessor::IssueDraw - RenderTargetCache::Update "
          "failed");
      return false;
    }
  }

  // Begin command buffer if needed (will use cache-provided render targets).
  BeginCommandBuffer();
  EnsureDrawRingCapacity();

  MTL::RenderPipelineState* pipeline = nullptr;
  // Select per-draw shader modifications (mirrors D3D12 PipelineCache).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (pixel_shader) {
    interpolator_mask = vertex_shader->writes_interpolators() &
                        pixel_shader->GetInterpolatorInputMask(
                            regs.Get<reg::SQ_PROGRAM_CNTL>(),
                            regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);
  }

  auto normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  Shader::HostVertexShaderType host_vertex_shader_type_for_translation =
      primitive_processing_result.host_vertex_shader_type;
  if (host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      host_vertex_shader_type_for_translation ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    if (!mesh_shader_supported_) {
      static bool host_vs_expansion_logged = false;
      if (!host_vs_expansion_logged) {
        host_vs_expansion_logged = true;
        XELOGW(
            "Metal: Host VS expansion requested without mesh shader support; "
            "skipping draw");
      }
      return true;
    }
    // Geometry emulation handles point/rectangle expansion; use the normal
    // vertex shader translation path to avoid unsupported host VS types.
    host_vertex_shader_type_for_translation =
        Shader::HostVertexShaderType::kVertex;
  }
  DxbcShaderTranslator::Modification vertex_shader_modification =
      GetCurrentVertexShaderModification(
          *vertex_shader, host_vertex_shader_type_for_translation,
          interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader ? GetCurrentPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control)
                   : DxbcShaderTranslator::Modification(0);

  PipelineGeometryShader geometry_shader_type = PipelineGeometryShader::kNone;
  if (!primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_shader_type = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        geometry_shader_type = PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_shader_type = PipelineGeometryShader::kQuadList;
        break;
      default:
        break;
    }
  }

  GeometryShaderKey geometry_shader_key;
  bool use_geometry_emulation = false;
  if (geometry_shader_type != PipelineGeometryShader::kNone) {
    bool can_build_geometry_shader =
        pixel_shader || !vertex_shader_modification.vertex.interpolator_mask;
    if (!can_build_geometry_shader) {
      static bool geom_interp_mismatch_logged = false;
      if (!geom_interp_mismatch_logged) {
        geom_interp_mismatch_logged = true;
        XELOGW(
            "Metal: geometry emulation skipped because pixel shader is null "
            "but vertex interpolators are present");
      }
    } else {
      use_geometry_emulation =
          GetGeometryShaderKey(geometry_shader_type, vertex_shader_modification,
                               pixel_shader_modification, geometry_shader_key);
    }
  }
  if (use_geometry_emulation && !mesh_shader_supported_) {
    static bool mesh_support_logged = false;
    if (!mesh_support_logged) {
      mesh_support_logged = true;
      XELOGW(
          "Metal: geometry emulation requested but mesh shaders are not "
          "supported on this device");
    }
    use_geometry_emulation = false;
  }
  if (use_geometry_emulation && !pixel_shader) {
    static bool geom_no_ps_logged = false;
    if (!geom_no_ps_logged) {
      geom_no_ps_logged = true;
      XELOGW(
          "Metal: geometry emulation requested without a pixel shader; using "
          "depth-only PS fallback");
    }
  }

  // Get or create shader translations for the selected modifications.
  auto vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  if (!vertex_translation->is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(*vertex_translation)) {
      XELOGE("Failed to translate vertex shader to DXBC");
      return false;
    }
  }
  if (!use_tessellation_emulation && !vertex_translation->is_valid()) {
    if (!vertex_translation->TranslateToMetal(device_, *dxbc_to_dxil_converter_,
                                              *metal_shader_converter_)) {
      XELOGE("Failed to translate vertex shader to Metal");
      return false;
    }
  }

  MetalShader::MetalTranslation* pixel_translation = nullptr;
  if (pixel_shader) {
    pixel_translation = static_cast<MetalShader::MetalTranslation*>(
        pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value));
    if (!pixel_translation->is_translated()) {
      if (!shader_translator_->TranslateAnalyzedShader(*pixel_translation)) {
        XELOGE("Failed to translate pixel shader to DXBC");
        return false;
      }
    }
    if (!pixel_translation->is_valid()) {
      if (!pixel_translation->TranslateToMetal(
              device_, *dxbc_to_dxil_converter_, *metal_shader_converter_)) {
        XELOGE("Failed to translate pixel shader to Metal");
        return false;
      }
    }
  }

  TessellationPipelineState* tessellation_pipeline_state = nullptr;
  GeometryPipelineState* geometry_pipeline_state = nullptr;
  if (use_tessellation_emulation) {
    tessellation_pipeline_state = GetOrCreateTessellationPipelineState(
        vertex_translation, pixel_translation, primitive_processing_result,
        regs);
    pipeline = tessellation_pipeline_state
                   ? tessellation_pipeline_state->pipeline
                   : nullptr;
  } else if (use_geometry_emulation) {
    geometry_pipeline_state = GetOrCreateGeometryPipelineState(
        vertex_translation, pixel_translation, geometry_shader_key, regs);
    pipeline =
        geometry_pipeline_state ? geometry_pipeline_state->pipeline : nullptr;
  } else {
    pipeline =
        GetOrCreatePipelineState(vertex_translation, pixel_translation, regs);
  }

  if (!pipeline) {
    XELOGE("Failed to create pipeline state");
    return false;
  }

  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (pixel_shader) {
    used_texture_mask |= pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  if (texture_cache_ && used_texture_mask) {
    texture_cache_->RequestTextures(used_texture_mask);
  }

  struct VertexBindingRange {
    uint32_t binding_index = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t stride = 0;
  };
  std::array<VertexBindingRange, 32> vertex_ranges;
  uint32_t vertex_range_count = 0;
  const auto& vb_bindings = vertex_shader->vertex_bindings();
  bool uses_vertex_fetch = ShaderUsesVertexFetch(*vertex_shader);

  // Sync shared memory before drawing - ensure GPU has latest data
  // This is particularly important for trace playback where memory is
  // written incrementally
  if (shared_memory_) {
    const Shader::ConstantRegisterMap& constant_map_vertex =
        vertex_shader->constant_register_map();
    for (uint32_t i = 0;
         i < xe::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining &= ~(uint32_t(1) << j);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
        switch (vfetch.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (::cvars::gpu_allow_invalid_fetch_constants) {
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type. "
                "Use --gpu_allow_invalid_fetch_constants to bypass.",
                vfetch_index, vfetch.dword_0, vfetch.dword_1);
            return false;
          default:
            XELOGW("Vertex fetch constant {} ({:08X} {:08X}) is invalid.",
                   vfetch_index, vfetch.dword_0, vfetch.dword_1);
            return false;
        }
        uint32_t buffer_offset = vfetch.address << 2;
        uint32_t buffer_length = vfetch.size << 2;
        if (buffer_offset > SharedMemory::kBufferSize ||
            SharedMemory::kBufferSize - buffer_offset < buffer_length) {
          XELOGW(
              "Vertex fetch constant {} out of range (offset=0x{:08X} size={})",
              vfetch_index, buffer_offset, buffer_length);
          return false;
        }
        if (!shared_memory_->RequestRange(buffer_offset, buffer_length)) {
          XELOGE(
              "Failed to request vertex buffer at 0x{:08X} (size {}) in shared "
              "memory",
              buffer_offset, buffer_length);
          return false;
        }
      }
    }

    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      uint32_t base_bytes = memexport_range.base_address_dwords << 2;
      if (!shared_memory_->RequestRange(base_bytes,
                                        memexport_range.size_bytes)) {
        XELOGE(
            "Failed to request memexport stream at 0x{:08X} (size {}) in "
            "shared "
            "memory",
            base_bytes, memexport_range.size_bytes);
        return false;
      }
    }

    for (const auto& binding : vb_bindings) {
      xenos::xe_gpu_vertex_fetch_t vfetch =
          regs.GetVertexFetch(binding.fetch_constant);
      uint32_t buffer_offset = vfetch.address << 2;
      uint32_t buffer_length = vfetch.size << 2;
      VertexBindingRange range;
      range.binding_index = static_cast<uint32_t>(binding.binding_index);
      range.offset = buffer_offset;
      range.length = buffer_length;
      range.stride = binding.stride_words * 4;
      assert_true(vertex_range_count < vertex_ranges.size());
      vertex_ranges[vertex_range_count++] = range;
    }
  }

  // Set pipeline state on encoder
  current_render_encoder_->setRenderPipelineState(pipeline);
  if (use_tessellation_emulation) {
    if (!tessellator_tables_buffer_) {
      XELOGE("Tessellation emulation requires tessellator tables buffer");
      return false;
    }
    current_render_encoder_->setObjectBuffer(
        tessellator_tables_buffer_, 0, kIRRuntimeTessellatorTablesBindPoint);
    current_render_encoder_->setMeshBuffer(
        tessellator_tables_buffer_, 0, kIRRuntimeTessellatorTablesBindPoint);
    UseRenderEncoderResource(tessellator_tables_buffer_,
                             MTL::ResourceUsageRead);
  }

  // Determine if shared memory should be UAV (for memexport).
  bool shared_memory_is_uav = memexport_used_vertex || memexport_used_pixel;
  MTL::ResourceUsage shared_memory_usage =
      shared_memory_is_uav ? (MTL::ResourceUsageRead | MTL::ResourceUsageWrite)
                           : MTL::ResourceUsageRead;
  // Bind IR Converter runtime resources.
  // The Metal Shader Converter expects resources at specific bind points.
  if (res_heap_ab_ && smp_heap_ab_ && uniforms_buffer_ && shared_memory_) {
    // Determine primitive type characteristics
    bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);

    // Get viewport info for NDC transform. Use the actual RT0 dimensions
    // when available so system constants match the current render target.
    uint32_t vp_width = render_target_width_;
    uint32_t vp_height = render_target_height_;
    if (render_target_cache_) {
      MTL::Texture* rt0_tex = render_target_cache_->GetColorTarget(0);
      if (rt0_tex) {
        vp_width = rt0_tex->width();
        vp_height = rt0_tex->height();
      } else if (MTL::Texture* depth_tex =
                     render_target_cache_->GetDepthTarget()) {
        vp_width = depth_tex->width();
        vp_height = depth_tex->height();
      } else if (MTL::Texture* dummy =
                     render_target_cache_->GetDummyColorTarget()) {
        vp_width = dummy->width();
        vp_height = dummy->height();
      }
    }
    draw_util::ViewportInfo viewport_info;
    auto depth_control = draw_util::GetNormalizedDepthControl(regs);
    constexpr uint32_t kViewportBoundsMax = 32767;
    bool host_render_targets_used = true;
    bool convert_z_to_float24 = host_render_targets_used &&
                                ::cvars::depth_float24_convert_in_pixel_shader;
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    draw_util::GetViewportInfoArgs gviargs{};
    gviargs.Setup(
        draw_resolution_scale_x, draw_resolution_scale_y,
        texture_cache_ ? texture_cache_->draw_resolution_scale_x_divisor()
                       : divisors::MagicDiv(1),
        texture_cache_ ? texture_cache_->draw_resolution_scale_y_divisor()
                       : divisors::MagicDiv(1),
        true, kViewportBoundsMax, kViewportBoundsMax, false, depth_control,
        convert_z_to_float24, host_render_targets_used,
        pixel_shader && pixel_shader->writes_depth());
    gviargs.SetupRegisterValues(regs);
    draw_util::GetHostViewportInfo(&gviargs, viewport_info);

    // Apply per-draw viewport and scissor so the Metal viewport
    // matches the guest viewport computed by draw_util.
    draw_util::Scissor scissor;
    draw_util::GetScissor(regs, scissor);
    // draw_resolution_scale_x/y already computed above for viewport.
    scissor.offset[0] *= draw_resolution_scale_x;
    scissor.offset[1] *= draw_resolution_scale_y;
    scissor.extent[0] *= draw_resolution_scale_x;
    scissor.extent[1] *= draw_resolution_scale_y;

    // Clamp scissor to actual render target bounds (Metal requires this).
    if (scissor.offset[0] + scissor.extent[0] > vp_width) {
      scissor.extent[0] =
          (scissor.offset[0] < vp_width) ? (vp_width - scissor.offset[0]) : 0;
    }
    if (scissor.offset[1] + scissor.extent[1] > vp_height) {
      scissor.extent[1] =
          (scissor.offset[1] < vp_height) ? (vp_height - scissor.offset[1]) : 0;
    }

    MTL::Viewport mtl_viewport;
    mtl_viewport.originX = static_cast<double>(viewport_info.xy_offset[0]);
    mtl_viewport.originY = static_cast<double>(viewport_info.xy_offset[1]);
    mtl_viewport.width = static_cast<double>(viewport_info.xy_extent[0]);
    mtl_viewport.height = static_cast<double>(viewport_info.xy_extent[1]);
    mtl_viewport.znear = viewport_info.z_min;
    mtl_viewport.zfar = viewport_info.z_max;
    current_render_encoder_->setViewport(mtl_viewport);

    MTL::ScissorRect mtl_scissor;
    mtl_scissor.x = scissor.offset[0];
    mtl_scissor.y = scissor.offset[1];
    mtl_scissor.width = scissor.extent[0];
    mtl_scissor.height = scissor.extent[1];
    current_render_encoder_->setScissorRect(mtl_scissor);

    ApplyRasterizerState(primitive_polygonal);

    // Fixed-function depth/stencil state is not part of the pipeline state in
    // Metal, so update it per draw.
    ApplyDepthStencilState(primitive_polygonal, depth_control);

    // Update full system constants from GPU registers
    // This populates flags, NDC transform, alpha test, blend constants, etc.
    uint32_t normalized_color_mask =
        pixel_shader ? draw_util::GetNormalizedColorMask(
                           regs, pixel_shader->writes_color_targets())
                     : 0;
    UpdateSystemConstantValues(
        shared_memory_is_uav, primitive_polygonal,
        primitive_processing_result.line_loop_closing_index,
        primitive_processing_result.host_shader_index_endian, viewport_info,
        used_texture_mask, depth_control, normalized_color_mask);

    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    bool blend_factor_update_needed =
        !ff_blend_factor_valid_ ||
        std::memcmp(ff_blend_factor_, blend_constants, sizeof(float) * 4) != 0;
    if (blend_factor_update_needed) {
      std::memcpy(ff_blend_factor_, blend_constants, sizeof(float) * 4);
      ff_blend_factor_valid_ = true;
      current_render_encoder_->setBlendColor(
          blend_constants[0], blend_constants[1], blend_constants[2],
          blend_constants[3]);
    }

    constexpr size_t kStageVertex = 0;
    constexpr size_t kStagePixel = 1;
    uint32_t ring_index = current_draw_index_ % uint32_t(draw_ring_count_);
    size_t table_index_vertex = size_t(ring_index) * kStageCount + kStageVertex;
    size_t table_index_pixel = size_t(ring_index) * kStageCount + kStagePixel;

    // Uniforms buffer layout (4KB per CBV for alignment):
    //   b0 (offset 0):     System constants (~512 bytes)
    //   b1 (offset 4096):  Float constants (256 float4s = 4KB)
    //   b2 (offset 8192):  Bool/loop constants (~256 bytes)
    //   b3 (offset 12288): Fetch constants (768 bytes)
    //   b4 (offset 16384): Descriptor indices (unused in bindful mode)
    const size_t kCBVSize = kCbvSizeBytes;
    uint8_t* uniforms_base =
        static_cast<uint8_t*>(uniforms_buffer_->contents());
    uint8_t* uniforms_vertex =
        uniforms_base + table_index_vertex * kUniformsBytesPerTable;
    uint8_t* uniforms_pixel =
        uniforms_base + table_index_pixel * kUniformsBytesPerTable;

    // b0: System constants.
    std::memcpy(uniforms_vertex, &system_constants_,
                sizeof(DxbcShaderTranslator::SystemConstants));
    std::memcpy(uniforms_pixel, &system_constants_,
                sizeof(DxbcShaderTranslator::SystemConstants));

    // b1: Float constants at offset 4096 (1 * kCBVSize).
    const size_t kFloatConstantOffset = 1 * kCBVSize;
    // DxbcShaderTranslator uses packed float constants, mirroring the D3D12
    // backend behavior: only the constants actually used by the shader are
    // written sequentially based on Shader::ConstantRegisterMap.
    auto write_packed_float_constants = [&](uint8_t* dst, const Shader& shader,
                                            uint32_t regs_base) {
      std::memset(dst, 0, kCBVSize);
      const Shader::ConstantRegisterMap& map = shader.constant_register_map();
      if (!map.float_count) {
        return;
      }
      uint8_t* out = dst;
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        uint32_t constant_index;
        while (xe::bit_scan_forward(bits, &constant_index)) {
          bits &= ~(uint64_t(1) << constant_index);
          if (out + 4 * sizeof(uint32_t) > dst + kCBVSize) {
            return;
          }
          std::memcpy(
              out, &regs.values[regs_base + (i << 8) + (constant_index << 2)],
              4 * sizeof(uint32_t));
          out += 4 * sizeof(uint32_t);
        }
      }
    };

    // Vertex shader uses c0-c255, pixel shader uses c256-c511.
    write_packed_float_constants(uniforms_vertex + kFloatConstantOffset,
                                 *vertex_shader,
                                 XE_GPU_REG_SHADER_CONSTANT_000_X);
    if (pixel_shader) {
      write_packed_float_constants(uniforms_pixel + kFloatConstantOffset,
                                   *pixel_shader,
                                   XE_GPU_REG_SHADER_CONSTANT_256_X);
    } else {
      std::memset(uniforms_pixel + kFloatConstantOffset, 0, kCBVSize);
    }

    // b2: Bool/Loop constants at offset 8192 (2 * kCBVSize).
    const size_t kBoolLoopConstantOffset = 2 * kCBVSize;
    constexpr size_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    std::memcpy(uniforms_vertex + kBoolLoopConstantOffset,
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    std::memcpy(uniforms_pixel + kBoolLoopConstantOffset,
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);

    // b3: Fetch constants at offset 12288 (3 * kCBVSize).
    // 32 fetch groups * 6 DWORDs = 192 DWORDs (same data as 96 vertex fetches).
    const size_t kFetchConstantOffset = 3 * kCBVSize;
    const size_t kFetchConstantCount =
        xenos::kTextureFetchConstantCount * 6;  // 192 DWORDs = 768 bytes
    std::memcpy(uniforms_vertex + kFetchConstantOffset,
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                kFetchConstantCount * sizeof(uint32_t));
    std::memcpy(uniforms_pixel + kFetchConstantOffset,
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                kFetchConstantCount * sizeof(uint32_t));

    auto* res_entries_all =
        reinterpret_cast<IRDescriptorTableEntry*>(res_heap_ab_->contents());
    const size_t kDescriptorTableCount = kStageCount * draw_ring_count_;
    const size_t uav_table_base_index =
        kResourceHeapSlotsPerTable * kDescriptorTableCount;
    auto* uav_entries_all = res_entries_all + uav_table_base_index;
    auto* smp_entries_all =
        reinterpret_cast<IRDescriptorTableEntry*>(smp_heap_ab_->contents());
    auto* cbv_entries_all =
        reinterpret_cast<IRDescriptorTableEntry*>(cbv_heap_ab_->contents());

    IRDescriptorTableEntry* res_entries_vertex =
        res_entries_all + table_index_vertex * kResourceHeapSlotsPerTable;
    IRDescriptorTableEntry* res_entries_pixel =
        res_entries_all + table_index_pixel * kResourceHeapSlotsPerTable;
    IRDescriptorTableEntry* uav_entries_vertex =
        uav_entries_all + table_index_vertex * kResourceHeapSlotsPerTable;
    IRDescriptorTableEntry* uav_entries_pixel =
        uav_entries_all + table_index_pixel * kResourceHeapSlotsPerTable;
    IRDescriptorTableEntry* smp_entries_vertex =
        smp_entries_all + table_index_vertex * kSamplerHeapSlotsPerTable;
    IRDescriptorTableEntry* smp_entries_pixel =
        smp_entries_all + table_index_pixel * kSamplerHeapSlotsPerTable;
    IRDescriptorTableEntry* cbv_entries_vertex =
        cbv_entries_all + table_index_vertex * kCbvHeapSlotsPerTable;
    IRDescriptorTableEntry* cbv_entries_pixel =
        cbv_entries_all + table_index_pixel * kCbvHeapSlotsPerTable;

    uint64_t res_heap_gpu_base_vertex =
        res_heap_ab_->gpuAddress() + table_index_vertex *
                                         kResourceHeapSlotsPerTable *
                                         sizeof(IRDescriptorTableEntry);
    uint64_t res_heap_gpu_base_pixel =
        res_heap_ab_->gpuAddress() + table_index_pixel *
                                         kResourceHeapSlotsPerTable *
                                         sizeof(IRDescriptorTableEntry);
    uint64_t uav_heap_gpu_base_vertex =
        res_heap_ab_->gpuAddress() +
        (uav_table_base_index +
         table_index_vertex * kResourceHeapSlotsPerTable) *
            sizeof(IRDescriptorTableEntry);
    uint64_t uav_heap_gpu_base_pixel =
        res_heap_ab_->gpuAddress() +
        (uav_table_base_index +
         table_index_pixel * kResourceHeapSlotsPerTable) *
            sizeof(IRDescriptorTableEntry);
    uint64_t smp_heap_gpu_base_vertex =
        smp_heap_ab_->gpuAddress() + table_index_vertex *
                                         kSamplerHeapSlotsPerTable *
                                         sizeof(IRDescriptorTableEntry);
    uint64_t smp_heap_gpu_base_pixel =
        smp_heap_ab_->gpuAddress() + table_index_pixel *
                                         kSamplerHeapSlotsPerTable *
                                         sizeof(IRDescriptorTableEntry);

    uint64_t uniforms_gpu_base_vertex =
        uniforms_buffer_->gpuAddress() +
        table_index_vertex * kUniformsBytesPerTable;
    uint64_t uniforms_gpu_base_pixel =
        uniforms_buffer_->gpuAddress() +
        table_index_pixel * kUniformsBytesPerTable;

    MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
    if (shared_mem_buffer) {
      IRDescriptorTableSetBuffer(&res_entries_vertex[0],
                                 shared_mem_buffer->gpuAddress(),
                                 shared_mem_buffer->length());
      IRDescriptorTableSetBuffer(&res_entries_pixel[0],
                                 shared_mem_buffer->gpuAddress(),
                                 shared_mem_buffer->length());
      IRDescriptorTableSetBuffer(&uav_entries_vertex[0],
                                 shared_mem_buffer->gpuAddress(),
                                 shared_mem_buffer->length());
      IRDescriptorTableSetBuffer(&uav_entries_pixel[0],
                                 shared_mem_buffer->gpuAddress(),
                                 shared_mem_buffer->length());
      UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
    }
    if (render_target_cache_) {
      if (MTL::Buffer* edram_buffer = render_target_cache_->GetEdramBuffer()) {
        IRDescriptorTableSetBuffer(&uav_entries_vertex[1],
                                   edram_buffer->gpuAddress(),
                                   edram_buffer->length());
        IRDescriptorTableSetBuffer(&uav_entries_pixel[1],
                                   edram_buffer->gpuAddress(),
                                   edram_buffer->length());
        UseRenderEncoderResource(
            edram_buffer, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
      }
    }

    std::array<MTL::Texture*, 64> textures_for_encoder;
    uint32_t textures_for_encoder_count = 0;
    auto track_texture_usage = [&](MTL::Texture* texture) {
      if (!texture) {
        return;
      }
      for (uint32_t i = 0; i < textures_for_encoder_count; ++i) {
        if (textures_for_encoder[i] == texture) {
          return;
        }
      }
      assert_true(textures_for_encoder_count < textures_for_encoder.size());
      textures_for_encoder[textures_for_encoder_count++] = texture;
    };

    auto bind_shader_textures = [&](const char* stage, MetalShader* shader,
                                    IRDescriptorTableEntry* stage_res_entries) {
      if (!shader || !texture_cache_) {
        return;
      }
      const auto& shader_texture_bindings =
          shader->GetTextureBindingsAfterTranslation();
      MetalTextureCache* metal_texture_cache = texture_cache_.get();
      for (size_t binding_index = 0;
           binding_index < shader_texture_bindings.size(); ++binding_index) {
        uint32_t srv_slot = 1 + static_cast<uint32_t>(binding_index);
        if (srv_slot >= kResourceHeapSlotsPerTable) {
          break;
        }
        const auto& binding = shader_texture_bindings[binding_index];
        MTL::Texture* texture = texture_cache_->GetTextureForBinding(
            binding.fetch_constant, binding.dimension, binding.is_signed);
        if (!texture) {
          // Use a dimension-compatible null texture to avoid Metal validation
          // errors (for example, cube-array expectations from converted
          // shaders).
          switch (binding.dimension) {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
            case xenos::FetchOpDimension::k3DOrStacked:
              texture = metal_texture_cache->GetNullTexture3D();
              break;
            case xenos::FetchOpDimension::kCube:
              texture = metal_texture_cache->GetNullTextureCube();
              break;
            default:
              texture = metal_texture_cache->GetNullTexture2D();
              break;
          }
          if (!logged_missing_texture_warning_) {
            XELOGW(
                "Metal: Missing texture for fetch constant {} (dimension {} "
                "signed {})",
                binding.fetch_constant, static_cast<int>(binding.dimension),
                binding.is_signed);
            logged_missing_texture_warning_ = true;
          }
        }
        if (texture) {
          IRDescriptorTableSetTexture(&stage_res_entries[srv_slot], texture,
                                      0.0f, 0);
          track_texture_usage(texture);
        }
      }
    };

    auto bind_shader_samplers = [&](const char* stage, MetalShader* shader,
                                    IRDescriptorTableEntry* stage_smp_entries) {
      if (!shader || !texture_cache_) {
        return;
      }
      const auto& sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      for (size_t sampler_index = 0; sampler_index < sampler_bindings.size();
           ++sampler_index) {
        if (sampler_index >= kSamplerHeapSlotsPerTable) {
          break;
        }
        auto parameters = texture_cache_->GetSamplerParameters(
            sampler_bindings[sampler_index]);
        MTL::SamplerState* sampler_state =
            texture_cache_->GetOrCreateSampler(parameters);
        if (!sampler_state) {
          sampler_state = null_sampler_;
        }
        if (sampler_state) {
          IRDescriptorTableSetSampler(&stage_smp_entries[sampler_index],
                                      sampler_state, 0.0f);
        }
      }
    };

    bind_shader_textures("VS", vertex_shader, res_entries_vertex);
    bind_shader_textures("PS", pixel_shader, res_entries_pixel);
    bind_shader_samplers("VS", vertex_shader, smp_entries_vertex);
    bind_shader_samplers("PS", pixel_shader, smp_entries_pixel);

    for (uint32_t i = 0; i < textures_for_encoder_count; ++i) {
      UseRenderEncoderResource(textures_for_encoder[i], MTL::ResourceUsageRead);
    }

    UseRenderEncoderResource(null_buffer_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(null_texture_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(res_heap_ab_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(smp_heap_ab_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(top_level_ab_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(cbv_heap_ab_, MTL::ResourceUsageRead);
    UseRenderEncoderResource(uniforms_buffer_, MTL::ResourceUsageRead);

    auto write_top_level_and_cbvs = [&](size_t table_index,
                                        uint64_t res_table_gpu_base,
                                        uint64_t uav_table_gpu_base,
                                        uint64_t smp_table_gpu_base,
                                        IRDescriptorTableEntry* cbv_entries,
                                        uint64_t uniforms_gpu_base) {
      size_t top_level_offset = table_index * kTopLevelABBytesPerTable;
      auto* top_level_ptrs = reinterpret_cast<uint64_t*>(
          static_cast<uint8_t*>(top_level_ab_->contents()) + top_level_offset);
      std::memset(top_level_ptrs, 0, kTopLevelABBytesPerTable);

      for (int i = 0; i < 4; ++i) {
        top_level_ptrs[i] = res_table_gpu_base;
      }
      top_level_ptrs[4] = res_table_gpu_base;
      for (int i = 5; i < 9; ++i) {
        top_level_ptrs[i] = uav_table_gpu_base;
      }
      top_level_ptrs[9] = smp_table_gpu_base;

      IRDescriptorTableSetBuffer(&cbv_entries[0],
                                 uniforms_gpu_base + 0 * kCbvSizeBytes,
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[1],
                                 uniforms_gpu_base + 1 * kCbvSizeBytes,
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[2],
                                 uniforms_gpu_base + 2 * kCbvSizeBytes,
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[3],
                                 uniforms_gpu_base + 3 * kCbvSizeBytes,
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[4],
                                 uniforms_gpu_base + 4 * kCbvSizeBytes,
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[5], null_buffer_->gpuAddress(),
                                 kCbvSizeBytes);
      IRDescriptorTableSetBuffer(&cbv_entries[6], null_buffer_->gpuAddress(),
                                 kCbvSizeBytes);

      uint64_t cbv_table_gpu_base =
          cbv_heap_ab_->gpuAddress() +
          table_index * kCbvHeapSlotsPerTable * sizeof(IRDescriptorTableEntry);
      top_level_ptrs[10] = cbv_table_gpu_base;
      top_level_ptrs[11] = cbv_table_gpu_base;
      top_level_ptrs[12] = cbv_table_gpu_base;
      top_level_ptrs[13] = cbv_table_gpu_base;
    };

    write_top_level_and_cbvs(table_index_vertex, res_heap_gpu_base_vertex,
                             uav_heap_gpu_base_vertex, smp_heap_gpu_base_vertex,
                             cbv_entries_vertex, uniforms_gpu_base_vertex);
    write_top_level_and_cbvs(table_index_pixel, res_heap_gpu_base_pixel,
                             uav_heap_gpu_base_pixel, smp_heap_gpu_base_pixel,
                             cbv_entries_pixel, uniforms_gpu_base_pixel);

    if (use_geometry_emulation || use_tessellation_emulation) {
      current_render_encoder_->setObjectBuffer(
          top_level_ab_, table_index_vertex * kTopLevelABBytesPerTable,
          kIRArgumentBufferBindPoint);
      current_render_encoder_->setMeshBuffer(
          top_level_ab_, table_index_vertex * kTopLevelABBytesPerTable,
          kIRArgumentBufferBindPoint);
      current_render_encoder_->setFragmentBuffer(
          top_level_ab_, table_index_pixel * kTopLevelABBytesPerTable,
          kIRArgumentBufferBindPoint);

      if (use_tessellation_emulation) {
        current_render_encoder_->setObjectBuffer(
            top_level_ab_, table_index_vertex * kTopLevelABBytesPerTable,
            kIRArgumentBufferHullDomainBindPoint);
        current_render_encoder_->setMeshBuffer(
            top_level_ab_, table_index_vertex * kTopLevelABBytesPerTable,
            kIRArgumentBufferHullDomainBindPoint);
      }

      current_render_encoder_->setObjectBuffer(res_heap_ab_, 0,
                                               kIRDescriptorHeapBindPoint);
      current_render_encoder_->setMeshBuffer(res_heap_ab_, 0,
                                             kIRDescriptorHeapBindPoint);
      current_render_encoder_->setFragmentBuffer(res_heap_ab_, 0,
                                                 kIRDescriptorHeapBindPoint);
      current_render_encoder_->setObjectBuffer(smp_heap_ab_, 0,
                                               kIRSamplerHeapBindPoint);
      current_render_encoder_->setMeshBuffer(smp_heap_ab_, 0,
                                             kIRSamplerHeapBindPoint);
      current_render_encoder_->setFragmentBuffer(smp_heap_ab_, 0,
                                                 kIRSamplerHeapBindPoint);
    } else {
      current_render_encoder_->setVertexBuffer(
          top_level_ab_, table_index_vertex * kTopLevelABBytesPerTable,
          kIRArgumentBufferBindPoint);
      current_render_encoder_->setFragmentBuffer(
          top_level_ab_, table_index_pixel * kTopLevelABBytesPerTable,
          kIRArgumentBufferBindPoint);

      current_render_encoder_->setVertexBuffer(res_heap_ab_, 0,
                                               kIRDescriptorHeapBindPoint);
      current_render_encoder_->setFragmentBuffer(res_heap_ab_, 0,
                                                 kIRDescriptorHeapBindPoint);
      current_render_encoder_->setVertexBuffer(smp_heap_ab_, 0,
                                               kIRSamplerHeapBindPoint);
      current_render_encoder_->setFragmentBuffer(smp_heap_ab_, 0,
                                                 kIRSamplerHeapBindPoint);
    }
  }

  // Bind vertex buffers / descriptors.
  if (use_geometry_emulation || use_tessellation_emulation) {
    IRRuntimeVertexBuffers vertex_buffers = {};
    MTL::Buffer* shared_mem_buffer =
        shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
    if (shared_mem_buffer) {
      UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
      for (uint32_t i = 0; i < vertex_range_count; ++i) {
        const auto& range = vertex_ranges[i];
        size_t binding_index = range.binding_index;
        if (binding_index <
            (sizeof(vertex_buffers) / sizeof(vertex_buffers[0]))) {
          vertex_buffers[binding_index].addr =
              shared_mem_buffer->gpuAddress() + range.offset;
          vertex_buffers[binding_index].length = range.length;
          vertex_buffers[binding_index].stride = range.stride;
        }
      }
    }
    // MSC manual: bind IRRuntimeVertexBuffers at kIRVertexBufferBindPoint (6)
    // for the object stage when using geometry emulation.
    current_render_encoder_->setObjectBytes(
        vertex_buffers, sizeof(vertex_buffers), kIRVertexBufferBindPoint);
  } else if (uses_vertex_fetch) {
    // Vertex fetch shaders read directly from shared memory via SRV, so avoid
    // stage-in bindings that can trigger invalid buffer loads.
    if (shared_memory_) {
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
      }
    }
  } else {
    // Bind vertex buffers at kIRVertexBufferBindPoint (index 6+) for stage-in.
    // The pipeline's vertex descriptor expects buffers at these indices,
    // populated from the vertex fetch constants. The buffer addresses come from
    // shared memory.
    if (shared_memory_ && !vb_bindings.empty()) {
      MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
      if (shared_mem_buffer) {
        // Mark shared memory as used for reading
        UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);

        // Bind vertex buffers for each binding
        for (uint32_t i = 0; i < vertex_range_count; ++i) {
          const auto& range = vertex_ranges[i];
          uint64_t buffer_index =
              kIRVertexBufferBindPoint + uint64_t(range.binding_index);
          current_render_encoder_->setVertexBuffer(shared_mem_buffer,
                                                   range.offset, buffer_index);
        }
      }
    } else if (shared_memory_) {
      // No vertex bindings, but still mark shared memory as resident
      if (MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer()) {
        UseRenderEncoderResource(shared_mem_buffer, shared_memory_usage);
      }
    }
  }

  auto request_guest_index_range = [&](uint64_t index_base,
                                       uint32_t index_count,
                                       MTL::IndexType index_type) -> bool {
    if (!shared_memory_) {
      return false;
    }
    uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
    uint64_t index_length = uint64_t(index_count) * index_stride;
    if (index_base > SharedMemory::kBufferSize ||
        SharedMemory::kBufferSize - index_base < index_length) {
      XELOGW(
          "Index buffer range out of bounds (base=0x{:08X} size={} count={})",
          static_cast<uint32_t>(index_base), index_length, index_count);
      return false;
    }
    return shared_memory_->RequestRange(static_cast<uint32_t>(index_base),
                                        static_cast<uint32_t>(index_length));
  };

  if (use_tessellation_emulation) {
    IRRuntimePrimitiveType tess_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        tess_primitive = IRRuntimePrimitiveType3ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadList:
        tess_primitive = IRRuntimePrimitiveType4ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType3ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        tess_primitive = (primitive_processing_result.tessellation_mode ==
                          xenos::TessellationMode::kAdaptive)
                             ? IRRuntimePrimitiveType4ControlPointPatchlist
                             : IRRuntimePrimitiveType1ControlPointPatchlist;
        break;
      default:
        XELOGE(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Metal tessellation path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    const IRRuntimeTessellationPipelineConfig& tess_config =
        tessellation_pipeline_state->config;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, tess_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
          index_buffer = shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
          index_offset = primitive_processing_result.guest_index_base;
          if (!request_guest_index_range(
                  index_offset,
                  primitive_processing_result.host_draw_vertex_count,
                  index_type)) {
            XELOGE(
                "IssueDraw: failed to validate guest index buffer range for "
                "tessellation");
            return false;
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle,
                index_offset);
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
            index_offset = primitive_processing_result.host_index_buffer_handle;
          }
          break;
        default:
          XELOGE("Unsupported index buffer type {} for tessellation",
                 uint32_t(primitive_processing_result.index_buffer_type));
          return false;
      }
      if (!index_buffer) {
        XELOGE("IssueDraw: index buffer is null for tessellation");
        return false;
      }
      UseRenderEncoderResource(index_buffer, MTL::ResourceUsageRead);
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPatchesTessellationEmulation(
          current_render_encoder_, tess_primitive, index_type, index_buffer,
          tess_config, 1, primitive_processing_result.host_draw_vertex_count, 0,
          0, start_index);
    }
  } else if (use_geometry_emulation) {
    IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_primitive = IRRuntimePrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kRectangleList:
        geometry_primitive = IRRuntimePrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_primitive = IRRuntimePrimitiveTypeLineWithAdj;
        break;
      default:
        XELOGE(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal geometry path",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    IRRuntimeGeometryPipelineConfig geometry_config = {};
    geometry_config.gsVertexSizeInBytes =
        geometry_pipeline_state->gs_vertex_size_in_bytes;
    geometry_config.gsMaxInputPrimitivesPerMeshThreadgroup =
        geometry_pipeline_state->gs_max_input_primitives_per_mesh_threadgroup;

    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, 0, 0);
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
          index_buffer = shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
          index_offset = primitive_processing_result.guest_index_base;
          if (!request_guest_index_range(
                  index_offset,
                  primitive_processing_result.host_draw_vertex_count,
                  index_type)) {
            XELOGE("IssueDraw: failed to validate guest index buffer range");
            return false;
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle,
                index_offset);
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
            index_offset = primitive_processing_result.host_index_buffer_handle;
          }
          break;
        default:
          XELOGE("Unsupported index buffer type {}",
                 uint32_t(primitive_processing_result.index_buffer_type));
          return false;
      }
      if (!index_buffer) {
        XELOGE("IssueDraw: index buffer is null for type {}",
               uint32_t(primitive_processing_result.index_buffer_type));
        return false;
      }
      UseRenderEncoderResource(index_buffer, MTL::ResourceUsageRead);
      uint32_t index_stride = (index_type == MTL::IndexTypeUInt16)
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t);
      uint32_t start_index =
          index_stride ? uint32_t(index_offset / index_stride) : 0;
      IRRuntimeDrawIndexedPrimitivesGeometryEmulation(
          current_render_encoder_, geometry_primitive, index_type, index_buffer,
          geometry_config, 1,
          primitive_processing_result.host_draw_vertex_count, start_index, 0,
          0);
    }
  } else {
    // Primitive topology - from primitive processor, like D3D12.
    MTL::PrimitiveType mtl_primitive = MTL::PrimitiveTypeTriangle;
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        mtl_primitive = MTL::PrimitiveTypePoint;
        break;
      case xenos::PrimitiveType::kLineList:
        mtl_primitive = MTL::PrimitiveTypeLine;
        break;
      case xenos::PrimitiveType::kLineStrip:
        mtl_primitive = MTL::PrimitiveTypeLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        mtl_primitive = MTL::PrimitiveTypeTriangle;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        mtl_primitive = MTL::PrimitiveTypeTriangleStrip;
        break;
      default:
        XELOGE(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Metal command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }

    // Draw using primitive processor output.
    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
      IRRuntimeDrawPrimitives(
          current_render_encoder_, mtl_primitive, NS::UInteger(0),
          NS::UInteger(primitive_processing_result.host_draw_vertex_count));
    } else {
      MTL::IndexType index_type =
          (primitive_processing_result.host_index_format ==
           xenos::IndexFormat::kInt16)
              ? MTL::IndexTypeUInt16
              : MTL::IndexTypeUInt32;
      MTL::Buffer* index_buffer = nullptr;
      uint64_t index_offset = 0;
      switch (primitive_processing_result.index_buffer_type) {
        case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
          index_buffer = shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
          index_offset = primitive_processing_result.guest_index_base;
          if (!request_guest_index_range(
                  index_offset,
                  primitive_processing_result.host_draw_vertex_count,
                  index_type)) {
            XELOGE("IssueDraw: failed to validate guest index buffer range");
            return false;
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle,
                index_offset);
          }
          break;
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
        case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
          if (primitive_processor_) {
            index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
            index_offset = primitive_processing_result.host_index_buffer_handle;
          }
          break;
        default:
          XELOGE("Unsupported index buffer type {}",
                 uint32_t(primitive_processing_result.index_buffer_type));
          return false;
      }
      if (!index_buffer) {
        XELOGE("IssueDraw: index buffer is null for type {}",
               uint32_t(primitive_processing_result.index_buffer_type));
        return false;
      }
      IRRuntimeDrawIndexedPrimitives(
          current_render_encoder_, mtl_primitive,
          NS::UInteger(primitive_processing_result.host_draw_vertex_count),
          index_type, index_buffer, index_offset, NS::UInteger(1), 0, 0);
    }
  }

  if (memexport_used && shared_memory_) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    }
  }

  // Advance ring-buffer indices for descriptor and argument buffers.
  ++current_draw_index_;

  return true;
}

bool MetalCommandProcessor::IssueCopy() {
  if (!BeginSubmission(true)) {
    return false;
  }

  // Finish any in-flight rendering so the render target contents are
  // available to the render target cache, similar to D3D12's
  // D3D12CommandProcessor::IssueCopy.
  if (current_render_encoder_) {
    current_render_encoder_->endEncoding();
    current_render_encoder_->release();
    current_render_encoder_ = nullptr;
  }

  if (!current_command_buffer_) {
    if (!EnsureCommandBuffer()) {
      XELOGE("MetalCommandProcessor::IssueCopy: no command buffer");
      return false;
    }
    current_command_buffer_->setLabel(
        NS::String::string("XeniaCopyCommandBuffer", NS::UTF8StringEncoding));
  }

  if (!render_target_cache_) {
    XELOGW("MetalCommandProcessor::IssueCopy - No render target cache");
    return true;
  }

  uint32_t written_address = 0;
  uint32_t written_length = 0;

  if (!render_target_cache_->Resolve(*memory_, written_address, written_length,
                                     current_command_buffer_)) {
    XELOGE("MetalCommandProcessor::IssueCopy - Resolve failed");
    return false;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode();
  bool do_readback = (readback_mode != ReadbackResolveMode::kDisabled);
  bool readback_scaled = false;
  bool readback_scaled_gpu = false;
  bool use_gpu_downscale = false;
  bool readback_scheduled = false;
  ReadbackBuffer* readback_buffer = nullptr;
  uint32_t write_index = 0;
  uint32_t read_index = 0;
  bool use_delayed_sync = false;
  bool wait_for_completion = false;
  bool should_copy = false;
  bool is_cache_miss = false;
  uint32_t source_length = 0;
  uint32_t readback_length = 0;
  uint32_t tile_count = 0;
  uint32_t pixel_size_log2 = 0;
  uint32_t scale_x = 1;
  uint32_t scale_y = 1;
  bool half_pixel_offset = false;
  uint32_t source_offset_bytes = 0;
  uint64_t scaled_range_offset_bytes = 0;
  uint64_t readback_base_offset_bytes = 0;
  uint64_t scaled_copy_length = 0;
  size_t source_buffer_binding_offset = 0;
  uint64_t source_offset_bytes_log = 0;

  if (do_readback) {
    // Early check: if destination memory is not accessible, skip readback.
    VirtualHeap* physical_heap = memory_->GetPhysicalHeap();
    bool memory_accessible = false;
    if (physical_heap) {
      HeapAllocationInfo alloc_info;
      if (physical_heap->QueryRegionInfo(written_address, &alloc_info) &&
          (alloc_info.state & kMemoryAllocationCommit) &&
          (alloc_info.protect & kMemoryProtectWrite)) {
        uint32_t end_address = written_address + written_length;
        uint32_t region_end = alloc_info.base_address + alloc_info.region_size;
        if (end_address <= region_end) {
          memory_accessible = true;
        }
      }
    }
    if (!memory_accessible) {
      do_readback = false;
    }
  }

  if (!written_length) {
    // Commit any in-flight work so ordering matches D3D12 submission behavior.
    EndSubmission(false);
    return true;
  }

  // Track this resolved region so the trace player can avoid overwriting it
  // with stale MemoryRead commands from the trace file.
  MarkResolvedMemory(written_address, written_length);

  if (do_readback) {
    MTL::Buffer* source_buffer = nullptr;
    size_t source_offset = 0;
    size_t source_length_size_t = 0;

    if (texture_cache_ && texture_cache_->IsDrawResolutionScaled()) {
      readback_scaled = true;
      auto* metal_texture_cache =
          static_cast<MetalTextureCache*>(texture_cache_.get());
      if (!metal_texture_cache ||
          !metal_texture_cache->GetCurrentScaledResolveBuffer(
              source_buffer, source_offset, source_length_size_t)) {
        XELOGE("MetalResolveReadback: failed to get scaled resolve buffer");
        do_readback = false;
      } else {
        scale_x = texture_cache_->draw_resolution_scale_x();
        scale_y = texture_cache_->draw_resolution_scale_y();
        uint64_t scale_area = uint64_t(scale_x) * uint64_t(scale_y);
        uint64_t range_start_scaled =
            metal_texture_cache->GetCurrentScaledResolveRangeStartScaled();
        if (scale_area && (range_start_scaled % scale_area) == 0) {
          uint64_t range_start_unscaled = range_start_scaled / scale_area;
          if (written_address >= range_start_unscaled) {
            scaled_range_offset_bytes =
                (uint64_t(written_address) - range_start_unscaled) * scale_area;
          }
        }
        if (scaled_range_offset_bytes > source_length_size_t) {
          XELOGE("MetalResolveReadback: scaled range offset out of bounds");
          do_readback = false;
        } else {
          readback_base_offset_bytes = scaled_range_offset_bytes;
        }
      }
    } else {
      source_buffer = shared_memory_ ? shared_memory_->GetBuffer() : nullptr;
      source_offset = written_address;
      source_length_size_t = written_length;
      source_offset_bytes_log = source_offset;
    }

    if (do_readback) {
      if (!source_buffer || source_length_size_t == 0) {
        do_readback = false;
      } else if (source_length_size_t > std::numeric_limits<uint32_t>::max()) {
        XELOGE("MetalResolveReadback: source length too large ({})",
               source_length_size_t);
        do_readback = false;
      } else if (source_offset + source_length_size_t >
                 size_t(source_buffer->length())) {
        XELOGE("MetalResolveReadback: source range out of bounds");
        do_readback = false;
      }
    }

    if (do_readback) {
      source_length = uint32_t(source_length_size_t);
      uint64_t scaled_available_bytes = source_length_size_t;
      if (readback_scaled) {
        if (scaled_range_offset_bytes >= source_length_size_t) {
          XELOGE("MetalResolveReadback: scaled range offset exceeds length");
          do_readback = false;
        } else {
          scaled_available_bytes =
              source_length_size_t - scaled_range_offset_bytes;
        }
      }
      uint64_t resolve_key =
          MakeReadbackResolveKey(written_address, written_length);
      ReadbackBuffer& rb = readback_buffers_[resolve_key];
      rb.last_used_frame = frame_current_;
      readback_buffer = &rb;
      write_index = rb.current_index;
      use_delayed_sync = (readback_mode == ReadbackResolveMode::kFast ||
                          readback_mode == ReadbackResolveMode::kSome);
      read_index = use_delayed_sync ? (1u - write_index) : write_index;

      readback_length = source_length;
      if (readback_scaled) {
        auto copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
        const FormatInfo* format_info =
            FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
        uint32_t bits_per_pixel = format_info->bits_per_pixel;
        xe::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2);
        uint32_t bytes_per_pixel = 1u << pixel_size_log2;
        uint32_t tile_size_1x = 32u * 32u * bytes_per_pixel;
        tile_count = written_length / tile_size_1x;
        half_pixel_offset = cvars::readback_resolve_half_pixel_offset &&
                            (scale_x > 1 || scale_y > 1);
        uint64_t tile_size_scaled =
            uint64_t(tile_size_1x) * uint64_t(scale_x) * uint64_t(scale_y);
        uint64_t required_scaled = uint64_t(tile_count) * tile_size_scaled;
        if (required_scaled > scaled_available_bytes) {
          tile_count = uint32_t(scaled_available_bytes / tile_size_scaled);
          required_scaled = uint64_t(tile_count) * tile_size_scaled;
        }
        if (tile_count == 0) {
          do_readback = false;
        }

        uint64_t source_offset_bytes_64 =
            uint64_t(source_offset) + scaled_range_offset_bytes;
        source_offset_bytes_log = source_offset_bytes_64;
        if (do_readback && tile_count != 0 && resolve_downscale_pipeline_) {
          uint32_t downscale_buffer_size =
              AlignReadbackBufferSize(written_length);
          if (downscale_buffer_size > resolve_downscale_buffer_size_) {
            if (resolve_downscale_buffer_) {
              resolve_downscale_buffer_->release();
              resolve_downscale_buffer_ = nullptr;
              resolve_downscale_buffer_size_ = 0;
            }
            if (device_) {
              resolve_downscale_buffer_ = device_->newBuffer(
                  downscale_buffer_size, MTL::ResourceStorageModePrivate);
              if (resolve_downscale_buffer_) {
                resolve_downscale_buffer_size_ = downscale_buffer_size;
              }
            }
          }
          if (resolve_downscale_buffer_) {
            use_gpu_downscale = true;
            readback_length = written_length;
            source_buffer_binding_offset =
                size_t(source_offset_bytes_64 & ~uint64_t(3));
            source_offset_bytes =
                uint32_t(source_offset_bytes_64 -
                         uint64_t(source_buffer_binding_offset));
          }
        }
        if (do_readback && !use_gpu_downscale) {
          scaled_copy_length = required_scaled;
          if (scaled_copy_length > std::numeric_limits<uint32_t>::max()) {
            XELOGE("MetalResolveReadback: scaled copy length too large ({})",
                   scaled_copy_length);
            do_readback = false;
          } else {
            readback_length = uint32_t(scaled_copy_length);
            source_offset = size_t(source_offset_bytes_64);
            readback_base_offset_bytes = 0;
          }
        }
      }

      uint32_t aligned_size = AlignReadbackBufferSize(readback_length);
      if (aligned_size > rb.sizes[write_index]) {
        if (!device_) {
          XELOGE("MetalResolveReadback: missing Metal device");
          do_readback = false;
        }
      }
      if (do_readback && aligned_size > rb.sizes[write_index]) {
        if (rb.buffers[write_index]) {
          rb.buffers[write_index]->release();
          rb.buffers[write_index] = nullptr;
        }
        rb.buffers[write_index] =
            device_->newBuffer(aligned_size, MTL::ResourceStorageModeShared);
        rb.sizes[write_index] = aligned_size;
        rb.submission_ids[write_index] = 0;
      }
      if (do_readback && !rb.buffers[write_index]) {
        XELOGE("MetalResolveReadback: failed to allocate readback buffer");
        do_readback = false;
      } else if (do_readback) {
        if (readback_scaled && use_gpu_downscale) {
          MTL::ComputeCommandEncoder* compute =
              current_command_buffer_->computeCommandEncoder();
          if (!compute) {
            XELOGE("MetalResolveReadback: failed to create compute encoder");
            do_readback = false;
          } else {
            struct ResolveDownscaleConstants {
              uint32_t scale_x;
              uint32_t scale_y;
              uint32_t pixel_size_log2;
              uint32_t tile_count;
              uint32_t source_offset_bytes;
              uint32_t half_pixel_offset;
            } constants;
            constants.scale_x = scale_x;
            constants.scale_y = scale_y;
            constants.pixel_size_log2 = pixel_size_log2;
            constants.tile_count = tile_count;
            constants.source_offset_bytes = source_offset_bytes;
            constants.half_pixel_offset = half_pixel_offset ? 1u : 0u;

            compute->setComputePipelineState(resolve_downscale_pipeline_);
            compute->setBytes(&constants, sizeof(constants), 0);
            compute->setBuffer(source_buffer, source_buffer_binding_offset, 1);
            compute->setBuffer(resolve_downscale_buffer_, 0, 2);
            compute->useResource(source_buffer, MTL::ResourceUsageRead);
            compute->useResource(resolve_downscale_buffer_,
                                 MTL::ResourceUsageWrite);
            compute->dispatchThreadgroups(MTL::Size::Make(tile_count, 1, 1),
                                          MTL::Size::Make(32, 32, 1));
            compute->endEncoding();

            MTL::BlitCommandEncoder* blit =
                current_command_buffer_->blitCommandEncoder();
            if (!blit) {
              XELOGE("MetalResolveReadback: failed to create blit encoder");
              do_readback = false;
            } else {
              blit->copyFromBuffer(resolve_downscale_buffer_, 0,
                                   rb.buffers[write_index], 0, readback_length);
              blit->endEncoding();
              rb.submission_ids[write_index] = GetCurrentSubmission();
              readback_scheduled = true;
              readback_scaled_gpu = true;
            }
          }
        } else {
          MTL::BlitCommandEncoder* blit =
              current_command_buffer_->blitCommandEncoder();
          if (!blit) {
            XELOGE("MetalResolveReadback: failed to create blit encoder");
            do_readback = false;
          } else {
            blit->copyFromBuffer(source_buffer, source_offset,
                                 rb.buffers[write_index], 0, readback_length);
            blit->endEncoding();
            rb.submission_ids[write_index] = GetCurrentSubmission();
            readback_scheduled = true;
          }
        }
      }

      ProcessCompletedSubmissions();
      if (readback_scheduled && use_delayed_sync) {
        if (rb.buffers[read_index] == nullptr ||
            readback_length > rb.sizes[read_index] ||
            rb.submission_ids[read_index] == 0 ||
            rb.submission_ids[read_index] > submission_completed_processed_) {
          is_cache_miss = true;
          read_index = write_index;
        }
      }

      wait_for_completion = !use_delayed_sync || is_cache_miss;
      should_copy =
          (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
      if (readback_scaled && tile_count == 0) {
        should_copy = false;
        wait_for_completion = false;
      }
    }
  }

  // Submit the command buffer without waiting - the resolve writes are now
  // ordered in the same submission as the preceding draws.
  uint64_t submission_to_wait = 0;
  bool defer_submission =
      readback_scheduled && use_delayed_sync && !wait_for_completion;
  if (readback_scheduled && wait_for_completion) {
    submission_to_wait = GetCurrentSubmission();
  }
  if (!defer_submission) {
    EndSubmission(false);
    if (submission_to_wait) {
      CheckSubmissionCompletion(submission_to_wait);
    }
  }

  if (readback_scheduled && should_copy && readback_buffer &&
      readback_buffer->buffers[read_index]) {
    static uint32_t readback_log_count = 0;
    if (readback_log_count < 8) {
      ++readback_log_count;
      XELOGI(
          "MetalResolveReadback: addr=0x{:08X} len={} scaled={} gpu={} "
          "scale={}x{} pix_log2={} tiles={} src_off={} "
          "scaled_off={} src_len={}",
          written_address, written_length, readback_scaled ? 1 : 0,
          readback_scaled_gpu ? 1 : 0, scale_x, scale_y, pixel_size_log2,
          tile_count, source_offset_bytes_log, scaled_range_offset_bytes,
          source_length);
    }
    const uint8_t* readback_bytes = static_cast<const uint8_t*>(
        readback_buffer->buffers[read_index]->contents());
    uint8_t* dest_ptr = memory_->TranslatePhysical(written_address);
    if (readback_bytes && dest_ptr) {
      if (readback_scaled) {
        if (readback_scaled_gpu) {
          memory::vastcpy(dest_ptr, const_cast<uint8_t*>(readback_bytes),
                          written_length);
        } else {
          const uint8_t* readback_base = readback_bytes;
          if (readback_base_offset_bytes < readback_length) {
            readback_base += readback_base_offset_bytes;
          }
          DownscaleResolveTileData(readback_base, dest_ptr, tile_count,
                                   pixel_size_log2, scale_x, scale_y,
                                   half_pixel_offset);
        }
        // Scaled resolve data isn't in shared memory; invalidate so CPU memory
        // becomes authoritative and shared memory uploads on demand.
        if (shared_memory_) {
          shared_memory_->MemoryInvalidationCallback(written_address,
                                                     written_length, true);
        }
        if (primitive_processor_) {
          primitive_processor_->MemoryInvalidationCallback(
              written_address, written_length, true);
        }
      } else {
        memory::vastcpy(dest_ptr, const_cast<uint8_t*>(readback_bytes),
                        written_length);
      }
    }
  }
  if (readback_scheduled && readback_buffer) {
    readback_buffer->current_index = 1u - readback_buffer->current_index;
  }

  return true;
}

void MetalCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void MetalCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
  }
}

MTL::CommandBuffer* MetalCommandProcessor::EnsureCommandBuffer() {
  if (current_command_buffer_) {
    return current_command_buffer_;
  }
  if (!submission_open_) {
    XELOGE("EnsureCommandBuffer: no open submission");
    return nullptr;
  }
  if (!command_queue_) {
    XELOGE("EnsureCommandBuffer: no command queue");
    return nullptr;
  }

  EnsureCommandBufferAutoreleasePool();

  // Note: commandBuffer() returns an autoreleased object, we must retain it.
  current_command_buffer_ = command_queue_->commandBuffer();
  if (!current_command_buffer_) {
    XELOGE("EnsureCommandBuffer: failed to create command buffer");
    return nullptr;
  }
  current_command_buffer_->retain();
  current_command_buffer_->setLabel(
      NS::String::string("XeniaCommandBuffer", NS::UTF8StringEncoding));

  return current_command_buffer_;
}

void MetalCommandProcessor::ProcessCompletedSubmissions() {
  CheckSubmissionCompletion(0);
}

void MetalCommandProcessor::CheckSubmissionCompletion(
    uint64_t await_submission) {
  if (!completion_timeline_) {
    return;
  }
  if (await_submission) {
    completion_timeline_->AwaitSubmissionAndUpdateCompleted(await_submission);
  } else {
    completion_timeline_->UpdateCompletedSubmission();
  }
  const uint64_t completed =
      completion_timeline_->GetCompletedSubmissionFromLastUpdate();
  if (completed <= submission_completed_processed_) {
    return;
  }
  submission_completed_processed_ = completed;
  if (primitive_processor_) {
    primitive_processor_->CompletedSubmissionUpdated();
  }
  if (texture_cache_) {
    texture_cache_->CompletedSubmissionUpdated(completed);
  }
}

bool MetalCommandProcessor::BeginSubmission(bool is_guest_command) {
  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  MaybeStartCapture();

  uint64_t await_submission = 0;
  if (is_opening_frame) {
    await_submission = closed_frame_submissions_[frame_current_ % kQueueFrames];
  }
  CheckSubmissionCompletion(await_submission);

  if (is_opening_frame) {
    frame_completed_ =
        std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] >
          GetCompletedSubmission()) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;
    if (!EnsureCommandBuffer()) {
      submission_open_ = false;
      return false;
    }
    if (primitive_processor_) {
      primitive_processor_->BeginSubmission();
    }
    if (texture_cache_) {
      texture_cache_->BeginSubmission(GetCurrentSubmission());
    }
  }

  if (is_opening_frame) {
    frame_open_ = true;
    if (primitive_processor_) {
      primitive_processor_->BeginFrame();
    }
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
    if (render_target_cache_) {
      render_target_cache_->BeginFrame();
    }
  }

  return true;
}

bool MetalCommandProcessor::EndSubmission(bool is_swap) {
  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    if (primitive_processor_) {
      primitive_processor_->EndFrame();
    }
  }

  if (submission_open_) {
    EndRenderEncoder();

    if (!current_command_buffer_) {
      XELOGW("MetalCommandProcessor::EndSubmission: missing command buffer");
    } else {
      if (completion_timeline_) {
        completion_timeline_->SignalAndAdvance(current_command_buffer_);
      }
      ScheduleDrawRingRelease(current_command_buffer_);
      current_command_buffer_->commit();
      current_command_buffer_->release();
      current_command_buffer_ = nullptr;
    }
    SetActiveDrawRing(nullptr);
    current_draw_index_ = 0;

    submission_open_ = false;
    DrainCommandBufferAutoreleasePool();
  }

  if (is_closing_frame) {
    if (shared_memory_ && ::cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    closed_frame_submissions_[frame_current_++ % kQueueFrames] =
        GetCurrentSubmission() - 1;
    EvictOldReadbackBuffers(readback_buffers_);
  }

  return true;
}

bool MetalCommandProcessor::CanEndSubmissionImmediately() const { return true; }

void MetalCommandProcessor::EnsureCommandBufferAutoreleasePool() {
  if (command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_ = NS::AutoreleasePool::alloc()->init();
}

void MetalCommandProcessor::DrainCommandBufferAutoreleasePool() {
  if (!command_buffer_autorelease_pool_) {
    return;
  }
  command_buffer_autorelease_pool_->release();
  command_buffer_autorelease_pool_ = nullptr;
}

void MetalCommandProcessor::EndRenderEncoder() {
  if (!current_render_encoder_) {
    return;
  }
  current_render_encoder_->endEncoding();
  current_render_encoder_->release();
  current_render_encoder_ = nullptr;
  current_render_pass_descriptor_ = nullptr;
  ResetRenderEncoderResourceUsage();
}

void MetalCommandProcessor::ResetRenderEncoderResourceUsage() {
  render_encoder_resource_usage_.clear();
  render_encoder_heap_usage_.clear();
}

void MetalCommandProcessor::UseRenderEncoderResource(MTL::Resource* resource,
                                                     MTL::ResourceUsage usage) {
  if (!current_render_encoder_ || !resource) {
    return;
  }
  UseRenderEncoderHeap(resource->heap());
  uint32_t usage_bits = static_cast<uint32_t>(usage);
  auto it = render_encoder_resource_usage_.find(resource);
  if (it != render_encoder_resource_usage_.end()) {
    if ((it->second & usage_bits) == usage_bits) {
      return;
    }
    it->second |= usage_bits;
  } else {
    render_encoder_resource_usage_.emplace(resource, usage_bits);
  }
  current_render_encoder_->useResource(resource, usage);
}

void MetalCommandProcessor::UseRenderEncoderHeap(MTL::Heap* heap) {
  if (!current_render_encoder_ || !heap) {
    return;
  }
  if (!render_encoder_heap_usage_.insert(heap).second) {
    return;
  }
  current_render_encoder_->useHeap(heap);
}

void MetalCommandProcessor::UseRenderEncoderAttachmentHeaps(
    MTL::RenderPassDescriptor* descriptor) {
  if (!current_render_encoder_ || !descriptor) {
    return;
  }
  auto* color_attachments = descriptor->colorAttachments();
  for (uint32_t i = 0; i < 8; ++i) {
    auto* attachment = color_attachments->object(i);
    if (!attachment) {
      continue;
    }
    MTL::Texture* texture = attachment->texture();
    if (texture) {
      UseRenderEncoderHeap(texture->heap());
    }
  }
  auto* depth_attachment = descriptor->depthAttachment();
  if (depth_attachment && depth_attachment->texture()) {
    UseRenderEncoderHeap(depth_attachment->texture()->heap());
  }
  auto* stencil_attachment = descriptor->stencilAttachment();
  if (stencil_attachment && stencil_attachment->texture()) {
    UseRenderEncoderHeap(stencil_attachment->texture()->heap());
  }
}

void MetalCommandProcessor::BeginCommandBuffer() {
  if (!EnsureCommandBuffer()) {
    return;
  }

  if (!current_render_encoder_ && (!render_encoder_resource_usage_.empty() ||
                                   !render_encoder_heap_usage_.empty())) {
    ResetRenderEncoderResourceUsage();
  }

  EnsureActiveDrawRing();

  // Obtain the render pass descriptor. Prefer the one provided by
  // MetalRenderTargetCache (host render-target path), falling back to the
  // legacy descriptor if needed.
  MTL::RenderPassDescriptor* pass_descriptor = render_pass_descriptor_;
  if (render_target_cache_) {
    if (MTL::RenderPassDescriptor* cache_desc =
            render_target_cache_->GetRenderPassDescriptor(1)) {
      pass_descriptor = cache_desc;
    }
  }
  if (!pass_descriptor) {
    XELOGE("BeginCommandBuffer: No render pass descriptor available");
    return;
  }

  // Detect Reverse-Z usage and update clear depth.
  if (register_file_) {
    auto depth_control = register_file_->Get<reg::RB_DEPTHCONTROL>();
    bool reverse_z =
        depth_control.z_enable &&
        (depth_control.zfunc == xenos::CompareFunction::kGreater ||
         depth_control.zfunc == xenos::CompareFunction::kGreaterEqual);
    if (auto* da = pass_descriptor->depthAttachment()) {
      if (reverse_z) {
        da->setClearDepth(0.0);
      } else {
        da->setClearDepth(1.0);
      }
    }
  }

  bool render_pass_dirty = render_target_cache_ &&
                           render_target_cache_->IsRenderPassDescriptorDirty();

  // If the render pass configuration has changed since the current render
  // encoder was created (e.g. dummy RT0 -> real RTs, depth/stencil binding),
  // restart the render encoder with the updated descriptor.
  if (current_render_encoder_ &&
      (current_render_pass_descriptor_ != pass_descriptor ||
       render_pass_dirty)) {
    EndRenderEncoder();
  }

  if (!current_render_encoder_) {
    // Note: renderCommandEncoder() returns an autoreleased object, we must
    // retain it.
    current_render_encoder_ =
        current_command_buffer_->renderCommandEncoder(pass_descriptor);
    if (!current_render_encoder_) {
      XELOGE("Failed to create render command encoder");
      return;
    }
    current_render_encoder_->retain();
    current_render_encoder_->setLabel(
        NS::String::string("XeniaRenderEncoder", NS::UTF8StringEncoding));
    ff_blend_factor_valid_ = false;
    current_render_pass_descriptor_ = pass_descriptor;
    UseRenderEncoderAttachmentHeaps(pass_descriptor);
  }

  // Derive viewport/scissor from the actual bound render pass attachments.
  uint32_t rt_width = render_target_width_;
  uint32_t rt_height = render_target_height_;
  MTL::Texture* pass_size_texture = nullptr;
  if (pass_descriptor) {
    if (auto* color_attachments = pass_descriptor->colorAttachments()) {
      if (auto* attachment = color_attachments->object(0)) {
        pass_size_texture = attachment->texture();
      }
    }
    if (!pass_size_texture) {
      if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
        pass_size_texture = depth_attachment->texture();
      }
    }
    if (!pass_size_texture) {
      if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
        pass_size_texture = stencil_attachment->texture();
      }
    }
  }
  if (!pass_size_texture && render_target_cache_) {
    pass_size_texture = render_target_cache_->GetColorTarget(0);
    if (!pass_size_texture) {
      pass_size_texture = render_target_cache_->GetDepthTarget();
    }
    if (!pass_size_texture) {
      pass_size_texture = render_target_cache_->GetDummyColorTarget();
    }
  }
  if (pass_size_texture) {
    rt_width = static_cast<uint32_t>(pass_size_texture->width());
    rt_height = static_cast<uint32_t>(pass_size_texture->height());
  }

  // Set viewport
  MTL::Viewport viewport = {
      0.0, 0.0, static_cast<double>(rt_width), static_cast<double>(rt_height),
      0.0, 1.0};
  current_render_encoder_->setViewport(viewport);

  // Set scissor (must not exceed render pass dimensions)
  MTL::ScissorRect scissor = {0, 0, rt_width, rt_height};
  current_render_encoder_->setScissorRect(scissor);
}

void MetalCommandProcessor::EnsureDrawRingCapacity() {
  if (current_draw_index_ < draw_ring_count_) {
    return;
  }

  auto ring = AcquireDrawRingBuffers();
  if (!ring) {
    XELOGE("Metal draw ring exhausted but failed to allocate a new ring");
    return;
  }

  SetActiveDrawRing(ring);
  command_buffer_draw_rings_.push_back(ring);
  current_draw_index_ = 0;
}

void MetalCommandProcessor::EndCommandBuffer() { EndSubmission(false); }

void MetalCommandProcessor::ApplyDepthStencilState(
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
  if (!current_render_encoder_ || !device_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_ref_mask_back =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);

  DepthStencilStateKey key;
  key.depth_control = normalized_depth_control.value;
  key.stencil_ref_mask_front = stencil_ref_mask_front.value;
  key.stencil_ref_mask_back = stencil_ref_mask_back.value;
  key.polygonal_and_backface =
      (primitive_polygonal ? 1u : 0u) |
      (normalized_depth_control.backface_enable ? 2u : 0u);

  MTL::DepthStencilState* state = nullptr;
  auto it = depth_stencil_state_cache_.find(key);
  if (it != depth_stencil_state_cache_.end()) {
    state = it->second;
  } else {
    MTL::DepthStencilDescriptor* ds_desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    if (normalized_depth_control.z_enable) {
      ds_desc->setDepthCompareFunction(
          ToMetalCompareFunction(normalized_depth_control.zfunc));
      ds_desc->setDepthWriteEnabled(normalized_depth_control.z_write_enable !=
                                    0);
    } else {
      ds_desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
      ds_desc->setDepthWriteEnabled(false);
    }

    if (normalized_depth_control.stencil_enable) {
      auto* front = MTL::StencilDescriptor::alloc()->init();
      front->setStencilCompareFunction(
          ToMetalCompareFunction(normalized_depth_control.stencilfunc));
      front->setStencilFailureOperation(
          ToMetalStencilOperation(normalized_depth_control.stencilfail));
      front->setDepthFailureOperation(
          ToMetalStencilOperation(normalized_depth_control.stencilzfail));
      front->setDepthStencilPassOperation(
          ToMetalStencilOperation(normalized_depth_control.stencilzpass));
      front->setReadMask(stencil_ref_mask_front.stencilmask);
      front->setWriteMask(stencil_ref_mask_front.stencilwritemask);

      ds_desc->setFrontFaceStencil(front);

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        auto* back = MTL::StencilDescriptor::alloc()->init();
        back->setStencilCompareFunction(
            ToMetalCompareFunction(normalized_depth_control.stencilfunc_bf));
        back->setStencilFailureOperation(
            ToMetalStencilOperation(normalized_depth_control.stencilfail_bf));
        back->setDepthFailureOperation(
            ToMetalStencilOperation(normalized_depth_control.stencilzfail_bf));
        back->setDepthStencilPassOperation(
            ToMetalStencilOperation(normalized_depth_control.stencilzpass_bf));
        back->setReadMask(stencil_ref_mask_back.stencilmask);
        back->setWriteMask(stencil_ref_mask_back.stencilwritemask);
        ds_desc->setBackFaceStencil(back);
        back->release();
      } else {
        ds_desc->setBackFaceStencil(front);
      }

      front->release();
    }

    state = device_->newDepthStencilState(ds_desc);
    ds_desc->release();

    if (!state) {
      XELOGE("Failed to create Metal depth/stencil state");
      return;
    }
    depth_stencil_state_cache_.emplace(key, state);
  }

  current_render_encoder_->setDepthStencilState(state);

  if (normalized_depth_control.stencil_enable) {
    uint32_t ref_front = stencil_ref_mask_front.stencilref;
    uint32_t ref_back = stencil_ref_mask_back.stencilref;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    uint32_t ref = ref_front;
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      ref = ref_back;
    } else if (primitive_polygonal &&
               normalized_depth_control.backface_enable &&
               ref_front != ref_back) {
      static bool mismatch_logged = false;
      if (!mismatch_logged) {
        mismatch_logged = true;
        XELOGW(
            "Metal: front/back stencil ref differ (front={}, back={}); using "
            "front for both",
            ref_front, ref_back);
      }
    }
    current_render_encoder_->setStencilReferenceValue(ref);
  }
}

void MetalCommandProcessor::ApplyRasterizerState(bool primitive_polygonal) {
  if (!current_render_encoder_ || !render_target_cache_) {
    return;
  }

  const RegisterFile& regs = *register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();

  MTL::CullMode cull_mode = MTL::CullModeNone;
  if (primitive_polygonal) {
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    if (cull_front && !cull_back) {
      cull_mode = MTL::CullModeFront;
    } else if (cull_back && !cull_front) {
      cull_mode = MTL::CullModeBack;
    }
  }
  current_render_encoder_->setCullMode(cull_mode);

  current_render_encoder_->setFrontFacingWinding(
      pa_su_sc_mode_cntl.face ? MTL::WindingClockwise
                              : MTL::WindingCounterClockwise);

  MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;
  if (primitive_polygonal &&
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (!pa_su_sc_mode_cntl.cull_front) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
    }
    if (!pa_su_sc_mode_cntl.cull_back) {
      polygon_type =
          std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
    }
    if (polygon_type != xenos::PolygonType::kTriangles) {
      fill_mode = MTL::TriangleFillModeLines;
    }
  }
  current_render_encoder_->setTriangleFillMode(fill_mode);

  float polygon_offset_scale = 0.0f;
  float polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(
      regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
  float depth_bias_factor = regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                                    xenos::DepthRenderTargetFormat::kD24S8
                                ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                                : draw_util::kD3D10PolygonOffsetFactorFloat24;
  float depth_bias_constant = polygon_offset * depth_bias_factor;
  float depth_bias_slope =
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
      float(std::max(render_target_cache_->draw_resolution_scale_x(),
                     render_target_cache_->draw_resolution_scale_y()));
  current_render_encoder_->setDepthBias(depth_bias_constant, depth_bias_slope,
                                        0.0f);

  current_render_encoder_->setDepthClipMode(pa_cl_clip_cntl.clip_disable
                                                ? MTL::DepthClipModeClamp
                                                : MTL::DepthClipModeClip);
}

MTL::RenderPassDescriptor*
MetalCommandProcessor::GetCurrentRenderPassDescriptor() {
  return render_pass_descriptor_;
}

MTL::RenderPipelineState* MetalCommandProcessor::GetOrCreatePipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const RegisterFile& regs) {
  if (!vertex_translation || !vertex_translation->metal_function()) {
    XELOGE("No valid vertex shader function");
    return nullptr;
  }

  // Determine attachment formats and sample count from the render target cache
  // so the pipeline matches the actual render pass. If no real RT is bound,
  // fall back to the dummy RT0 format used by the cache.
  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (render_target_cache_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      switch (depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          stencil_format = depth_format;
          break;
        default:
          stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  struct PipelineKey {
    const void* vs;
    const void* ps;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};
  key_data.vs = vertex_translation;
  key_data.ps = pixel_translation;
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  uint32_t pixel_shader_writes_color_targets =
      pixel_translation ? pixel_translation->shader().writes_color_targets()
                        : 0;
  key_data.normalized_color_mask = 0;
  if (pixel_shader_writes_color_targets) {
    key_data.normalized_color_mask = draw_util::GetNormalizedColorMask(
        regs, pixel_shader_writes_color_targets);
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  key_data.alpha_to_mask_enable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.blendcontrol[i] =
        regs.Get<reg::RB_BLENDCONTROL>(
                reg::RB_BLENDCONTROL::rt_register_indices[i])
            .value;
  }
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  PipelineDiskCacheEntry disk_entry;
  bool record_disk_entry =
      ::cvars::metal_pipeline_disk_cache && pipeline_disk_cache_file_;
  if (record_disk_entry) {
    disk_entry.pipeline_key = key;
    disk_entry.vertex_shader_cache_key = MetalShaderCache::GetCacheKey(
        vertex_translation->shader().ucode_data_hash(),
        vertex_translation->modification(),
        static_cast<uint32_t>(vertex_translation->shader().type()));
    if (pixel_translation) {
      disk_entry.pixel_shader_cache_key = MetalShaderCache::GetCacheKey(
          pixel_translation->shader().ucode_data_hash(),
          pixel_translation->modification(),
          static_cast<uint32_t>(pixel_translation->shader().type()));
    }
    disk_entry.sample_count = key_data.sample_count;
    disk_entry.depth_format = key_data.depth_format;
    disk_entry.stencil_format = key_data.stencil_format;
    std::memcpy(disk_entry.color_formats, key_data.color_formats,
                sizeof(key_data.color_formats));
    disk_entry.normalized_color_mask = key_data.normalized_color_mask;
    disk_entry.alpha_to_mask_enable = key_data.alpha_to_mask_enable;
    std::memcpy(disk_entry.blendcontrol, key_data.blendcontrol,
                sizeof(key_data.blendcontrol));
  }

  // Check cache
  auto it = pipeline_cache_.find(key);
  if (it != pipeline_cache_.end()) {
    return it->second;
  }

  // Create pipeline descriptor
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();

  desc->setVertexFunction(vertex_translation->metal_function());

  if (pixel_translation && pixel_translation->metal_function()) {
    desc->setFragmentFunction(pixel_translation->metal_function());
  }

  // Set render target formats and sample count to match bound RTs.
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setSampleCount(sample_count);
  desc->setAlphaToCoverageEnabled(key_data.alpha_to_mask_enable != 0);

  // Fixed-function blending and color write masks.
  // These are part of the render pipeline state, so the cache key must include
  // the relevant register-derived values (mask, RB_BLENDCONTROL, A2C).
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    if (color_formats[i] == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (key_data.normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }

  // Configure vertex fetch layout for MSC stage-in.
  // NOTE: The translated shaders use vfetch (buffer load) to read vertices
  // directly from shared memory via SRV descriptors, NOT stage-in attributes.
  // This vertex descriptor may be unnecessary.
  const Shader& vertex_shader_ref = vertex_translation->shader();
  const auto& vertex_bindings = vertex_shader_ref.vertex_bindings();
  if (!ShaderUsesVertexFetch(vertex_shader_ref) && !vertex_bindings.empty()) {
    auto map_vertex_format =
        [](const ParsedVertexFetchInstruction::Attributes& attrs)
        -> MTL::VertexFormat {
      using xenos::VertexFormat;
      switch (attrs.data_format) {
        case VertexFormat::k_8_8_8_8:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatChar4
                                   : MTL::VertexFormatUChar4;
          }
          return attrs.is_signed ? MTL::VertexFormatChar4Normalized
                                 : MTL::VertexFormatUChar4Normalized;
        case VertexFormat::k_2_10_10_10:
          // Metal only supports normalized variants of 10:10:10:2.
          return attrs.is_signed ? MTL::VertexFormatInt1010102Normalized
                                 : MTL::VertexFormatUInt1010102Normalized;
        case VertexFormat::k_10_11_11:
        case VertexFormat::k_11_11_10:
          return MTL::VertexFormatFloatRG11B10;
        case VertexFormat::k_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort2
                                   : MTL::VertexFormatUShort2;
          }
          return attrs.is_signed ? MTL::VertexFormatShort2Normalized
                                 : MTL::VertexFormatUShort2Normalized;
        case VertexFormat::k_16_16_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort4
                                   : MTL::VertexFormatUShort4;
          }
          return attrs.is_signed ? MTL::VertexFormatShort4Normalized
                                 : MTL::VertexFormatUShort4Normalized;
        case VertexFormat::k_16_16_FLOAT:
          return MTL::VertexFormatHalf2;
        case VertexFormat::k_16_16_16_16_FLOAT:
          return MTL::VertexFormatHalf4;
        case VertexFormat::k_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt
                                   : MTL::VertexFormatUInt;
          }
          return MTL::VertexFormatFloat;
        case VertexFormat::k_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt2
                                   : MTL::VertexFormatUInt2;
          }
          return MTL::VertexFormatFloat2;
        case VertexFormat::k_32_32_32_FLOAT:
          return MTL::VertexFormatFloat3;
        case VertexFormat::k_32_32_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt4
                                   : MTL::VertexFormatUInt4;
          }
          return MTL::VertexFormatFloat4;
        case VertexFormat::k_32_32_32_32_FLOAT:
          return MTL::VertexFormatFloat4;
        default:
          return MTL::VertexFormatInvalid;
      }
    };

    MTL::VertexDescriptor* vertex_desc = MTL::VertexDescriptor::alloc()->init();
    if (record_disk_entry) {
      disk_entry.vertex_attributes.clear();
      disk_entry.vertex_layouts.clear();
    }

    uint32_t attr_index = static_cast<uint32_t>(kIRStageInAttributeStartIndex);
    for (const auto& binding : vertex_bindings) {
      uint64_t buffer_index =
          kIRVertexBufferBindPoint + uint64_t(binding.binding_index);
      bool used_any_attribute = false;

      for (const auto& attr : binding.attributes) {
        MTL::VertexFormat fmt = map_vertex_format(attr.fetch_instr.attributes);
        if (fmt == MTL::VertexFormatInvalid) {
          ++attr_index;
          continue;
        }
        auto attr_desc = vertex_desc->attributes()->object(attr_index);
        attr_desc->setFormat(fmt);
        attr_desc->setOffset(
            static_cast<NS::UInteger>(attr.fetch_instr.attributes.offset * 4));
        attr_desc->setBufferIndex(static_cast<NS::UInteger>(buffer_index));
        if (record_disk_entry) {
          PipelineDiskCacheVertexAttribute cached_attr = {};
          cached_attr.attribute_index = attr_index;
          cached_attr.format = static_cast<uint32_t>(fmt);
          cached_attr.offset = attr.fetch_instr.attributes.offset * 4;
          cached_attr.buffer_index = static_cast<uint32_t>(buffer_index);
          disk_entry.vertex_attributes.push_back(cached_attr);
        }
        used_any_attribute = true;
        ++attr_index;
      }

      if (used_any_attribute) {
        auto layout = vertex_desc->layouts()->object(buffer_index);
        layout->setStride(binding.stride_words * 4);
        layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        layout->setStepRate(1);
        if (record_disk_entry) {
          PipelineDiskCacheVertexLayout cached_layout = {};
          cached_layout.buffer_index = static_cast<uint32_t>(buffer_index);
          cached_layout.stride = binding.stride_words * 4;
          cached_layout.step_function =
              static_cast<uint32_t>(MTL::VertexStepFunctionPerVertex);
          cached_layout.step_rate = 1;
          disk_entry.vertex_layouts.push_back(cached_layout);
        }
      }
    }

    desc->setVertexDescriptor(vertex_desc);
    vertex_desc->release();
  }

  if (pipeline_binary_archive_) {
    NS::Array* archives = NS::Array::array(pipeline_binary_archive_);
    desc->setBinaryArchives(archives);
    NS::Error* archive_error = nullptr;
    if (pipeline_binary_archive_->addRenderPipelineFunctions(desc,
                                                             &archive_error)) {
      pipeline_binary_archive_dirty_ = true;
    }
  }

  // Create pipeline state
  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline = nullptr;
  pipeline = device_->newRenderPipelineState(desc, &error);
  desc->release();

  if (!pipeline) {
    if (error) {
      XELOGE("Failed to create pipeline state: {}",
             error->localizedDescription()->utf8String());
    } else {
      XELOGE("Failed to create pipeline state (unknown error)");
    }
    return nullptr;
  }

  pipeline_cache_[key] = pipeline;
  if (record_disk_entry) {
    AppendPipelineDiskCacheEntry(disk_entry);
  }

  return pipeline;
}

MetalCommandProcessor::GeometryPipelineState*
MetalCommandProcessor::GetOrCreateGeometryPipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key, const RegisterFile& regs) {
  if (!vertex_translation) {
    XELOGE("No valid vertex shader translation for geometry pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Library* pixel_library =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_library();
  const char* pixel_function = use_fallback_pixel_shader
                                   ? nullptr
                                   : pixel_translation->function_name().c_str();
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      XELOGE("Geometry pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    XELOGE("No valid pixel shader translation for geometry pipeline");
    return nullptr;
  }

  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (render_target_cache_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      switch (depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          stencil_format = depth_format;
          break;
        default:
          stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  struct GeometryPipelineKey {
    const void* vs;
    const void* ps;
    uint32_t geometry_key;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.vs = vertex_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.geometry_key = geometry_shader_key.key;
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  uint32_t pixel_shader_writes_color_targets =
      use_fallback_pixel_shader
          ? 0
          : (pixel_translation
                 ? pixel_translation->shader().writes_color_targets()
                 : 0);
  key_data.normalized_color_mask =
      pixel_shader_writes_color_targets
          ? draw_util::GetNormalizedColorMask(regs,
                                              pixel_shader_writes_color_targets)
          : 0;
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  key_data.alpha_to_mask_enable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.blendcontrol[i] =
        regs.Get<reg::RB_BLENDCONTROL>(
                reg::RB_BLENDCONTROL::rt_register_indices[i])
            .value;
  }
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = geometry_pipeline_cache_.find(key);
  if (it != geometry_pipeline_cache_.end()) {
    return &it->second;
  }

  auto get_vertex_stage = [&]() -> GeometryVertexStageState* {
    auto vertex_it = geometry_vertex_stage_cache_.find(vertex_translation);
    if (vertex_it != geometry_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    std::vector<uint8_t> dxil_data = vertex_translation->dxil_data();
    if (dxil_data.empty()) {
      std::string dxil_error;
      if (!dxbc_to_dxil_converter_->Convert(
              vertex_translation->translated_binary(), dxil_data,
              &dxil_error)) {
        XELOGE("Geometry VS: DXBC to DXIL conversion failed: {}", dxil_error);
        return nullptr;
      }
    }

    struct InputAttribute {
      uint32_t input_slot = 0;
      uint32_t offset = 0;
      IRFormat format = IRFormatUnknown;
    };
    std::vector<InputAttribute> attribute_map;
    attribute_map.reserve(32);

    auto map_ir_format =
        [](const ParsedVertexFetchInstruction::Attributes& attrs) -> IRFormat {
      using xenos::VertexFormat;
      switch (attrs.data_format) {
        case VertexFormat::k_8_8_8_8:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR8G8B8A8Sint
                                   : IRFormatR8G8B8A8Uint;
          }
          return attrs.is_signed ? IRFormatR8G8B8A8Snorm
                                 : IRFormatR8G8B8A8Unorm;
        case VertexFormat::k_2_10_10_10:
          if (attrs.is_integer) {
            return IRFormatR10G10B10A2Uint;
          }
          return IRFormatR10G10B10A2Unorm;
        case VertexFormat::k_10_11_11:
        case VertexFormat::k_11_11_10:
          return IRFormatR11G11B10Float;
        case VertexFormat::k_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR16G16Sint : IRFormatR16G16Uint;
          }
          return attrs.is_signed ? IRFormatR16G16Snorm : IRFormatR16G16Unorm;
        case VertexFormat::k_16_16_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR16G16B16A16Sint
                                   : IRFormatR16G16B16A16Uint;
          }
          return attrs.is_signed ? IRFormatR16G16B16A16Snorm
                                 : IRFormatR16G16B16A16Unorm;
        case VertexFormat::k_16_16_FLOAT:
          return IRFormatR16G16Float;
        case VertexFormat::k_16_16_16_16_FLOAT:
          return IRFormatR16G16B16A16Float;
        case VertexFormat::k_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32Sint : IRFormatR32Uint;
          }
          return IRFormatR32Float;
        case VertexFormat::k_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32G32Sint : IRFormatR32G32Uint;
          }
          return IRFormatR32G32Float;
        case VertexFormat::k_32_32_32_FLOAT:
          return IRFormatR32G32B32Float;
        case VertexFormat::k_32_32_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32G32B32A32Sint
                                   : IRFormatR32G32B32A32Uint;
          }
          return IRFormatR32G32B32A32Float;
        case VertexFormat::k_32_32_32_32_FLOAT:
          return IRFormatR32G32B32A32Float;
        default:
          return IRFormatUnknown;
      }
    };

    const Shader& vertex_shader_ref = vertex_translation->shader();
    const auto& vertex_bindings = vertex_shader_ref.vertex_bindings();
    uint32_t attr_index = 0;
    for (const auto& binding : vertex_bindings) {
      for (const auto& attr : binding.attributes) {
        if (attr_index >= 31) {
          break;
        }
        InputAttribute mapped = {};
        mapped.input_slot = static_cast<uint32_t>(binding.binding_index);
        mapped.offset =
            static_cast<uint32_t>(attr.fetch_instr.attributes.offset * 4);
        mapped.format = map_ir_format(attr.fetch_instr.attributes);
        attribute_map.push_back(mapped);
        ++attr_index;
      }
      if (attr_index >= 31) {
        break;
      }
    }

    IRInputTopology input_topology = IRInputTopologyUndefined;
    switch (geometry_shader_key.type) {
      case PipelineGeometryShader::kPointList:
        input_topology = IRInputTopologyPoint;
        break;
      case PipelineGeometryShader::kRectangleList:
        input_topology = IRInputTopologyTriangle;
        break;
      case PipelineGeometryShader::kQuadList:
        // Quad lists use LineWithAdjacency in DXBC; MSC input topology doesn't
        // model adjacency, so leave undefined to avoid mismatches.
        input_topology = IRInputTopologyUndefined;
        break;
      default:
        input_topology = IRInputTopologyUndefined;
        break;
    }
    MetalShaderConversionResult vertex_result;
    MetalShaderReflectionInfo vertex_reflection;

    // First pass: get reflection for vertex inputs.
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kVertex, dxil_data, vertex_result,
            &vertex_reflection, nullptr, nullptr, true,
            static_cast<int>(input_topology))) {
      XELOGE("Geometry VS: DXIL to Metal conversion failed: {}",
             vertex_result.error_message);
      return nullptr;
    }

    IRVersionedInputLayoutDescriptor input_layout = {};
    input_layout.version = IRInputLayoutDescriptorVersion_1;
    input_layout.desc_1_0.numElements = 0;
    std::vector<std::string> semantic_names_storage;
    if (!vertex_reflection.vertex_inputs.empty()) {
      semantic_names_storage.reserve(vertex_reflection.vertex_inputs.size());
      uint32_t element_count = 0;
      for (const auto& input : vertex_reflection.vertex_inputs) {
        if (element_count >= 31) {
          break;
        }
        if (input.attribute_index >= attribute_map.size()) {
          XELOGW("Geometry VS: vertex input {} out of range (max {})",
                 input.attribute_index, attribute_map.size());
          continue;
        }
        const InputAttribute& mapped = attribute_map[input.attribute_index];
        if (mapped.format == IRFormatUnknown) {
          XELOGW("Geometry VS: unknown IRFormat for vertex input {}",
                 input.attribute_index);
          continue;
        }
        std::string semantic_base = input.name;
        uint32_t semantic_index = 0;
        if (!semantic_base.empty()) {
          size_t digit_pos = semantic_base.size();
          while (digit_pos > 0 && std::isdigit(static_cast<unsigned char>(
                                      semantic_base[digit_pos - 1]))) {
            --digit_pos;
          }
          if (digit_pos < semantic_base.size()) {
            semantic_index = static_cast<uint32_t>(
                std::strtoul(semantic_base.c_str() + digit_pos, nullptr, 10));
            semantic_base.resize(digit_pos);
          }
        }
        if (semantic_base.empty()) {
          semantic_base = "TEXCOORD";
        }
        semantic_names_storage.push_back(std::move(semantic_base));
        input_layout.desc_1_0.semanticNames[element_count] =
            semantic_names_storage.back().c_str();
        IRInputElementDescriptor1& element =
            input_layout.desc_1_0.inputElementDescs[element_count];
        element.semanticIndex = semantic_index;
        element.format = mapped.format;
        element.inputSlot = mapped.input_slot;
        element.alignedByteOffset = mapped.offset;
        element.instanceDataStepRate = 0;
        element.inputSlotClass = IRInputClassificationPerVertexData;
        ++element_count;
      }
      input_layout.desc_1_0.numElements = element_count;
    }

    // Second pass: synthesize stage-in using the input layout.
    std::vector<uint8_t> stage_in_metallib;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kVertex, dxil_data, vertex_result,
            &vertex_reflection, &input_layout, &stage_in_metallib, true,
            static_cast<int>(input_topology))) {
      XELOGE("Geometry VS: DXIL to Metal conversion failed: {}",
             vertex_result.error_message);
      return nullptr;
    }
    if (stage_in_metallib.empty()) {
      XELOGE(
          "Geometry VS: Failed to synthesize stage-in function "
          "(vertex_inputs={}, output_size={})",
          vertex_reflection.vertex_input_count,
          vertex_reflection.vertex_output_size_in_bytes);
      return nullptr;
    }

    NS::Error* error = nullptr;
    dispatch_data_t vertex_data = dispatch_data_create(
        vertex_result.metallib_data.data(), vertex_result.metallib_data.size(),
        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* vertex_library = device_->newLibrary(vertex_data, &error);
    dispatch_release(vertex_data);
    if (!vertex_library) {
      XELOGE("Geometry VS: Failed to create Metal library: {}",
             error ? error->localizedDescription()->utf8String()
                   : "unknown error");
      return nullptr;
    }

    NS::Error* stage_in_error = nullptr;
    dispatch_data_t stage_in_data =
        dispatch_data_create(stage_in_metallib.data(), stage_in_metallib.size(),
                             nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* stage_in_library =
        device_->newLibrary(stage_in_data, &stage_in_error);
    dispatch_release(stage_in_data);
    if (!stage_in_library) {
      XELOGE("Geometry VS: Failed to create stage-in library: {}",
             stage_in_error
                 ? stage_in_error->localizedDescription()->utf8String()
                 : "unknown error");
      vertex_library->release();
      return nullptr;
    }

    GeometryVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result.function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      XELOGE(
          "Geometry VS: reflection returned zero output size "
          "(vertex_inputs={})",
          vertex_reflection.vertex_input_count);
    }

    auto [inserted_it, inserted] = geometry_vertex_stage_cache_.emplace(
        vertex_translation, std::move(state));
    return &inserted_it->second;
  };

  auto get_geometry_stage = [&]() -> GeometryShaderStageState* {
    auto geom_it = geometry_shader_stage_cache_.find(geometry_shader_key);
    if (geom_it != geometry_shader_stage_cache_.end()) {
      return &geom_it->second;
    }

    const std::vector<uint32_t>& dxbc_dwords =
        GetGeometryShader(geometry_shader_key);
    std::vector<uint8_t> dxbc_bytes(dxbc_dwords.size() * sizeof(uint32_t));
    std::memcpy(dxbc_bytes.data(), dxbc_dwords.data(), dxbc_bytes.size());

    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!dxbc_to_dxil_converter_->Convert(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Geometry GS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    IRInputTopology input_topology = IRInputTopologyUndefined;
    switch (geometry_shader_key.type) {
      case PipelineGeometryShader::kPointList:
        input_topology = IRInputTopologyPoint;
        break;
      case PipelineGeometryShader::kRectangleList:
        input_topology = IRInputTopologyTriangle;
        break;
      case PipelineGeometryShader::kQuadList:
        // Quad lists use LineWithAdjacency in DXBC; MSC input topology doesn't
        // model adjacency, so leave undefined to avoid mismatches.
        input_topology = IRInputTopologyUndefined;
        break;
      default:
        input_topology = IRInputTopologyUndefined;
        break;
    }
    MetalShaderConversionResult geometry_result;
    MetalShaderReflectionInfo geometry_reflection;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kGeometry, dxil_data, geometry_result,
            &geometry_reflection, nullptr, nullptr, true,
            static_cast<int>(input_topology))) {
      XELOGE("Geometry GS: DXIL to Metal conversion failed: {}",
             geometry_result.error_message);
      return nullptr;
    }
    if (!geometry_result.has_mesh_stage &&
        !geometry_result.has_geometry_stage) {
      XELOGE(
          "Geometry GS: MSC did not emit mesh or geometry stage (mesh={}, "
          "geometry={})",
          geometry_result.has_mesh_stage, geometry_result.has_geometry_stage);
      return nullptr;
    }
    if (!geometry_result.has_mesh_stage) {
      static bool mesh_missing_logged = false;
      if (!mesh_missing_logged) {
        mesh_missing_logged = true;
        XELOGW(
            "Geometry GS: MSC did not emit mesh stage; using geometry stage "
            "library");
      }
    }

    NS::Error* error = nullptr;
    dispatch_data_t geometry_data =
        dispatch_data_create(geometry_result.metallib_data.data(),
                             geometry_result.metallib_data.size(), nullptr,
                             DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* geometry_library = device_->newLibrary(geometry_data, &error);
    dispatch_release(geometry_data);
    if (!geometry_library) {
      XELOGE("Geometry GS: Failed to create Metal library: {}",
             error ? error->localizedDescription()->utf8String()
                   : "unknown error");
      return nullptr;
    }

    GeometryShaderStageState state;
    state.library = geometry_library;
    state.function_name = geometry_result.function_name;
    state.max_input_primitives_per_mesh_threadgroup =
        geometry_reflection.gs_max_input_primitives_per_mesh_threadgroup;
    state.function_constants = geometry_reflection.function_constants;
    if (state.max_input_primitives_per_mesh_threadgroup == 0) {
      XELOGE("Geometry GS: reflection returned zero max input primitives");
    }

    auto [inserted_it, inserted] = geometry_shader_stage_cache_.emplace(
        geometry_shader_key, std::move(state));
    return &inserted_it->second;
  };

  GeometryVertexStageState* vertex_stage = get_vertex_stage();
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }
  GeometryShaderStageState* geometry_stage = get_geometry_stage();
  if (!geometry_stage || !geometry_stage->library) {
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();

  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setRasterSampleCount(sample_count);
  desc->setAlphaToCoverageEnabled(key_data.alpha_to_mask_enable != 0);

  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    if (color_formats[i] == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (key_data.normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
  if (!vertex_stage->vertex_output_size_in_bytes ||
      !geometry_stage->max_input_primitives_per_mesh_threadgroup) {
    XELOGE(
        "Geometry pipeline: invalid reflection (vs_output={}, gs_max_input={})",
        vertex_stage->vertex_output_size_in_bytes,
        geometry_stage->max_input_primitives_per_mesh_threadgroup);
    return nullptr;
  }

  IRGeometryEmulationPipelineDescriptor ir_desc = {};
  ir_desc.stageInLibrary = vertex_stage->stage_in_library;
  ir_desc.vertexLibrary = vertex_stage->library;
  ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
  ir_desc.geometryLibrary = geometry_stage->library;
  ir_desc.geometryFunctionName = geometry_stage->function_name.c_str();
  ir_desc.fragmentLibrary = pixel_library;
  ir_desc.fragmentFunctionName = pixel_function;
  ir_desc.basePipelineDescriptor = desc;
  ir_desc.pipelineConfig.gsVertexSizeInBytes =
      vertex_stage->vertex_output_size_in_bytes;
  ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
      geometry_stage->max_input_primitives_per_mesh_threadgroup;

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      IRRuntimeNewGeometryEmulationPipeline(device_, &ir_desc, &error);
  desc->release();

  if (!pipeline) {
    XELOGE(
        "Failed to create geometry pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    LogMetalErrorDetails("Geometry pipeline error", error);
    return nullptr;
  }

  GeometryPipelineState state;
  state.pipeline = pipeline;
  state.gs_vertex_size_in_bytes = ir_desc.pipelineConfig.gsVertexSizeInBytes;
  state.gs_max_input_primitives_per_mesh_threadgroup =
      ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup;

  auto [inserted_it, inserted] =
      geometry_pipeline_cache_.emplace(key, std::move(state));
  return &inserted_it->second;
}

MetalCommandProcessor::TessellationPipelineState*
MetalCommandProcessor::GetOrCreateTessellationPipelineState(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const RegisterFile& regs) {
  if (!domain_translation) {
    XELOGE("No valid domain shader translation for tessellation pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  MTL::Library* pixel_library =
      use_fallback_pixel_shader ? nullptr : pixel_translation->metal_library();
  const char* pixel_function = use_fallback_pixel_shader
                                   ? nullptr
                                   : pixel_translation->function_name().c_str();
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      XELOGE("Tessellation pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    XELOGE("No valid pixel shader translation for tessellation pipeline");
    return nullptr;
  }

  uint32_t sample_count = 1;
  MTL::PixelFormat color_formats[4] = {
      MTL::PixelFormatInvalid, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid,
      MTL::PixelFormatInvalid};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  if (render_target_cache_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (MTL::Texture* rt = render_target_cache_->GetColorTargetForDraw(i)) {
        color_formats[i] = rt->pixelFormat();
        if (rt->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(rt->sampleCount()));
        }
      }
    }
    if (color_formats[0] == MTL::PixelFormatInvalid) {
      if (MTL::Texture* dummy =
              render_target_cache_->GetDummyColorTargetForDraw()) {
        color_formats[0] = dummy->pixelFormat();
        if (dummy->sampleCount() > 0) {
          sample_count = std::max<uint32_t>(
              sample_count, static_cast<uint32_t>(dummy->sampleCount()));
        }
      }
    }
    if (MTL::Texture* depth_tex =
            render_target_cache_->GetDepthTargetForDraw()) {
      depth_format = depth_tex->pixelFormat();
      switch (depth_format) {
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
          stencil_format = depth_format;
          break;
        default:
          stencil_format = MTL::PixelFormatInvalid;
          break;
      }
      if (depth_tex->sampleCount() > 0) {
        sample_count = std::max<uint32_t>(
            sample_count, static_cast<uint32_t>(depth_tex->sampleCount()));
      }
    }
  }

  struct TessellationPipelineKey {
    const void* ds;
    const void* ps;
    uint32_t host_vs_type;
    uint32_t tessellation_mode;
    uint32_t host_prim;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.ds = domain_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.host_vs_type =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  key_data.tessellation_mode =
      uint32_t(primitive_processing_result.tessellation_mode);
  key_data.host_prim =
      uint32_t(primitive_processing_result.host_primitive_type);
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  uint32_t pixel_shader_writes_color_targets =
      use_fallback_pixel_shader
          ? 0
          : (pixel_translation
                 ? pixel_translation->shader().writes_color_targets()
                 : 0);
  key_data.normalized_color_mask =
      pixel_shader_writes_color_targets
          ? draw_util::GetNormalizedColorMask(regs,
                                              pixel_shader_writes_color_targets)
          : 0;
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  key_data.alpha_to_mask_enable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.blendcontrol[i] =
        regs.Get<reg::RB_BLENDCONTROL>(
                reg::RB_BLENDCONTROL::rt_register_indices[i])
            .value;
  }
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = tessellation_pipeline_cache_.find(key);
  if (it != tessellation_pipeline_cache_.end()) {
    return &it->second;
  }

  xenos::TessellationMode tessellation_mode =
      primitive_processing_result.tessellation_mode;

  auto get_vertex_stage = [&]() -> TessellationVertexStageState* {
    struct VertexStageKey {
      const void* shader;
      uint32_t tessellation_mode;
    } vertex_key = {domain_translation, uint32_t(tessellation_mode)};
    uint32_t vertex_key_hash =
        uint32_t(XXH3_64bits(&vertex_key, sizeof(vertex_key)));
    auto vertex_it = tessellation_vertex_stage_cache_.find(vertex_key_hash);
    if (vertex_it != tessellation_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    const uint8_t* vs_bytes = nullptr;
    size_t vs_size = 0;
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      vs_bytes = ::tessellation_adaptive_vs;
      vs_size = sizeof(::tessellation_adaptive_vs);
    } else {
      vs_bytes = ::tessellation_indexed_vs;
      vs_size = sizeof(::tessellation_indexed_vs);
    }
    std::vector<uint8_t> dxbc_bytes(vs_bytes, vs_bytes + vs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!dxbc_to_dxil_converter_->Convert(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Tessellation VS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    struct InputAttribute {
      uint32_t input_slot = 0;
      uint32_t offset = 0;
      IRFormat format = IRFormatUnknown;
    };
    std::vector<InputAttribute> attribute_map;
    attribute_map.reserve(32);

    auto map_ir_format =
        [](const ParsedVertexFetchInstruction::Attributes& attrs) -> IRFormat {
      using xenos::VertexFormat;
      switch (attrs.data_format) {
        case VertexFormat::k_8_8_8_8:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR8G8B8A8Sint
                                   : IRFormatR8G8B8A8Uint;
          }
          return attrs.is_signed ? IRFormatR8G8B8A8Snorm
                                 : IRFormatR8G8B8A8Unorm;
        case VertexFormat::k_2_10_10_10:
          if (attrs.is_integer) {
            return IRFormatR10G10B10A2Uint;
          }
          return IRFormatR10G10B10A2Unorm;
        case VertexFormat::k_10_11_11:
        case VertexFormat::k_11_11_10:
          return IRFormatR11G11B10Float;
        case VertexFormat::k_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR16G16Sint : IRFormatR16G16Uint;
          }
          return attrs.is_signed ? IRFormatR16G16Snorm : IRFormatR16G16Unorm;
        case VertexFormat::k_16_16_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR16G16B16A16Sint
                                   : IRFormatR16G16B16A16Uint;
          }
          return attrs.is_signed ? IRFormatR16G16B16A16Snorm
                                 : IRFormatR16G16B16A16Unorm;
        case VertexFormat::k_16_16_FLOAT:
          return IRFormatR16G16Float;
        case VertexFormat::k_16_16_16_16_FLOAT:
          return IRFormatR16G16B16A16Float;
        case VertexFormat::k_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32Sint : IRFormatR32Uint;
          }
          return IRFormatR32Float;
        case VertexFormat::k_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32G32Sint : IRFormatR32G32Uint;
          }
          return IRFormatR32G32Float;
        case VertexFormat::k_32_32_32_FLOAT:
          return IRFormatR32G32B32Float;
        case VertexFormat::k_32_32_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? IRFormatR32G32B32A32Sint
                                   : IRFormatR32G32B32A32Uint;
          }
          return IRFormatR32G32B32A32Float;
        case VertexFormat::k_32_32_32_32_FLOAT:
          return IRFormatR32G32B32A32Float;
        default:
          return IRFormatUnknown;
      }
    };

    const Shader& vertex_shader_ref = domain_translation->shader();
    const auto& vertex_bindings = vertex_shader_ref.vertex_bindings();
    uint32_t attr_index = 0;
    for (const auto& binding : vertex_bindings) {
      for (const auto& attr : binding.attributes) {
        if (attr_index >= 31) {
          break;
        }
        InputAttribute mapped = {};
        mapped.input_slot = static_cast<uint32_t>(binding.binding_index);
        mapped.offset =
            static_cast<uint32_t>(attr.fetch_instr.attributes.offset * 4);
        mapped.format = map_ir_format(attr.fetch_instr.attributes);
        attribute_map.push_back(mapped);
        ++attr_index;
      }
      if (attr_index >= 31) {
        break;
      }
    }

    IRInputTopology input_topology = IRInputTopologyUndefined;

    MetalShaderConversionResult vertex_result;
    MetalShaderReflectionInfo vertex_reflection;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kVertex, dxil_data, vertex_result,
            &vertex_reflection, nullptr, nullptr, true,
            static_cast<int>(input_topology))) {
      XELOGE("Tessellation VS: DXIL to Metal conversion failed: {}",
             vertex_result.error_message);
      return nullptr;
    }

    IRVersionedInputLayoutDescriptor input_layout = {};
    input_layout.version = IRInputLayoutDescriptorVersion_1;
    input_layout.desc_1_0.numElements = 0;
    std::vector<std::string> semantic_names_storage;
    if (!vertex_reflection.vertex_inputs.empty()) {
      semantic_names_storage.reserve(vertex_reflection.vertex_inputs.size());
      uint32_t element_count = 0;
      for (const auto& input : vertex_reflection.vertex_inputs) {
        if (element_count >= 31) {
          break;
        }
        if (input.attribute_index >= attribute_map.size()) {
          XELOGW("Tessellation VS: vertex input {} out of range (max {})",
                 input.attribute_index, attribute_map.size());
          continue;
        }
        const InputAttribute& mapped = attribute_map[input.attribute_index];
        if (mapped.format == IRFormatUnknown) {
          XELOGW("Tessellation VS: unknown IRFormat for vertex input {}",
                 input.attribute_index);
          continue;
        }
        std::string semantic_base = input.name;
        uint32_t semantic_index = 0;
        if (!semantic_base.empty()) {
          size_t digit_pos = semantic_base.size();
          while (digit_pos > 0 && std::isdigit(static_cast<unsigned char>(
                                      semantic_base[digit_pos - 1]))) {
            --digit_pos;
          }
          if (digit_pos < semantic_base.size()) {
            semantic_index = static_cast<uint32_t>(
                std::strtoul(semantic_base.c_str() + digit_pos, nullptr, 10));
            semantic_base.resize(digit_pos);
          }
        }
        if (semantic_base.empty()) {
          semantic_base = "TEXCOORD";
        }
        semantic_names_storage.push_back(std::move(semantic_base));
        input_layout.desc_1_0.semanticNames[element_count] =
            semantic_names_storage.back().c_str();
        IRInputElementDescriptor1& element =
            input_layout.desc_1_0.inputElementDescs[element_count];
        element.semanticIndex = semantic_index;
        element.format = mapped.format;
        element.inputSlot = mapped.input_slot;
        element.alignedByteOffset = mapped.offset;
        element.instanceDataStepRate = 0;
        element.inputSlotClass = IRInputClassificationPerVertexData;
        ++element_count;
      }
      input_layout.desc_1_0.numElements = element_count;
    }

    std::vector<uint8_t> stage_in_metallib;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kVertex, dxil_data, vertex_result,
            &vertex_reflection, &input_layout, &stage_in_metallib, true,
            static_cast<int>(input_topology))) {
      XELOGE("Tessellation VS: DXIL to Metal conversion failed: {}",
             vertex_result.error_message);
      return nullptr;
    }
    if (stage_in_metallib.empty()) {
      XELOGE("Tessellation VS: Failed to synthesize stage-in function");
      return nullptr;
    }

    NS::Error* error = nullptr;
    dispatch_data_t vertex_data = dispatch_data_create(
        vertex_result.metallib_data.data(), vertex_result.metallib_data.size(),
        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* vertex_library = device_->newLibrary(vertex_data, &error);
    dispatch_release(vertex_data);
    if (!vertex_library) {
      XELOGE("Tessellation VS: Failed to create Metal library: {}",
             error ? error->localizedDescription()->utf8String()
                   : "unknown error");
      return nullptr;
    }

    NS::Error* stage_in_error = nullptr;
    dispatch_data_t stage_in_data =
        dispatch_data_create(stage_in_metallib.data(), stage_in_metallib.size(),
                             nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* stage_in_library =
        device_->newLibrary(stage_in_data, &stage_in_error);
    dispatch_release(stage_in_data);
    if (!stage_in_library) {
      XELOGE("Tessellation VS: Failed to create stage-in library: {}",
             stage_in_error
                 ? stage_in_error->localizedDescription()->utf8String()
                 : "unknown error");
      vertex_library->release();
      return nullptr;
    }

    TessellationVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result.function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      XELOGE("Tessellation VS: reflection returned zero output size");
    }

    auto [inserted_it, inserted] = tessellation_vertex_stage_cache_.emplace(
        vertex_key_hash, std::move(state));
    return &inserted_it->second;
  };

  auto get_hull_stage = [&]() -> TessellationHullStageState* {
    struct HullStageKey {
      uint32_t host_vs_type;
      uint32_t tessellation_mode;
    } hull_key = {uint32_t(primitive_processing_result.host_vertex_shader_type),
                  uint32_t(tessellation_mode)};
    uint64_t hull_key_hash = XXH3_64bits(&hull_key, sizeof(hull_key));
    auto hull_it = tessellation_hull_stage_cache_.find(hull_key_hash);
    if (hull_it != tessellation_hull_stage_cache_.end()) {
      return &hull_it->second;
    }

    const uint8_t* hs_bytes = nullptr;
    size_t hs_size = 0;
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::discrete_triangle_3cp_hs;
            hs_size = sizeof(::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::discrete_triangle_1cp_hs;
            hs_size = sizeof(::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::discrete_quad_4cp_hs;
            hs_size = sizeof(::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::discrete_quad_1cp_hs;
            hs_size = sizeof(::discrete_quad_1cp_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::continuous_triangle_3cp_hs;
            hs_size = sizeof(::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::continuous_triangle_1cp_hs;
            hs_size = sizeof(::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::continuous_quad_4cp_hs;
            hs_size = sizeof(::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::continuous_quad_1cp_hs;
            hs_size = sizeof(::continuous_quad_1cp_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::adaptive_triangle_hs;
            hs_size = sizeof(::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::adaptive_quad_hs;
            hs_size = sizeof(::adaptive_quad_hs);
            break;
          default:
            XELOGE(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      default:
        XELOGE("Tessellation HS: unsupported tessellation mode {}",
               uint32_t(tessellation_mode));
        return nullptr;
    }

    std::vector<uint8_t> dxbc_bytes(hs_bytes, hs_bytes + hs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!dxbc_to_dxil_converter_->Convert(dxbc_bytes, dxil_data, &dxil_error)) {
      XELOGE("Tessellation HS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    MetalShaderConversionResult hull_result;
    MetalShaderReflectionInfo hull_reflection;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kHull, dxil_data, hull_result, &hull_reflection,
            nullptr, nullptr, true,
            static_cast<int>(IRInputTopologyUndefined))) {
      XELOGE("Tessellation HS: DXIL to Metal conversion failed: {}",
             hull_result.error_message);
      return nullptr;
    }

    NS::Error* error = nullptr;
    dispatch_data_t hull_data = dispatch_data_create(
        hull_result.metallib_data.data(), hull_result.metallib_data.size(),
        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* hull_library = device_->newLibrary(hull_data, &error);
    dispatch_release(hull_data);
    if (!hull_library) {
      XELOGE("Tessellation HS: Failed to create Metal library: {}",
             error ? error->localizedDescription()->utf8String()
                   : "unknown error");
      return nullptr;
    }

    TessellationHullStageState state;
    state.library = hull_library;
    state.function_name = hull_result.function_name;
    state.reflection = hull_reflection;
    if (!state.reflection.has_hull_info) {
      XELOGE("Tessellation HS: reflection missing hull info");
    }

    auto [inserted_it, inserted] =
        tessellation_hull_stage_cache_.emplace(hull_key_hash, std::move(state));
    return &inserted_it->second;
  };

  auto get_domain_stage = [&]() -> TessellationDomainStageState* {
    uint64_t domain_key =
        XXH3_64bits(&domain_translation, sizeof(domain_translation));
    auto domain_it = tessellation_domain_stage_cache_.find(domain_key);
    if (domain_it != tessellation_domain_stage_cache_.end()) {
      return &domain_it->second;
    }

    std::vector<uint8_t> dxil_data = domain_translation->dxil_data();
    if (dxil_data.empty()) {
      std::string dxil_error;
      if (!dxbc_to_dxil_converter_->Convert(
              domain_translation->translated_binary(), dxil_data,
              &dxil_error)) {
        XELOGE("Tessellation DS: DXBC to DXIL conversion failed: {}",
               dxil_error);
        return nullptr;
      }
    }

    MetalShaderConversionResult domain_result;
    MetalShaderReflectionInfo domain_reflection;
    if (!metal_shader_converter_->ConvertWithStageEx(
            MetalShaderStage::kDomain, dxil_data, domain_result,
            &domain_reflection, nullptr, nullptr, true,
            static_cast<int>(IRInputTopologyUndefined))) {
      XELOGE("Tessellation DS: DXIL to Metal conversion failed: {}",
             domain_result.error_message);
      return nullptr;
    }

    NS::Error* error = nullptr;
    dispatch_data_t domain_data = dispatch_data_create(
        domain_result.metallib_data.data(), domain_result.metallib_data.size(),
        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* domain_library = device_->newLibrary(domain_data, &error);
    dispatch_release(domain_data);
    if (!domain_library) {
      XELOGE("Tessellation DS: Failed to create Metal library: {}",
             error ? error->localizedDescription()->utf8String()
                   : "unknown error");
      return nullptr;
    }

    TessellationDomainStageState state;
    state.library = domain_library;
    state.function_name = domain_result.function_name;
    state.reflection = domain_reflection;
    if (!state.reflection.has_domain_info) {
      XELOGE("Tessellation DS: reflection missing domain info");
    }

    auto [inserted_it, inserted] =
        tessellation_domain_stage_cache_.emplace(domain_key, std::move(state));
    return &inserted_it->second;
  };

  TessellationVertexStageState* vertex_stage = get_vertex_stage();
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }
  TessellationHullStageState* hull_stage = get_hull_stage();
  if (!hull_stage || !hull_stage->library) {
    return nullptr;
  }
  TessellationDomainStageState* domain_stage = get_domain_stage();
  if (!domain_stage || !domain_stage->library) {
    return nullptr;
  }

  IRRuntimeTessellatorOutputPrimitive output_primitive =
      IRRuntimeTessellatorOutputUndefined;
  switch (hull_stage->reflection.hs_tessellator_output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      output_primitive = IRRuntimeTessellatorOutputPoint;
      break;
    case IRRuntimeTessellatorOutputLine:
      output_primitive = IRRuntimeTessellatorOutputLine;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCW;
      break;
    case IRRuntimeTessellatorOutputTriangleCCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCCW;
      break;
    default:
      XELOGE("Tessellation pipeline: unsupported tessellator output {}",
             hull_stage->reflection.hs_tessellator_output_primitive);
      return nullptr;
  }

  IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
  const char* geometry_function = kIRTrianglePassthroughGeometryShader;
  switch (output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      geometry_primitive = IRRuntimePrimitiveTypePoint;
      geometry_function = kIRPointPassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputLine:
      geometry_primitive = IRRuntimePrimitiveTypeLine;
      geometry_function = kIRLinePassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
    case IRRuntimeTessellatorOutputTriangleCCW:
      geometry_primitive = IRRuntimePrimitiveTypeTriangle;
      geometry_function = kIRTrianglePassthroughGeometryShader;
      break;
    default:
      break;
  }

  if (!IRRuntimeValidateTessellationPipeline(
          output_primitive, geometry_primitive,
          hull_stage->reflection.hs_output_control_point_size,
          domain_stage->reflection.ds_input_control_point_size,
          hull_stage->reflection.hs_patch_constants_size,
          domain_stage->reflection.ds_patch_constants_size,
          hull_stage->reflection.hs_output_control_point_count,
          domain_stage->reflection.ds_input_control_point_count)) {
    XELOGE("Tessellation pipeline: validation failed for HS/DS pairing");
    return nullptr;
  }

  MTL::MeshRenderPipelineDescriptor* desc =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(depth_format);
  desc->setStencilAttachmentPixelFormat(stencil_format);
  desc->setRasterSampleCount(sample_count);
  desc->setAlphaToCoverageEnabled(key_data.alpha_to_mask_enable != 0);

  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    if (color_formats[i] == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }
    uint32_t rt_write_mask = (key_data.normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    MTL::BlendFactor src_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_srcblend);
    MTL::BlendFactor dst_rgb =
        ToMetalBlendFactorRgb(blendcontrol.color_destblend);
    MTL::BlendOperation op_rgb =
        ToMetalBlendOperation(blendcontrol.color_comb_fcn);
    MTL::BlendFactor src_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_srcblend);
    MTL::BlendFactor dst_alpha =
        ToMetalBlendFactorAlpha(blendcontrol.alpha_destblend);
    MTL::BlendOperation op_alpha =
        ToMetalBlendOperation(blendcontrol.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
  IRGeometryTessellationEmulationPipelineDescriptor ir_desc = {};
  ir_desc.stageInLibrary = vertex_stage->stage_in_library;
  ir_desc.vertexLibrary = vertex_stage->library;
  ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
  ir_desc.hullLibrary = hull_stage->library;
  ir_desc.hullFunctionName = hull_stage->function_name.c_str();
  ir_desc.domainLibrary = domain_stage->library;
  ir_desc.domainFunctionName = domain_stage->function_name.c_str();
  ir_desc.geometryLibrary = nullptr;
  ir_desc.geometryFunctionName = geometry_function;
  ir_desc.fragmentLibrary = pixel_library;
  ir_desc.fragmentFunctionName = pixel_function;
  ir_desc.basePipelineDescriptor = desc;
  ir_desc.pipelineConfig.outputPrimitiveType = output_primitive;
  ir_desc.pipelineConfig.vsOutputSizeInBytes =
      vertex_stage->vertex_output_size_in_bytes;
  ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
      domain_stage->reflection.ds_max_input_prims_per_mesh_threadgroup;
  ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup =
      hull_stage->reflection.hs_max_patches_per_object_threadgroup;
  ir_desc.pipelineConfig.hsInputControlPointCount =
      hull_stage->reflection.hs_input_control_point_count;
  ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup =
      hull_stage->reflection.hs_max_object_threads_per_patch;
  ir_desc.pipelineConfig.hsMaxTessellationFactor =
      hull_stage->reflection.hs_max_tessellation_factor;
  ir_desc.pipelineConfig.gsInstanceCount = 1;

  if (!ir_desc.pipelineConfig.vsOutputSizeInBytes ||
      !ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup ||
      !ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup ||
      !ir_desc.pipelineConfig.hsInputControlPointCount ||
      !ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup) {
    XELOGE(
        "Tessellation pipeline: invalid reflection values (vs_output={}, "
        "gs_max_input={}, hs_patches={}, hs_cp_count={}, hs_threads={})",
        ir_desc.pipelineConfig.vsOutputSizeInBytes,
        ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
        ir_desc.pipelineConfig.hsMaxPatchesPerObjectThreadgroup,
        ir_desc.pipelineConfig.hsInputControlPointCount,
        ir_desc.pipelineConfig.hsMaxObjectThreadsPerThreadgroup);
    desc->release();
    return nullptr;
  }

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      IRRuntimeNewGeometryTessellationEmulationPipeline(device_, &ir_desc,
                                                        &error);
  desc->release();
  if (!pipeline) {
    XELOGE(
        "Failed to create tessellation pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    return nullptr;
  }

  TessellationPipelineState state;
  state.pipeline = pipeline;
  state.config = ir_desc.pipelineConfig;
  state.primitive = geometry_primitive;

  auto [inserted_it, inserted] =
      tessellation_pipeline_cache_.emplace(key, std::move(state));
  return &inserted_it->second;
}

bool MetalCommandProcessor::EnsureDepthOnlyPixelShader() {
  if (depth_only_pixel_library_) {
    return true;
  }
  if (!shader_translator_ || !dxbc_to_dxil_converter_ ||
      !metal_shader_converter_) {
    XELOGE("Depth-only PS: shader translation not initialized");
    return false;
  }

  std::vector<uint8_t> dxbc_data =
      shader_translator_->CreateDepthOnlyPixelShader();
  if (dxbc_data.empty()) {
    XELOGE("Depth-only PS: failed to create DXBC");
    return false;
  }

  std::vector<uint8_t> dxil_data;
  std::string dxil_error;
  if (!dxbc_to_dxil_converter_->Convert(dxbc_data, dxil_data, &dxil_error)) {
    XELOGE("Depth-only PS: DXBC to DXIL conversion failed: {}", dxil_error);
    return false;
  }

  MetalShaderConversionResult result;
  if (!metal_shader_converter_->ConvertWithStage(MetalShaderStage::kFragment,
                                                 dxil_data, result)) {
    XELOGE("Depth-only PS: DXIL to Metal conversion failed: {}",
           result.error_message);
    return false;
  }

  NS::Error* error = nullptr;
  dispatch_data_t lib_data = dispatch_data_create(
      result.metallib_data.data(), result.metallib_data.size(), nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  depth_only_pixel_library_ = device_->newLibrary(lib_data, &error);
  dispatch_release(lib_data);
  if (!depth_only_pixel_library_) {
    XELOGE("Depth-only PS: Failed to create Metal library: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  depth_only_pixel_function_name_ = result.function_name;
  if (depth_only_pixel_function_name_.empty()) {
    XELOGE("Depth-only PS: missing function name");
    return false;
  }
  return true;
}

bool MetalCommandProcessor::CreateIRConverterBuffers() {
  // Buffer creation is now done inline in SetupContext
  // This function exists for header compatibility
  return res_heap_ab_ && smp_heap_ab_ && uniforms_buffer_;
}

std::shared_ptr<MetalCommandProcessor::DrawRingBuffers>
MetalCommandProcessor::CreateDrawRingBuffers() {
  if (!device_) {
    XELOGE("CreateDrawRingBuffers: Metal device is null");
    return nullptr;
  }
  if (!null_buffer_ || !null_texture_ || !null_sampler_) {
    XELOGE("CreateDrawRingBuffers: Null resources not initialized");
    return nullptr;
  }

  static uint32_t ring_id = 0;
  auto ring = std::make_shared<DrawRingBuffers>();

  const size_t kDescriptorTableCount = kStageCount * draw_ring_count_;
  const size_t kResourceHeapSlots =
      kResourceHeapSlotsPerTable * kDescriptorTableCount;
  const size_t kUavTableBaseIndex = kResourceHeapSlots;
  const size_t kResourceHeapSlotsTotal = kResourceHeapSlots * 2;
  const size_t kResourceHeapBytes =
      kResourceHeapSlotsTotal * sizeof(IRDescriptorTableEntry);
  const size_t kSamplerHeapSlots =
      kSamplerHeapSlotsPerTable * kDescriptorTableCount;
  const size_t kSamplerHeapBytes =
      kSamplerHeapSlots * sizeof(IRDescriptorTableEntry);
  const size_t kUniformsBufferSize =
      kUniformsBytesPerTable * kDescriptorTableCount;
  const size_t kTopLevelABTotalBytes =
      kTopLevelABBytesPerTable * kDescriptorTableCount;
  const size_t kDrawArgsSize = 64;  // Enough for draw arguments struct
  const size_t kCBVHeapSlots = kCbvHeapSlotsPerTable * kDescriptorTableCount;
  const size_t kCBVHeapBytes = kCBVHeapSlots * sizeof(IRDescriptorTableEntry);

  ring->res_heap_ab =
      device_->newBuffer(kResourceHeapBytes, MTL::ResourceStorageModeShared);
  if (!ring->res_heap_ab) {
    XELOGE("Failed to create resource descriptor heap buffer");
    return nullptr;
  }
  std::string ring_label_suffix = std::to_string(ring_id);
  ring->res_heap_ab->setLabel(NS::String::string(
      ("ResourceDescriptorHeap_" + ring_label_suffix).c_str(),
      NS::UTF8StringEncoding));

  // Initialize all tables:
  // - Slot 0: null buffer (will be replaced with shared memory per draw).
  // - Slots 1+: null texture (safe default for any accidental access).
  auto* res_entries =
      reinterpret_cast<IRDescriptorTableEntry*>(ring->res_heap_ab->contents());
  auto* uav_entries = res_entries + kUavTableBaseIndex;
  for (size_t table = 0; table < kDescriptorTableCount; ++table) {
    IRDescriptorTableEntry* table_entries =
        res_entries + table * kResourceHeapSlotsPerTable;
    IRDescriptorTableSetBuffer(&table_entries[0], null_buffer_->gpuAddress(),
                               kNullBufferSize);
    for (size_t i = 1; i < kResourceHeapSlotsPerTable; ++i) {
      IRDescriptorTableSetTexture(&table_entries[i], null_texture_, 0.0f, 0);
    }
    IRDescriptorTableEntry* uav_table_entries =
        uav_entries + table * kResourceHeapSlotsPerTable;
    for (size_t i = 0; i < kResourceHeapSlotsPerTable; ++i) {
      IRDescriptorTableSetBuffer(&uav_table_entries[i],
                                 null_buffer_->gpuAddress(), kNullBufferSize);
    }
  }

  ring->smp_heap_ab =
      device_->newBuffer(kSamplerHeapBytes, MTL::ResourceStorageModeShared);
  if (!ring->smp_heap_ab) {
    XELOGE("Failed to create sampler descriptor heap buffer");
    return nullptr;
  }
  ring->smp_heap_ab->setLabel(
      NS::String::string(("SamplerDescriptorHeap_" + ring_label_suffix).c_str(),
                         NS::UTF8StringEncoding));
  auto* smp_entries =
      reinterpret_cast<IRDescriptorTableEntry*>(ring->smp_heap_ab->contents());
  for (size_t i = 0; i < kSamplerHeapSlots; ++i) {
    IRDescriptorTableSetSampler(&smp_entries[i], null_sampler_, 0.0f);
  }

  ring->uniforms_buffer =
      device_->newBuffer(kUniformsBufferSize, MTL::ResourceStorageModeShared);
  if (!ring->uniforms_buffer) {
    XELOGE("Failed to create uniforms buffer");
    return nullptr;
  }
  ring->uniforms_buffer->setLabel(NS::String::string(
      ("UniformsBuffer_" + ring_label_suffix).c_str(), NS::UTF8StringEncoding));
  std::memset(ring->uniforms_buffer->contents(), 0, kUniformsBufferSize);

  ring->top_level_ab =
      device_->newBuffer(kTopLevelABTotalBytes, MTL::ResourceStorageModeShared);
  if (!ring->top_level_ab) {
    XELOGE("Failed to create top-level argument buffer");
    return nullptr;
  }
  ring->top_level_ab->setLabel(NS::String::string(
      ("TopLevelArgumentBuffer_" + ring_label_suffix).c_str(),
      NS::UTF8StringEncoding));
  std::memset(ring->top_level_ab->contents(), 0, kTopLevelABTotalBytes);

  ring->draw_args_buffer =
      device_->newBuffer(kDrawArgsSize, MTL::ResourceStorageModeShared);
  if (!ring->draw_args_buffer) {
    XELOGE("Failed to create draw arguments buffer");
    return nullptr;
  }
  ring->draw_args_buffer->setLabel(
      NS::String::string(("DrawArgumentsBuffer_" + ring_label_suffix).c_str(),
                         NS::UTF8StringEncoding));
  std::memset(ring->draw_args_buffer->contents(), 0, kDrawArgsSize);

  ring->cbv_heap_ab =
      device_->newBuffer(kCBVHeapBytes, MTL::ResourceStorageModeShared);
  if (!ring->cbv_heap_ab) {
    XELOGE("Failed to create CBV descriptor heap buffer");
    return nullptr;
  }
  ring->cbv_heap_ab->setLabel(
      NS::String::string(("CBVDescriptorHeap_" + ring_label_suffix).c_str(),
                         NS::UTF8StringEncoding));
  std::memset(ring->cbv_heap_ab->contents(), 0, kCBVHeapBytes);

  ++ring_id;

  return ring;
}

std::shared_ptr<MetalCommandProcessor::DrawRingBuffers>
MetalCommandProcessor::AcquireDrawRingBuffers() {
  std::lock_guard<std::mutex> lock(draw_ring_mutex_);
  if (!draw_ring_pool_.empty()) {
    auto ring = draw_ring_pool_.back();
    draw_ring_pool_.pop_back();
    return ring;
  }
  return CreateDrawRingBuffers();
}

void MetalCommandProcessor::SetActiveDrawRing(
    const std::shared_ptr<DrawRingBuffers>& ring) {
  active_draw_ring_ = ring;
  res_heap_ab_ = ring ? ring->res_heap_ab : nullptr;
  smp_heap_ab_ = ring ? ring->smp_heap_ab : nullptr;
  cbv_heap_ab_ = ring ? ring->cbv_heap_ab : nullptr;
  uniforms_buffer_ = ring ? ring->uniforms_buffer : nullptr;
  top_level_ab_ = ring ? ring->top_level_ab : nullptr;
  draw_args_buffer_ = ring ? ring->draw_args_buffer : nullptr;
}

void MetalCommandProcessor::EnsureActiveDrawRing() {
  if (!active_draw_ring_) {
    auto ring = AcquireDrawRingBuffers();
    if (!ring) {
      return;
    }
    SetActiveDrawRing(ring);
  }
  if (command_buffer_draw_rings_.empty()) {
    command_buffer_draw_rings_.push_back(active_draw_ring_);
    current_draw_index_ = 0;
  }
}

void MetalCommandProcessor::ScheduleDrawRingRelease(
    MTL::CommandBuffer* command_buffer) {
  if (!command_buffer || command_buffer_draw_rings_.empty()) {
    return;
  }
  auto rings = std::move(command_buffer_draw_rings_);
  command_buffer->addCompletedHandler(
      [this, rings](MTL::CommandBuffer*) mutable {
        std::lock_guard<std::mutex> lock(draw_ring_mutex_);
        for (auto& ring : rings) {
          draw_ring_pool_.push_back(ring);
        }
      });
}

void MetalCommandProcessor::PopulateIRConverterBuffers() {
  if (!res_heap_ab_ || !smp_heap_ab_ || !uniforms_buffer_ || !shared_memory_) {
    return;
  }

  // Get shared memory buffer for vertex data fetching
  MTL::Buffer* shared_mem_buffer = shared_memory_->GetBuffer();
  if (!shared_mem_buffer) {
    XELOGW("PopulateIRConverterBuffers: No shared memory buffer available");
    return;
  }

  // Populate resource descriptor heap slot 0 with shared memory buffer
  // Xbox 360 shaders use vfetch instructions that read from this buffer
  IRDescriptorTableEntry* res_heap =
      static_cast<IRDescriptorTableEntry*>(res_heap_ab_->contents());

  // Set slot 0 (t0) to shared memory for vertex buffer fetching
  // The metadata encodes buffer size for bounds checking
  uint64_t shared_mem_gpu_addr = shared_mem_buffer->gpuAddress();
  uint64_t shared_mem_size = shared_mem_buffer->length();

  // Use IRDescriptorTableSetBuffer to properly encode the descriptor
  // metadata = buffer size in low 32 bits for bounds checking
  IRDescriptorTableSetBuffer(&res_heap[0], shared_mem_gpu_addr,
                             shared_mem_size);

  // Populate uniforms buffer with system constants and fetch constants
  // Layout matches DxbcShaderTranslator::SystemConstants (b0)
  uint8_t* uniforms = static_cast<uint8_t*>(uniforms_buffer_->contents());

  // For now, populate minimal system constants for passthrough rendering
  // This structure needs to match what the translated shaders expect
  struct MinimalSystemConstants {
    uint32_t flags;                      // 0x00
    float tessellation_factor_range[2];  // 0x04
    uint32_t line_loop_closing_index;    // 0x0C

    uint32_t vertex_index_endian;  // 0x10
    uint32_t vertex_index_offset;  // 0x14
    uint32_t vertex_index_min;     // 0x18
    uint32_t vertex_index_max;     // 0x1C

    float user_clip_planes[6][4];  // 0x20 - 6 clip planes * 4 floats = 96 bytes

    float ndc_scale[3];               // 0x80
    float point_vertex_diameter_min;  // 0x8C

    float ndc_offset[3];              // 0x90
    float point_vertex_diameter_max;  // 0x9C
  };

  MinimalSystemConstants* sys_const =
      reinterpret_cast<MinimalSystemConstants*>(uniforms);

  // Initialize to zero
  std::memset(sys_const, 0, sizeof(MinimalSystemConstants));

  // Set passthrough NDC transform (identity)
  sys_const->ndc_scale[0] = 1.0f;
  sys_const->ndc_scale[1] = 1.0f;
  sys_const->ndc_scale[2] = 1.0f;
  sys_const->ndc_offset[0] = 0.0f;
  sys_const->ndc_offset[1] = 0.0f;
  sys_const->ndc_offset[2] = 0.0f;

  // Set reasonable vertex index bounds
  sys_const->vertex_index_min = 0;
  sys_const->vertex_index_max = 0xFFFFFFFF;

  // Copy fetch constants from register file (b3 in DXBC)
  // Fetch constants start at register XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0
  // There are 32 fetch constants, each 6 DWORDs = 192 DWORDs total = 768 bytes
  const uint32_t* regs = register_file_->values;
  const size_t kFetchConstantOffset = 512;    // After system constants (b0)
  const size_t kFetchConstantCount = 32 * 6;  // 32 fetch constants * 6 DWORDs

  std::memcpy(uniforms + kFetchConstantOffset,
              &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              kFetchConstantCount * sizeof(uint32_t));

  // Bind IR Converter runtime buffers to render encoder
  if (current_render_encoder_) {
    // Bind resource descriptor heap at index 0 (kIRDescriptorHeapBindPoint)
    current_render_encoder_->setVertexBuffer(res_heap_ab_, 0,
                                             kIRDescriptorHeapBindPoint);
    current_render_encoder_->setFragmentBuffer(res_heap_ab_, 0,
                                               kIRDescriptorHeapBindPoint);

    // Bind sampler heap at index 1 (kIRSamplerHeapBindPoint)
    current_render_encoder_->setVertexBuffer(smp_heap_ab_, 0,
                                             kIRSamplerHeapBindPoint);
    current_render_encoder_->setFragmentBuffer(smp_heap_ab_, 0,
                                               kIRSamplerHeapBindPoint);

    // Bind uniforms at index 5 (kIRArgumentBufferUniformsBindPoint)
    current_render_encoder_->setVertexBuffer(
        uniforms_buffer_, 0, kIRArgumentBufferUniformsBindPoint);
    current_render_encoder_->setFragmentBuffer(
        uniforms_buffer_, 0, kIRArgumentBufferUniformsBindPoint);

    // Make shared memory resident for GPU access
    UseRenderEncoderResource(shared_mem_buffer, MTL::ResourceUsageRead);
  }
}

DxbcShaderTranslator::Modification
MetalCommandProcessor::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  const auto& regs = *register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification
MetalCommandProcessor::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  const auto& regs = *register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
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

  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(
           regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Initialize MIN/MAX blend pre-multiply factors to kOne (no pre-multiply).
  // These must be explicitly set, as zero-initialized bits would be kZero
  // which causes the shader to multiply output color by zero (black).
  modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;

  return modification;
}

void MetalCommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal,
    uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  // Get color info for each render target
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  // Build flags
  uint32_t flags = 0;

  // Shared memory mode - determines whether shaders read from SRV (T0) or UAV
  // (U0)
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }

  // W0 division control from PA_CL_VTE_CNTL
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }

  // Primitive type flags
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }

  // Depth format
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }

  // Alpha test - encode compare function in flags
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;

  // Gamma conversion flags for render targets
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  system_constants_.flags = flags;

  // Tessellation factor range
  float tessellation_factor_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Vertex index configuration
  system_constants_.vertex_index_endian = index_endian;
  system_constants_.vertex_index_offset = vgt_indx_offset;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (when not CLIP_DISABLE)
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (xe::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const float* user_clip_plane_regs = reinterpret_cast<const float*>(
          &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4]);
      std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                  4 * sizeof(float));
      user_clip_plane_write_ptr += 4;
    }
  }

  // NDC scale and offset from viewport info
  for (uint32_t i = 0; i < 3; ++i) {
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size parameters
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    system_constants_.point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    system_constants_.point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[0] =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[1] =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    // Screen to NDC radius conversion.
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader. Include draw_resolution_scale to
    // match D3D12 behavior.
    uint32_t point_draw_resolution_scale_x =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_x()
                             : 1;
    uint32_t point_draw_resolution_scale_y =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_y()
                             : 1;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        (/* 0.5f * 2.0f * */ float(point_draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
  }

  // Texture signedness / resolution scaling (mirror D3D12 logic).
  // Always update textures_resolution_scaled, even when used_texture_mask is 0,
  // to avoid stale values from previous draws.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    if (texture_cache_) {
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = (texture_index & 3) * 8;
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = uint32_t(0xFF) << texture_signs_shift;
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
  }
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Sample count log2 for alpha to mask
  uint32_t sample_count_log2_x =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test reference
  system_constants_.alpha_test_reference = rb_alpha_ref;

  // Alpha to mask
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    // Fixed-point render targets (k_16_16 / k_16_16_16_16) are backed by
    // *_SNORM in the host render targets path. If full-range emulation is
    // requested, remap from -32...32 to -1...1 by dividing the output values
    // by 32.
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_16_16) {
      if (!render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    } else if (color_infos[i].color_format ==
               xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (!render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale = xe::memory::Reinterpret<float>(
        int32_t(0x3F800000 + (color_exp_bias << 23)));
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  // Blend constants (used by EDRAM and for host blending)
  system_constants_.edram_blend_constant[0] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  system_constants_.edram_blend_constant[1] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  system_constants_.edram_blend_constant[2] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  system_constants_.edram_blend_constant[3] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);

  system_constants_dirty_ = true;
}

#define COMMAND_PROCESSOR MetalCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
