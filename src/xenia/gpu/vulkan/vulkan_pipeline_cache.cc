/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_builtin_geometry_shader.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

// Shader bytecode.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_indexed_vs.h"
// Placeholder pixel shader for pipeline hot-swap.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_ps.h"
// Ucode interpreter VS placeholder + its debug color pixel shader.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_color_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/ucode_interpreter_vs.h"
}  // namespace shaders

DEFINE_int32(
    vulkan_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "Vulkan");

DECLARE_bool(vulkan_dynamic_rendering);
DECLARE_bool(spirv_disable_rounding_mode_rte);
DECLARE_bool(precise_interpolation);

namespace xe {
namespace gpu {
namespace vulkan {

VulkanPipelineCache::VulkanPipelineCache(
    VulkanCommandProcessor& command_processor,
    const RegisterFile& register_file,
    VulkanRenderTargetCache& render_target_cache,
    VkShaderStageFlags guest_shader_vertex_stages, bool zpd_hybrid_supported)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages),
      zpd_hybrid_supported_(zpd_hybrid_supported),
      guest_shader_cache_(*this, register_file, render_target_cache) {}

VulkanPipelineCache::~VulkanPipelineCache() { Shutdown(); }

std::unique_ptr<SpirvShaderTranslator> VulkanPipelineCache::CreateTranslator()
    const {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  return std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(),
      edram_fragment_shader_interlock, precise_interpolation_supported(),
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());
}

bool VulkanPipelineCache::precise_interpolation_supported() const {
  // The translator gates emission on the device barycentric feature, so the
  // cvar is the only extra control here.
  return cvars::precise_interpolation;
}

bool VulkanPipelineCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  // Cache device features for geometry shader creation.
  const SpirvShaderTranslator::Features features(vulkan_device);
  spirv_version_ = features.spirv_version;
  signed_zero_inf_nan_preserve_float32_ =
      features.signed_zero_inf_nan_preserve_float32;
  denorm_flush_to_zero_float32_ = features.denorm_flush_to_zero_float32;
  rounding_mode_rte_float32_ = features.rounding_mode_rte_float32 &&
                               !cvars::spirv_disable_rounding_mode_rte;

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  if (!guest_shader_cache_.Initialize()) {
    return false;
  }

  {
    std::vector<uint8_t> depth_only_fragment_shader_code =
        guest_shader_cache_.translator().CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(
            depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to create the depth/stencil-only "
          "fragment shader");
      return false;
    }
  }

  if (edram_fragment_shader_interlock) {
    for (size_t i = 0; i < xe::countof(depth_only_fragment_shaders_); ++i) {
      std::vector<uint8_t> depth_only_fragment_shader_code =
          guest_shader_cache_.translator().CreateDepthOnlyFragmentShader(
              xenos::MsaaSamples(i));
      depth_only_fragment_shaders_[i] = ui::vulkan::util::CreateShaderModule(
          vulkan_device,
          reinterpret_cast<const uint32_t*>(
              depth_only_fragment_shader_code.data()),
          depth_only_fragment_shader_code.size());
      if (depth_only_fragment_shaders_[i] == VK_NULL_HANDLE) {
        XELOGE(
            "VulkanPipelineCache: Failed to create the {}-sample "
            "depth/stencil-only fragment shader for the fragment shader "
            "interlock render backend implementation",
            UINT32_C(1) << i);
        return false;
      }
    }
  }

  if (zpd_hybrid_supported_) {
    using DepthStencilMode =
        SpirvShaderTranslator::Modification::DepthStencilMode;
    auto build = [&](DepthStencilMode mode, VkShaderModule& out) -> bool {
      std::vector<uint8_t> code =
          guest_shader_cache_.translator().CreateDepthOnlyFragmentShader(mode,
                                                                         true);
      out = ui::vulkan::util::CreateShaderModule(
          vulkan_device, reinterpret_cast<const uint32_t*>(code.data()),
          code.size());
      return out != VK_NULL_HANDLE;
    };
    if (!build(DepthStencilMode::kNoModifiers,
               zpd_total_depth_only_fragment_shader_) ||
        (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
         (!build(DepthStencilMode::kFloat24Truncating,
                 zpd_total_float24_truncate_fragment_shader_) ||
          !build(DepthStencilMode::kFloat24Rounding,
                 zpd_total_float24_round_fragment_shader_)))) {
      XELOGE(
          "VulkanPipelineCache: Failed to create a ZPD Total depth-only "
          "fragment shader");
      return false;
    }
  }

  // Substitute fragment shaders for guest depth-only draws when in-PS float24
  // conversion is active - keep the depth buffer's encoding consistent with
  // PS-converted draws (matches the DXBC backend's
  // float24_{truncate,round}_ps).
  // Not on the FSI path - it converts the depth in the EDRAM ROP itself, and
  // the modification keeps the sample count where the mode would be.
  if (!edram_fragment_shader_interlock &&
      render_target_cache_.depth_float24_convert_in_pixel_shader()) {
    using DepthStencilMode =
        SpirvShaderTranslator::Modification::DepthStencilMode;
    auto build = [&](DepthStencilMode mode, VkShaderModule& out) -> bool {
      std::vector<uint8_t> code =
          guest_shader_cache_.translator().CreateDepthOnlyFragmentShader(mode);
      out = ui::vulkan::util::CreateShaderModule(
          vulkan_device, reinterpret_cast<const uint32_t*>(code.data()),
          code.size());
      return out != VK_NULL_HANDLE;
    };
    if (!build(DepthStencilMode::kFloat24Truncating,
               float24_truncate_fragment_shader_) ||
        !build(DepthStencilMode::kFloat24Rounding,
               float24_round_fragment_shader_)) {
      XELOGE(
          "VulkanPipelineCache: Failed to create the float24 substitute "
          "depth-only fragment shaders");
      return false;
    }
  }

  // Create tessellation shaders if tessellation is supported.
  if (vulkan_device->properties().tessellationShader) {
    // Vertex shaders for tessellation.
    tessellation_indexed_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_indexed_vs,
        sizeof(shaders::tessellation_indexed_vs));
    tessellation_adaptive_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_adaptive_vs,
        sizeof(shaders::tessellation_adaptive_vs));
    // Discrete mode hull shaders.
    discrete_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_1cp_hs,
        sizeof(shaders::discrete_triangle_1cp_hs));
    discrete_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_3cp_hs,
        sizeof(shaders::discrete_triangle_3cp_hs));
    discrete_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_1cp_hs,
        sizeof(shaders::discrete_quad_1cp_hs));
    discrete_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_4cp_hs,
        sizeof(shaders::discrete_quad_4cp_hs));
    // Continuous mode hull shaders.
    continuous_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_1cp_hs,
        sizeof(shaders::continuous_triangle_1cp_hs));
    continuous_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_3cp_hs,
        sizeof(shaders::continuous_triangle_3cp_hs));
    continuous_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_1cp_hs,
        sizeof(shaders::continuous_quad_1cp_hs));
    continuous_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_4cp_hs,
        sizeof(shaders::continuous_quad_4cp_hs));
    // Adaptive mode hull shaders.
    adaptive_triangle_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_triangle_hs,
        sizeof(shaders::adaptive_triangle_hs));
    adaptive_quad_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_quad_hs,
        sizeof(shaders::adaptive_quad_hs));

    // Verify all tessellation shaders were created successfully.
    if (tessellation_indexed_vs_ == VK_NULL_HANDLE ||
        tessellation_adaptive_vs_ == VK_NULL_HANDLE ||
        discrete_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_4cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_4cp_hs_ == VK_NULL_HANDLE ||
        adaptive_triangle_hs_ == VK_NULL_HANDLE ||
        adaptive_quad_hs_ == VK_NULL_HANDLE) {
      XELOGW(
          "VulkanPipelineCache: Failed to create one or more tessellation "
          "shaders - tessellation will not be available");
    }
  }

  if (cvars::force_depth_clamp && !vulkan_device->properties().depthClamp) {
    XELOGW(
        "force_depth_clamp is enabled, but the device doesn't support depth "
        "clamping - guest draws with clipping enabled will still be clipped "
        "to the host planes");
  }

  // Create placeholder pixel shader for pipeline hot-swap (stutter reduction).
  placeholder_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::placeholder_ps, sizeof(shaders::placeholder_ps));
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to create placeholder pixel shader - "
        "pipeline hot-swap will not be available");
  }

  // Create the ucode interpreter VS placeholder (and its debug color PS). If it
  // fails, the interpreter is simply not used - the normal placeholder path
  // still applies.
  ucode_interpreter_vs_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::ucode_interpreter_vs,
      sizeof(shaders::ucode_interpreter_vs));
  if (ucode_interpreter_vs_ == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to create the ucode interpreter vertex "
        "shader - the VS interpreter placeholder will not be available");
  }
  placeholder_color_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::placeholder_color_ps,
      sizeof(shaders::placeholder_color_ps));

  // Create Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  if (vulkan_device->functions().vkCreatePipelineCache(
          vulkan_device->device(), &pipeline_cache_create_info, nullptr,
          &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW("VulkanPipelineCache: Failed to create pipeline cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  creation_queue_.Initialize(
      "Vulkan Pipelines",
      [this](const PipelineCreationArguments& creation_arguments,
             SpirvShaderTranslator* worker_translator) {
        return CreateQueuedPipeline(creation_arguments, worker_translator);
      },
      [this](const PipelineCreationArguments& creation_arguments,
             VkPipeline pipeline) {
        StoreCreatedPipeline(creation_arguments, pipeline, false);
      },
      [this]() { return guest_shader_cache_.CreateWorkerTranslator(); });
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                   logical_processor_count);
    }
    creation_queue_.SetThreadCount(creation_thread_count);
  }

  return true;
}

