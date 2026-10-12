/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
#define XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_

#include <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/metal/dxil_shader.h"
#include "xenia/gpu/metal/metal_dxil_binder.h"
#include "xenia/gpu/metal/metal_primitive_processor.h"
#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/metal/metal_shader_converter.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/metal_zpd_visibility_pool.h"
#include "xenia/gpu/metal/msl_bindings.h"
#include "xenia/gpu/metal/msl_shader.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/ui/metal/metal_api.h"
#include "xenia/ui/metal/metal_provider.h"

namespace MTL {
class Heap;
class SharedEvent;
}  // namespace MTL

namespace xe {
namespace gpu {
namespace metal {

class MetalGraphicsSystem;

class MetalCommandProcessor : public CommandProcessor {
 protected:
#define OVERRIDING_BASE_CMDPROCESSOR
#include "../pm4_command_processor_declare.h"
#undef OVERRIDING_BASE_CMDPROCESSOR

 public:
  explicit MetalCommandProcessor(MetalGraphicsSystem* graphics_system,
                                 kernel::KernelState* kernel_state);
  ~MetalCommandProcessor();

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;
  void InitializeTrace() override;
  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void ClearReadbackBuffers() override;

  // Guest memory a resolve or memexport draw wrote from the still-open command
  // buffer. Cleared once a split puts the writes behind a queue boundary.
  void MarkResolvedMemory(uint32_t base_ptr, uint32_t length);
  bool IsResolvedMemory(uint32_t base_ptr, uint32_t length) const;
  void ClearResolvedMemory();

  ui::metal::MetalProvider& GetMetalProvider() const;

  // Get the Metal device and command queue
  MTL::Device* GetMetalDevice() const { return device_; }
  MTL::CommandQueue* GetMetalCommandQueue() const { return command_queue_; }
  MTL::CommandBuffer* GetCurrentCommandBuffer() const {
    return current_command_buffer_;
  }
  bool HasActiveRenderEncoder() const {
    return current_render_encoder_ != nullptr;
  }
  uint32_t current_draw_index() const { return current_draw_index_; }
  uint64_t GetCurrentSubmission() const;
  uint64_t GetCompletedSubmission() const override;
  // What a command buffer was created for, to attribute the per-frame count.
  // Submission kinds name what ended the previous one, since that is what
  // forced a new submission to be started.
  enum class CommandBufferKind : uint32_t {
    kSubmissionOther,
    kSubmissionCopyToDrawSync,
    kSubmissionZpdQuery,
    kSubmissionUniformsRollover,
    kSubmissionPrimaryBufferEnd,
    kSubmissionWait,
    kTextureUploadBatch,
    kTextureUploadPrivate,
    kTextureOther,
    kRenderTargetResolve,
    kRenderTargetDump,
    kRenderTargetOther,
    kCount,
  };

  MTL::CommandBuffer* EnsureCommandBuffer();
  // Creates a command buffer with GPU time accounting attached. Every command
  // buffer the backend commits outside the submission one has to come from
  // here, or its work is missing from gpu_busy_us_per_frame.
  MTL::CommandBuffer* CreateAccountedCommandBuffer(CommandBufferKind kind);
  // For one released without ever being committed - its completion handler
  // will never run, so the accounting has to be released by hand.
  void DiscardAccountedCommandBuffer(MTL::CommandBuffer* command_buffer,
                                     CommandBufferKind kind);
  void EndRenderEncoder();
  void ResetRenderEncoderResourceUsage();
  void UseRenderEncoderResource(MTL::Resource* resource,
                                MTL::ResourceUsage usage);
  void EnsureCommandBufferAutoreleasePool();
  void DrainCommandBufferAutoreleasePool();
  // Sub-allocates from a command-buffer scoped pool, recycled once the GPU is
  // done with it.
  bool AcquireSpirvArgumentBufferSlice(uint32_t bytes, uint32_t alignment,
                                       MTL::Buffer** buffer_out,
                                       NS::UInteger* offset_out);
  const MetalShaderConverter& metal_shader_converter() const {
    return metal_shader_converter_;
  }
  MTL::Buffer* null_buffer() const { return null_buffer_; }
  MTL::SamplerState* null_sampler() const { return null_sampler_; }

  // Get current render pass descriptor (for render target binding)
  MTL::RenderPassDescriptor* GetCurrentRenderPassDescriptor();

  // Force issue a swap to push render target to presenter (for trace dumps)
  void ForceIssueSwap();
  bool HasSeenSwap() const { return saw_swap_; }
  void SetSwapDestSwap(uint32_t dest_base, bool swap);
  bool ConsumeSwapDestSwap(uint32_t dest_base, bool* swap_out);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  std::string MakeTitleStateSuffix() const override;
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr) override;

  // Flush pending GPU work before entering wait state.
  // This ensures Metal command buffers are submitted and completed before
  // the autorelease pool is drained, preventing hangs from deferred
  // deallocation.
  void PrepareForWait() override;
  void PollCompletedSubmission() override;

  // ZPD occlusion query backend overrides. Metal has no query pool object:
  // visibility counting is a render encoder mode writing into an offset of the
  // pass descriptor's visibility result buffer, so the buffer must be attached
  // before the encoder is created and a segment can't outlive its encoder.
  void EnsureQueryResources() override;
  void ShutdownQueryResources() override;
  bool IsQueryPoolReady() const override;
  bool CanOpenQuery() const override;
  QueryOpenResult OpenQuery(bool can_close_submission) override;
  bool CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                  uint64_t& out_submission) override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle,
                         uint64_t wait_for_submission) override;

