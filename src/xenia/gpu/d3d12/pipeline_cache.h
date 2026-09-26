/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_PIPELINE_CACHE_H_
#define XENIA_GPU_D3D12_PIPELINE_CACHE_H_

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/hash.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/guest_spirv_shader_cache.h"
#include "xenia/gpu/pipeline_creation_queue.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/shader_translator.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace gpu {

class SpirvShader;
class SpirvShaderTranslator;

namespace d3d12 {

class D3D12CommandProcessor;

class PipelineCache : public GuestSpirvShaderCache::Host {
 public:
  PipelineCache(D3D12CommandProcessor& command_processor,
                const RegisterFile& register_file,
                const D3D12RenderTargetCache& render_target_cache,
                bool bindless_resources_used, bool zpd_hybrid_supported);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();
  // No ClearCache because it's undesirable with the persistent shader storage
  // (if the storage is reloaded, effectively nothing is cleared, while the call
  // takes a long time, and if it's not, there will be heavy stuttering for the
  // rest of the execution of the guest).

  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);
  void ShutdownShaderStorage();

  void EndSubmission();
  bool IsCreatingPipelines();
  // Waits for any pipeline creation needed by the current draw path to finish
  // before state is consumed. This was added so strict ZPD query paths stop
  // racing pipeline compilation and then blocking work on incomplete state.
  void AwaitPipelineCompletion();

  SpirvShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) {
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer_);
    }
  }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed. Returns a SpirvShaderTranslator::Modification
  // value raw (as uint64_t) to keep glslang out of this header.
  uint64_t GetCurrentSpirvVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  uint64_t GetCurrentSpirvPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader) const;

  // Ensures the shader's ucode analysis and a Translation for the modification
  // exist, WITHOUT translating to SPIR-V. Returns the (possibly untranslated)
  // Translation.
  Shader::Translation* EnsureGuestMesaSpirvTranslation(
      SpirvShader& shader, uint64_t spirv_modification);
  // Translates an already-created Translation to SPIR-V using the given
  // translator (the main thread's for vertex shaders, a creation thread's for
  // pixel shaders). use_try_claim coordinates concurrent translation of the
  // same Translation. Returns the valid Translation or nullptr. Any thread.
  Shader::Translation* TranslateGuestMesaSpirv(
      SpirvShaderTranslator& translator, Shader::Translation& translation,
      bool use_try_claim);
  // Main-thread convenience: ensure + translate on the shared translator.
  Shader::Translation* EnsureGuestMesaSpirv(SpirvShader& shader,
                                            uint64_t spirv_modification);
  // GuestSpirvShaderCache::Host. CreateTranslator builds the guest Mesa SPIR-V
  // translator (standard feature set), one per creation thread plus the main
  // thread (the translator is not thread safe).
  std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const override;
  bool precise_interpolation_supported() const;
  bool depth_float24_round() const override {
    return render_target_cache_.depth_float24_round();
  }
  bool depth_float24_convert_in_pixel_shader() const override {
    return render_target_cache_.depth_float24_convert_in_pixel_shader();
  }
  // Converts already-translated SPIR-V to signed Mesa DXIL, caching it by
  // (ucode hash, modification). Returns nullptr on failure. Thread safe (cache
  // guarded by mesa_dxil_cache_mutex_, conversion serialized internally), so it
  // runs on the main thread for vertex shaders and creation threads for pixel
  // shaders.
  const std::vector<uint8_t>* ConvertGuestMesaSpirvToDxil(
      uint64_t ucode_hash, uint64_t spirv_modification,
      const std::vector<uint8_t>& spirv, xenos::ShaderType shader_type);
  // Returns the guest SpirvShader for a ucode hash once its bindings are
  // published (bindings_ready), or nullptr. Its texture/sampler bindings define
  // the order the Mesa DXIL expects in the bindless index buffers.
  SpirvShader* GetGuestMesaSpirvShader(uint64_t ucode_hash);

  // Linked tessellation DXIL: the host vertex and hull shaders plus the guest
  // domain shader, translated together so their inter-stage signatures match.
  struct MesaTessellationDxil {
    std::vector<uint8_t> host_vertex;  // host tessellation vertex shader
    std::vector<uint8_t> host_hull;    // host hull shader
    std::vector<uint8_t> domain;       // guest domain (tess eval) shader
  };
  // Links the host VS + host HS (selected by tessellation mode and domain type)
  // with the already-translated guest domain SPIR-V, returning the three signed
  // DXIL blobs, cached by (domain ucode hash, modification). Returns nullptr on
  // failure. Thread safe (cache guarded by mesa_dxil_cache_mutex_).
  const MesaTessellationDxil* ConvertGuestMesaTessellationToDxil(
      uint64_t domain_ucode_hash, uint64_t domain_spirv_modification,
      const std::vector<uint8_t>& domain_spirv,
      xenos::TessellationMode tessellation_mode,
      Shader::HostVertexShaderType host_vertex_shader_type);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this!
  bool ConfigurePipeline(
      Shader::Translation* vertex_shader, Shader::Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader,
      bool zpd_total, uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_targets_formats,
      bool use_interpreter, void** pipeline_handle_out,
      ID3D12RootSignature** root_signature_out);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline or still being created asynchronously.
  // While async creation is in progress this may be a placeholder pipeline
  // (real vertex shader, no-op pixel shader) that the creation thread swaps for
  // the real one when ready - check IsPlaceholderPipeline if that matters.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->state.load(
        std::memory_order_acquire);
  }
  bool IsPlaceholderPipeline(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->is_placeholder.load(
        std::memory_order_acquire);
  }
  // Loads the current pipeline state once and reports whether it is a
  // placeholder, and whether that placeholder is the ucode interpreter one.
  // A placeholder draw must bind this concrete PSO instead of the swappable
  // handle: the handle is resolved again when the deferred command list is
  // replayed, so a real pipeline hot-swapped in meanwhile would run against
  // bindings built for the placeholder - an empty bindless texture/sampler
  // index buffer for the still-translating pixel shader, and (interpreter) the
  // full-256 float constants the real VS doesn't use.
  ID3D12PipelineState* GetD3D12PipelineForDraw(
      void* handle, bool* is_placeholder_out,
      bool* is_interpreter_placeholder_out) const {
    const Pipeline* pipeline = reinterpret_cast<const Pipeline*>(handle);
    ID3D12PipelineState* state =
        pipeline->state.load(std::memory_order_acquire);
    bool is_placeholder =
        state != nullptr &&
        state == pipeline->placeholder_state.load(std::memory_order_acquire);
    *is_placeholder_out = is_placeholder;
    *is_interpreter_placeholder_out =
        is_placeholder &&
        pipeline->uses_interpreter.load(std::memory_order_acquire);
    return state;
  }
  ID3D12PipelineState* AwaitD3D12PipelineByHandle(void* handle);
  // Waits until the real (non-placeholder) pipeline for the handle is ready,
  // returning it (or nullptr if its creation failed). Needed by occlusion
  // queries, where the no-op placeholder would skip the guest shader's pixel
  // kills and miscount.
  ID3D12PipelineState* AwaitRealD3D12PipelineByHandle(void* handle);

  ID3D12RootSignature* GetRootSignatureByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)
        ->description.root_signature;
  }

 private:
  // Update PipelineDescription::kVersion if any of the Pipeline* enums are
  // changed!

  enum class PipelineStripCutIndex : uint32_t {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };

  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,
    kContinuous,
    kAdaptive,
  };

  enum class PipelinePatchType : uint32_t {
    kNone,
    kLine,
    kTriangle,
    kQuad,
  };

  enum class PipelinePrimitiveTopologyType : uint32_t {
    kPoint,
    kLine,
    kTriangle,
  };

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
    // Lines expanded to 1 guest pixel wide for resolution-scaled draws.
    kLineList,
  };

  enum class PipelineCullMode : uint32_t {
    kNone,
    kFront,
    kBack,
    // Special case, handled via disabling the pixel shader and depth / stencil.
    kDisableRasterization,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
    kAlphaFactor,
    kInvAlphaFactor,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  XEPACKEDSTRUCT(PipelineRenderTarget, {
    uint32_t used : 1;                          // 1
    xenos::ColorRenderTargetFormat format : 4;  // 5
    PipelineBlendFactor src_blend : 4;          // 9
    PipelineBlendFactor dest_blend : 4;         // 13
    xenos::BlendOp blend_op : 3;                // 16
    PipelineBlendFactor src_blend_alpha : 4;    // 20
    PipelineBlendFactor dest_blend_alpha : 4;   // 24
    xenos::BlendOp blend_op_alpha : 3;          // 27
    uint32_t write_mask : 4;                    // 31
  });

  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if drawing without a pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    int32_t depth_bias;
    float depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;  // 2
    // PipelinePrimitiveTopologyType for a vertex shader.
    // xenos::TessellationMode for a domain shader.
    uint32_t primitive_topology_type_or_tessellation_mode : 2;  // 4
    // Zero for non-kVertex host_vertex_shader_type.
    PipelineGeometryShader geometry_shader : 3;       // 7
    uint32_t fill_mode_wireframe : 1;                 // 8
    PipelineCullMode cull_mode : 2;                   // 10
    uint32_t front_counter_clockwise : 1;             // 11
    uint32_t depth_clip : 1;                          // 12
    xenos::MsaaSamples host_msaa_samples : 2;         // 14
    xenos::DepthRenderTargetFormat depth_format : 1;  // 15
    xenos::CompareFunction depth_func : 3;            // 18
    uint32_t depth_write : 1;                         // 19
    uint32_t stencil_enable : 1;                      // 20
    uint32_t stencil_read_mask : 8;                   // 28
    // Marks pipelines that use the spirv_to_dxil (Mesa) path so they keep
    // their own cache entries.
    uint32_t use_mesa_dxil : 1;  // 29
    // Native draw (scale threshold), keeps slope-scale unscaled.
    uint32_t resolution_scale_native : 1;  // 30
    // ROV only - selects the depth-only pixel shader, which is specialized
    // for the guest count. host_msaa_samples can't, guest 2x is rasterized as
    // host 4x there.
    xenos::MsaaSamples guest_msaa_samples : 2;  // 32

    uint32_t stencil_write_mask : 8;                   // 8
    xenos::StencilOp stencil_front_fail_op : 3;        // 11
    xenos::StencilOp stencil_front_depth_fail_op : 3;  // 14
    xenos::StencilOp stencil_front_pass_op : 3;        // 17
    xenos::CompareFunction stencil_front_func : 3;     // 20
    xenos::StencilOp stencil_back_fail_op : 3;         // 23
    xenos::StencilOp stencil_back_depth_fail_op : 3;   // 26
    xenos::StencilOp stencil_back_pass_op : 3;         // 29
    xenos::CompareFunction stencil_back_func : 3;      // 32

    // Hybrid occlusion query draw (RTV + shader counting for Total).
    // Selects the counting depth-only pixel shader when there's no guest PS.
    uint32_t zpd_total : 1;  // 1

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    inline bool operator==(const PipelineDescription& other) const;
    // Bumped to invalidate caches: vertex/pixel_shader_modification are now
    // the canonical SPIR-V (spirv_to_dxil) modifications, not DXBC; then
    // again for the constant-alpha blend state; then again for
    // guest_msaa_samples changing the bitfield layout; then again for
    // zpd_total; then again for the line geometry shader widening
    // geometry_shader.
    static constexpr uint32_t kVersion = 0x20260926;
  });

  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  struct PipelineRuntimeDescription {
    ID3D12RootSignature* root_signature;
    Shader::Translation* vertex_shader;
    Shader::Translation* pixel_shader;
    // When non-null, the spirv_to_dxil guest path supplies the VS/PS bytecode
    // and root_signature is the Mesa root signature. These point into
    // mesa_dxil_cache_, which never erases entries. Default-initialized so the
    // storage reload path (which builds this struct without a memset) leaves
    // them null until it re-translates the guest shaders.
    const std::vector<uint8_t>* mesa_vertex_dxil = nullptr;
    const std::vector<uint8_t>* mesa_pixel_dxil = nullptr;
    // Guest SPIR-V translations deferred to a creation thread: translated
    // (ucode->SPIR-V) and converted to DXIL there, off the main thread. Point
    // into a SpirvShader's Translation in shaders_ (never erased). The
    // live draw path defers only the pixel shader (the vertex DXIL is built
    // synchronously for the placeholder). The storage warm-up defers both via
    // mesa_vertex_translation, since there is no placeholder to satisfy.
    Shader::Translation* mesa_vertex_translation = nullptr;
    Shader::Translation* mesa_pixel_translation = nullptr;
    // Mesa DXIL for the built-in geometry shader (primitive expansion). When
    // set alongside use_mesa_dxil, the pipeline binds it as the geometry
    // shader. Points into mesa_geometry_dxil_cache_ (never erased).
    const std::vector<uint8_t>* mesa_geometry_dxil = nullptr;
    // Tessellation: the host vertex and hull shaders, produced together with
    // the guest domain shader (held in mesa_vertex_dxil, bound as the DS) by
    // one linked translation so the hull and domain signatures match. Point
    // into mesa_tess_dxil_cache_ (never erased).
    const std::vector<uint8_t>* mesa_tess_host_vs_dxil = nullptr;
    const std::vector<uint8_t>* mesa_tess_host_hs_dxil = nullptr;
    PipelineDescription description;
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 3;
      uint32_t interpolator_count : 5;
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
    bool operator==(const GeometryShaderKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const GeometryShaderKey& other_key) const {
      return !(*this == other_key);
    }
  };

  SpirvShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count,
                          uint64_t data_hash);

  // Analyzes shaders in parallel for storage loading. D3D12 renders only
  // through spirv_to_dxil, so only the ucode analysis is needed here, ahead of
  // the translations the creation threads run.
  void AnalyzeShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed);

  // Builds the Mesa DXIL (vertex/domain + tessellation host shaders + pixel +
  // built-in geometry) for a pipeline being rebuilt from storage and fills the
  // runtime description's mesa_*_dxil pointers + Mesa root signature. The
  // SpirvShader translation objects must already exist (created on the main
  // thread via EnsureGuestMesaSpirvTranslation). The ucode->SPIR-V translation
  // and SPIR-V->DXIL conversion run on the given translator's thread, so this
  // is called on a creation thread for the warm-up (use_try_claim coordinates
  // concurrent translation). Returns false on any translation failure.
  bool BuildMesaPipelineDxil(Shader::Translation* vertex_translation,
                             Shader::Translation* pixel_translation,
                             SpirvShaderTranslator& translator,
                             bool use_try_claim,
                             PipelineRuntimeDescription& runtime_description);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this. Shaders need not be translated
  // yet, only their hash and modification are used. The root signature uses the
  // VS bindings and is updated after background translation.
  bool GetCurrentStateDescription(
      Shader::Translation* vertex_shader, Shader::Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, bool depth_bias_in_pixel_shader,
      bool zpd_total, uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      PipelineRuntimeDescription& runtime_description_out);

  // Modifications are raw SpirvShaderTranslator::Modification values (uint64_t
  // to keep spirv_shader_translator.h, and glslang, out of this header).
  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   uint64_t vertex_shader_modification,
                                   uint64_t pixel_shader_modification,
                                   GeometryShaderKey& key_out);

  // Produces and caches the Mesa DXIL for the built-in geometry shader (shared
  // SPIR-V generator -> spirv_to_dxil), keyed by GeometryShaderKey, for the
  // guest spirv_to_dxil path. Returns nullptr on failure. Thread safe.
  const std::vector<uint8_t>* GetGuestMesaGeometryDxil(GeometryShaderKey key);

  // Hot-swap placeholder PSO variants, built with the fixed (bindless) Mesa
  // root signature so the pixel (and, for the interpreter, vertex) shader need
  // not be translated yet.
  enum class PipelinePlaceholderMode {
    kNone,         // Real vertex shader + real pixel shader.
    kRealVertex,   // Real vertex shader + no-op pixel shader.
    kInterpreter,  // Ucode interpreter vertex shader + no-op/debug pixel
                   // shader.
  };
  ID3D12PipelineState* CreateD3D12Pipeline(
      const PipelineRuntimeDescription& runtime_description,
      PipelinePlaceholderMode placeholder_mode =
          PipelinePlaceholderMode::kNone);

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  const D3D12RenderTargetCache& render_target_cache_;
  bool bindless_resources_used_;
  bool zpd_hybrid_supported_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  StringBuffer ucode_disasm_buffer_;

  // Guest shader path: SpirvShaderTranslator output fed to Mesa spirv_to_dxil.
  // The shared cache owns the translator and the modification derivation.
  // SPIR-V translation (EnsureGuestMesaSpirv) is done on the main/draw thread,
  // mirroring how Vulkan translates SPIR-V on the main thread before queuing
  // pipeline work.
  GuestSpirvShaderCache guest_shader_cache_;
  // Mesa DXIL by ucode hash then SPIR-V modification.
  std::unordered_map<uint64_t,
                     std::unordered_map<uint64_t, std::vector<uint8_t>>>
      mesa_dxil_cache_;
  // Built-in geometry shader Mesa DXIL, keyed by GeometryShaderKey value.
  std::unordered_map<uint32_t, std::vector<uint8_t>> mesa_geometry_dxil_cache_;
  // Linked tessellation DXIL (host VS + host HS + guest domain), by guest
  // domain ucode hash then SPIR-V modification.
  std::unordered_map<uint64_t,
                     std::unordered_map<uint64_t, MesaTessellationDxil>>
      mesa_tess_dxil_cache_;
  // Guards mesa_dxil_cache_, mesa_geometry_dxil_cache_ and
  // mesa_tess_dxil_cache_: SPIR-V to DXIL conversion runs on both the main
  // thread (vertex/geometry/ tessellation) and the creation threads (pixel).
  std::mutex mesa_dxil_cache_mutex_;
  // Mesa DXIL for the FSI depth-only pixel shader, generated once at init for
  // pixel-shader-less ROV draws - one per guest sample count, which the FSI
  // shaders are specialized for. Empty if not ROV or generation failed (falls
  // back to the no-op placeholder_ps).
  std::vector<uint8_t>
      mesa_depth_only_rov_pixel_shaders_[size_t(xenos::MsaaSamples::k4X) + 1];

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, SpirvShader*, xe::hash::IdentityHasher<uint64_t>>
      shaders_;

  // Host render target path - keeps RTV draws that write nothing rasterized
  // for occlusion queries.
  std::vector<uint8_t> mesa_depth_only_pixel_shader_;
  // Hybrid occlusion query depth-only pixel shaders, counting pre-test
  // coverage into the ZPD counter's Total lane.
  std::vector<uint8_t> zpd_total_depth_only_pixel_shader_;
  std::vector<uint8_t> zpd_total_float24_truncate_pixel_shader_;
  std::vector<uint8_t> zpd_total_float24_round_pixel_shader_;

  struct Pipeline {
    // nullptr if creation has failed or still pending. May hold a placeholder
    // pipeline (see is_placeholder) until the real one is swapped in.
    std::atomic<ID3D12PipelineState*> state{nullptr};
    // True while state holds a hot-swap placeholder pipeline, before the
    // creation thread swaps in the real one.
    std::atomic<bool> is_placeholder{false};
    // True when the placeholder rasterizes with the ucode interpreter VS (so
    // the draw must feed it full float constants + the ucode location). Only
    // meaningful while is_placeholder.
    std::atomic<bool> uses_interpreter{false};
    // The placeholder PSO handle (nullptr if none). A draw loads state once and
    // compares it against this to know whether it is about to bind the
    // placeholder, consistent even though is_placeholder is cleared a few
    // instructions after the real pipeline is swapped into state.
    std::atomic<ID3D12PipelineState*> placeholder_state{nullptr};
    PipelineRuntimeDescription description;
    // For background creation: stores the untranslated shaders.
    // Background thread translates both VS and PS together, then creates the
    // pipeline. Set to nullptr after translation is done.
    Shader::Translation* pending_vertex_shader{nullptr};
    Shader::Translation* pending_pixel_shader{nullptr};
    // Built speculatively by the storage warm-up, not by a draw. Cleared on
    // rebuild, which is what limits it to one attempt. A live pipeline that
    // fails is a real failure and is never retried.
    bool from_storage{false};
    // Set by a creation thread when real creation failed. A warm-up build that
    // ran before its shaders finished translating has no DXIL to create from,
    // and the entry would otherwise skip its draws for the rest of the session.
    std::atomic<bool> creation_failed{false};

    // Whether the next draw should rebuild this entry from live state.
    bool wants_rebuild() const {
      return from_storage && creation_failed.load(std::memory_order_acquire);
    }
  };

  // Builds the deferred DXIL for a pipeline on the calling thread (a creation
  // thread, or the processor thread in blocking mode). Storage warm-up
  // pipelines defer the whole build. Live async pipelines defer only the pixel
  // shader. The translation runs on mesa_spirv_translator (one per creation
  // thread). use_try_claim coordinates concurrent translation of the same
  // shader.
  void EnsurePipelineShadersTranslated(
      Pipeline* pipeline, SpirvShaderTranslator* mesa_spirv_translator,
      bool use_try_claim);

  // All previously generated pipelines identified by hash and the description.
  std::unordered_multimap<uint64_t, Pipeline*,
                          xe::hash::IdentityHasher<uint64_t>>
      pipelines_;

  // Previously used pipeline. This matches our current state settings and
  // allows us to quickly(ish) reuse the pipeline if no registers have been
  // changed.
  Pipeline* current_pipeline_ = nullptr;

  // Currently open shader storage state.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // Builds one queued pipeline, on a creation thread or on the processor thread
  // draining the warm-up queue. Null if it failed.
  ID3D12PipelineState* CreateQueuedPipeline(
      Pipeline* pipeline, SpirvShaderTranslator* mesa_spirv_translator);
  // Swaps a created pipeline (or a failure, |state| null) into its entry.
  void StoreCreatedPipeline(Pipeline* pipeline, ID3D12PipelineState* state);
  PipelineCreationQueue<Pipeline*, ID3D12PipelineState*, SpirvShaderTranslator>
      creation_queue_;

  // Placeholder pipelines replaced by their real counterpart on a creation
  // thread, paired with the submission they may still be referenced by. Real
  // releases happen in ProcessDeferredDestructions once the GPU has passed it.
  void ProcessDeferredDestructions();
  std::mutex deferred_destroy_mutex_;
  std::vector<std::pair<ID3D12PipelineState*, uint64_t>>
      deferred_destroy_pipelines_;
};
inline bool PipelineCache::PipelineDescription::operator==(
    const PipelineDescription& other) const {
  constexpr size_t cmp_size = sizeof(PipelineDescription);
#if XE_ARCH_AMD64 == 1
  if constexpr (cmp_size == 64) {
    if (vertex_shader_hash != other.vertex_shader_hash ||
        vertex_shader_modification != other.vertex_shader_modification) {
      return false;
    }
    const __m128i* thiz = (const __m128i*)this;
    const __m128i* thoze = (const __m128i*)&other;
    __m128i cmp32 =
        _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 1), _mm_loadu_si128(thoze + 1));

    cmp32 = _mm_and_si128(cmp32, _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 2),
                                                _mm_loadu_si128(thoze + 2)));

    cmp32 = _mm_and_si128(cmp32, _mm_cmpeq_epi8(_mm_loadu_si128(thiz + 3),
                                                _mm_loadu_si128(thoze + 3)));

    return _mm_movemask_epi8(cmp32) == 0xFFFF;

  } else
#endif
  {
    return !memcmp(this, &other, cmp_size);
  }
}
}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_PIPELINE_CACHE_H_