void VulkanPipelineCache::Shutdown() {
  // Shut down shader storage first.
  ShutdownShaderStorage();

  // Shut down all threads, before destroying the pipelines since they may be
  // creating them. Also drops the pending completion callback, which may
  // capture 'this'.
  std::vector<std::pair<PipelineCreationArguments, VkPipeline>> parked =
      creation_queue_.Shutdown();
  startup_loading_ = false;

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Process any remaining deferred destructions (force destroy all since
  // device should be idle at shutdown).
  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    for (const auto& pipeline_pair : deferred_destroy_pipelines_) {
      if (pipeline_pair.first != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline_pair.first, nullptr);
      }
    }
    deferred_destroy_pipelines_.clear();
  }

  // Nothing will publish what the creation threads held back, so destroy it.
  for (const auto& publication : parked) {
    if (publication.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, publication.second, nullptr);
    }
  }

  // Destroy all pipelines.
  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    if (pipeline_pair.second.pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline_pair.second.pipeline, nullptr);
    }
  }
  pipelines_.clear();

  // Destroy the pipeline cache.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  // Destroy all internal shaders.
  for (VkShaderModule& depth_only_fragment_shader :
       depth_only_fragment_shaders_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                           depth_only_fragment_shader);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         zpd_total_depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyShaderModule, device,
      zpd_total_float24_truncate_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyShaderModule, device,
      zpd_total_float24_round_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         float24_truncate_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         float24_round_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_pixel_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         ucode_interpreter_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_color_pixel_shader_);
  // Destroy tessellation shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_indexed_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_adaptive_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_triangle_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_quad_hs_);
  for (const auto& geometry_shader_pair : geometry_shaders_) {
    if (geometry_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, geometry_shader_pair.second, nullptr);
    }
  }
  geometry_shaders_.clear();

  // Destroy all translated shaders.
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  // Shut down shader translation.
  guest_shader_cache_.Shutdown();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address,
                                              uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  uint64_t data_hash =
      XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader =
      new VulkanShader(command_processor_.GetVulkanDevice(), shader_type,
                       data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  return SpirvShaderTranslator::Modification(
      guest_shader_cache_.GetVertexShaderModification(
          shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_used));
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader) const {
  return SpirvShaderTranslator::Modification(
      guest_shader_cache_.GetPixelShaderModification(
          shader, interpolator_mask, param_gen_pos, normalized_depth_control,
          normalized_color_mask, apply_polygon_offset_in_shader));
}

bool VulkanPipelineCache::EnsureShadersTranslated(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    SpirvShaderTranslator* translator) {
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  // The shared translator is single-threaded, so background (creation-thread)
  // callers pass their own worker translator. Runtime translation always claims
  // the translation so it happens once even if draw and creation threads race
  // the same modification. The ucode interpreter defers translation entirely by
  // not calling this on the draw thread - the creation thread does it here.
  SpirvShaderTranslator& used_translator =
      translator ? *translator : guest_shader_cache_.translator();
  if (!vertex_shader->is_translated()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(used_translator, *vertex_shader,
                                 /*use_try_claim=*/true)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated()) {
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(used_translator, *pixel_shader,
                                   /*use_try_claim=*/true)) {
        XELOGE("Failed to translate the pixel shader!");
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::CanCreatePipelineAsync(bool has_pixel_shader) const {
  return cvars::async_shader_compilation && creation_queue_.has_threads() &&
         has_pixel_shader && placeholder_pixel_shader_ != VK_NULL_HANDLE;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanPipelineCache::GetGuestGraphicsPipelineLayout(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader) {
  // Binding counts come from translation. A shader whose bindings aren't ready
  // yet (being translated on a creation thread) reports 0 - reading its binding
  // vectors here would race the creation thread populating them. That yields
  // the minimal layout; the creation thread recomputes the real one once ready.
  auto texture_count =
      [](const VulkanShader::VulkanTranslation* translation) -> size_t {
    const VulkanShader& shader =
        static_cast<const VulkanShader&>(translation->shader());
    return shader.bindings_ready()
               ? shader.GetTextureBindingsAfterTranslation().size()
               : 0;
  };
  auto sampler_count =
      [](const VulkanShader::VulkanTranslation* translation) -> size_t {
    const VulkanShader& shader =
        static_cast<const VulkanShader&>(translation->shader());
    return shader.bindings_ready()
               ? shader.GetSamplerBindingsAfterTranslation().size()
               : 0;
  };
  return command_processor_.GetPipelineLayout(
      pixel_shader ? texture_count(pixel_shader) : 0,
      pixel_shader ? sampler_count(pixel_shader) : 0,
      texture_count(vertex_shader), sampler_count(vertex_shader));
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    bool use_interpreter, bool zpd_total,
    VulkanPipelineCache::Pipeline** pipeline_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  PipelineDescription description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, render_pass_key,
          zpd_total, description)) {
    return false;
  }
  if (last_pipeline_ && last_pipeline_->first == description) {
    *pipeline_out = &last_pipeline_->second;
    return true;
  }
  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    last_pipeline_ = &*it;
    *pipeline_out = &it->second;
    return true;
  }

  // Create the pipeline if not the latest and not already existing. For an
  // interpreter placeholder the shaders aren't translated yet, so their binding
  // counts read as 0 and this yields the minimal (no-texture) layout - the
  // creation thread upgrades it to the real layout after translation.
  const PipelineLayoutProvider* pipeline_layout =
      GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader);
  if (!pipeline_layout) {
    return false;
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  if (description.geometry_shader != PipelineGeometryShader::kNone) {
    GeometryShaderKey geometry_shader_key;
    GuestSpirvShaderCache::GetGeometryShaderKey(
        description.geometry_shader, vertex_shader->modification(),
        pixel_shader ? pixel_shader->modification() : 0, geometry_shader_key);
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return false;
    }
  }

  VkRenderPass render_pass =
      render_target_cache_.GetPath() ==
              RenderTargetCache::Path::kPixelShaderInterlock
          ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
          : render_target_cache_.GetHostRenderTargetsRenderPass(
                render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }

  auto& pipeline_pair =
      *pipelines_.emplace(description, Pipeline(pipeline_layout)).first;

  if (storage_writer_.is_active()) {
    VulkanShader& vs = static_cast<VulkanShader&>(vertex_shader->shader());
    if (vs.try_set_ucode_storage_index(storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&vs);
    }
    if (pixel_shader) {
      VulkanShader& ps = static_cast<VulkanShader&>(pixel_shader->shader());
      if (ps.try_set_ucode_storage_index(storage_writer_.storage_index())) {
        shader_storage_file_flush_needed_ = true;
        storage_writer_.QueueShaderWrite(&ps);
      }
    }

    pipeline_storage_file_flush_needed_ = true;
    PipelineStoredDescription stored_description;
    stored_description.description_hash = description.GetHash();
    std::memcpy(&stored_description.description, &description,
                sizeof(description));
    storage_writer_.QueuePipelineWrite(stored_description);
  }

  // Get tessellation shaders if needed.
  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  XELOGD("VulkanPipelineCache: ConfigurePipeline tessellation_mode={} patch={}",
         static_cast<uint32_t>(description.tessellation_mode),
         static_cast<uint32_t>(description.tessellation_patch));
  if (description.tessellation_mode != PipelineTessellationMode::kNone) {
    tessellation_vertex_shader =
        GetTessellationVertexShader(description.tessellation_mode);
    // Determine if we should use multi-control-point hull shaders.
    // For adaptive mode, we always use multi-control-point (per-edge factors).
    // For discrete/continuous, the host vertex shader type determines this.
    bool use_control_point_count =
        (description.tessellation_mode == PipelineTessellationMode::kAdaptive);
    tessellation_control_shader = GetTessellationControlShader(
        description.tessellation_mode, description.tessellation_patch,
        use_control_point_count);
    if (tessellation_vertex_shader == VK_NULL_HANDLE ||
        tessellation_control_shader == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to get tessellation shaders for mode {} "
          "patch {}",
          static_cast<uint32_t>(description.tessellation_mode),
          static_cast<uint32_t>(description.tessellation_patch));
      return false;
    }
  }

  // Pipeline hot-swap: When async mode is enabled, we have creation threads and
  // a pixel shader, create a placeholder pipeline immediately (fast compile)
  // and queue the real pipeline creation in the background. This reduces
  // stutter from pipeline compilation.
  bool use_async = CanCreatePipelineAsync(pixel_shader != nullptr);

  // The interpreter can only stand in via the async placeholder path.
  use_interpreter =
      use_interpreter && use_async && ucode_interpreter_vs_ != VK_NULL_HANDLE;

  if (use_async) {
    // The draw thread never translates for async pipelines (so it never stalls
    // on background translation). Create an immediate placeholder when we can -
    // the interpreter VS, or the real VS if it's already translated - and queue
    // the real pipeline (translate + create) on a background thread. A
    // non-interpretable draw whose shaders aren't translated yet gets NO
    // placeholder; the caller skips it until the real pipeline is ready. The
    // pipeline layout starts minimal (0 texture counts from untranslated
    // shaders) and is upgraded to the real one on the creation thread.
    pipeline_pair.second.uses_interpreter.store(use_interpreter,
                                                std::memory_order_release);
    bool make_placeholder = use_interpreter || vertex_shader->is_translated();
    if (make_placeholder) {
      // Set is_placeholder BEFORE creating the pipeline to avoid a race with
      // the creation thread checking this flag.
      pipeline_pair.second.is_placeholder.store(true,
                                                std::memory_order_release);
      PipelineCreationArguments placeholder_args;
      placeholder_args.pipeline = &pipeline_pair;
      placeholder_args.vertex_shader = vertex_shader;
      placeholder_args.pixel_shader = nullptr;  // Will use placeholder PS
      placeholder_args.geometry_shader = geometry_shader;
      placeholder_args.tessellation_vertex_shader = tessellation_vertex_shader;
      placeholder_args.tessellation_control_shader =
          tessellation_control_shader;
      placeholder_args.render_pass = render_pass;
      placeholder_args.render_pass_key = render_pass_key;
      bool placeholder_created =
          use_interpreter
              ? EnsurePipelineCreatedWithInterpreterPlaceholder(
                    placeholder_args)
              : EnsurePipelineCreatedWithPlaceholder(placeholder_args);
      if (placeholder_created) {
        if (use_interpreter) {
          XELOGI(
              "VS interpreter placeholder created (interpreter VS + no-op PS): "
              "VS {:016X}, PS {:016X}",
              vertex_shader->shader().ucode_data_hash(),
              pixel_shader ? pixel_shader->shader().ucode_data_hash() : 0);
        }
      } else {
        XELOGW(
            "VS {} placeholder FAILED to create - dropping draws until the "
            "real "
            "pipeline is ready: VS {:016X}, PS {:016X}",
            use_interpreter ? "interpreter" : "real-VS",
            vertex_shader->shader().ucode_data_hash(),
            pixel_shader ? pixel_shader->shader().ucode_data_hash() : 0);
        // No placeholder - fall through to queue-only (drop until ready).
        pipeline_pair.second.is_placeholder.store(false,
                                                  std::memory_order_release);
        pipeline_pair.second.uses_interpreter.store(false,
                                                    std::memory_order_release);
      }
    }

    // Queue the real pipeline creation in the background.
    PipelineCreationArguments creation_arguments;
    creation_arguments.pipeline = &pipeline_pair;
    creation_arguments.vertex_shader = vertex_shader;
    creation_arguments.pixel_shader = pixel_shader;
    creation_arguments.geometry_shader = geometry_shader;
    creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
    creation_arguments.tessellation_control_shader =
        tessellation_control_shader;
    creation_arguments.render_pass = render_pass;
    creation_arguments.render_pass_key = render_pass_key;
    pipeline_pair.second.creation_pending.store(true,
                                                std::memory_order_relaxed);
    creation_queue_.Push(creation_arguments);
  } else {
    // Sync mode (no creation threads / async off / no pixel shader): translate
    // on this thread and create the pipeline immediately.
    if (!vertex_shader->is_translated()) {
      if (!EnsureShadersTranslated(vertex_shader, pixel_shader)) {
        return false;
      }
      const PipelineLayoutProvider* real_layout =
          GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader);
      if (!real_layout) {
        return false;
      }
      pipeline_pair.second.pipeline_layout.store(real_layout,
                                                 std::memory_order_release);
    }
    PipelineCreationArguments creation_arguments;
    creation_arguments.pipeline = &pipeline_pair;
    creation_arguments.vertex_shader = vertex_shader;
    creation_arguments.pixel_shader = pixel_shader;
    creation_arguments.geometry_shader = geometry_shader;
    creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
    creation_arguments.tessellation_control_shader =
        tessellation_control_shader;
    creation_arguments.render_pass = render_pass;
    creation_arguments.render_pass_key = render_pass_key;
    if (!EnsurePipelineCreated(creation_arguments)) {
      return false;
    }
  }

  last_pipeline_ = &pipeline_pair;
  *pipeline_out = &pipeline_pair.second;
  return true;
}

void VulkanPipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_file_flush_needed_,
                                 pipeline_storage_file_flush_needed_);
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (creation_queue_.has_threads()) {
    if (startup_loading_) {
      // Non-blocking: let background threads work asynchronously.
      creation_queue_.Notify();
    } else {
      // Blocking: wait for all queued pipelines.
      creation_queue_.AwaitCompletion();
    }
  }

  // Process deferred destructions when the GPU is idle.
  ProcessDeferredDestructions();
}

bool VulkanPipelineCache::IsCreatingPipelines() {
  return creation_queue_.IsBusy();
}

void VulkanPipelineCache::AwaitPipelineCompletion() {
  creation_queue_.AwaitCompletion();
}

VkPipeline VulkanPipelineCache::CreateQueuedPipeline(
    const PipelineCreationArguments& creation_arguments,
    SpirvShaderTranslator* worker_translator) {
  VkPipeline created_pipeline = VK_NULL_HANDLE;
  const char* failed_stage = nullptr;
  if (!EnsureShadersTranslated(creation_arguments.vertex_shader,
                               creation_arguments.pixel_shader,
                               worker_translator)) {
    failed_stage = "shader translation";
  } else {
    // Async pipelines are created with a minimal (no-texture) layout on the
    // draw thread since their shaders weren't translated there (interpreter
    // placeholder, or drop-until-ready with no placeholder). Now that they're
    // translated, compute the real layout before creating the real pipeline.
    // Idempotent when the layout was already real (real-VS placeholder path).
    const PipelineLayoutProvider* real_layout = GetGuestGraphicsPipelineLayout(
        creation_arguments.vertex_shader, creation_arguments.pixel_shader);
    if (real_layout) {
      creation_arguments.pipeline->second.pipeline_layout.store(
          real_layout, std::memory_order_release);
    }
    if (!EnsurePipelineCreated(creation_arguments, VK_NULL_HANDLE,
                               VK_NULL_HANDLE, &created_pipeline)) {
      failed_stage = "pipeline creation";
    }
  }
  if (failed_stage) {
    // Many of EnsurePipelineCreated's failure paths log nothing, so this is
    // the only trace of what went wrong for most of them.
    XELOGE("Async {} failed (VS {:016X}, PS {:016X})", failed_stage,
           creation_arguments.vertex_shader->shader().ucode_data_hash(),
           creation_arguments.pixel_shader
               ? creation_arguments.pixel_shader->shader().ucode_data_hash()
               : 0);
  }
  return created_pipeline;
}