  // Use base class WriteRegister - don't override with empty implementation!
  // The base class stores values in register_file_->values[] which we need.
  void OnPrimaryBufferEnd() override;
  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  // SPIRV-Cross draw path — called from IssueDraw. Handles shader translation,
  // pipeline creation, resource binding, and draw dispatch using native Metal
  // encoder calls.
  bool IssueDrawMsl(
      Shader* vertex_shader, Shader* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, bool is_rasterization_done, bool memexport_used,
      uint32_t normalized_color_mask, const RegisterFile& regs);
  // SPIR-V -> DXIL -> AIR draw path. Resources go through the Metal Shader
  // Converter argument buffer MetalDxilBinder builds instead of Metal slots.
  bool IssueDrawDxil(
      Shader* vertex_shader, Shader* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, bool memexport_used,
      uint32_t normalized_color_mask, const RegisterFile& regs);
  bool IssueCopy() override;
  void WriteRegister(uint32_t index, uint32_t value) override;

 private:
  // Initialize shader translation pipeline
  bool InitializeShaderTranslation();

  // Command buffer management
  void BeginCommandBuffer();
  // next_kind attributes the submission this forces to be started.
  void EndCommandBuffer(
      CommandBufferKind next_kind = CommandBufferKind::kSubmissionOther);
  // Reset per-render-encoder cached bindings/state for the SPIRV-Cross (MSL)
  // path. Safe to call when no render encoder is active.
  void ResetMslRenderEncoderStateCache();
  // Reset cross-encoder SPIRV-Cross reuse caches at command-buffer boundaries.
  void ResetMslCrossEncoderReuseCaches();
  // Drops the cached bindings and states the render target cache's transfer
  // draws overwrote while sharing the guest's render encoder.
  void InvalidateRenderEncoderStateAfterDrawPassTransfers(
      MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations);
  // Blocks until the compile threads have drained. For work that needs the
  // guest's exact shaders rather than a placeholder or a skipped draw.
  void AwaitAsyncCompiles();
  // Blocks until the given submission's command buffer has completed. The
  // submission must already be committed.
  void AwaitSubmissionCompletion(uint64_t submission);
  // Commits the open submission, then blocks until every submission completes.
  void AwaitAllQueueOperationsCompletion();
  void AddGpuTimeHandler(MTL::CommandBuffer* command_buffer);
  void WaitForPendingCompletionHandlers();
  void ProcessCompletedSubmissions();
  bool EnsureDrawRingCapacity();
  // SPIRV-Cross path: uniforms buffer is command-buffer scoped to avoid
  // CPU writes racing ahead of in-flight GPU reads.
  bool EnsureSpirvUniformBuffer();
  bool EnsureSpirvUniformBufferCapacity();
  void ScheduleSpirvUniformBufferRelease(MTL::CommandBuffer* command_buffer);
  void ScheduleSpirvArgumentBufferRelease(MTL::CommandBuffer* command_buffer);

  // Fixed-function depth/stencil state (mirrors Vulkan/D3D12 dynamic state).
  void ApplyDepthStencilState(bool primitive_polygonal,
                              reg::RB_DEPTHCONTROL normalized_depth_control);
  void ApplyRasterizerState(bool primitive_polygonal);

  // Draw setup shared by both guest shader paths.

  // Makes the vertex fetch and memexport ranges the draw touches resident in
  // shared memory.
  bool RequestDrawSharedMemoryRanges(const Shader& vertex_shader,
                                     const RegisterFile& regs);
  void ComputeDrawViewportInfo(const RegisterFile& regs,
                               const Shader* pixel_shader,
                               reg::RB_DEPTHCONTROL normalized_depth_control,
                               draw_util::ViewportInfo& viewport_info_out);
  void ApplyViewportAndScissor(const RegisterFile& regs,
                               const draw_util::ViewportInfo& viewport_info);
  // Splits the command buffer when the draw samples memory a pending resolve
  // wrote, then requests the textures the draw uses. Returns false when the
  // draw has to be skipped.
  bool PrepareDrawTextures(uint32_t used_texture_mask,
                           const RegisterFile& regs);
  // Refreshes the packed float / bool-loop / fetch constant blobs from the
  // guest registers, skipping any whose registers and used-constant layout are
  // unchanged.
  void UpdateGuestConstantCaches(const Shader* vertex_shader,
                                 const Shader* pixel_shader,
                                 const RegisterFile& regs);
  // What to feed the draw call: the host primitive type, and either an index
  // buffer or a plain vertex range.
  struct DrawIndexBuffer {
    MTL::PrimitiveType primitive_type = MTL::PrimitiveTypeTriangle;
    uint32_t index_count = 0;
    bool indexed = false;
    MTL::IndexType index_type = MTL::IndexTypeUInt16;
    MTL::Buffer* buffer = nullptr;
    uint64_t offset = 0;
  };
  bool ResolveDrawIndexBuffer(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      Shader::HostVertexShaderType host_vertex_shader_type, bool tessellated,
      DrawIndexBuffer& index_buffer_out);

