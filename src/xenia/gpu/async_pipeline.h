/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_ASYNC_PIPELINE_H_
#define XENIA_GPU_ASYNC_PIPELINE_H_

#include <cstdint>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

// How a draw gets by while its pipeline is created in the background, shared
// by the backends. The backend supplies what it can do and carries out the
// plan with its own pipeline objects.

namespace xe {
namespace gpu {

// A draw whose pipeline doesn't exist yet.
struct AsyncPipelineDraw {
  // async_shader_compilation is on and the backend can create pipelines on
  // creation threads.
  bool async_available = false;
  // RenderTargetCache::GetPipelineStandInWaitReason found no reason to wait.
  bool stand_in_allowed = false;
  // Not an occlusion query being measured, which a skipped draw counts nothing
  // for.
  bool skip_allowed = false;
  bool has_pixel_shader = false;
  bool vertex_shader_translated = false;
  // Whether the backend can draw with a placeholder pixel shader.
  bool placeholder_supported = false;
  // Whether the ucode interpreter can rasterize the vertex shader, from
  // CanInterpretVertexShader and the backend's own limits.
  bool interpreter_eligible = false;
};

// What the draw does about it.
struct AsyncPipelinePlan {
  enum class StandIn {
    // Nothing, so the draw is skipped until the real pipeline is ready.
    kNone,
    // The ucode interpreter vertex shader with the placeholder pixel shader.
    kInterpreter,
    // The real vertex shader with the placeholder pixel shader.
    kRealVertexShader,
  };
  // Whether the real pipeline is created on a creation thread rather than on
  // the draw's thread.
  bool async = false;
  StandIn stand_in = StandIn::kNone;
  // Whether the draw's thread readies each shader, translating it if needed.
  // The creation thread does the rest.
  bool vertex_shader_on_draw_thread = false;
  bool pixel_shader_on_draw_thread = false;
};

inline AsyncPipelinePlan PlanAsyncPipeline(const AsyncPipelineDraw& draw) {
  AsyncPipelinePlan plan;
  // Without a pixel shader the only stand-in is skipping the draw, which
  // async_shader_skip_draws governs.
  plan.async = draw.async_available && draw.stand_in_allowed &&
               (draw.has_pixel_shader || cvars::async_shader_skip_draws);
  if (!plan.async) {
    plan.vertex_shader_on_draw_thread = true;
    plan.pixel_shader_on_draw_thread = draw.has_pixel_shader;
    return plan;
  }
  // There's no vertex-only placeholder as it would compile the real vertex
  // shader all the same.
  if (!draw.has_pixel_shader || !draw.placeholder_supported) {
    return plan;
  }
  if (!draw.vertex_shader_translated && draw.interpreter_eligible) {
    plan.stand_in = AsyncPipelinePlan::StandIn::kInterpreter;
  } else if (draw.vertex_shader_translated || !cvars::async_shader_skip_draws ||
             !draw.skip_allowed) {
    // An untranslated vertex shader is translated here rather than the draw
    // skipped.
    plan.stand_in = AsyncPipelinePlan::StandIn::kRealVertexShader;
    plan.vertex_shader_on_draw_thread = true;
  }
  return plan;
}

// Whether the ucode interpreter can rasterize a draw with the vertex shader in
// place of its translation, as far as the shader and the primitive go: a plain
// vertex shader with static control flow and no texture fetch or memory
// export. It reads the ucode from shared memory, so not one loaded inline
// (address 0). It doesn't clip or cull the raw position (raw_clipped) with clip
// and cull distances.
inline bool CanInterpretVertexShader(
    const Shader& vertex_shader, uint32_t ucode_address,
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::PrimitiveType host_primitive_type, bool raw_clipped) {
  return cvars::async_shader_vs_interpreter && ucode_address != 0 &&
         !raw_clipped &&
         host_vertex_shader_type == Shader::HostVertexShaderType::kVertex &&
         host_primitive_type != xenos::PrimitiveType::kPointList &&
         host_primitive_type != xenos::PrimitiveType::kRectangleList &&
         host_primitive_type != xenos::PrimitiveType::kQuadList &&
         vertex_shader.texture_bindings().empty() &&
         !vertex_shader.uses_subroutine_calls() &&
         vertex_shader.memexport_eM_written() == 0 &&
         vertex_shader.constant_register_map().loop_bitmap == 0;
}

// Gets a draw's pipeline ready as far as its async creation allows. The draw
// waits for the real pipeline when it can't use a stand-in, or when it has
// nothing to draw with and can't be skipped. Returns whether it has something
// to draw with, logging the draw as skipped if not.
template <typename HasPipeline, typename IsPending, typename Expedite>
bool AwaitDrawPipeline(const RenderTargetCache& render_target_cache,
                       const char* stand_in_wait_reason, bool skip_allowed,
                       const Shader& vertex_shader, const Shader* pixel_shader,
                       HasPipeline&& has_pipeline, IsPending&& is_pending,
                       Expedite&& expedite) {
  const char* wait_reason = stand_in_wait_reason;
  if (!wait_reason && !skip_allowed && !has_pipeline()) {
    wait_reason = "occlusion query";
  }
  if (wait_reason && is_pending()) {
    uint64_t await_start = xe::Clock::QueryHostTickCount();
    expedite();
    XELOGI(
        "Awaited real pipeline for a draw into {} ({}): VS {:016X}, PS "
        "{:016X}, {:.2f} ms",
        render_target_cache.GetLastUpdateDrawTargetName(), wait_reason,
        vertex_shader.ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
        double(xe::Clock::QueryHostTickCount() - await_start) * 1000.0 /
            double(xe::Clock::QueryHostTickFrequency()));
  }
  if (!has_pipeline()) {
    XELOGI("Draw skipped, pipeline not ready yet: VS {:016X}, PS {:016X}",
           vertex_shader.ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return false;
  }
  return true;
}

// For shader_profiling, times the pipeline work a draw that can't use a
// stand-in does on its own thread. 0 when not timed.
inline uint64_t BeginInlinePipelineWork(const char* stand_in_wait_reason) {
  return cvars::async_shader_compilation && stand_in_wait_reason &&
                 cvars::shader_profiling
             ? xe::Clock::QueryHostTickCount()
             : 0;
}

inline void EndInlinePipelineWork(uint64_t start_ticks,
                                  const RenderTargetCache& render_target_cache,
                                  const char* stand_in_wait_reason,
                                  const Shader& vertex_shader,
                                  const Shader* pixel_shader) {
  if (!start_ticks) {
    return;
  }
  double inline_work_ms =
      double(xe::Clock::QueryHostTickCount() - start_ticks) * 1000.0 /
      double(xe::Clock::QueryHostTickFrequency());
  // A lookup takes microseconds. Anything slower translated shaders or built
  // the pipeline.
  if (inline_work_ms >= 1.0) {
    XELOGI(
        "shader_profiling: pipeline work inline for a draw into {} ({}): VS "
        "{:016X}, PS {:016X}, {:.2f} ms",
        render_target_cache.GetLastUpdateDrawTargetName(), stand_in_wait_reason,
        vertex_shader.ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0, inline_work_ms);
  }
}

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_ASYNC_PIPELINE_H_