bool VulkanPipelineCache::TranslateAnalyzedShader(
    SpirvShaderTranslator& translator,
    VulkanShader::VulkanTranslation& translation, bool use_try_claim) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.shader());

  // Perform translation (optimization is already disabled in translator
  // constructor). If this fails the shader will be marked as invalid and
  // ignored later. The translation object is not thread-safe, so when draw and
  // creation threads can both reach the same modification, claim it so it's
  // translated exactly once and the loser waits for the winner.
  if (!translation.is_translated()) {
    bool should_translate = true;
    if (use_try_claim) {
      should_translate = translation.TryClaimTranslation();
      if (!should_translate) {
        while (!translation.is_translated()) {
          xe::threading::MaybeYield();
        }
      }
    }
    if (should_translate) {
      bool profile = cvars::shader_profiling;
      std::chrono::steady_clock::time_point spirv_gen_start;
      if (profile) {
        spirv_gen_start = std::chrono::steady_clock::now();
      }
      if (!translator.TranslateAnalyzedShader(translation)) {
        XELOGE("Shader {:016X} translation failed; marking as ignored",
               shader.ucode_data_hash());
        return false;
      }
      if (profile) {
        XELOGI(
            "shader_profiling: {} {:016X} ucode->SPIR-V {:.3f} ms {} B SPIR-V",
            shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
            shader.ucode_data_hash(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - spirv_gen_start)
                .count(),
            translation.translated_binary().size());
      }
    }
  }

  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }

  // Dump shader files if desired.
  if (!cvars::dump_shaders.empty()) {
    translation.Dump(cvars::dump_shaders, "vulkan");
  }

  // Set up the texture binding layout.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    // texture_binding_layout_map_ / texture_binding_layouts_ are shared and may
    // be mutated concurrently now that pixel shaders (which can have textures)
    // are translated on multiple creation threads for deferred interpreter
    // draws.
    std::lock_guard<std::mutex> layout_lock(layouts_mutex_);
    // Obtain the unique IDs of the binding layout if there are any texture
    // bindings, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      uint64_t texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because it's assumed that 0 is the UID for "
            "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);

    // Use the sampler count for samplers because it's the only thing that must
    // be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for creating
    // actual samplers).
    static_assert(
        kLayoutUIDEmpty == 0,
        "Empty layout UID is assumed to be 0 because for bindful samplers, the "
        "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(
        shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    bool edram_fsi_used) {
  uint64_t translation_start = xe::Clock::QueryHostTickCount();

  std::vector<std::pair<VulkanShader*, uint64_t>> translations_to_do;
  translations_to_do.reserve(translations_needed.size());
  for (const auto& needed : translations_needed) {
    auto shader_it = shaders_.find(needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    VulkanShader* shader = shader_it->second;
    VulkanShader::VulkanTranslation* translation =
        static_cast<VulkanShader::VulkanTranslation*>(
            shader->GetOrCreateTranslation(needed.second));
    if (translation && !translation->is_translated()) {
      translations_to_do.emplace_back(shader, needed.second);
    }
  }

  if (translations_to_do.empty()) {
    return;
  }

  std::atomic<size_t> translation_index{0};
  std::atomic<size_t> translations_completed{0};

  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  bool msaa_2x_attachments =
      render_target_cache_.msaa_2x_attachments_supported();
  bool msaa_2x_no_attachments =
      render_target_cache_.msaa_2x_no_attachments_supported();
  uint32_t draw_res_x = render_target_cache_.draw_resolution_scale_x();
  uint32_t draw_res_y = render_target_cache_.draw_resolution_scale_y();
  bool precise_interpolation = precise_interpolation_supported();

  auto translate_function =
      [this, &translations_to_do, &translation_index, &translations_completed,
       vulkan_device, msaa_2x_attachments, msaa_2x_no_attachments,
       edram_fsi_used, precise_interpolation, draw_res_x, draw_res_y]() {
        // Each thread needs its own translator.
        SpirvShaderTranslator translator(
            SpirvShaderTranslator::Features(vulkan_device), msaa_2x_attachments,
            msaa_2x_no_attachments, edram_fsi_used, precise_interpolation,
            draw_res_x, draw_res_y);

        while (true) {
          size_t index = translation_index.fetch_add(1);
          if (index >= translations_to_do.size()) {
            break;
          }
          VulkanShader* shader = translations_to_do[index].first;
          uint64_t modification = translations_to_do[index].second;
          VulkanShader::VulkanTranslation* translation =
              static_cast<VulkanShader::VulkanTranslation*>(
                  shader->GetTranslation(modification));
          if (translation && !translation->is_translated()) {
            if (TranslateAnalyzedShader(translator, *translation)) {
              translations_completed.fetch_add(1);
            }
          }
        }
      };

  size_t thread_count = 0;
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    uint32_t logical_processor_count =
        std::max(uint32_t(1), xe::threading::logical_processor_count());
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      thread_count = std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                              logical_processor_count);
    }
    thread_count = std::min(thread_count, translations_to_do.size());
  }

  std::vector<std::unique_ptr<xe::threading::Thread>> translation_threads;
  for (size_t i = 0; i < thread_count; ++i) {
    auto thread = xe::threading::Thread::Create({}, translate_function);
    if (thread) {
      thread->set_name("Shader Translation");
      translation_threads.push_back(std::move(thread));
    }
  }

  // Main thread also participates.
  translate_function();

  for (auto& thread : translation_threads) {
    xe::threading::Wait(thread.get(), false);
  }

  XELOGI("Translated {} shaders in {} ms", translations_completed.load(),
         (xe::Clock::QueryHostTickCount() - translation_start) * 1000 /
             xe::Clock::QueryHostTickFrequency());
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    assert_zero(write_mask & ~uint32_t(0b1111));
    // 32 because of 0x1F mask, for safety (all unknown to zero).
    static constexpr PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstColor,
        /*  9 */ PipelineBlendFactor::kOneMinusDstColor,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantColor,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantColor,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    // Like kBlendFactorMap, but with the color factors changed to their alpha
    // equivalents. Alpha is scalar, so hardware treats a _COLOR factor in the
    // alpha slot as the matching _ALPHA factor.
    static constexpr PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstAlpha,
        /*  9 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantAlpha,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    render_target_out.src_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorAlphaMap[uint32_t(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorAlphaMap[uint32_t(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()
             ->properties()
             .constantAlphaColorBlendFactors) {
      if (blend_control.color_srcblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_srcblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
      if (blend_control.color_destblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_destblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
    }
  } else {
    render_target_out.src_color_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_color_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.color_blend_op = xenos::BlendOp::kAdd;
    render_target_out.src_alpha_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.alpha_blend_op = xenos::BlendOp::kAdd;
  }
  render_target_out.color_write_mask = write_mask;
}

bool VulkanPipelineCache::GetCurrentStateDescription(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key, bool zpd_total,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  const RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash =
      vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();
  if (pixel_shader) {
    description_out.pixel_shader_hash =
        pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }
  description_out.render_pass_key = render_pass_key;
  description_out.zpd_total = uint32_t(zpd_total);

  // TODO(Triang3l): Implement primitive types currently using geometry shaders
  // without them.
  PipelineGeometryShader geometry_shader = PipelineGeometryShader::kNone;
  PipelinePrimitiveTopology primitive_topology;

  // Handle tessellated and non-tessellated draws separately, like D3D12.
  if (primitive_processing_result.IsTessellated()) {
    // Tessellation is enabled - use patch list topology.
    primitive_topology = PipelinePrimitiveTopology::kPatchList;

    // Get tessellation mode from registers.
    auto vgt_hos_cntl = regs.Get<reg::VGT_HOS_CNTL>();
    switch (vgt_hos_cntl.tess_mode) {
      case xenos::TessellationMode::kDiscrete:
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
      case xenos::TessellationMode::kContinuous:
        description_out.tessellation_mode =
            PipelineTessellationMode::kContinuous;
        break;
      case xenos::TessellationMode::kAdaptive:
        description_out.tessellation_mode = PipelineTessellationMode::kAdaptive;
        break;
      default:
        // Unknown tessellation mode, fall back to discrete.
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
    }

    // Determine patch type based on primitive type.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTrianglePatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kQuadPatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kQuad;
        break;
      default:
        XELOGE("VulkanPipelineCache: Unsupported tessellated primitive type {}",
               uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }
  } else {
    // Non-tessellated draw.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_shader = PipelineGeometryShader::kPointList;
        primitive_topology = PipelinePrimitiveTopology::kPointList;
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
        // Host lines are 1 host pixel wide, expand them to 1 guest pixel when
        // the draw is resolution-scaled. Without geometry shaders, lines stay
        // thin rather than being dropped.
        if (device_properties.geometryShader &&
            (render_target_cache_.GetDrawScaleX() > 1 ||
             render_target_cache_.GetDrawScaleY() > 1)) {
          geometry_shader = PipelineGeometryShader::kLineList;
        }
        primitive_topology = primitive_processing_result.host_primitive_type ==
                                     xenos::PrimitiveType::kLineList
                                 ? PipelinePrimitiveTopology::kLineList
                                 : PipelinePrimitiveTopology::kLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        // The check should be performed at primitive processing time.
        assert_true(device_properties.triangleFans);
        primitive_topology = PipelinePrimitiveTopology::kTriangleFan;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
        break;
      case xenos::PrimitiveType::kRectangleList:
        // Only use geometry shader if not using the fallback AsTriangleStrip
        // vertex shader type (which is used when geometry shaders aren't
        // supported).
        if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
          geometry_shader = PipelineGeometryShader::kRectangleList;
        }
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_shader = PipelineGeometryShader::kQuadList;
        primitive_topology = PipelinePrimitiveTopology::kLineListWithAdjacency;
        break;
      default:
        // TODO(Triang3l): Remaining primitive types.
        return false;
    }
  }
  description_out.geometry_shader = geometry_shader;
  description_out.primitive_topology = primitive_topology;
  description_out.primitive_restart =
      primitive_processing_result.host_primitive_reset_enabled;

  // With force_depth_clamp, use the host viewport clamp instead of near and far
  // Z plane clipping. X/Y/W clipping is unchanged. Both 494707EE and 41560881
  // have passes that rely on alpha inputs that currently gets dropped by
  // near-plane clipping.
  // TODO(boma): Investigate whether the difference is in shader arithmetic or
  // the clipper itself.
  description_out.depth_clamp_enable =
      device_properties.depthClamp &&
      (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ||
       cvars::force_depth_clamp);

  // TODO(Triang3l): Tessellation.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (primitive_polygonal) {
    // Vulkan only allows the polygon mode to be set for both faces - pick the
    // most special one (more likely to represent the developer's deliberate
    // intentions - fill is very generic, wireframe is common in debug, points
    // are for pretty unusual things, but closer to debug purposes too - on the
    // Xenos, points have the lowest register value and triangles have the
    // highest) based on which faces are not culled.
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    description_out.cull_front = cull_front;
    description_out.cull_back = cull_back;
    if (device_properties.fillModeNonSolid) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!cull_front) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!cull_back) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        polygon_type = xenos::PolygonType::kTriangles;
      }
      switch (polygon_type) {
        case xenos::PolygonType::kPoints:
          // When points are not supported, use lines instead, preserving
          // debug-like purpose.
          description_out.polygon_mode = device_properties.pointPolygons
                                             ? PipelinePolygonMode::kPoint
                                             : PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kLines:
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          assert_unhandled_case(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    if (render_pass_key.depth_and_color_used & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_write_enable =
            normalized_depth_control.z_write_enable;
        description_out.depth_compare_op = normalized_depth_control.zfunc;
      } else {
        description_out.depth_compare_op = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_test_enable = 1;
        description_out.stencil_front_fail_op =
            normalized_depth_control.stencilfail;
        description_out.stencil_front_pass_op =
            normalized_depth_control.stencilzpass;
        description_out.stencil_front_depth_fail_op =
            normalized_depth_control.stencilzfail;
        description_out.stencil_front_compare_op =
            normalized_depth_control.stencilfunc;
        if (primitive_polygonal && normalized_depth_control.backface_enable) {
          description_out.stencil_back_fail_op =
              normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_pass_op =
              normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_depth_fail_op =
              normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_compare_op =
              normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_compare_op =
              description_out.stencil_front_compare_op;
        }
      }
    }

    // Color blending and write masks (filled only for the attachments present
    // in the render pass object).
    uint32_t render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    assert_true(device_properties.independentBlend);
    uint32_t render_pass_color_rts_remaining = render_pass_color_rts;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(render_pass_color_rts_remaining,
                                &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<reg::RB_BLENDCONTROL>(
              reg::RB_BLENDCONTROL::rt_register_indices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

bool VulkanPipelineCache::ArePipelineRequirementsMet(
    const PipelineDescription& description) const {
  if (description.zpd_total && !zpd_hybrid_supported_) {
    return false;
  }

  VkShaderStageFlags vertex_shader_stage =
      Shader::IsHostVertexShaderTypeDomain(
          SpirvShaderTranslator::Modification(
              description.vertex_shader_modification)
              .vertex.host_vertex_shader_type)
          ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
          : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }

  if (!device_properties.triangleFans &&
      description.primitive_topology ==
          PipelinePrimitiveTopology::kTriangleFan) {
    return false;
  }

  if (!device_properties.depthClamp && description.depth_clamp_enable) {
    return false;
  }

  if (!device_properties.pointPolygons &&
      description.polygon_mode == PipelinePolygonMode::kPoint) {
    return false;
  }

  if (!device_properties.fillModeNonSolid &&
      description.polygon_mode != PipelinePolygonMode::kFill) {
    return false;
  }

  assert_true(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    uint32_t color_rts_remaining =
        description.render_pass_key.depth_and_color_used >> 1;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      const PipelineRenderTarget& color_rt =
          description.render_targets[color_rt_index];
      if (color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha) {
        return false;
      }
    }
  }

  return true;
}

VkShaderModule VulkanPipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }

  // When user clip planes are present, the max count is used to reduce
  // variants.
  std::vector<unsigned int> shader_code =
      BuildGuestPrimitiveGeometryShaderSpirv(
          BuiltinGeometryShaderType(uint32_t(key.type)), key.interpolator_count,
          key.user_clip_plane_count > 0 ? 6u : 0u,
          bool(key.user_clip_plane_cull), bool(key.has_vertex_kill_and),
          bool(key.has_point_size), bool(key.has_point_coordinates),
          spirv_version_, denorm_flush_to_zero_float32_,
          signed_zero_inf_nan_preserve_float32_, rounding_mode_rte_float32_);

  // With --dump_shaders, write the built-in GS SPIR-V for signature checks.
  if (!cvars::dump_shaders.empty() && !shader_code.empty()) {
    std::filesystem::path dir = std::filesystem::absolute(cvars::dump_shaders);
    std::filesystem::create_directories(dir);
    std::filesystem::path spirv_path =
        dir / fmt::format("shader_geometry_{:08X}.spirv.bin.geom", key.key);
    if (FILE* spirv_file = xe::filesystem::OpenFile(spirv_path, "wb")) {
      fwrite(shader_code.data(), sizeof(uint32_t), shader_code.size(),
             spirv_file);
      fclose(spirv_file);
    }
  }

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(),
      reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  geometry_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkShaderModule VulkanPipelineCache::GetTessellationControlShader(
    PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
    bool use_control_point_count) const {
  if (mode == PipelineTessellationMode::kNone ||
      patch_type == PipelineTessellationPatchType::kNone) {
    return VK_NULL_HANDLE;
  }

  switch (mode) {
    case PipelineTessellationMode::kDiscrete:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? discrete_triangle_3cp_hs_
                                       : discrete_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? discrete_quad_4cp_hs_
                                       : discrete_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kContinuous:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? continuous_triangle_3cp_hs_
                                       : continuous_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? continuous_quad_4cp_hs_
                                       : continuous_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kAdaptive:
      // Adaptive mode always uses per-corner control points.
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return adaptive_triangle_hs_;
      } else {
        return adaptive_quad_hs_;
      }
    default:
      return VK_NULL_HANDLE;
  }
}

VkShaderModule VulkanPipelineCache::GetTessellationVertexShader(
    PipelineTessellationMode mode) const {
  if (mode == PipelineTessellationMode::kNone) {
    return VK_NULL_HANDLE;
  }
  // Adaptive mode reads edge factors from index buffer; other modes pass
  // vertex indices.
  return (mode == PipelineTessellationMode::kAdaptive)
             ? tessellation_adaptive_vs_
             : tessellation_indexed_vs_;
}

VkShaderModule VulkanPipelineCache::GetPlaceholderFragmentShader(
    const PipelineCreationArguments& creation_arguments,
    bool allow_debug_color) const {
  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock) {
    // Depth goes through the pixel shader here, so write it like a depth-only
    // draw rather than drawing nothing.
    VkShaderModule depth_only_fragment_shader =
        depth_only_fragment_shaders_[size_t(
            creation_arguments.pipeline->first.render_pass_key.msaa_samples)];
    if (depth_only_fragment_shader != VK_NULL_HANDLE) {
      return depth_only_fragment_shader;
    }
    // Not null - creating_placeholder is derived from this being non-null.
    return placeholder_pixel_shader_;
  }
  if (allow_debug_color && cvars::async_shader_vs_interpreter_debug_color &&
      placeholder_color_pixel_shader_ != VK_NULL_HANDLE) {
    return placeholder_color_pixel_shader_;
  }
  return placeholder_pixel_shader_;
}

bool VulkanPipelineCache::EnsurePipelineCreatedWithInterpreterPlaceholder(
    const PipelineCreationArguments& creation_arguments) {
  return EnsurePipelineCreated(
      creation_arguments,
      GetPlaceholderFragmentShader(creation_arguments,
                                   /*allow_debug_color=*/true),
      ucode_interpreter_vs_);
}

