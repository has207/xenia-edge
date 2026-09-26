/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/hash.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/guest_spirv_shader_cache.h"
#include "xenia/gpu/pipeline_creation_queue.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

// TODO(Triang3l): Create a common base for both the Vulkan and the Direct3D
// implementations.
class VulkanPipelineCache : public GuestSpirvShaderCache::Host {
 public:
  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;

   protected:
    PipelineLayoutProvider() = default;
  };

  struct Pipeline {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    // The layouts are owned by the VulkanCommandProcessor, and must not be
    // destroyed by it while the pipeline cache is active. Atomic because an
    // interpreter placeholder is created with a minimal (no-texture) layout on
    // the draw thread, then upgraded to the real layout by the creation thread
    // once the deferred shaders are translated (before the pipeline hot-swap).
    std::atomic<const PipelineLayoutProvider*> pipeline_layout;

    // Placeholder pipeline support for reduced stutter.
    // When true, the current pipeline uses a placeholder pixel shader and
    // the real pipeline is being compiled in the background.
    std::atomic<bool> is_placeholder{false};
    // When true, the placeholder rasterizes with the ucode interpreter VS (so
    // the draw must feed it full float constants and the ucode location). Set
    // once when the interpreter placeholder is built, gated by is_placeholder.
    std::atomic<bool> uses_interpreter{false};
    // The placeholder VkPipeline handle (VK_NULL_HANDLE if none). A draw
    // compares the pipeline handle it bound against this to know whether it
    // bound the placeholder - consistent with the single `pipeline` load,
    // unlike the separate is_placeholder flag which is cleared a few
    // instructions after the real pipeline is swapped in.
    std::atomic<VkPipeline> placeholder_pipeline{VK_NULL_HANDLE};

    Pipeline(const PipelineLayoutProvider* pipeline_layout_provider)
        : pipeline_layout(pipeline_layout_provider) {}

    // Copy constructor needed for unordered_map
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(
              other.pipeline_layout.load(std::memory_order_acquire)),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)),
          uses_interpreter(
              other.uses_interpreter.load(std::memory_order_acquire)),
          placeholder_pipeline(
              other.placeholder_pipeline.load(std::memory_order_acquire)) {}

    // Move constructor
    Pipeline(Pipeline&& other) noexcept
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(
              other.pipeline_layout.load(std::memory_order_acquire)),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)),
          uses_interpreter(
              other.uses_interpreter.load(std::memory_order_acquire)),
          placeholder_pipeline(
              other.placeholder_pipeline.load(std::memory_order_acquire)) {}

    // Deleted copy assignment to prevent accidental copying
    Pipeline& operator=(const Pipeline&) = delete;

    // Deleted move assignment
    Pipeline& operator=(Pipeline&&) = delete;
  };

  static constexpr size_t kLayoutUIDEmpty = 0;

  VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                      const RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages,
                      bool zpd_hybrid_supported);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();

  // Shader and pipeline storage.
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

  VulkanShader* LoadShader(xenos::ShaderType shader_type,
                           const uint32_t* host_address, uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader) const;

  // The ucode interpreter placeholder defers translation by not calling this on
  // the draw thread; the creation thread calls it (passing its own worker
  // translator, since the shared one is single-threaded). nullptr uses the
  // shared translator.
  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader,
                               SpirvShaderTranslator* translator = nullptr);
  // use_interpreter requests the ucode interpreter VS placeholder for a new,
  // untranslated vertex shader (falls back to normal translation if the async
  // placeholder path can't be taken for this draw).
  bool ConfigurePipeline(
      VulkanShader::VulkanTranslation* vertex_shader,
      VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      bool use_interpreter, bool zpd_total, Pipeline** pipeline_out);

  // True while this draw must be fed the ucode interpreter's inputs (full float
  // constants + ucode location). False once hot-swapped to the real VS.
  static bool IsInterpreterPlaceholder(const Pipeline* pipeline) {
    return pipeline->uses_interpreter.load(std::memory_order_acquire) &&
           pipeline->is_placeholder.load(std::memory_order_acquire);
  }

  // Whether ConfigurePipeline will create the pipeline asynchronously (so the
  // draw thread must not translate shaders itself). Matches the use_async
  // condition inside ConfigurePipeline. has_pixel_shader because the async
  // placeholder path needs a pixel shader.
  bool CanCreatePipelineAsync(bool has_pixel_shader) const;

 private:
  // PipelineGeometryShader and GeometryShaderKey come from
  // GuestSpirvShaderCache.
  using GeometryShaderKey = GuestSpirvShaderCache::GeometryShaderKey;

  enum class PipelinePrimitiveTopology : uint32_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : uint32_t {
    kFill,
    kLine,
    kPoint,
  };

  // Tessellation mode for pipeline creation.
  // Must match the TCS (hull shader) selection logic.
  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,    // Integer tessellation factors.
    kContinuous,  // Fractional (fractional_even) tessellation factors.
    kAdaptive,    // Per-edge factors from index buffer.
  };

  // Tessellation primitive type.
  enum class PipelineTessellationPatchType : uint32_t {
    kNone,
    kTriangle,
    kQuad,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  XEPACKEDSTRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;  // 4
    PipelineBlendFactor dst_color_blend_factor : 4;  // 8
    xenos::BlendOp color_blend_op : 3;               // 11
    PipelineBlendFactor src_alpha_blend_factor : 4;  // 15
    PipelineBlendFactor dst_alpha_blend_factor : 4;  // 19
    xenos::BlendOp alpha_blend_op : 3;               // 22
    uint32_t color_write_mask : 4;                   // 26
  });

  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if no pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    // Shader stages.
    PipelineGeometryShader geometry_shader : 3;            // 3
    PipelineTessellationMode tessellation_mode : 2;        // 5
    PipelineTessellationPatchType tessellation_patch : 2;  // 7
    // Input assembly.
    PipelinePrimitiveTopology primitive_topology : 3;  // 10
    uint32_t primitive_restart : 1;                    // 11
    // Rasterization.
    uint32_t depth_clamp_enable : 1;       // 7
    PipelinePolygonMode polygon_mode : 2;  // 9
    uint32_t cull_front : 1;               // 10
    uint32_t cull_back : 1;                // 11
    uint32_t front_face_clockwise : 1;     // 12
    // Depth / stencil.
    uint32_t depth_write_enable : 1;                      // 13
    xenos::CompareFunction depth_compare_op : 3;          // 15
    uint32_t stencil_test_enable : 1;                     // 17
    xenos::StencilOp stencil_front_fail_op : 3;           // 20
    xenos::StencilOp stencil_front_pass_op : 3;           // 23
    xenos::StencilOp stencil_front_depth_fail_op : 3;     // 26
    xenos::CompareFunction stencil_front_compare_op : 3;  // 29
    xenos::StencilOp stencil_back_fail_op : 3;            // 32

    xenos::StencilOp stencil_back_pass_op : 3;           // 3
    xenos::StencilOp stencil_back_depth_fail_op : 3;     // 6
    xenos::CompareFunction stencil_back_compare_op : 3;  // 9
    // Hybrid occlusion query draw (FBO + shader counting for Total).
    // Selects counting the depth-only fragment shader
    // when there's no guest PS.
    uint32_t zpd_total : 1;  // 10

    // Filled only for the attachments present in the render pass object.
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    // Including all the padding, for a stable hash.
    PipelineDescription() { Reset(); }
    PipelineDescription(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() { std::memset(this, 0, sizeof(*this)); }
    uint64_t GetHash() const { return XXH3_64bits(this, sizeof(*this)); }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return size_t(description.GetHash());
      }
    };

    // Bumped for the line geometry shader widening geometry_shader.
    static constexpr uint32_t kVersion = 0x20260926;
  });

  // Pipeline storage constants.
  static constexpr uint32_t kPipelineStorageVersionWithoutAPI = 0x20201219;
  static constexpr uint32_t kPipelineStorageAPIMagicVulkan = 'VLKN';

  // Pipeline storage description.
  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  // creation threads, with everything needed from caches pre-looked-up.
  struct PipelineCreationArguments {
    std::pair<const PipelineDescription, Pipeline>* pipeline;
    VulkanShader::VulkanTranslation* vertex_shader;
    VulkanShader::VulkanTranslation* pixel_shader;
    VkShaderModule geometry_shader;
    // Tessellation shaders (only used when tessellation is active).
    VkShaderModule tessellation_vertex_shader;   // VS for passing data to TCS.
    VkShaderModule tessellation_control_shader;  // TCS (hull shader).
    VkRenderPass render_pass;
    // For dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3).
    VulkanRenderTargetCache::RenderPassKey render_pass_key;
  };

  // Can be called from multiple threads. use_try_claim atomically claims the
  // translation so concurrent callers (draw thread + creation threads)
  // translate it exactly once, the losers waiting for the winner.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation,
                               bool use_try_claim = false);

  // Translates shaders in parallel for storage loading.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
      bool edram_fsi_used);

  // Guest graphics pipeline layout for the given (translated) shaders. Binding
  // counts come from translation, so untranslated shaders yield the minimal
  // no-texture layout used by the interpreter placeholder. Thread-safe.
  const PipelineLayoutProvider* GetGuestGraphicsPipelineLayout(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader);

  void WritePipelineRenderTargetDescription(
      reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
      PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key, bool zpd_total,
      PipelineDescription& description_out) const;

  // Whether the pipeline for the given description is supported by the device.
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  VkShaderModule GetGeometryShader(GeometryShaderKey key);

  bool precise_interpolation_supported() const;

  // GuestSpirvShaderCache::Host.
  std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const override;
  bool depth_float24_round() const override {
    return render_target_cache_.depth_float24_round();
  }
  bool depth_float24_convert_in_pixel_shader() const override {
    return render_target_cache_.depth_float24_convert_in_pixel_shader();
  }

  // Get the appropriate tessellation control shader (hull shader) module.
  VkShaderModule GetTessellationControlShader(
      PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
      bool use_control_point_count) const;

  // Get the appropriate tessellation vertex shader module.
  VkShaderModule GetTessellationVertexShader(
      PipelineTessellationMode mode) const;

  // Can be called from creation threads - all needed data must be fully set up
  // at the point of the call: shaders must be translated, pipeline layout and
  // render pass objects must be available.
  // If fragment_shader_override is not VK_NULL_HANDLE, it is used instead of
  // the pixel shader from creation_arguments (for placeholder pipelines).
  // vertex_shader_override, if not VK_NULL_HANDLE, is used instead of the
  // translated vertex shader module (for the ucode interpreter placeholder,
  // whose guest VS is intentionally not translated yet).
  // If out_unpublished_pipeline is given, the pipeline is returned through it
  // instead of swapped into the entry, for a caller that publishes it later.
  // Set on every path, VK_NULL_HANDLE when there is nothing new to publish.
  bool EnsurePipelineCreated(
      const PipelineCreationArguments& creation_arguments,
      VkShaderModule fragment_shader_override = VK_NULL_HANDLE,
      VkShaderModule vertex_shader_override = VK_NULL_HANDLE,
      VkPipeline* out_unpublished_pipeline = nullptr);

  // Swaps a created pipeline (or a failure, a null handle) into its entry.
  void StoreCreatedPipeline(const PipelineCreationArguments& creation_arguments,
                            VkPipeline pipeline, bool creating_placeholder);

  // The pixel shader a placeholder pipeline rasterizes with. The no-op one
  // draws nothing on the FSI path, where depth goes through the pixel shader.
  VkShaderModule GetPlaceholderFragmentShader(
      const PipelineCreationArguments& creation_arguments,
      bool allow_debug_color) const;

  // Creates a placeholder pipeline using the placeholder pixel shader.
  // Used for pipeline hot-swap to reduce stutter.
  bool EnsurePipelineCreatedWithPlaceholder(
      const PipelineCreationArguments& creation_arguments) {
    return EnsurePipelineCreated(
        creation_arguments,
        GetPlaceholderFragmentShader(creation_arguments,
                                     /*allow_debug_color=*/false));
  }

  // Creates a placeholder pipeline that rasterizes the guest geometry via the
  // ucode interpreter VS while the real VS+PS compile in the background.
  bool EnsurePipelineCreatedWithInterpreterPlaceholder(
      const PipelineCreationArguments& creation_arguments);

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;
  bool zpd_hybrid_supported_;

  // Cached device features for geometry shader creation.
  unsigned int spirv_version_;
  bool signed_zero_inf_nan_preserve_float32_;
  bool denorm_flush_to_zero_float32_;
  // Already combined with the spirv_disable_rounding_mode_rte cvar.
  bool rounding_mode_rte_float32_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  StringBuffer ucode_disasm_buffer_;
  // Shared guest SPIR-V translator, modification derivation and geometry keys.
  GuestSpirvShaderCache guest_shader_cache_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID,
                          xe::hash::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, VulkanShader*,
                     xe::hash::IdentityHasher<uint64_t>>
      shaders_;

  // Geometry shaders for Xenos primitive types not supported by Vulkan.
  // Stores VK_NULL_HANDLE if failed to create.
  std::unordered_map<GeometryShaderKey, VkShaderModule,
                     GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer using fragment
  // shader interlock when no Xenos pixel shader provided.
  // One per guest sample count - FSI shaders are specialized for it.
  VkShaderModule
      depth_only_fragment_shaders_[size_t(xenos::MsaaSamples::k4X) + 1] = {};
  // Host render target path - keeps FBO draws that write nothing rasterized
  // for occlusion queries.
  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;

  // Substitute depth-only pixel shaders that perform float24 conversion of the
  // rasterizer's depth, bound for guest depth-only draws when in-PS float24
  // conversion is active and the depth buffer is D24FS8. Mirrors the DXBC
  // backend's float24_{truncate,round}_ps.
  VkShaderModule float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule float24_round_fragment_shader_ = VK_NULL_HANDLE;

  VkShaderModule zpd_total_depth_only_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule zpd_total_float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule zpd_total_float24_round_fragment_shader_ = VK_NULL_HANDLE;

  // Placeholder pixel shader for pipeline hot-swap to reduce stutter.
  // Outputs transparent black while the real shader compiles in background.
  VkShaderModule placeholder_pixel_shader_ = VK_NULL_HANDLE;

  // Ucode interpreter vertex shader - interprets a guest VS's ucode to render
  // its real geometry while the real VS translates+compiles in the background.
  VkShaderModule ucode_interpreter_vs_ = VK_NULL_HANDLE;
  // Flat grey debug pixel shader for interpreter placeholders, so the interim
  // geometry is visible (host-render-target path only).
  VkShaderModule placeholder_color_pixel_shader_ = VK_NULL_HANDLE;

  // Tessellation shaders.
  // Vertex shaders for tessellation - pass indices/factors to TCS.
  VkShaderModule tessellation_indexed_vs_ = VK_NULL_HANDLE;
  VkShaderModule tessellation_adaptive_vs_ = VK_NULL_HANDLE;
  // Tessellation control shaders (hull shaders) for different modes and
  // primitive types.
  // Discrete mode (integer tessellation factors).
  VkShaderModule discrete_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Continuous mode (fractional_even tessellation factors).
  VkShaderModule continuous_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Adaptive mode (per-edge factors from index buffer).
  VkShaderModule adaptive_triangle_hs_ = VK_NULL_HANDLE;
  VkShaderModule adaptive_quad_hs_ = VK_NULL_HANDLE;

  // Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCache vk_pipeline_cache_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher>
      pipelines_;

  // Previously used pipeline, to avoid lookups if the state wasn't changed.
  std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;

  // Builds one queued pipeline on a creation thread. VK_NULL_HANDLE if it
  // failed.
  VkPipeline CreateQueuedPipeline(
      const PipelineCreationArguments& creation_arguments,
      SpirvShaderTranslator* worker_translator);

  // For asynchronous creation.
  PipelineCreationQueue<PipelineCreationArguments, VkPipeline,
                        SpirvShaderTranslator>
      creation_queue_;
  // During startup loading, don't block on pipeline creation to allow game
  // boot.
  bool startup_loading_ = false;

  // Deferred destruction of pipelines.
  // Pipelines are only destroyed after the GPU submission that might reference
  // them has completed (tracked via submission numbers from command processor).
  void ProcessDeferredDestructions();
  std::vector<std::pair<VkPipeline, uint64_t>> deferred_destroy_pipelines_;
  std::mutex deferred_destroy_mutex_;

  // Shader and pipeline storage.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // VkPipelineCache persistence path.
  std::filesystem::path vk_pipeline_cache_path_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
