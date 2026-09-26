/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GUEST_SPIRV_SHADER_CACHE_H_
#define XENIA_GPU_GUEST_SPIRV_SHADER_CACHE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

// Modifications cross this interface as raw uint64_t (not
// SpirvShaderTranslator::Modification) to keep glslang, pulled in by
// spirv_shader_translator.h, out of backend headers that include this one.

namespace xe {
namespace gpu {

class RegisterFile;
class RenderTargetCache;
class SpirvShader;
class SpirvShaderTranslator;

// Built-in geometry shader that expands a guest primitive type the host can't
// draw directly. Values are part of the pipeline storage layout, so both
// backends share this exact enum.
enum class PipelineGeometryShader : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
  // Lines expanded to 1 guest pixel wide for resolution-scaled draws.
  kLineList,
};

// Guest shader logic shared by the D3D12 (spirv_to_dxil) and Vulkan backends:
// the SpirvShaderTranslator (built per backend via the Host seam), the
// SpirvShaderTranslator::Modification derivation, ucode->SPIR-V translation,
// and the built-in geometry shader key. The shader object cache stays
// per-backend (their SpirvShader subclasses differ). This operates on
// shaders/translations passed in. The host-specific tail (DXIL/VkPipeline
// bytecode, pipeline state and storage) stays in each backend behind the Host
// seam below.
class GuestSpirvShaderCache {
 public:
  // Implemented by each backend to supply the host-specific pieces the shared
  // logic needs.
  class Host {
   public:
    virtual ~Host() = default;

    // Builds a translator configured for this backend (Vulkan derives Features
    // from the device, D3D12 enables all and overrides a few). Called once for
    // the cache's main-thread translator and once per worker translator.
    virtual std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const = 0;

    // Depth float24 policy, which lives on the backend's RenderTargetCache
    // subclass (not the shared base, so it can't be read through the base ref).
    virtual bool depth_float24_round() const = 0;
    virtual bool depth_float24_convert_in_pixel_shader() const = 0;
  };

  GuestSpirvShaderCache(Host& host, const RegisterFile& register_file,
                        const RenderTargetCache& render_target_cache);
  ~GuestSpirvShaderCache();

  // Creates the main-thread translator. Call once after construction, on the
  // thread that will own draw-time translation. Returns false if the translator
  // could not be created.
  bool Initialize();
  void Shutdown();

  // The shared translator owned by this cache, for main-thread translation.
  SpirvShaderTranslator& translator() const { return *translator_; }
  // A fresh translator for a worker thread (the translator is not thread safe).
  std::unique_ptr<SpirvShaderTranslator> CreateWorkerTranslator() const;

  // SPIR-V modification derivation (returns SpirvShaderTranslator::Modification
  // values as raw uint64_t to keep glslang out of backend headers). These read
  // the register file and render target cache policy. ps_param_gen_used only
  // affects kPointListAsTriangleStrip host vertex shaders (D3D12 passes false
  // since it expands points with the built-in geometry shader instead).
  uint64_t GetVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  uint64_t GetPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader) const;

  // Ensures a Translation for the modification on the given (backend-owned)
  // shader exists WITHOUT translating to SPIR-V. Analyzes the ucode if needed.
  // Main/draw thread only. Returns the (possibly untranslated) Translation.
  Shader::Translation* EnsureTranslation(SpirvShader& shader,
                                         uint64_t modification);
  // Translates a Translation to SPIR-V on the given translator (the main
  // thread's, or a worker's). use_try_claim coordinates concurrent translation
  // of the same Translation. Returns the valid Translation or nullptr. The
  // backend converts the SPIR-V to host bytecode afterward. Any thread.
  Shader::Translation* TranslateSpirv(SpirvShaderTranslator& translator,
                                      Shader::Translation& translation,
                                      bool use_try_claim);
  // Main-thread convenience: EnsureTranslation + TranslateSpirv on the shared
  // translator.
  Shader::Translation* EnsureAndTranslate(SpirvShader& shader,
                                          uint64_t modification);

  // Built-in primitive-expansion geometry shader key (point/rect/quad). Derived
  // from SPIR-V modifications. Each backend builds the SPIR-V and converts it
  // to its own bytecode.
  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 3;
      uint32_t interpolator_count : 5;
      // Raw enabled count (0-6). Vulkan keys on this too but always builds 6.
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
    };
    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }
    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other) const {
      return key == other.key;
    }
    bool operator!=(const GeometryShaderKey& other) const {
      return !(*this == other);
    }
  };
  // Returns false (no geometry shader) for kNone and for the *AsTriangleStrip
  // fallback host vertex shader types. Takes SPIR-V modification values.
  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   uint64_t vertex_shader_modification,
                                   uint64_t pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  // Each backend selects the host tessellation vertex/hull shaders itself. That
  // selection references the generated vulkan_spirv blobs, whose generation
  // links xenia-gpu, and this base-library component cannot depend on xenia-gpu
  // without a dependency cycle.

 private:
  Host& host_;
  const RegisterFile& register_file_;
  const RenderTargetCache& render_target_cache_;

  std::unique_ptr<SpirvShaderTranslator> translator_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GUEST_SPIRV_SHADER_CACHE_H_