bool VulkanPipelineCache::EnsurePipelineCreated(
    const PipelineCreationArguments& creation_arguments,
    VkShaderModule fragment_shader_override,
    VkShaderModule vertex_shader_override,
    VkPipeline* out_unpublished_pipeline) {
  if (out_unpublished_pipeline) {
    // Defined on every path below, including the early returns.
    *out_unpublished_pipeline = VK_NULL_HANDLE;
  }
  // Check if we already have a pipeline.
  // If it's a placeholder and we're not creating another placeholder,
  // we need to replace it with the real pipeline.
  VkPipeline existing_pipeline =
      creation_arguments.pipeline->second.pipeline.load(
          std::memory_order_acquire);
  bool is_placeholder = creation_arguments.pipeline->second.is_placeholder.load(
      std::memory_order_acquire);
  bool creating_placeholder = fragment_shader_override != VK_NULL_HANDLE;

  if (existing_pipeline != VK_NULL_HANDLE) {
    if (!is_placeholder || creating_placeholder) {
      // Already have a real pipeline, or trying to create another placeholder.
      return true;
    }
    // Have a placeholder, and we're creating the real pipeline to replace it.
  }

  // This function preferably should validate the description to prevent
  // unsupported behavior that may be dangerous/crashing because pipelines can
  // be created from the disk storage.

  if (creation_arguments.pixel_shader) {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash(),
             creation_arguments.pixel_shader->shader().ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!ArePipelineRequirementsMet(description)) {
    assert_always(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  bool is_tessellated =
      description.tessellation_mode != PipelineTessellationMode::kNone;

  // Up to 5 shader stages: VS, TCS, TES, GS, FS.
  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  uint32_t shader_stage_count = 0;

  // Vertex shader or tessellation evaluation shader. The interpreter
  // placeholder substitutes its own fixed VS module, so the guest VS is
  // intentionally not translated yet in that case.
  if (vertex_shader_override == VK_NULL_HANDLE) {
    assert_true(creation_arguments.vertex_shader->is_translated());
    if (!creation_arguments.vertex_shader->is_valid()) {
      return false;
    }
  }

  if (is_tessellated) {
    // For tessellation: use our pre-compiled VS for passing data to TCS,
    // then TCS (hull shader), then the translated Xenos vertex shader as TES
    // (domain shader).

    // Tessellation vertex shader (passes indices to TCS).
    VkPipelineShaderStageCreateInfo& shader_stage_tess_vs =
        shader_stages[shader_stage_count++];
    shader_stage_tess_vs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_vs.pNext = nullptr;
    shader_stage_tess_vs.flags = 0;
    shader_stage_tess_vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_tess_vs.module = creation_arguments.tessellation_vertex_shader;
    shader_stage_tess_vs.pName = "main";
    shader_stage_tess_vs.pSpecializationInfo = nullptr;

    // Tessellation control shader (hull shader).
    VkPipelineShaderStageCreateInfo& shader_stage_tcs =
        shader_stages[shader_stage_count++];
    shader_stage_tcs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tcs.pNext = nullptr;
    shader_stage_tcs.flags = 0;
    shader_stage_tcs.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shader_stage_tcs.module = creation_arguments.tessellation_control_shader;
    shader_stage_tcs.pName = "main";
    shader_stage_tcs.pSpecializationInfo = nullptr;

    // Tessellation evaluation shader (domain shader) - the translated Xenos
    // vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_tes =
        shader_stages[shader_stage_count++];
    shader_stage_tes.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tes.pNext = nullptr;
    shader_stage_tes.flags = 0;
    shader_stage_tes.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    // GetOrCreateShaderModule, not shader_module(): the module is created
    // lazily after is_translated() is set, and another thread may have
    // translated the VS without creating the module yet, so shader_module()
    // could still be null.
    shader_stage_tes.module =
        creation_arguments.vertex_shader->GetOrCreateShaderModule();
    if (shader_stage_tes.module == VK_NULL_HANDLE) {
      return false;
    }
    shader_stage_tes.pName = "main";
    shader_stage_tes.pSpecializationInfo = nullptr;
  } else {
    // Non-tessellated: standard vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_vertex =
        shader_stages[shader_stage_count++];
    shader_stage_vertex.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    // GetOrCreateShaderModule, not shader_module() - see the TES note above.
    shader_stage_vertex.module =
        vertex_shader_override != VK_NULL_HANDLE
            ? vertex_shader_override
            : creation_arguments.vertex_shader->GetOrCreateShaderModule();
    if (shader_stage_vertex.module == VK_NULL_HANDLE) {
      return false;
    }
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;
  }

  // Geometry shader.
  if (creation_arguments.geometry_shader != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo& shader_stage_geometry =
        shader_stages[shader_stage_count++];
    shader_stage_geometry.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_geometry.pNext = nullptr;
    shader_stage_geometry.flags = 0;
    shader_stage_geometry.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shader_stage_geometry.module = creation_arguments.geometry_shader;
    shader_stage_geometry.pName = "main";
    shader_stage_geometry.pSpecializationInfo = nullptr;
  }
  // Fragment shader.
  VkPipelineShaderStageCreateInfo& shader_stage_fragment =
      shader_stages[shader_stage_count++];
  shader_stage_fragment.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_fragment.pNext = nullptr;
  shader_stage_fragment.flags = 0;
  shader_stage_fragment.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stage_fragment.module = VK_NULL_HANDLE;
  shader_stage_fragment.pName = "main";
  shader_stage_fragment.pSpecializationInfo = nullptr;
  if (fragment_shader_override != VK_NULL_HANDLE) {
    // Use the override shader (for placeholder pipelines).
    shader_stage_fragment.module = fragment_shader_override;
  } else if (creation_arguments.pixel_shader) {
    assert_true(creation_arguments.pixel_shader->is_translated());
    if (!creation_arguments.pixel_shader->is_valid()) {
      return false;
    }
    shader_stage_fragment.module =
        creation_arguments.pixel_shader->GetOrCreateShaderModule();
    if (shader_stage_fragment.module == VK_NULL_HANDLE) {
      return false;
    }
  } else {
    if (description.zpd_total) {
      // Native ZPD query without a guest pixel shader.
      // Coverage still has to be counted.
      shader_stage_fragment.module = zpd_total_depth_only_fragment_shader_;
      if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
          (description.depth_write_enable ||
           description.depth_compare_op != xenos::CompareFunction::kAlways) &&
          (description.render_pass_key.depth_and_color_used & 0b1) &&
          description.render_pass_key.depth_format ==
              xenos::DepthRenderTargetFormat::kD24FS8) {
        shader_stage_fragment.module =
            render_target_cache_.depth_float24_round()
                ? zpd_total_float24_round_fragment_shader_
                : zpd_total_float24_truncate_fragment_shader_;
      }
    } else if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shaders_[size_t(
          description.render_pass_key.msaa_samples)];
    } else if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
               (description.depth_write_enable ||
                description.depth_compare_op !=
                    xenos::CompareFunction::kAlways) &&
               (description.render_pass_key.depth_and_color_used & 0b1) &&
               description.render_pass_key.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24FS8) {
      // No guest pixel shader, but depth matters and the host buffer is
      // float24 - bind a substitute that converts gl_FragCoord.z so the
      // depth buffer encoding stays consistent with PS-converted draws.
      shader_stage_fragment.module = render_target_cache_.depth_float24_round()
                                         ? float24_round_fragment_shader_
                                         : float24_truncate_fragment_shader_;
    } else if (!description.depth_write_enable) {
      // Bind an empty PS to force rasterization.
      // D3D drops PS-less draws without depth/stencil writes,
      // breaking occlusion queries (4541096E, 5553083B).
      // Vulkan stencil write mask is dynamic state. Only check depth.
      shader_stage_fragment.module = depth_only_fragment_shader_;
    }
  }
  if (shader_stage_fragment.module == VK_NULL_HANDLE) {
    --shader_stage_count;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology =
          VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      assert_unhandled_case(description.primitive_topology);
      return false;
  }
  input_assembly_state.primitiveRestartEnable =
      description.primitive_restart ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.depthClampEnable =
      description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kFill:
      rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
      break;
    case PipelinePolygonMode::kLine:
      rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    default:
      assert_unhandled_case(description.polygon_mode);
      return false;
  }
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  rasterization_state.frontFace = description.front_face_clockwise
                                      ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Depth bias is dynamic (even toggling - pipeline creation is expensive).
  // "If no depth attachment is present, r is undefined" in the depth bias
  // formula, though Z has no effect on anything if a depth attachment is not
  // used (the guest shader can't access Z), enabling only when there's a
  // depth / stencil attachment for correctness.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock &&
       (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  // TODO(Triang3l): Wide lines.
  rasterization_state.lineWidth = 1.0f;

  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  if (description.render_pass_key.msaa_samples == xenos::MsaaSamples::k2X &&
      !render_target_cache_.IsMsaa2xSupported(
          !edram_fragment_shader_interlock &&
          description.render_pass_key.depth_and_color_used != 0)) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
    // sample locations, but still top-left and bottom-right - however, this can
    // be adjusted with custom sample locations).
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    sample_mask = 0b1001;
    // TODO(Triang3l): Research sample mask behavior without attachments (in
    // Direct3D, it's completely ignored in this case).
    multisample_state.pSampleMask = &sample_mask;
  } else {
    multisample_state.rasterizationSamples = VkSampleCountFlagBits(
        uint32_t(1) << uint32_t(description.render_pass_key.msaa_samples));
  }

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil_state.pNext = nullptr;
  if (!edram_fragment_shader_interlock) {
    if (description.depth_write_enable ||
        description.depth_compare_op != xenos::CompareFunction::kAlways) {
      depth_stencil_state.depthTestEnable = VK_TRUE;
      depth_stencil_state.depthWriteEnable =
          description.depth_write_enable ? VK_TRUE : VK_FALSE;
      depth_stencil_state.depthCompareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    uint32_t color_rts_used =
        description.render_pass_key.depth_and_color_used >> 1;
    {
      static constexpr VkBlendFactor kBlendFactorMap[] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_CONSTANT_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
          VK_BLEND_FACTOR_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
      };
      // 8 entries for safety since 3 bits from the guest are passed directly.
      static constexpr VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_SUBTRACT,
                                                  VK_BLEND_OP_MIN,
                                                  VK_BLEND_OP_MAX,
                                                  VK_BLEND_OP_REVERSE_SUBTRACT,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD};
      // Check if the shader pre-multiplies by blend factors for MIN/MAX.
      SpirvShaderTranslator::Modification pixel_shader_modification(
          description.pixel_shader_modification);
      bool rt0_rgb_premult =
          pixel_shader_modification.pixel.rt0_blend_rgb_factor_for_premult !=
          xenos::BlendFactor::kOne;
      bool rt0_a_premult =
          pixel_shader_modification.pixel.rt0_blend_a_factor_for_premult !=
          xenos::BlendFactor::kOne;

      uint32_t color_rts_remaining = color_rts_used;
      uint32_t color_rt_index;
      while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
        VkPipelineColorBlendAttachmentState& color_blend_attachment =
            color_blend_attachments[color_rt_index];
        const PipelineRenderTarget& color_rt =
            description.render_targets[color_rt_index];
        if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.color_blend_op != xenos::BlendOp::kAdd ||
            color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
          color_blend_attachment.blendEnable = VK_TRUE;
          color_blend_attachment.srcColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp =
              kBlendOpMap[uint32_t(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp =
              kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];

          // If the shader pre-multiplies by the source blend factor for RT0
          // MIN/MAX, set the pipeline source factor to ONE since it's already
          // applied in the shader.
          if (color_rt_index == 0) {
            if (rt0_rgb_premult) {
              color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            }
            if (rt0_a_premult) {
              color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
          }
        }
        // Not writing an output leaves the target undefined, not unchanged.
        color_blend_attachment.colorWriteMask =
            fragment_shader_override != VK_NULL_HANDLE &&
                    fragment_shader_override == placeholder_pixel_shader_
                ? 0
                : VkColorComponentFlags(color_rt.color_write_mask);
      }
    }
    color_blend_state.attachmentCount = 32 - xe::lzcnt(color_rts_used);
    color_blend_state.pAttachments = color_blend_attachments;
  }

  std::array<VkDynamicState, 7> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  // Regardless of whether some of this state actually has any effect on the
  // pipeline, marking all as dynamic because otherwise, binding any pipeline
  // with such state not marked as dynamic will cause the dynamic state to be
  // invalidated (again, even if it has no effect).
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (!edram_fragment_shader_interlock) {
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_DEPTH_BIAS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_REFERENCE;
  }

  // Tessellation state (only used when tessellation is active).
  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  tessellation_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  if (is_tessellated) {
    // Determine patch control point count based on mode and patch type.
    // For adaptive mode, we use the actual patch corner count (3 for triangles,
    // 4 for quads) since each control point has its own edge factor.
    // For discrete/continuous modes, we use 1 control point (the Xenos vertex
    // shader receives the patch index and computes all corners internally).
    if (description.tessellation_mode == PipelineTessellationMode::kAdaptive) {
      tessellation_state.patchControlPoints =
          (description.tessellation_patch ==
           PipelineTessellationPatchType::kTriangle)
              ? 3
              : 4;
    } else {
      tessellation_state.patchControlPoints = 1;
    }
  }

  // Dynamic rendering support (VK_KHR_dynamic_rendering / Vulkan 1.3).
  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VulkanRenderTargetCache::RenderPassKey key =
        creation_arguments.render_pass_key;

    // Set up color attachment formats.
    xenos::ColorRenderTargetFormat color_formats[] = {
        key.color_0_view_format, key.color_1_view_format,
        key.color_2_view_format, key.color_3_view_format};
    uint32_t color_attachment_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (key.depth_and_color_used & (1 << (1 + i))) {
        color_attachment_formats[i] =
            render_target_cache_.GetColorVulkanFormat(color_formats[i]);
        color_attachment_count = i + 1;
      }
    }
    pipeline_rendering_create_info.colorAttachmentCount =
        color_attachment_count;
    pipeline_rendering_create_info.pColorAttachmentFormats =
        color_attachment_formats;

    // Set up depth/stencil format.
    if (key.depth_and_color_used & 0b1) {
      VkFormat depth_format =
          render_target_cache_.GetDepthVulkanFormat(key.depth_format);
      pipeline_rendering_create_info.depthAttachmentFormat = depth_format;
      pipeline_rendering_create_info.stencilAttachmentFormat = depth_format;
    }
  }

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext =
      use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = shader_stage_count;
  pipeline_create_info.pStages = shader_stages.data();
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState =
      is_tessellated ? &tessellation_state : nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      creation_arguments.pipeline->second.pipeline_layout
          .load(std::memory_order_acquire)
          ->GetPipelineLayout();
  pipeline_create_info.renderPass =
      use_dynamic_rendering ? VK_NULL_HANDLE : creation_arguments.render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  bool profile = cvars::shader_profiling;
  std::chrono::steady_clock::time_point pso_create_start;
  if (profile) {
    pso_create_start = std::chrono::steady_clock::now();
  }
  VkPipeline pipeline;
  VkResult result = dfn.vkCreateGraphicsPipelines(
      device, vk_pipeline_cache_, 1, &pipeline_create_info, nullptr, &pipeline);
  if (result == VK_SUCCESS && profile) {
    // Driver SPIR-V->ISA compile + link time.
    XELOGI(
        "shader_profiling: pipeline create ({}) VS {:016X} PS {:016X} {:.3f} "
        "ms",
        creating_placeholder ? "placeholder" : "real",
        creation_arguments.vertex_shader->shader().ucode_data_hash(),
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->shader().ucode_data_hash()
            : 0,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pso_create_start)
            .count());
  }
  if (result != VK_SUCCESS) {
    if (creation_arguments.pixel_shader) {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    } else {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    }
    return false;
  }

  if (out_unpublished_pipeline) {
    // The caller publishes it, in its turn.
    *out_unpublished_pipeline = pipeline;
    return true;
  }
  StoreCreatedPipeline(creation_arguments, pipeline, creating_placeholder);
  return true;
}