  // Constants shared between the guest shader paths.
  static constexpr size_t kStageCount = 2;  // Vertex + pixel.
  static constexpr size_t kNullBufferSize = 4096;
  static constexpr size_t kCbvSizeBytes = 4096;
  static constexpr size_t kUniformsBytesPerTable = 6 * kCbvSizeBytes;
  static constexpr size_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
  static constexpr size_t kFetchConstantsSize =
      xenos::kTextureFetchConstantCount * 6 * sizeof(uint32_t);

  struct SpirvArgumentBufferPage {
    MTL::Buffer* buffer = nullptr;
    size_t bytes = 0;
    size_t offset = 0;

    ~SpirvArgumentBufferPage();
  };

  // System constants population (mirrors D3D12 implementation)
  void UpdateSystemConstantValues(bool shared_memory_is_uav,
                                  bool primitive_polygonal,
                                  uint32_t line_loop_closing_index,
                                  xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);

  // SPIRV-Cross (MSL) path - shader modification and pipeline helpers.
  SpirvShaderTranslator::Modification GetCurrentSpirvVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  SpirvShaderTranslator::Modification GetCurrentSpirvPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask) const;
  void UpdateSpirvSystemConstantValues(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, uint32_t line_loop_closing_index,
      xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
      uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask);
  enum class ShaderCompileStatus {
    kReady,
    kPending,
    kFailed,
    kNotQueued,
  };
  enum class PipelineCompileStatus {
    kReady,
    kPending,
    kFailed,
  };
  struct PipelineCompileRequest;
  void InitializeAsyncCompilation();
  void ShutdownAsyncCompilation();
  ShaderCompileStatus GetShaderCompileStatus(Shader::Translation* translation);
  // For a translation the caller has just read as kNotQueued. False means there
  // is no worker pool, so the caller has to compile it itself.
  bool EnqueueShaderCompilation(Shader::Translation* translation, bool is_ios,
                                uint8_t priority);
  // Records a translation the draw thread could not translate to SPIR-V, so
  // the failure is reported once rather than retried by every later draw.
  void NoteShaderCompileFailed(Shader::Translation* translation);
  // Claimed so only one thread translates it. kPending means another holds the
  // claim and the caller should retry rather than wait.
  ShaderCompileStatus EnsureTranslationSpirv(Shader::Translation* translation,
                                             SpirvShaderTranslator& translator);
  // Builds the Metal function of one translation on whichever guest shader
  // path is active: SPIR-V -> MSL, or SPIR-V -> DXIL -> AIR.
  bool CompileHostShader(Shader::Translation* translation, bool is_ios);
  std::unique_ptr<SpirvShaderTranslator> CreateSpirvShaderTranslator() const;
  // The compiled function of a translation on the active path, null until
  // CompileHostShader has succeeded for it.
  static MTL::Function* GetHostShaderFunction(
      const Shader::Translation* translation);
  bool EnqueuePipelineCompilation(const PipelineCompileRequest& request);
  // Rate-limited "draw skipped, still compiling" logging, shared by both guest
  // shader paths.
  void LogShaderCompilePending(const Shader::Translation* translation,
                               const char* stage_tag);
  void LogPipelineCompilePending(const Shader::Translation* vertex_translation,
                                 const Shader::Translation* pixel_translation);
  void LogPlaceholderDraw(const Shader::Translation* vertex_translation,
                          const Shader::Translation* pixel_translation);
  // Both require async_compile_mutex_ held.
  bool IsAsyncCompileIdleLocked() const;
  void FinishAsyncCompileTaskLocked();
  // Formats, write masks and blend state, shared by the render and mesh
  // pipeline descriptors.
  static void ApplyColorAttachmentState(
      MTL::RenderPipelineColorAttachmentDescriptorArray* attachments,
      const PipelineCompileRequest& request);
  MTL::RenderPipelineState* CreatePipelineState(
      const PipelineCompileRequest& request, std::string* error_out);
  void AsyncCompileThread(size_t thread_index);
  // Fills a request's attachment formats and blend state from the active render
  // pass and the registers, returning the pipeline cache key. The translations
  // only identify the shaders within that key.
  uint64_t PopulatePipelineCompileRequest(
      const RegisterFile& regs, const Shader::Translation* vertex_translation,
      const Shader::Translation* pixel_translation,
      PipelineCompileRequest& request);
  MTL::RenderPipelineState* GetOrCreatePipelineState(
      const Shader::Translation* vertex_translation,
      const Shader::Translation* pixel_translation, const RegisterFile& regs,
      PipelineCompileStatus* compile_status_out = nullptr);
  // Shared by every pipeline kind. A compile thread can reach neither the
  // register file nor the render pass, so the request must carry it all.
  MTL::RenderPipelineState* AcquirePipelineState(
      const PipelineCompileRequest& request, bool allow_async,
      PipelineCompileStatus* compile_status_out);
  // Stands in for a pipeline whose pixel shader isn't compiled yet: the vertex
  // shader alone, color writes off. Shared across that shader's pipelines.
  MTL::RenderPipelineState* GetOrCreatePlaceholderPipelineState(
      const Shader::Translation* vertex_translation, const RegisterFile& regs);

  // Never null - an unbuildable one reports kFailed, so later draws don't retry
  // it. allow_async false builds it here and waits for one already queued.
  // TODO(macos): translate the guest ucode to SPIR-V on the compile threads
  // too, instead of on the draw thread.
  Shader::Translation* GetOrCreateHostTranslation(
      SpirvShader& shader, uint64_t modification, bool allow_async,
      ShaderCompileStatus* compile_status_out);

  // One MSC stage of a tessellated draw.
  struct DxilTessellationStage {
    MTL::Library* library = nullptr;
    MTL::Function* function = nullptr;
    std::string function_name;
    MetalShaderReflection reflection;
  };
  // The host vertex and hull shaders linked with one guest domain shader.
  // Tessellation emulation links all three together, so they are cached as a
  // unit rather than per stage.
  struct DxilTessellationShaders {
    DxilTessellationShaders() = default;
    DxilTessellationShaders(const DxilTessellationShaders&) = delete;
    DxilTessellationShaders& operator=(const DxilTessellationShaders&) = delete;
    ~DxilTessellationShaders() {
      for (DxilTessellationStage* stage : {&vertex, &hull, &domain}) {
        if (stage->function) {
          stage->function->release();
        }
        if (stage->library) {
          stage->library->release();
        }
      }
      if (stage_in_library) {
        stage_in_library->release();
      }
    }

    // The synthesized stage-in function linked into the vertex stage.
    MTL::Library* stage_in_library = nullptr;
    DxilTessellationStage vertex;
    DxilTessellationStage hull;
    DxilTessellationStage domain;
  };
  // The three linked stages for one guest domain shader. A compile thread
  // builds them, so this reports kPending until it lands.
  DxilTessellationShaders* GetDxilTessellationShaders(
      DxilShader& domain_shader, uint64_t domain_modification,
      xenos::TessellationMode tessellation_mode,
      Shader::HostVertexShaderType host_vertex_shader_type, bool allow_async,
      ShaderCompileStatus* compile_status_out);
  MTL::RenderPipelineState* GetOrCreateDxilTessellationPipelineState(
      const DxilTessellationShaders& shaders,
      const Shader::Translation* domain_translation,
      xenos::TessellationMode tessellation_mode,
      Shader::HostVertexShaderType host_vertex_shader_type,
      DxilShader::DxilTranslation* pixel_translation, const RegisterFile& regs,
      PipelineCompileStatus* compile_status_out);
  MTL::RenderPipelineState* CreateDxilTessellationPipelineState(
      const PipelineCompileRequest& request, std::string* error_out);
  // Builds one linked stage set. Runs on a compile thread, from the domain
  // SPIR-V the draw thread already produced.
  bool BuildDxilTessellationShaders(
      const std::vector<uint8_t>& domain_spirv, uint64_t domain_ucode_hash,
      xenos::TessellationMode tessellation_mode,
      Shader::HostVertexShaderType host_vertex_shader_type,
      DxilTessellationShaders& shaders_out);
  // MSC's tessellator lookup tables, allocated once and kept resident.
  bool EnsureTessellatorTablesBuffer();

  // Metal device and command queue (from provider)
  MTL::Device* device_ = nullptr;
  MTL::CommandQueue* command_queue_ = nullptr;
  MTL::SharedEvent* wait_shared_event_ = nullptr;
  uint64_t wait_shared_event_value_ = 0;

  // Render targets
  MTL::Texture* render_target_texture_ = nullptr;
  MTL::Texture* depth_stencil_texture_ = nullptr;
  MTL::RenderPassDescriptor* render_pass_descriptor_ = nullptr;
  uint32_t render_target_width_ = 1280;
  uint32_t render_target_height_ = 720;

  // Current command buffer and encoder
  MTL::CommandBuffer* current_command_buffer_ = nullptr;
  MTL::RenderCommandEncoder* current_render_encoder_ = nullptr;
  MTL::RenderPassDescriptor* current_render_pass_descriptor_ = nullptr;
  NS::AutoreleasePool* command_buffer_autorelease_pool_ = nullptr;
  // Whether the descriptor the current encoder was created from carried the
  // visibility result buffer, so a ZPD segment may be opened on it.
  bool render_encoder_has_zpd_visibility_ = false;

  // Tracks resources marked via useResource for the current render encoder
  // to avoid redundant driver calls across draws within the same encoder.
  std::unordered_map<MTL::Resource*, uint32_t> render_encoder_resource_usage_;

  // Shared memory for Xbox 360 memory access
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalPrimitiveProcessor> primitive_processor_;
  bool frame_open_ = false;

  bool saw_swap_ = false;
  uint32_t last_swap_ptr_ = 0;
  uint32_t last_swap_width_ = 0;
  uint32_t last_swap_height_ = 0;
  std::unordered_map<uint32_t, bool> swap_dest_swaps_by_base_;

 public:
  MetalSharedMemory* shared_memory() const { return shared_memory_.get(); }
  MetalRenderTargetCache* render_target_cache() const {
    return render_target_cache_.get();
  }
  MetalTextureCache* texture_cache() const { return texture_cache_.get(); }

 private:
  // Shader ucode disassembly buffer (used by AnalyzeUcode).
  StringBuffer ucode_disasm_buffer_;

  // The cache holds MslShader or DxilShader depending on the active path; both
  // derive from SpirvShader and share this translator's output. The translator
  // is not thread safe, so each compile thread builds its own.
  std::unique_ptr<SpirvShaderTranslator> spirv_shader_translator_;
  SpirvShaderTranslator::Features spirv_translator_features_{false};
  bool spirv_translator_native_2x_msaa_ = false;
  uint32_t spirv_translator_resolution_scale_x_ = 1;
  uint32_t spirv_translator_resolution_scale_y_ = 1;
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>>
      guest_shader_cache_;
  // Owns the root signature the DXIL shaders are compiled against.
  MetalShaderConverter metal_shader_converter_;
  MetalDxilBinder dxil_binder_;
  // Keyed by domain shader hash, modification, tessellation mode and host
  // vertex shader type, which together pick all three linked stages.
  std::unordered_map<uint64_t, std::unique_ptr<DxilTessellationShaders>>
      dxil_tessellation_cache_;
  MTL::Buffer* tessellator_tables_buffer_ = nullptr;
  // Fragment stage for tessellated DXIL draws without a pixel shader.
  MTL::Library* dxil_tessellation_depth_only_library_ = nullptr;
  std::string dxil_tessellation_depth_only_function_name_;
  bool mesh_shader_supported_ = false;
  // Includes user clip planes and tessellation constants.
  SpirvShaderTranslator::SystemConstants spirv_system_constants_ = {};
  struct ShaderCompileRequest {
    Shader::Translation* translation = nullptr;
    uint64_t shader_hash = 0;
    uint64_t modification = 0;
    bool is_ios = false;
    uint8_t priority = 0;
  };
  // What a queued pipeline request builds. Each kind reads a different subset
  // of the request, but all of them are captured on the draw thread.
  enum class PipelineKind : uint32_t {
    kRender,
    kMslTessellation,
    kDxilTessellation,
  };
  static constexpr bool IsTessellationPipelineKind(PipelineKind kind) {
    return kind == PipelineKind::kMslTessellation ||
           kind == PipelineKind::kDxilTessellation;
  }
  // Everything that distinguishes one pipeline, in a form that survives to
  // disk. Its hash is the cache key, so replay lands where a draw looks.
  // Update kVersion if anything here changes!
  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 with no pixel shader - the depth-only and placeholder pipelines.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    uint32_t color_formats[4];  // MTL::PixelFormat
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t blendcontrol[4];  // RB_BLENDCONTROL, zeroed for unwritten targets
    uint32_t normalized_color_mask;
    uint32_t sample_count;

    PipelineKind kind : 2;                          // 2
    uint32_t alpha_to_mask_enable : 1;              // 3
    xenos::TessellationMode tessellation_mode : 2;  // 5
    Shader::HostVertexShaderType host_vertex_shader_type
        : Shader::kHostVertexShaderTypeBitCount;  // 9
    uint32_t padding : 23;                        // 32

    // Increment when the layout or its meaning changes.
    static constexpr uint32_t kVersion = 0x20260830;

    // Copied whole, padding included, because the hash covers every byte.
    PipelineDescription() { Reset(); }
    PipelineDescription(const PipelineDescription& other) {
      std::memcpy(this, &other, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& other) {
      std::memcpy(this, &other, sizeof(*this));
      return *this;
    }
    void Reset() { std::memset(this, 0, sizeof(*this)); }
    uint64_t GetHash() const { return XXH3_64bits(this, sizeof(*this)); }
    bool operator==(const PipelineDescription& other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  });
  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });
  static constexpr uint32_t kPipelineStorageAPIMagicMetal = 'MTLP';

  struct PipelineCompileRequest {
    PipelineDescription description;
    // description.GetHash(), the key into async_pipeline_cache_.
    uint64_t pipeline_key = 0;
    MTL::Function* vertex_function = nullptr;
    MTL::Function* fragment_function = nullptr;
    uint8_t priority = 0;
    // kDxilTessellation: owned by dxil_tessellation_cache_, which outlives
    // every compile thread, as does the fragment library's translation.
    const DxilTessellationShaders* tessellation_shaders = nullptr;
    MTL::Library* fragment_library = nullptr;
    std::string fragment_function_name;
  };
  // Building one DxilTessellationShaders set, off the draw thread.
  struct TessellationShadersCompileRequest {
    uint64_t key = 0;
    // Points into the domain Translation, which is immutable once translated.
    const std::vector<uint8_t>* domain_spirv = nullptr;
    uint64_t domain_ucode_hash = 0;
    xenos::TessellationMode tessellation_mode =
        xenos::TessellationMode::kDiscrete;
    Shader::HostVertexShaderType host_vertex_shader_type =
        Shader::HostVertexShaderType::kVertex;
  };
  struct ShaderCompileRequestCompare {
    bool operator()(const ShaderCompileRequest& a,
                    const ShaderCompileRequest& b) const {
      return a.priority < b.priority;
    }
  };
  struct PipelineCompileRequestCompare {
    bool operator()(const PipelineCompileRequest& a,
                    const PipelineCompileRequest& b) const {
      return a.priority < b.priority;
    }
  };
  std::priority_queue<ShaderCompileRequest, std::vector<ShaderCompileRequest>,
                      ShaderCompileRequestCompare>
      async_shader_queue_;
  std::priority_queue<PipelineCompileRequest,
                      std::vector<PipelineCompileRequest>,
                      PipelineCompileRequestCompare>
      async_pipeline_queue_;
  std::unordered_set<Shader::Translation*> async_shader_pending_;
  std::unordered_set<Shader::Translation*> async_shader_failed_;
  std::unordered_set<uint64_t> async_pipeline_pending_;
  std::unordered_set<uint64_t> async_pipeline_failed_;
  std::queue<TessellationShadersCompileRequest> async_tess_shaders_queue_;
  std::unordered_set<uint64_t> async_tess_shaders_pending_;
  std::unordered_set<uint64_t> async_tess_shaders_failed_;
  std::mutex async_compile_mutex_;
  std::condition_variable async_compile_cv_;
  // Signalled when the last in-flight task finishes, for AwaitAsyncCompiles.
  std::condition_variable async_compile_idle_cv_;
  std::vector<std::thread> async_compile_threads_;
  size_t async_compile_busy_ = 0;
  bool async_compile_shutdown_ = false;
  std::atomic<int64_t> async_shader_failure_last_log_ns_{0};
  std::atomic<int64_t> async_pipeline_failure_last_log_ns_{0};
  std::atomic<int64_t> async_shader_pending_last_log_ns_{0};
  std::atomic<int64_t> async_pipeline_pending_last_log_ns_{0};
  std::atomic<int64_t> async_placeholder_last_log_ns_{0};
  std::atomic<int64_t> async_compile_drain_last_log_ns_{0};
  // Guest ucode (.xsh, shared with the other backends) and this backend's
  // pipeline descriptions (.metal.xpso), replayed at startup.
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;
  bool shader_storage_flush_needed_ = false;
  bool pipeline_storage_flush_needed_ = false;
  // Set while rebuilding from storage, so replay doesn't re-record what it
  // read.
  bool replaying_stored_pipelines_ = false;
  // Turns the stored (ucode hash, modification) pairs into compiled shaders.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed);
  // Rebuilds the stored pipelines.
  void CreateStoredPipelines(
      const std::vector<PipelineStoredDescription>& stored_descriptions);
  SpirvShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count,
                          uint64_t hash);
  // Queues the creation of a pipeline loaded from storage for
  // ReplayStoredPipelines. Returns nullptr with |vertex_shader_out| set, or why
  // it didn't.
  const char* QueueStoredPipeline(const PipelineStoredDescription& stored,
                                  Shader*& vertex_shader_out);
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> async_pipeline_cache_;

  // SPIRV-Cross tessellation support.
  MTL::ComputePipelineState* tess_factor_pipeline_tri_ = nullptr;
  MTL::ComputePipelineState* tess_factor_pipeline_quad_ = nullptr;
  // Adaptive tessellation factor pipelines (read per-edge factors from shared
  // memory instead of using a uniform value).
  MTL::ComputePipelineState* tess_factor_pipeline_adaptive_tri_ = nullptr;
  MTL::ComputePipelineState* tess_factor_pipeline_adaptive_quad_ = nullptr;
  MTL::Buffer* tess_factor_buffer_ = nullptr;
  uint32_t tess_factor_buffer_patch_capacity_ = 0;
  bool InitializeMslTessellation();
  void ShutdownMslTessellation();
  MTL::RenderPipelineState* GetOrCreateMslTessPipelineState(
      MslShader::MslTranslation* domain_translation,
      MslShader::MslTranslation* pixel_translation,
      Shader::HostVertexShaderType host_vertex_shader_type,
      const RegisterFile& regs, PipelineCompileStatus* compile_status_out);
  MTL::RenderPipelineState* CreateMslTessellationPipelineState(
      const PipelineCompileRequest& request, std::string* error_out);
  bool EnsureTessFactorBuffer(uint32_t patch_count);

  struct DepthStencilStateKey {
    uint32_t depth_control;
    uint32_t stencil_ref_mask_front;
    uint32_t stencil_ref_mask_back;
    uint32_t polygonal_and_backface;
    bool operator==(const DepthStencilStateKey& other) const {
      return depth_control == other.depth_control &&
             stencil_ref_mask_front == other.stencil_ref_mask_front &&
             stencil_ref_mask_back == other.stencil_ref_mask_back &&
             polygonal_and_backface == other.polygonal_and_backface;
    }
    struct Hasher {
      size_t operator()(const DepthStencilStateKey& key) const {
        size_t h = size_t(key.depth_control);
        h ^= size_t(key.stencil_ref_mask_front) << 1;
        h ^= size_t(key.stencil_ref_mask_back) << 2;
        h ^= size_t(key.polygonal_and_backface) << 3;
        return h;
      }
    };
  };

  std::unordered_map<DepthStencilStateKey, MTL::DepthStencilState*,
                     DepthStencilStateKey::Hasher>
      depth_stencil_state_cache_;

  // Texture cache for guest texture uploads
  std::unique_ptr<MetalTextureCache> texture_cache_;

  // Render target cache for framebuffer management
  std::unique_ptr<MetalRenderTargetCache> render_target_cache_;

  // Null resources for unbound slots (shared between MSC and SPIRV-Cross)
  MTL::Buffer* null_buffer_ = nullptr;
  MTL::Texture* null_texture_ = nullptr;
  MTL::SamplerState* null_sampler_ = nullptr;

  // Uniforms buffer and draw ring count (shared between MSC and SPIRV-Cross)
  MTL::Buffer* uniforms_buffer_ = nullptr;
  // SPIRV path may rotate through multiple uniforms buffers within a single
  // Metal command buffer (at ring wrap boundaries). Track all of them so
  // completion handlers can return every buffer to the available pool.
  std::vector<MTL::Buffer*> command_buffer_spirv_uniform_buffers_;
  size_t draw_ring_count_ = 0;
  // Owning storage for all SPIRV-Cross uniforms buffers allocated for the
  // current context.
  std::vector<MTL::Buffer*> spirv_uniforms_pool_;
  // Reusable SPIRV-Cross uniforms buffers returned from completed command
  // buffers to reduce iOS allocation churn.
  std::vector<MTL::Buffer*> spirv_uniforms_available_;
  std::mutex spirv_uniforms_mutex_;
  dispatch_semaphore_t spirv_uniforms_available_semaphore_ = nullptr;
  bool spirv_uniforms_pool_initialized_ = false;
  std::vector<std::shared_ptr<SpirvArgumentBufferPage>> spirv_argbuf_pool_;
  std::vector<std::shared_ptr<SpirvArgumentBufferPage>>
      command_buffer_spirv_argbuf_pages_;
  std::unordered_map<MTL::CommandBuffer*,
                     std::vector<std::shared_ptr<SpirvArgumentBufferPage>>>
      pending_spirv_argbuf_releases_;
  std::mutex spirv_argbuf_mutex_;

  MTL::Library* depth_only_pixel_library_ = nullptr;
  std::string depth_only_pixel_function_name_;

  bool logged_missing_texture_warning_ = false;
  // SPIRV-Cross system/clip/tess constants versioning.
  // Each ring-table slot tracks the last source version copied into it so
  // draws can skip per-draw memcmp/copy churn for unchanged constants.
  uint64_t msl_system_constants_version_ = 1;
  MTL::Buffer* msl_constants_versioned_uniform_buffer_ = nullptr;
  std::vector<uint64_t> msl_system_constants_written_vertex_versions_;
  std::vector<uint64_t> msl_system_constants_written_pixel_versions_;
  // SPIRV-Cross path: highest texture/sampler slot counts bound on the current
  // render encoder. Used to clear trailing slots when a later draw uses fewer
  // resources, preventing stale state leakage between draws.
  uint32_t msl_bound_vertex_texture_count_ = 0;
  uint32_t msl_bound_pixel_texture_count_ = 0;
  uint32_t msl_bound_vertex_sampler_count_ = 0;
  uint32_t msl_bound_pixel_sampler_count_ = 0;
  uint64_t msl_bound_vertex_texture_binding_uid_ = 0;
  uint64_t msl_bound_pixel_texture_binding_uid_ = 0;
  uint64_t msl_bound_vertex_sampler_binding_uid_ = 0;
  uint64_t msl_bound_pixel_sampler_binding_uid_ = 0;
  std::array<MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_bound_vertex_textures_{};
  std::array<MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_bound_pixel_textures_{};
  std::array<MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_bound_vertex_samplers_{};
  std::array<MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_bound_pixel_samplers_{};
  MTL::Buffer* msl_bound_vertex_argument_buffer_ = nullptr;
  MTL::Buffer* msl_bound_pixel_argument_buffer_ = nullptr;
  NS::UInteger msl_bound_vertex_argument_buffer_offset_ = 0;
  NS::UInteger msl_bound_pixel_argument_buffer_offset_ = 0;
  bool msl_bound_vertex_argument_buffer_offset_valid_ = false;
  bool msl_bound_pixel_argument_buffer_offset_valid_ = false;
  MTL::Buffer* msl_bound_shared_memory_buffer_ = nullptr;
  MTL::Buffer* msl_bound_null_buffer_ = nullptr;
  // Cached argument buffer content for change detection — skip re-encoding
  // when textures/samplers haven't changed between draws.
  std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_last_argbuf_vertex_textures_{};
  uint32_t msl_last_argbuf_vertex_texture_count_ = 0;
  std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_last_argbuf_vertex_samplers_{};
  uint32_t msl_last_argbuf_vertex_sampler_count_ = 0;
  MTL::Buffer* msl_last_argbuf_vertex_buffer_ = nullptr;
  NS::UInteger msl_last_argbuf_vertex_offset_ = 0;
  const MslShader::MslTranslation* msl_last_argbuf_vertex_translation_ =
      nullptr;
  uint32_t msl_last_argbuf_vertex_encoded_length_ = 0;
  uint64_t msl_last_argbuf_vertex_layout_uid_ = 0;
  std::array<const MTL::Texture*, MslTextureIndex::kMaxPerStage>
      msl_last_argbuf_pixel_textures_{};
  uint32_t msl_last_argbuf_pixel_texture_count_ = 0;
  std::array<const MTL::SamplerState*, MslSamplerIndex::kMaxPerStage>
      msl_last_argbuf_pixel_samplers_{};
  uint32_t msl_last_argbuf_pixel_sampler_count_ = 0;
  MTL::Buffer* msl_last_argbuf_pixel_buffer_ = nullptr;
  NS::UInteger msl_last_argbuf_pixel_offset_ = 0;
  const MslShader::MslTranslation* msl_last_argbuf_pixel_translation_ = nullptr;
  uint32_t msl_last_argbuf_pixel_encoded_length_ = 0;
  uint64_t msl_last_argbuf_pixel_layout_uid_ = 0;
  // D3D12-style SPIRV constant cache state.
  std::array<uint64_t, 4> msl_current_float_constant_map_vertex_{};
  std::array<uint64_t, 4> msl_current_float_constant_map_pixel_{};
  bool msl_float_constants_dirty_vertex_ = true;
  bool msl_float_constants_dirty_pixel_ = true;
  bool msl_bool_loop_constants_dirty_ = true;
  bool msl_fetch_constants_dirty_ = true;
  std::array<uint8_t, kCbvSizeBytes> msl_cached_float_constants_vertex_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_float_constants_pixel_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_bool_loop_constants_{};
  std::array<uint8_t, kCbvSizeBytes> msl_cached_fetch_constants_{};
  // Uniforms slot binding dedupe.
  MTL::Buffer* msl_bound_uniforms_buffer_ = nullptr;
  NS::UInteger msl_bound_uniforms_vs_base_offset_ = 0;
  NS::UInteger msl_bound_uniforms_ps_base_offset_ = 0;
  bool msl_bound_uniforms_offsets_valid_ = false;
  MTL::RenderPipelineState* msl_bound_pipeline_state_ = nullptr;
  bool msl_viewport_valid_ = false;
  MTL::Viewport msl_viewport_ = {};
  bool msl_scissor_valid_ = false;
  MTL::ScissorRect msl_scissor_ = {};
  bool msl_rasterizer_state_valid_ = false;
  MTL::CullMode msl_cull_mode_ = MTL::CullModeNone;
  MTL::Winding msl_winding_ = MTL::WindingClockwise;
  MTL::TriangleFillMode msl_fill_mode_ = MTL::TriangleFillModeFill;
  float msl_depth_bias_constant_ = 0.0f;
  float msl_depth_bias_slope_ = 0.0f;
  float msl_depth_bias_clamp_ = 0.0f;
  MTL::DepthClipMode msl_depth_clip_mode_ = MTL::DepthClipModeClip;
  MTL::DepthStencilState* msl_depth_stencil_state_ = nullptr;
  bool msl_stencil_reference_valid_ = false;
  uint32_t msl_stencil_reference_ = 0;

  // Fixed-function dynamic state cached per render encoder.
  float ff_blend_factor_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool ff_blend_factor_valid_ = false;

  std::atomic<uint64_t> completed_command_buffers_{0};
  std::atomic<uint32_t> pending_completion_handlers_{0};
  // Summed GPU busy time of the completed command buffers, from Metal's own
  // timestamps rather than the wall clock, so it isn't confounded by whatever
  // the CPU is doing. Published as a rolling per-frame mean by IssueSwap.
  std::atomic<uint64_t> completed_gpu_time_ns_{0};
  uint64_t gpu_time_window_start_ns_ = 0;
  uint32_t gpu_time_window_frames_ = 0;
  // Completed command buffers, published per frame beside the busy time - a
  // sum of per-command-buffer intervals is not readable without it.
  std::atomic<uint64_t> gpu_time_command_buffers_{0};
  uint64_t gpu_time_command_buffers_window_start_ = 0;
  // Created command buffers by kind, to attribute that per-frame count.
  static constexpr size_t kCommandBufferKindCount =
      size_t(CommandBufferKind::kCount);
  uint64_t command_buffer_kind_counts_[kCommandBufferKindCount] = {};
  uint64_t command_buffer_kind_window_start_[kCommandBufferKindCount] = {};
  CommandBufferKind next_submission_kind_ = CommandBufferKind::kSubmissionOther;
  // Each render encoder is a tile store plus an attachment reload on a TBDR
  // GPU, so the count per frame is comparable against Vulkan's render passes.
  uint64_t render_passes_total_ = 0;
  uint64_t render_passes_window_start_ = 0;
  uint64_t submission_current_ = 0;
  uint64_t submission_completed_processed_ = 0;
  // Signaled by the submission completion handler so waits for a specific
  // submission don't have to poll.
  std::mutex completion_mutex_;
  std::condition_variable completion_cond_;

  // ZPD visibility query state. Metal has no query pool object; each physical
  // query segment gets one fresh 8-byte offset in the visibility buffer.
  struct MetalZPDResolve {
    uint64_t submission = 0;
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t scale_area = 1;
    ReportHandle report_handle = kInvalidReportHandle;
    VIZQueryHandle viz;
  };
  struct MetalZPDActiveQuery {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    size_t offset = 0;

    bool is_open() const { return index != UINT32_MAX; }
    void Reset() { *this = {}; }
  };
  std::unique_ptr<MetalZPDVisibilityPool> zpd_visibility_pool_;
  MetalZPDActiveQuery zpd_active_query_;
  std::deque<MetalZPDResolve> zpd_resolves_in_flight_;

  // Draw counter for ring-buffer descriptor heap allocation
  // Each draw uses a different region of the descriptor heap to avoid
  // overwriting previous draws' descriptors before GPU execution
  uint32_t current_draw_index_ = 0;
  bool copy_resolve_writes_pending_ = false;

  // Host viewport of the previous draw, reused while the inputs it was derived
  // from stay the same.
  draw_util::GetViewportInfoArgs previous_viewport_info_args_{};
  draw_util::ViewportInfo previous_viewport_info_{};

  // Memexport tracking for shared memory invalidation.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
  // Whether fences and coherency requests await memexport_ranges_' output.
  bool memexport_ranges_awaited_ = false;
  // Export output lands in guest RAM through the shared buffer, so nothing is
  // ever staged for readback.
  void FlushMemexportStagingReadback() {}
  void AwaitMemexportSubmission(uint64_t submission) {
    AwaitAllQueueOperationsCompletion();
  }
  // Page tracking so a fence the guest reads can await export output. The
  // fragment's host/device routing half is unused - Metal has one buffer.
#include "../command_processor_memexport.inc"
  void NoteMemexportRangesWritten();

  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;

  struct ResolvedRange {
    uint32_t base;
    uint32_t length;
  };
  std::vector<ResolvedRange> resolved_memory_ranges_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