void VulkanPipelineCache::StoreCreatedPipeline(
    const PipelineCreationArguments& creation_arguments, VkPipeline pipeline,
    bool creating_placeholder) {
  if (pipeline == VK_NULL_HANDLE) {
    // Nothing else will be created for this entry, so say what it is left as.
    // Clearing the flag also stops it reporting as "waiting for real" forever,
    // which would hang an occlusion-query await.
    const bool had_placeholder =
        creation_arguments.pipeline->second.is_placeholder.load(
            std::memory_order_acquire);
    XELOGE(
        "Pipeline stuck for the rest of the run, {} (VS {:016X}, PS {:016X})",
        had_placeholder ? "drawing through its placeholder"
                        : "its draws skipped",
        creation_arguments.vertex_shader->shader().ucode_data_hash(),
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->shader().ucode_data_hash()
            : 0);
    if (had_placeholder) {
      creation_arguments.pipeline->second.is_placeholder.store(
          false, std::memory_order_release);
    }
    creation_arguments.pipeline->second.creation_pending.store(
        false, std::memory_order_release);
    return;
  }
  // Record the placeholder handle before publishing it, so a draw that observes
  // this pipeline handle also observes it as the placeholder (see Pipeline::
  // placeholder_pipeline). Stored before the exchange so the release on the
  // exchange carries it.
  if (creating_placeholder) {
    creation_arguments.pipeline->second.placeholder_pipeline.store(
        pipeline, std::memory_order_release);
  }

  // Store the new pipeline, handling placeholder hot-swap.
  VkPipeline old_pipeline =
      creation_arguments.pipeline->second.pipeline.exchange(
          pipeline, std::memory_order_acq_rel);

  if (old_pipeline != VK_NULL_HANDLE) {
    // We're replacing a placeholder pipeline with the real one.
    // Queue the old placeholder for deferred destruction, recording the
    // current submission number so we only destroy after the GPU is done.
    uint64_t current_submission = command_processor_.GetCurrentSubmission();
    {
      std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
      deferred_destroy_pipelines_.emplace_back(old_pipeline,
                                               current_submission);
    }
  }

  // Mark as no longer a placeholder (for the case where we just created real).
  if (!creating_placeholder) {
    creation_arguments.pipeline->second.is_placeholder.store(
        false, std::memory_order_release);
    XELOGI("Pipeline created for VS {:016X}, PS {:016X}",
           creation_arguments.vertex_shader->shader().ucode_data_hash(),
           creation_arguments.pixel_shader
               ? creation_arguments.pixel_shader->shader().ucode_data_hash()
               : 0);
    creation_arguments.pipeline->second.creation_pending.store(
        false, std::memory_order_release);
  }
}

void VulkanPipelineCache::ProcessDeferredDestructions() {
  std::vector<VkPipeline> pipelines_to_destroy;

  uint64_t completed_submission = command_processor_.GetCompletedSubmission();

  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    if (deferred_destroy_pipelines_.empty()) {
      return;
    }

    // Only destroy pipelines whose submission has completed on the GPU.
    // Keep pipelines that are still potentially in-flight.
    auto it = deferred_destroy_pipelines_.begin();
    while (it != deferred_destroy_pipelines_.end()) {
      if (it->second <= completed_submission) {
        // This submission has completed, safe to destroy.
        pipelines_to_destroy.push_back(it->first);
        it = deferred_destroy_pipelines_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (pipelines_to_destroy.empty()) {
    return;
  }

  // Destroy pipelines now that we know GPU is done with them.
  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  ShutdownShaderStorage();

  shader_storage_title_id_ = title_id;

  if (!blocking) {
    startup_loading_ = true;
    if (completion_callback) {
      completion_callback = [this, orig = std::move(completion_callback)]() {
        startup_loading_ = false;
        orig();
      };
    } else {
      completion_callback = [this]() { startup_loading_ = false; };
    }
  }

  bool edram_fsi_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  // Full ZPD counters change every FSI fragment shader, so they get their own
  // storage.
  pipeline_config.file_suffix = fmt::format(
      ".{}{}.vk.xpso", edram_fsi_used ? "fsi" : "fbo",
      edram_fsi_used && cvars::occlusion_query_full_counters ? "-fc" : "");
  pipeline_config.api_magic = kPipelineStorageAPIMagicVulkan;
  // Sum so a bump of either version invalidates - both only ever move up.
  pipeline_config.version = PipelineDescription::kVersion +
                            SpirvShaderTranslator::Modification::kVersion;

  uint32_t storage_index = storage_writer_.storage_index() + 1;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          // Shader load callback.
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            VulkanShader* shader =
                LoadShader(type, ucode_dwords, ucode_dword_count);
            if (!shader || shader->ucode_storage_index() == storage_index) {
              return true;  // Continue reading.
            }
            shader->set_ucode_storage_index(storage_index);
            if (!shader->is_ucode_analyzed()) {
              shader->AnalyzeUcode(ucode_disasm_buffer_);
            }
            return true;
          },
          // Shader translate callback - handles parallel translation.
          [this, edram_fsi_used](const std::set<std::pair<uint64_t, uint64_t>>
                                     & translations_needed) {
            TranslateShadersForStorage(translations_needed, edram_fsi_used);
          },
          pipeline_stored_descriptions)) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

  // Load VkPipelineCache from disk if available.
  auto shader_storage_local_root = GetShaderStorageLocalRoot(cache_root);
  if (!std::filesystem::exists(shader_storage_local_root)) {
    std::error_code ec;
    std::filesystem::create_directories(shader_storage_local_root, ec);
  }
  vk_pipeline_cache_path_ =
      shader_storage_local_root /
      fmt::format("{:08X}.vk.bin", shader_storage_title_id_);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Try to load existing VkPipelineCache data.
  std::vector<uint8_t> pipeline_cache_data;
  if (FILE* cache_file =
          xe::filesystem::OpenFile(vk_pipeline_cache_path_, "rb")) {
    xe::filesystem::Seek(cache_file, 0, SEEK_END);
    int64_t cache_size = xe::filesystem::Tell(cache_file);
    if (cache_size > 0) {
      pipeline_cache_data.resize(size_t(cache_size));
      xe::filesystem::Seek(cache_file, 0, SEEK_SET);
      pipeline_cache_data.resize(fread(pipeline_cache_data.data(), 1,
                                       pipeline_cache_data.size(), cache_file));
    }
    fclose(cache_file);
    XELOGI("Loaded {} bytes of VkPipelineCache data",
           pipeline_cache_data.size());
  }

  // Recreate the VkPipelineCache with the loaded data.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  pipeline_cache_create_info.initialDataSize = pipeline_cache_data.size();
  pipeline_cache_create_info.pInitialData =
      pipeline_cache_data.empty() ? nullptr : pipeline_cache_data.data();
  if (dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                                &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW(
        "VulkanPipelineCache: Failed to create pipeline cache with "
        "initial data, creating empty cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
    pipeline_cache_create_info.initialDataSize = 0;
    pipeline_cache_create_info.pInitialData = nullptr;
    dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                              &vk_pipeline_cache_);
  }

  // Create pipelines from stored descriptions.
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start = xe::Clock::QueryHostTickCount();

    size_t pipelines_created = 0;
    size_t pipelines_already_exist = 0;
    size_t pipelines_vs_not_found = 0;
    size_t pipelines_vs_translation_missing = 0;
    size_t pipelines_ps_not_found = 0;
    size_t pipelines_ps_translation_missing = 0;
    size_t pipelines_render_pass_failed = 0;
    size_t pipelines_layout_failed = 0;
    size_t pipelines_requirements_not_met = 0;

    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description =
          pipeline_stored_description.description;

      // Skip pipelines not supported by this device.
      if (!ArePipelineRequirementsMet(pipeline_description)) {
        ++pipelines_requirements_not_met;
        continue;
      }

      // Skip already known pipelines.
      auto it = pipelines_.find(pipeline_description);
      if (it != pipelines_.end()) {
        ++pipelines_already_exist;
        continue;
      }

      // Look up vertex shader.
      auto vertex_shader_it =
          shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        ++pipelines_vs_not_found;
        continue;
      }
      VulkanShader* vertex_shader = vertex_shader_it->second;
      VulkanShader::VulkanTranslation* vertex_translation =
          static_cast<VulkanShader::VulkanTranslation*>(
              vertex_shader->GetTranslation(
                  pipeline_description.vertex_shader_modification));
      if (!vertex_translation || !vertex_translation->is_translated() ||
          !vertex_translation->is_valid()) {
        ++pipelines_vs_translation_missing;
        continue;
      }

      // Look up pixel shader if present.
      VulkanShader* pixel_shader = nullptr;
      VulkanShader::VulkanTranslation* pixel_translation = nullptr;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it =
            shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          ++pipelines_ps_not_found;
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pixel_translation = static_cast<VulkanShader::VulkanTranslation*>(
            pixel_shader->GetTranslation(
                pipeline_description.pixel_shader_modification));
        if (!pixel_translation || !pixel_translation->is_translated() ||
            !pixel_translation->is_valid()) {
          ++pipelines_ps_translation_missing;
          continue;
        }
      }

      // Get render pass.
      VkRenderPass render_pass =
          render_target_cache_.GetPath() ==
                  RenderTargetCache::Path::kPixelShaderInterlock
              ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
              : render_target_cache_.GetHostRenderTargetsRenderPass(
                    pipeline_description.render_pass_key);
      if (render_pass == VK_NULL_HANDLE) {
        ++pipelines_render_pass_failed;
        continue;
      }

      // Get pipeline layout.
      const PipelineLayoutProvider* pipeline_layout =
          command_processor_.GetPipelineLayout(
              pixel_shader
                  ? pixel_shader->GetTextureBindingsAfterTranslation().size()
                  : 0,
              pixel_shader
                  ? pixel_shader->GetSamplerBindingsAfterTranslation().size()
                  : 0,
              vertex_shader->GetTextureBindingsAfterTranslation().size(),
              vertex_shader->GetSamplerBindingsAfterTranslation().size());
      if (!pipeline_layout) {
        ++pipelines_layout_failed;
        continue;
      }

      // Get geometry shader if needed.
      VkShaderModule geometry_shader = VK_NULL_HANDLE;
      if (pipeline_description.geometry_shader !=
          PipelineGeometryShader::kNone) {
        GeometryShaderKey geometry_shader_key;
        GuestSpirvShaderCache::GetGeometryShaderKey(
            pipeline_description.geometry_shader,
            vertex_translation->modification(),
            pixel_translation ? pixel_translation->modification() : 0,
            geometry_shader_key);
        geometry_shader = GetGeometryShader(geometry_shader_key);
        if (geometry_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Get tessellation shaders if needed.
      VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
      VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
      if (pipeline_description.tessellation_mode !=
          PipelineTessellationMode::kNone) {
        tessellation_vertex_shader =
            GetTessellationVertexShader(pipeline_description.tessellation_mode);
        bool use_control_point_count =
            (pipeline_description.tessellation_mode ==
             PipelineTessellationMode::kAdaptive);
        tessellation_control_shader = GetTessellationControlShader(
            pipeline_description.tessellation_mode,
            pipeline_description.tessellation_patch, use_control_point_count);
        if (tessellation_vertex_shader == VK_NULL_HANDLE ||
            tessellation_control_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Create the pipeline entry.
      auto& pipeline_pair =
          *pipelines_.emplace(pipeline_description, Pipeline(pipeline_layout))
               .first;

      PipelineCreationArguments creation_arguments;
      creation_arguments.pipeline = &pipeline_pair;
      creation_arguments.vertex_shader = vertex_translation;
      creation_arguments.pixel_shader = pixel_translation;
      creation_arguments.geometry_shader = geometry_shader;
      creation_arguments.tessellation_vertex_shader =
          tessellation_vertex_shader;
      creation_arguments.tessellation_control_shader =
          tessellation_control_shader;
      creation_arguments.render_pass = render_pass;
      creation_arguments.render_pass_key = pipeline_description.render_pass_key;
      if (creation_queue_.has_threads()) {
        // Nothing is drawing these yet, so they publish as they are built.
        creation_arguments.pipeline->second.creation_pending.store(
            true, std::memory_order_relaxed);
        creation_queue_.PushUnordered(creation_arguments);
      } else {
        // No creation threads - create synchronously.
        EnsurePipelineCreated(creation_arguments);
      }

      ++pipelines_created;
    }

    if (creation_queue_.has_threads()) {
      if (blocking) {
        // Blocking mode: wait for all pipelines to be created.
        creation_queue_.AwaitCompletion();
      } else {
        // Non-blocking mode: the creation threads invoke the callback when they
        // are done - or leave it to the tail of this function if they already
        // are.
        creation_queue_.TakeCompletionCallback(completion_callback);
      }
    }

    XELOGI("Pipeline cache: {} created, {} already exist, {} total in {} ms",
           pipelines_created, pipelines_already_exist,
           pipeline_stored_descriptions.size(),
           (xe::Clock::QueryHostTickCount() - pipeline_creation_start) * 1000 /
               xe::Clock::QueryHostTickFrequency());

    if (pipelines_vs_not_found || pipelines_vs_translation_missing ||
        pipelines_ps_not_found || pipelines_ps_translation_missing ||
        pipelines_render_pass_failed || pipelines_layout_failed ||
        pipelines_requirements_not_met) {
      XELOGI(
          "Pipeline cache skipped: {} VS not found, {} VS translation missing, "
          "{} PS not found, {} PS translation missing, {} render pass failed, "
          "{} layout failed, {} requirements not met",
          pipelines_vs_not_found, pipelines_vs_translation_missing,
          pipelines_ps_not_found, pipelines_ps_translation_missing,
          pipelines_render_pass_failed, pipelines_layout_failed,
          pipelines_requirements_not_met);
    }
  }

  // Invoke completion callback if no async work was queued.
  if (completion_callback) {
    completion_callback();
  }
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  // Save VkPipelineCache to disk before shutting down storage.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE &&
      !vk_pipeline_cache_path_.empty()) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    size_t cache_size = 0;
    if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                   nullptr) == VK_SUCCESS &&
        cache_size > 0) {
      std::vector<uint8_t> cache_data(cache_size);
      if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                     cache_data.data()) == VK_SUCCESS) {
        if (FILE* cache_file =
                xe::filesystem::OpenFile(vk_pipeline_cache_path_, "wb")) {
          fwrite(cache_data.data(), 1, cache_size, cache_file);
          fclose(cache_file);
          XELOGI("Saved {} bytes of VkPipelineCache data", cache_size);
        }
      }
    }
  }
  vk_pipeline_cache_path_.clear();

  // Shut down the storage writer (closes files, stops write thread).
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
