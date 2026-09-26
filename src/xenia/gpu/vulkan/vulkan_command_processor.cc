/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_fsi_system_constants.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(gpu_debug_markers);
DECLARE_bool(memexport_enable);
DECLARE_bool(submit_on_primary_buffer_end);

DEFINE_bool(
    vulkan_dynamic_rendering, true,
    "Use VK_KHR_dynamic_rendering instead of traditional render passes. "
    "May improve or worsen performance depending on driver. Requires Vulkan "
    "1.3 or VK_KHR_dynamic_rendering extension support.",
    "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_extreme_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeUniformBufferDynamic = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        SpirvShaderTranslator::kConstantBufferCount*
            kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageBufferAndImage[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kLinkedTypeDescriptorPoolSetCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kLinkedTypeDescriptorPoolSetCount}};

// 2x descriptors for texture images because of unsigned and signed bindings.
constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         2 * kLinkedTypeDescriptorPoolSetCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(
    VulkanGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      completion_timeline_(static_cast<const ui::vulkan::VulkanProvider*>(
                               graphics_system->provider())
                               ->vulkan_device(),
                           "cp"),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBufferDynamic, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_and_image_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeStorageBufferAndImage, 2,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures,
          uint32_t(xe::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::UpdateDebugMarkersEnabled() {
  // Enable debug markers if the CVAR is set or RenderDoc is detected.
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void VulkanCommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_.CmdVkBeginDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_buffer_.CmdVkEndDebugUtilsLabelEXT();
}

void VulkanCommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_.CmdVkInsertDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  shared_memory_->InvalidateAllPages();
}

void VulkanCommandProcessor::ClearReadbackBuffers() {
  if (AwaitAllQueueOperationsCompletion()) {
    ClearReadbackStagingBuffers();
    // Their staging buffers are gone, so there is nothing left to copy out.
    memexport_staged_.clear();
  }
}

void VulkanCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                      uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void VulkanCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking,
                                           std::move(completion_callback));
}

void VulkanCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Starting a new frame because descriptors may be needed.
  if (!BeginSubmission(true)) {
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void VulkanCommandProcessor::PollCompletedSubmission() {
  // Strict ZPD can skip unnecessary work here that can wait for the next full
  // CheckSubmissionCompletionAndDeviceLoss and it's not needed for retirement.
  if (device_lost_) {
    return;
  }
  completion_timeline_.AwaitSubmissionAndUpdateCompleted(
      GetCompletedSubmission());
  PumpQueryResolves();
}

std::string VulkanCommandProcessor::GetTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  switch (render_target_cache_->GetPath()) {
    case RenderTargetCache::Path::kHostRenderTargets:
      suffix << " - FBO";
      break;
    case RenderTargetCache::Path::kPixelShaderInterlock:
      suffix << " - FSI";
      break;
    default:
      break;
  }
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  // Check if debug markers should be enabled (CVAR or RenderDoc detection).
  UpdateDebugMarkersEnabled();
  if (debug_markers_enabled_) {
    XELOGI("GPU debug markers enabled for RenderDoc/debug tools");
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      xe::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                         size_t(16384)),
                size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_empty_) != VK_SUCCESS) {
    XELOGE("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding =
        descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferSystem]
          .stageFlags =
      // Visible to the vertex/domain stages for user clip planes and to the
      // control/eval/vertex stages for tessellation, plus the usual guest
      // stages.
      guest_shader_stages | guest_shader_vertex_stages_ |
      (device_properties.tessellationShader
           ? (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
           : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatVertex]
          .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatPixel]
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferBoolLoop]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFetch]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(xe::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_constants_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: one storage buffer. Kept identically defined to the render
  // target cache's storage buffer layout - sets from both are bound into the
  // same slots, and stage flags are part of layout compatibility.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings =
      &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBuffer)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer");
    return false;
  }
  // Transient: the texture load scratch buffer plus the image the compute blit
  // writes, replacing vkCmdCopyBufferToImage.
  VkDescriptorSetLayoutBinding
      descriptor_set_layout_bindings_storage_buffer_and_image[2] = {};
  descriptor_set_layout_bindings_storage_buffer_and_image[0].binding = 0;
  descriptor_set_layout_bindings_storage_buffer_and_image[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings_storage_buffer_and_image[0].descriptorCount =
      1;
  descriptor_set_layout_bindings_storage_buffer_and_image[0].stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings_storage_buffer_and_image[1].binding = 1;
  descriptor_set_layout_bindings_storage_buffer_and_image[1].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_set_layout_bindings_storage_buffer_and_image[1].descriptorCount =
      1;
  descriptor_set_layout_bindings_storage_buffer_and_image[1].stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_create_info.bindingCount = 2;
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_storage_buffer_and_image;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::
                  kStorageBufferAndStorageImage)]) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "and a storage image");
    return false;
  }

  shared_memory_ = std::make_unique<VulkanSharedMemory>(
      *this, *memory_, trace_writer_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Read-watch consumption tracking for resolves.
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  primitive_processor_ = std::make_unique<VulkanPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1)
                                         << shared_memory_binding_count_log2;

  // Requires the transient descriptor set layouts.
  // Get draw resolution scale and clamp based on device capabilities
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);
  // Check if sparse binding is supported for resolution scaling
  bool has_sparse_binding = device_properties.sparseBinding &&
                            device_properties.sparseResidencyBuffer;
  if (!TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, has_sparse_binding,
          0)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    XELOGW(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  // Shared memory, EDRAM, and ZPD counter descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  zpd_hybrid_supported_ =
      cvars::occlusion_query_full_counters &&
      !edram_fragment_shader_interlock &&
      device_properties.fragmentStoresAndAtomics &&
      device_properties.sampleRateShading &&
      shared_memory_binding_count <
          device_properties.maxPerStageDescriptorStorageBuffers;
  bool zpd_counter_binding_used =
      edram_fragment_shader_interlock || zpd_hybrid_supported_;
  VkDescriptorSetLayoutBinding
      shared_memory_and_edram_descriptor_set_layout_bindings[3];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags =
      guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers =
      nullptr;
  VkDescriptorSetLayoutCreateInfo
      shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (zpd_counter_binding_used) {
    // ZPD counter, used by FSI and hybrid queries.
    VkDescriptorSetLayoutBinding& zpd_counter_binding =
        shared_memory_and_edram_descriptor_set_layout_bindings[1];
    zpd_counter_binding.binding = 1;
    zpd_counter_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    zpd_counter_binding.descriptorCount = 1;
    zpd_counter_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    zpd_counter_binding.pImmutableSamplers = nullptr;
  }
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    VkDescriptorSetLayoutBinding& edram_binding =
        shared_memory_and_edram_descriptor_set_layout_bindings[2];
    edram_binding.binding = 2;
    edram_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    edram_binding.descriptorCount = 1;
    edram_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    edram_binding.pImmutableSamplers = nullptr;
  }
  shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount =
      1 + uint32_t(zpd_counter_binding_used) +
      uint32_t(edram_fragment_shader_interlock);
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info,
          nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_,
      guest_shader_vertex_stages_, zpd_hybrid_supported_);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }

  // Requires the transient descriptor set layouts.
  // Use the same draw resolution scale as render target cache
  texture_cache_ = VulkanTextureCache::Create(
      *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = draw_resolution_scale_x;
  zpd_draw_resolution_scale_y_ = draw_resolution_scale_y;

  const VkDeviceSize zpd_counter_sink_range =
      XenosZPDReport::kCounterSizeBytes * kZPDQueryPoolCapacity;
  if (zpd_counter_binding_used &&
      !ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, zpd_counter_sink_range,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          zpd_counter_sink_buffer_, zpd_counter_sink_buffer_memory_)) {
    XELOGE("Failed to create the ZPD counter sink buffer");
    return false;
  }

  // Shared memory, EDRAM, and ZPD counter common bindings. A second set
  // (binding 0 -> host-imported buffer) is allocated for memexport routing when
  // the host buffer is available.
  const bool create_host_shared_memory_set =
      shared_memory_->host_buffer() != VK_NULL_HANDLE;
  const uint32_t shared_memory_set_count =
      create_host_shared_memory_set ? 2u : 1u;
  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_set_count *
      (shared_memory_binding_count + uint32_t(edram_fragment_shader_interlock) +
       uint32_t(zpd_counter_binding_used));
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = shared_memory_set_count;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool =
      shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  if (create_host_shared_memory_set) {
    if (dfn.vkAllocateDescriptorSets(
            device, &descriptor_set_allocate_info,
            &shared_memory_host_and_edram_descriptor_set_) != VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the host-imported Vulkan descriptor set for "
          "shared memory memexport routing");
      return false;
    }
  }
  // Writes binding 0 (shared memory, split across shared_memory_binding_count
  // sub-ranges) and, when used, binding 1 (ZPD counter placeholder) and
  // binding 2 (EDRAM, FSI only) of one shared-memory/EDRAM set.
  // shared_memory_buffer selects the device-local buffer or the host-imported
  // buffer.
  auto write_shared_memory_and_edram_set = [&](VkDescriptorSet set,
                                               VkBuffer shared_memory_buffer) {
    VkDescriptorBufferInfo
        shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize /
                                              (128 << 20)];
    uint32_t shared_memory_binding_range =
        SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
    for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
      VkDescriptorBufferInfo& info = shared_memory_descriptor_buffers_info[i];
      info.buffer = shared_memory_buffer;
      info.offset = shared_memory_binding_range * i;
      info.range = shared_memory_binding_range;
    }
    VkWriteDescriptorSet write_descriptor_sets[3];
    VkWriteDescriptorSet& write_shared_memory = write_descriptor_sets[0];
    write_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_shared_memory.pNext = nullptr;
    write_shared_memory.dstSet = set;
    write_shared_memory.dstBinding = 0;
    write_shared_memory.dstArrayElement = 0;
    write_shared_memory.descriptorCount = shared_memory_binding_count;
    write_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_shared_memory.pImageInfo = nullptr;
    write_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
    write_shared_memory.pTexelBufferView = nullptr;
    uint32_t write_count = 1;
    VkDescriptorBufferInfo zpd_counter_descriptor_buffer_info;
    if (zpd_counter_binding_used) {
      // ZPD counter - kept valid until the real counter buffer is ready.
      zpd_counter_descriptor_buffer_info.buffer = zpd_counter_sink_buffer_;
      zpd_counter_descriptor_buffer_info.offset = 0;
      zpd_counter_descriptor_buffer_info.range = zpd_counter_sink_range;
      VkWriteDescriptorSet& write_zpd_counter =
          write_descriptor_sets[write_count++];
      write_zpd_counter.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_zpd_counter.pNext = nullptr;
      write_zpd_counter.dstSet = set;
      write_zpd_counter.dstBinding = 1;
      write_zpd_counter.dstArrayElement = 0;
      write_zpd_counter.descriptorCount = 1;
      write_zpd_counter.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_zpd_counter.pImageInfo = nullptr;
      write_zpd_counter.pBufferInfo = &zpd_counter_descriptor_buffer_info;
      write_zpd_counter.pTexelBufferView = nullptr;
    }
    VkDescriptorBufferInfo edram_descriptor_buffer_info;
    if (edram_fragment_shader_interlock) {
      edram_descriptor_buffer_info.buffer =
          render_target_cache_->edram_buffer();
      edram_descriptor_buffer_info.offset = 0;
      edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
      VkWriteDescriptorSet& write_edram = write_descriptor_sets[write_count++];
      write_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_edram.pNext = nullptr;
      write_edram.dstSet = set;
      write_edram.dstBinding = 2;
      write_edram.dstArrayElement = 0;
      write_edram.descriptorCount = 1;
      write_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_edram.pImageInfo = nullptr;
      write_edram.pBufferInfo = &edram_descriptor_buffer_info;
      write_edram.pTexelBufferView = nullptr;
    }
    dfn.vkUpdateDescriptorSets(device, write_count, write_descriptor_sets, 0,
                               nullptr);
  };
  write_shared_memory_and_edram_set(shared_memory_and_edram_descriptor_set_,
                                    shared_memory_->buffer());
  if (create_host_shared_memory_set) {
    write_shared_memory_and_edram_set(
        shared_memory_host_and_edram_descriptor_set_,
        shared_memory_->host_buffer());
  }
  if (zpd_counter_binding_used) {
    zpd_counter_descriptor_buffer_ = zpd_counter_sink_buffer_;
    zpd_counter_descriptor_range_ = zpd_counter_sink_range;
  }

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize =
      kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size =
      kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage =
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode =
      VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info,
                         nullptr, &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(
        device, gamma_ramp_buffer_,
        &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local &
         vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex)) ||
        xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex))) {
      VkMemoryAllocateInfo*
          gamma_ramp_host_visible_buffer_memory_allocate_info_last =
              &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo
          gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext =
            nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image =
            VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer =
            gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(
              device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
              nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_,
                                   gamma_ramp_buffer_memory_,
                                   0) == VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0,
                              VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      XELOGE("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      XELOGE("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0,
                        VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      XELOGE("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format =
      VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset =
        kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2 + 1]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings =
      &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_uniform_texel_buffer_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }
  // FXAA source descriptor set layout (combined image sampler for linear
  // filtering).
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &fxaa_source_descriptor_set_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA source descriptor set layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes =
      swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type =
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount =
        gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  // Destination storage images for compute shader output.
  // Also includes storage image for writing to FXAA source (gamma+luma output).
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount =
        kMaxFramesInFlight * 2;  // dest + FXAA source storage
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  // FXAA source combined image samplers.
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info,
                                 nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    XELOGE("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_dest_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation destination image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &fxaa_source_descriptor_set_layout_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source image descriptor sets");
      return false;
    }
  }
  // Allocate storage image descriptor sets for writing to FXAA source.
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_storage_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source storage descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView =
        &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0,
                               nullptr);
  }

  // Gamma ramp application compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetDest] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_push_constant_range;
  swap_apply_gamma_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_push_constant_range.offset = 0;
  swap_apply_gamma_push_constant_range.size = sizeof(ApplyGammaConstants);
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
          &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipelines.
  VkShaderModule swap_apply_gamma_table_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_cs,
          sizeof(shaders::apply_gamma_table_cs));
  VkShaderModule swap_apply_gamma_pwl_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::apply_gamma_pwl_cs,
                                           sizeof(shaders::apply_gamma_pwl_cs));
  VkShaderModule swap_apply_gamma_table_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  VkShaderModule swap_apply_gamma_pwl_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_table_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_table_fxaa_luma_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_fxaa_luma_shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to create the gamma ramp application compute shader modules");
    if (swap_apply_gamma_table_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_pwl_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_table_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
    }
    if (swap_apply_gamma_pwl_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.stage.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.stage.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.stage =
      VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_pipeline_create_info.stage.pName = "main";
  swap_apply_gamma_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_create_info.layout =
      swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;

  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_create_result =
      dfn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                   &swap_apply_gamma_pipeline_create_info,
                                   nullptr, &swap_apply_gamma_pwl_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
  if (swap_apply_gamma_pipeline_256_entry_table_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result !=
          VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application compute pipelines");
    return false;
  }

  // FXAA sampler (linear filtering, clamp to edge).
  VkSamplerCreateInfo fxaa_sampler_create_info;
  fxaa_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  fxaa_sampler_create_info.pNext = nullptr;
  fxaa_sampler_create_info.flags = 0;
  fxaa_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  fxaa_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.mipLodBias = 0.0f;
  fxaa_sampler_create_info.anisotropyEnable = VK_FALSE;
  fxaa_sampler_create_info.maxAnisotropy = 1.0f;
  fxaa_sampler_create_info.compareEnable = VK_FALSE;
  fxaa_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  fxaa_sampler_create_info.minLod = 0.0f;
  fxaa_sampler_create_info.maxLod = 0.0f;
  fxaa_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  fxaa_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &fxaa_sampler_create_info, nullptr,
                          &fxaa_sampler_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA sampler");
    return false;
  }

  // FXAA pipeline layout.
  // set=0: destination storage image (rgb10_a2)
  // set=1: source combined image sampler (FXAA source with luma in alpha)
  std::array<VkDescriptorSetLayout, 2> fxaa_descriptor_set_layouts = {
      swap_descriptor_set_layout_storage_image_,
      fxaa_source_descriptor_set_layout_,
  };
  VkPushConstantRange fxaa_push_constant_range;
  fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_push_constant_range.offset = 0;
  fxaa_push_constant_range.size = sizeof(FxaaConstants);
  VkPipelineLayoutCreateInfo fxaa_pipeline_layout_create_info;
  fxaa_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  fxaa_pipeline_layout_create_info.pNext = nullptr;
  fxaa_pipeline_layout_create_info.flags = 0;
  fxaa_pipeline_layout_create_info.setLayoutCount =
      uint32_t(fxaa_descriptor_set_layouts.size());
  fxaa_pipeline_layout_create_info.pSetLayouts =
      fxaa_descriptor_set_layouts.data();
  fxaa_pipeline_layout_create_info.pushConstantRangeCount = 1;
  fxaa_pipeline_layout_create_info.pPushConstantRanges =
      &fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &fxaa_pipeline_layout_create_info,
                                 nullptr,
                                 &fxaa_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA pipeline layout");
    return false;
  }

  // FXAA compute pipelines.
  VkShaderModule fxaa_shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs));
  VkShaderModule fxaa_extreme_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::fxaa_extreme_cs,
                                           sizeof(shaders::fxaa_extreme_cs));
  if (fxaa_shader_module == VK_NULL_HANDLE ||
      fxaa_extreme_shader_module == VK_NULL_HANDLE) {
    XELOGE("Failed to create the FXAA compute shader modules");
    if (fxaa_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
    }
    if (fxaa_extreme_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo fxaa_pipeline_create_info;
  fxaa_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  fxaa_pipeline_create_info.pNext = nullptr;
  fxaa_pipeline_create_info.flags = 0;
  fxaa_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fxaa_pipeline_create_info.stage.pNext = nullptr;
  fxaa_pipeline_create_info.stage.flags = 0;
  fxaa_pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_pipeline_create_info.stage.pName = "main";
  fxaa_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  fxaa_pipeline_create_info.layout = fxaa_pipeline_layout_;
  fxaa_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  fxaa_pipeline_create_info.basePipelineIndex = -1;

  fxaa_pipeline_create_info.stage.module = fxaa_shader_module;
  VkResult fxaa_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_pipeline_);
  fxaa_pipeline_create_info.stage.module = fxaa_extreme_shader_module;
  VkResult fxaa_extreme_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_extreme_pipeline_);
  dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
  dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
  if (fxaa_pipeline_create_result != VK_SUCCESS ||
      fxaa_extreme_pipeline_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA compute pipelines");
    return false;
  }

  // Resolve downscale compute shader for scaled resolution readback.
  // Descriptor set layout: binding 0 = source storage buffer,
  //                        binding 1 = destination storage buffer.
  {
    std::array<VkDescriptorSetLayoutBinding, 2> resolve_downscale_bindings;
    // Source buffer (readonly).
    resolve_downscale_bindings[0].binding = 0;
    resolve_downscale_bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[0].descriptorCount = 1;
    resolve_downscale_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[0].pImmutableSamplers = nullptr;
    // Destination buffer (writeonly).
    resolve_downscale_bindings[1].binding = 1;
    resolve_downscale_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[1].descriptorCount = 1;
    resolve_downscale_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo resolve_downscale_set_layout_create_info;
    resolve_downscale_set_layout_create_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    resolve_downscale_set_layout_create_info.pNext = nullptr;
    resolve_downscale_set_layout_create_info.flags = 0;
    resolve_downscale_set_layout_create_info.bindingCount =
        uint32_t(resolve_downscale_bindings.size());
    resolve_downscale_set_layout_create_info.pBindings =
        resolve_downscale_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(
            device, &resolve_downscale_set_layout_create_info, nullptr,
            &resolve_downscale_descriptor_set_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale descriptor set layout");
      return false;
    }

    // Pipeline layout with push constants.
    VkPushConstantRange resolve_downscale_push_constant_range;
    resolve_downscale_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_push_constant_range.offset = 0;
    resolve_downscale_push_constant_range.size =
        sizeof(ResolveDownscaleConstants);

    VkPipelineLayoutCreateInfo resolve_downscale_pipeline_layout_create_info;
    resolve_downscale_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_downscale_pipeline_layout_create_info.pNext = nullptr;
    resolve_downscale_pipeline_layout_create_info.flags = 0;
    resolve_downscale_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_downscale_pipeline_layout_create_info.pSetLayouts =
        &resolve_downscale_descriptor_set_layout_;
    resolve_downscale_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_downscale_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_downscale_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_downscale_pipeline_layout_create_info, nullptr,
            &resolve_downscale_pipeline_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale pipeline layout");
      return false;
    }

    // Compute pipeline.
    VkShaderModule resolve_downscale_shader_module =
        ui::vulkan::util::CreateShaderModule(
            vulkan_device, shaders::resolve_downscale_cs,
            sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_shader_module == VK_NULL_HANDLE) {
      XELOGE("Failed to create the resolve downscale shader module");
      return false;
    }

    VkComputePipelineCreateInfo resolve_downscale_pipeline_create_info;
    resolve_downscale_pipeline_create_info.sType =
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.pNext = nullptr;
    resolve_downscale_pipeline_create_info.flags = 0;
    resolve_downscale_pipeline_create_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.stage.pNext = nullptr;
    resolve_downscale_pipeline_create_info.stage.flags = 0;
    resolve_downscale_pipeline_create_info.stage.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_pipeline_create_info.stage.module =
        resolve_downscale_shader_module;
    resolve_downscale_pipeline_create_info.stage.pName = "main";
    resolve_downscale_pipeline_create_info.stage.pSpecializationInfo = nullptr;
    resolve_downscale_pipeline_create_info.layout =
        resolve_downscale_pipeline_layout_;
    resolve_downscale_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
    resolve_downscale_pipeline_create_info.basePipelineIndex = -1;

    VkResult resolve_downscale_pipeline_result = dfn.vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &resolve_downscale_pipeline_create_info,
        nullptr, &resolve_downscale_pipeline_);
    dfn.vkDestroyShaderModule(device, resolve_downscale_shader_module, nullptr);
    if (resolve_downscale_pipeline_result != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale compute pipeline");
      return false;
    }

    // Descriptor pool chain for resolve downscale shader.
    // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
    // Each pool has 64 sets with 2 storage buffers each.
    VkDescriptorPoolSize resolve_downscale_pool_size;
    resolve_downscale_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_pool_size.descriptorCount = 128;  // 64 sets * 2 buffers
    resolve_downscale_descriptor_pool_chain_ =
        std::make_unique<ui::vulkan::VulkanDescriptorPoolChain>(
            vulkan_device, 0, 64, &resolve_downscale_pool_size, 1,
            resolve_downscale_descriptor_set_layout_);
  }

  // Initialize the ZPD occlusion query pool and resources.
  zpd_host_query_pool_ = std::make_unique<VulkanZPDQueryPool>();
  EnsureZPDQueryResources();
  CreateGpuTimeQueryPool();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));
  // ZPD counter uses UINT32_MAX as its skip sentinel outside query draws.
  system_constants_.zpd_fsi_counter_index = UINT32_MAX;
  zpd_counter_index_force_update_ = true;

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  ResetResolveReadWatch();
  DestroyResolveFaultCopy();

  ShutdownZPDQueryResources();
  zpd_host_query_pool_.reset();
  if (gpu_time_untimed_submissions_) {
    XELOGW("GPU busy total is missing {} submission(s) with no free query slot",
           gpu_time_untimed_submissions_);
  }
  DestroyGpuTimeQueryPool();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  DestroyScratchBuffer();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);

  // FXAA cleanup.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device,
                                         fxaa_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                         fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         fxaa_source_memory_);
  fxaa_source_width_ = 0;
  fxaa_source_height_ = 0;
  fxaa_source_last_submission_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         fxaa_source_descriptor_set_layout_);

  // Resolve downscale cleanup.
  ClearReadbackStagingBuffers();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      resolve_downscale_descriptor_set_layout_);
  resolve_downscale_descriptor_pool_chain_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                           gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorPool, device,
      shared_memory_and_edram_descriptor_pool_);
  // Both sets are freed with the pool - drop the routing handle so it stays
  // null (routing disabled) until the pool is recreated.
  shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  ResetMemexportPages();
  memexport_staged_.clear();
  zpd_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  zpd_counter_descriptor_range_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         zpd_counter_sink_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         zpd_counter_sink_buffer_memory_);

  texture_cache_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(
        device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair :
       descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second,
                                     nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(
        dfn.vkDestroyDescriptorSetLayout, device,
        descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device, descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
  } else if (index == XE_GPU_REG_VGT_MAX_VTX_INDX ||
             index == XE_GPU_REG_VGT_MIN_VTX_INDX ||
             index == XE_GPU_REG_VGT_INDX_OFFSET ||
             index == XE_GPU_REG_VGT_DMA_SIZE ||
             index == XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL ||
             index == XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) {
    // Tessellation factor range and index parameters are in the system
    // constants buffer.
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  } else if ((index >= XE_GPU_REG_PA_CL_UCP_0_X &&
              index <= XE_GPU_REG_PA_CL_UCP_5_W) ||
             index == XE_GPU_REG_PA_CL_CLIP_CNTL) {
    // User clip planes are in the system constants buffer.
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}
void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    VulkanCommandProcessor::WriteRegister(start_index + i, data);
  }
}
void VulkanCommandProcessor::SparseBindBuffer(
    VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
    VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds,
                              binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

bool VulkanCommandProcessor::CreateReadbackStagingBuffer(
    ReadbackStagingBuffer& buffer, uint32_t size) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, buffer.buffer,
          buffer.memory, &buffer.memory_type, &buffer.memory_size)) {
    XELOGE("Failed to create a {} KB readback staging buffer", size >> 10);
    return false;
  }
  if (dfn.vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0,
                      &buffer.mapped) != VK_SUCCESS) {
    buffer.mapped = nullptr;
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           buffer.memory);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           buffer.buffer);
    return false;
  }
  return true;
}

void VulkanCommandProcessor::DestroyReadbackStagingBuffer(
    ReadbackStagingBuffer& buffer) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (buffer.mapped != nullptr) {
    dfn.vkUnmapMemory(device, buffer.memory);
    buffer.mapped = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         buffer.buffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         buffer.memory);
}

// The copy into a staging buffer is recorded, so it has to drain first.
void VulkanCommandProcessor::PrepareReadbackStagingEviction() {
  AwaitAllQueueOperationsCompletion();
}

// Orders a copy into a staging buffer against the previous one into the same
// buffer, which may still be in flight from when the slot last came around.
void VulkanCommandProcessor::OrderReadbackStagingWrite(
    VkBuffer staging_buffer) {
  PushBufferMemoryBarrier(
      staging_buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, false);
}

// Makes a completed GPU write to a staging buffer visible to the CPU read that
// follows. A no-op on a coherent memory type.
void VulkanCommandProcessor::InvalidateReadbackStaging(
    const ReadbackStagingBuffer& buffer) {
  if (buffer.memory == VK_NULL_HANDLE) {
    return;
  }
  ui::vulkan::util::InvalidateMappedMemoryRange(
      GetVulkanDevice(), buffer.memory, buffer.memory_type, 0,
      buffer.memory_size);
}

bool VulkanCommandProcessor::FaultCopyResolveToGuestRam(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (ranges.empty() || device_lost_) {
    return false;
  }
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (resolve_fault_command_pool_ == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo command_pool_create_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool_create_info.queueFamilyIndex =
        vulkan_device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                &resolve_fault_command_pool_) != VK_SUCCESS) {
      XELOGE("VulkanCommandProcessor: Failed to create the fault copy pool");
      resolve_fault_command_pool_ = VK_NULL_HANDLE;
      return false;
    }
    VkCommandBufferAllocateInfo command_buffer_allocate_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_buffer_allocate_info.commandPool = resolve_fault_command_pool_;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandBufferCount = 1;
    const VkFenceCreateInfo fence_create_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                     &resolve_fault_command_buffer_) !=
            VK_SUCCESS ||
        dfn.vkCreateFence(device, &fence_create_info, nullptr,
                          &resolve_fault_fence_) != VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to create the fault copy command "
          "buffer");
      DestroyResolveFaultCopy();
      return false;
    }
  }
  if (dfn.vkResetCommandPool(device, resolve_fault_command_pool_, 0) !=
      VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(resolve_fault_command_buffer_, &begin_info) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  bool staging = false;
  if (shared_memory_->is_zero_copy()) {
    // With zero-copy the resolve wrote guest RAM itself, so there is nothing
    // to copy, only its writes to make visible to the host.
    memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    dfn.vkCmdPipelineBarrier(resolve_fault_command_buffer_,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory_barrier,
                             0, nullptr, 0, nullptr);
  } else {
    // Barriers apply across submissions in submission order, so these order the
    // copy after the GPU thread's earlier writes, including the resolve, and
    // before whatever it submits later. A global barrier, as this can't know
    // what its submissions have in flight.
    memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory_barrier.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn.vkCmdPipelineBarrier(resolve_fault_command_buffer_,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &memory_barrier, 0, nullptr, 0, nullptr);
    // Without the host buffer, the ranges are packed into a readback buffer and
    // copied out to guest RAM here once the GPU is done.
    VkBuffer dest = shared_memory_->host_buffer();
    staging = dest == VK_NULL_HANDLE;
    resolve_fault_copy_regions_.clear();
    VkDeviceSize staging_offset = 0;
    for (const auto& range : ranges) {
      VkBufferCopy& region = resolve_fault_copy_regions_.emplace_back();
      region.srcOffset = range.first;
      region.dstOffset = staging ? staging_offset : range.first;
      region.size = range.second;
      staging_offset += range.second;
    }
    if (staging) {
      if (!EnsureResolveFaultReadback(uint32_t(staging_offset))) {
        dfn.vkEndCommandBuffer(resolve_fault_command_buffer_);
        return false;
      }
      dest = resolve_fault_readback_.buffer;
    }
    dfn.vkCmdCopyBuffer(resolve_fault_command_buffer_, shared_memory_->buffer(),
                        dest, uint32_t(resolve_fault_copy_regions_.size()),
                        resolve_fault_copy_regions_.data());
    memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                   VK_ACCESS_MEMORY_WRITE_BIT |
                                   VK_ACCESS_HOST_READ_BIT;
    dfn.vkCmdPipelineBarrier(
        resolve_fault_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
        &memory_barrier, 0, nullptr, 0, nullptr);
  }
  if (dfn.vkEndCommandBuffer(resolve_fault_command_buffer_) != VK_SUCCESS) {
    return false;
  }
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &resolve_fault_command_buffer_;
  VkResult submit_result;
  {
    ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device->AcquireQueue(
            vulkan_device->queue_family_graphics_compute(), 0);
    submit_result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1,
                                      &submit_info, resolve_fault_fence_);
  }
  if (submit_result != VK_SUCCESS) {
    return false;
  }
  VkResult wait_result = dfn.vkWaitForFences(device, 1, &resolve_fault_fence_,
                                             VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device, 1, &resolve_fault_fence_);
  if (wait_result != VK_SUCCESS) {
    return false;
  }
  if (staging) {
    InvalidateReadbackStaging(resolve_fault_readback_);
    CopyPackedRangesToGuestRam(resolve_fault_readback_.mapped, ranges);
  }
  return true;
}

bool VulkanCommandProcessor::EnsureResolveFaultReadback(uint32_t size) {
  if (resolve_fault_readback_.buffer != VK_NULL_HANDLE &&
      resolve_fault_readback_size_ >= size) {
    return true;
  }
  // No copy into it is in flight, so it can go right away.
  DestroyReadbackStagingBuffer(resolve_fault_readback_);
  resolve_fault_readback_size_ = 0;
  uint32_t buffer_size = AlignReadbackBufferSize(size);
  if (!CreateReadbackStagingBuffer(resolve_fault_readback_, buffer_size)) {
    return false;
  }
  resolve_fault_readback_size_ = buffer_size;
  return true;
}

void VulkanCommandProcessor::DestroyResolveFaultCopy() {
  // Every copy is awaited before returning, so nothing is in flight.
  DestroyReadbackStagingBuffer(resolve_fault_readback_);
  resolve_fault_readback_size_ = 0;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (resolve_fault_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, resolve_fault_fence_, nullptr);
    resolve_fault_fence_ = VK_NULL_HANDLE;
  }
  // Frees the command buffer with it.
  if (resolve_fault_command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, resolve_fault_command_pool_, nullptr);
    resolve_fault_command_pool_ = VK_NULL_HANDLE;
  }
  resolve_fault_command_buffer_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                       uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  EndZPDFrame();

  // Before the presenter check, so the paths that return early end the frame
  // too.
  NoteResolveFrame();

  CollectCompletedGpuTimeQueries();
  static constexpr uint32_t kRenderPassWindowFrames = 60;
  if (++render_pass_window_frames_ >= kRenderPassWindowFrames) {
    uint64_t submissions_now = GetCurrentSubmission();
    COUNT_profile_set(
        "gpu/command_buffers_per_frame",
        int64_t((submissions_now - gpu_time_submissions_window_start_) /
                render_pass_window_frames_));
    gpu_time_submissions_window_start_ = submissions_now;
    COUNT_profile_set(
        "gpu/render_passes_per_frame",
        int64_t((render_passes_total_ - render_passes_window_start_) /
                render_pass_window_frames_));
    render_passes_window_start_ = render_passes_total_;

    COUNT_profile_set("gpu/gpu_busy_us_per_frame",
                      int64_t((gpu_time_total_ns_ - gpu_time_window_start_ns_) /
                              (uint64_t(1000) * render_pass_window_frames_)));
    gpu_time_window_start_ns_ = gpu_time_total_ns_;

    render_pass_window_frames_ = 0;
  }

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    return;
  }

  // Obtaining the actual front buffer size to pass to RefreshGuestOutput,
  // resolution-scaled if it's a resolve destination, or not otherwise.
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  xenos::TextureFormat frontbuffer_format;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format);
  if (swap_texture_view == VK_NULL_HANDLE) {
    return;
  }

  auto aspect = graphics_system_->GetScaledAspectRatio();

  presenter->RefreshGuestOutput(
      frontbuffer_width_scaled, frontbuffer_height_scaled, aspect.first,
      aspect.second,
      [this, frontbuffer_width_scaled, frontbuffer_height_scaled,
       frontbuffer_format, swap_texture_view](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context = static_cast<
            ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
            context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn =
            vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index =
            uint32_t(frame_current_ % kMaxFramesInFlight);

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format ==
                xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_image_ != VK_NULL_HANDLE &&
              (fxaa_source_width_ != frontbuffer_width_scaled ||
               fxaa_source_height_ != frontbuffer_height_scaled)) {
            // Need to resize the FXAA source texture.
            if (GetCompletedSubmission() < fxaa_source_last_submission_) {
              // Still in use - defer destruction.
              destroy_memory_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_memory_);
              destroy_images_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_image_);
              destroy_image_views_.emplace_back(fxaa_source_last_submission_,
                                                fxaa_source_image_view_);
            } else {
              dfn.vkDestroyImageView(device, fxaa_source_image_view_, nullptr);
              dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
              dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
            }
            fxaa_source_image_ = VK_NULL_HANDLE;
            fxaa_source_image_view_ = VK_NULL_HANDLE;
            fxaa_source_memory_ = VK_NULL_HANDLE;
            fxaa_source_width_ = 0;
            fxaa_source_height_ = 0;
            fxaa_source_last_submission_ = 0;
          }
          if (fxaa_source_image_ == VK_NULL_HANDLE) {
            // Create the FXAA source texture.
            VkImageCreateInfo fxaa_source_image_create_info;
            fxaa_source_image_create_info.sType =
                VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            fxaa_source_image_create_info.pNext = nullptr;
            fxaa_source_image_create_info.flags = 0;
            fxaa_source_image_create_info.imageType = VK_IMAGE_TYPE_2D;
            fxaa_source_image_create_info.format = kFxaaSourceFormat;
            fxaa_source_image_create_info.extent.width =
                frontbuffer_width_scaled;
            fxaa_source_image_create_info.extent.height =
                frontbuffer_height_scaled;
            fxaa_source_image_create_info.extent.depth = 1;
            fxaa_source_image_create_info.mipLevels = 1;
            fxaa_source_image_create_info.arrayLayers = 1;
            fxaa_source_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
            fxaa_source_image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            fxaa_source_image_create_info.usage =
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            fxaa_source_image_create_info.sharingMode =
                VK_SHARING_MODE_EXCLUSIVE;
            fxaa_source_image_create_info.queueFamilyIndexCount = 0;
            fxaa_source_image_create_info.pQueueFamilyIndices = nullptr;
            fxaa_source_image_create_info.initialLayout =
                VK_IMAGE_LAYOUT_UNDEFINED;
            if (dfn.vkCreateImage(device, &fxaa_source_image_create_info,
                                  nullptr, &fxaa_source_image_) != VK_SUCCESS) {
              XELOGE("Failed to create the FXAA source image");
              use_fxaa = false;
            } else {
              VkMemoryRequirements fxaa_source_memory_requirements;
              dfn.vkGetImageMemoryRequirements(
                  device, fxaa_source_image_, &fxaa_source_memory_requirements);
              VkMemoryAllocateInfo fxaa_source_memory_allocate_info;
              fxaa_source_memory_allocate_info.sType =
                  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
              fxaa_source_memory_allocate_info.pNext = nullptr;
              fxaa_source_memory_allocate_info.allocationSize =
                  fxaa_source_memory_requirements.size;
              fxaa_source_memory_allocate_info.memoryTypeIndex =
                  ui::vulkan::util::ChooseMemoryType(
                      vulkan_device->memory_types(),
                      fxaa_source_memory_requirements.memoryTypeBits,
                      ui::vulkan::util::MemoryPurpose::kDeviceLocal);
              if (fxaa_source_memory_allocate_info.memoryTypeIndex ==
                      UINT32_MAX ||
                  dfn.vkAllocateMemory(
                      device, &fxaa_source_memory_allocate_info, nullptr,
                      &fxaa_source_memory_) != VK_SUCCESS) {
                XELOGE("Failed to allocate FXAA source image memory");
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else if (dfn.vkBindImageMemory(device, fxaa_source_image_,
                                               fxaa_source_memory_,
                                               0) != VK_SUCCESS) {
                XELOGE("Failed to bind FXAA source image memory");
                dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                fxaa_source_memory_ = VK_NULL_HANDLE;
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else {
                VkImageViewCreateInfo fxaa_source_image_view_create_info;
                fxaa_source_image_view_create_info.sType =
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                fxaa_source_image_view_create_info.pNext = nullptr;
                fxaa_source_image_view_create_info.flags = 0;
                fxaa_source_image_view_create_info.image = fxaa_source_image_;
                fxaa_source_image_view_create_info.viewType =
                    VK_IMAGE_VIEW_TYPE_2D;
                fxaa_source_image_view_create_info.format = kFxaaSourceFormat;
                fxaa_source_image_view_create_info.components.r =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.g =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.b =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.a =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseMipLevel = 0;
                fxaa_source_image_view_create_info.subresourceRange.levelCount =
                    1;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseArrayLayer = 0;
                fxaa_source_image_view_create_info.subresourceRange.layerCount =
                    1;
                if (dfn.vkCreateImageView(
                        device, &fxaa_source_image_view_create_info, nullptr,
                        &fxaa_source_image_view_) != VK_SUCCESS) {
                  XELOGE("Failed to create the FXAA source image view");
                  dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                  fxaa_source_memory_ = VK_NULL_HANDLE;
                  dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                  fxaa_source_image_ = VK_NULL_HANDLE;
                  use_fxaa = false;
                } else {
                  fxaa_source_width_ = frontbuffer_width_scaled;
                  fxaa_source_height_ = frontbuffer_height_scaled;
                }
              }
            }
          }
        }

        // FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref =
            use_pwl_gamma_ramp ? gamma_ramp_pwl_current_frame_
                               : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable =
              sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize =
              kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size = use_pwl_gamma_ramp
                                         ? kGammaRampSizePWL
                                         : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) +
              gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little &&
              use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                    gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry =
                  gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(
                gamma_ramp_frame_upload,
                use_pwl_gamma_ramp
                    ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                    : static_cast<const void*>(gamma_ramp_256_entry_table()),
                gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer =
              gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            InsertDebugMarker("Gamma Ramp Upload: %u bytes",
                              static_cast<uint32_t>(gamma_ramp_size));
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_,
                                                     gamma_ramp_buffer_, 1,
                                                     &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref =
              gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        // Track FXAA source texture submission.
        if (use_fxaa) {
          fxaa_source_last_submission_ = GetCurrentSubmission();
        }

        // Determine the destination image for gamma application.
        // If FXAA is enabled, gamma writes to the FXAA source texture.
        // Otherwise, it writes directly to the guest output.
        VkImage apply_gamma_dest_image =
            use_fxaa ? fxaa_source_image_ : vulkan_context.image();
        VkImageView apply_gamma_dest_image_view =
            use_fxaa ? fxaa_source_image_view_ : vulkan_context.image_view();

        // Transition destination image for compute shader storage image write.
        if (use_fxaa) {
          // FXAA source - transition from undefined (we'll be overwriting).
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        } else if (vulkan_context.image_ever_written_previously()) {
          // Guest output - insert a barrier after the last presenter's usage.
          // Will be overwriting all the contents, so oldLayout can be
          // UNDEFINED.
          PushImageMemoryBarrier(
              apply_gamma_dest_image,
              ui::vulkan::util::InitializeSubresourceRange(),
              ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
              VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL);
        } else {
          // Guest output - first write to the image, just transition.
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        }

        // End the current render pass before inserting barriers.
        SubmitBarriers(true);

        PushDebugMarker("Apply Gamma Ramp: %s%s",
                        use_pwl_gamma_ramp ? "PWL" : "256-entry table",
                        use_fxaa ? " (with FXAA luma)" : "");

        // Select the appropriate gamma pipeline.
        VkPipeline gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          gamma_pipeline = use_fxaa ? swap_apply_gamma_pwl_fxaa_luma_pipeline_
                                    : swap_apply_gamma_pwl_pipeline_;
        } else {
          gamma_pipeline =
              use_fxaa ? swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_
                       : swap_apply_gamma_256_entry_table_pipeline_;
        }
        deferred_command_buffer_.CmdVkBindPipeline(
            VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline);

        // Update the source descriptor set with the swap texture.
        VkDescriptorSet swap_descriptor_source =
            swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType =
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo =
            &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;

        // Update the destination descriptor set with the destination image.
        // When FXAA is enabled, use a separate descriptor set for writing to
        // FXAA source to avoid the issue where both passes would use the same
        // descriptor set (which gets updated twice before submission).
        VkDescriptorSet swap_descriptor_dest =
            use_fxaa ? fxaa_source_storage_descriptors_[swap_frame_index]
                     : swap_descriptors_dest_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_dest_image_info;
        swap_descriptor_dest_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_dest_image_info.imageView = apply_gamma_dest_image_view;
        swap_descriptor_dest_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet swap_descriptor_dest_write;
        swap_descriptor_dest_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_dest_write.pNext = nullptr;
        swap_descriptor_dest_write.dstSet = swap_descriptor_dest;
        swap_descriptor_dest_write.dstBinding = 0;
        swap_descriptor_dest_write.dstArrayElement = 0;
        swap_descriptor_dest_write.descriptorCount = 1;
        swap_descriptor_dest_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        swap_descriptor_dest_write.pImageInfo =
            &swap_descriptor_dest_image_info;
        swap_descriptor_dest_write.pBufferInfo = nullptr;
        swap_descriptor_dest_write.pTexelBufferView = nullptr;

        std::array<VkWriteDescriptorSet, 2> swap_descriptor_writes = {
            swap_descriptor_source_write, swap_descriptor_dest_write};
        dfn.vkUpdateDescriptorSets(device,
                                   uint32_t(swap_descriptor_writes.size()),
                                   swap_descriptor_writes.data(), 0, nullptr);

        // Set push constants.
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = frontbuffer_width_scaled;
        apply_gamma_constants.size[1] = frontbuffer_height_scaled;
        deferred_command_buffer_.CmdVkPushConstants(
            swap_apply_gamma_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(apply_gamma_constants), &apply_gamma_constants);

        // Bind descriptor sets.
        std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount>
            swap_descriptor_sets{};
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
            swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                         uint32_t(use_pwl_gamma_ramp)];
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] =
            swap_descriptor_source;
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetDest] =
            swap_descriptor_dest;
        // TODO(Triang3l): Red / blue swap without imageViewFormatSwizzle.
        deferred_command_buffer_.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_pipeline_layout_,
            0, uint32_t(swap_descriptor_sets.size()),
            swap_descriptor_sets.data(), 0, nullptr);

        // Dispatch compute shader. Local size is 16x8.
        uint32_t group_count_x = (frontbuffer_width_scaled + 15) / 16;
        uint32_t group_count_y = (frontbuffer_height_scaled + 7) / 8;
        ++submission_in_progress_.dispatch_count;
        deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

        PopDebugMarker();

        // Apply FXAA if enabled.
        if (use_fxaa) {
          PushDebugMarker("FXAA: %s",
                          swap_post_effect == SwapPostEffect::kFxaaExtreme
                              ? "Extreme"
                              : "Standard");

          // Transition FXAA source from storage image write to sampled read.
          // Transition guest output to storage image write.
          PushImageMemoryBarrier(
              fxaa_source_image_,
              ui::vulkan::util::InitializeSubresourceRange(),
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          if (vulkan_context.image_ever_written_previously()) {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          } else {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);

          // Bind FXAA pipeline.
          deferred_command_buffer_.CmdVkBindPipeline(
              VK_PIPELINE_BIND_POINT_COMPUTE,
              swap_post_effect == SwapPostEffect::kFxaaExtreme
                  ? fxaa_extreme_pipeline_
                  : fxaa_pipeline_);

          // Update FXAA source descriptor (combined image sampler).
          VkDescriptorSet fxaa_source_descriptor =
              fxaa_source_descriptors_[swap_frame_index];
          VkDescriptorImageInfo fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_image_info.sampler = fxaa_sampler_;
          fxaa_source_descriptor_image_info.imageView = fxaa_source_image_view_;
          fxaa_source_descriptor_image_info.imageLayout =
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          VkWriteDescriptorSet fxaa_source_descriptor_write;
          fxaa_source_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_source_descriptor_write.pNext = nullptr;
          fxaa_source_descriptor_write.dstSet = fxaa_source_descriptor;
          fxaa_source_descriptor_write.dstBinding = 0;
          fxaa_source_descriptor_write.dstArrayElement = 0;
          fxaa_source_descriptor_write.descriptorCount = 1;
          fxaa_source_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          fxaa_source_descriptor_write.pImageInfo =
              &fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_write.pBufferInfo = nullptr;
          fxaa_source_descriptor_write.pTexelBufferView = nullptr;

          // Update FXAA destination descriptor (guest output image).
          VkDescriptorSet fxaa_dest_descriptor =
              swap_descriptors_dest_[swap_frame_index];
          VkDescriptorImageInfo fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_image_info.sampler = VK_NULL_HANDLE;
          fxaa_dest_descriptor_image_info.imageView =
              vulkan_context.image_view();
          fxaa_dest_descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet fxaa_dest_descriptor_write;
          fxaa_dest_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_dest_descriptor_write.pNext = nullptr;
          fxaa_dest_descriptor_write.dstSet = fxaa_dest_descriptor;
          fxaa_dest_descriptor_write.dstBinding = 0;
          fxaa_dest_descriptor_write.dstArrayElement = 0;
          fxaa_dest_descriptor_write.descriptorCount = 1;
          fxaa_dest_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          fxaa_dest_descriptor_write.pImageInfo =
              &fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_write.pBufferInfo = nullptr;
          fxaa_dest_descriptor_write.pTexelBufferView = nullptr;

          std::array<VkWriteDescriptorSet, 2> fxaa_descriptor_writes = {
              fxaa_source_descriptor_write, fxaa_dest_descriptor_write};
          dfn.vkUpdateDescriptorSets(device,
                                     uint32_t(fxaa_descriptor_writes.size()),
                                     fxaa_descriptor_writes.data(), 0, nullptr);

          // Set FXAA push constants.
          FxaaConstants fxaa_constants;
          fxaa_constants.size[0] = frontbuffer_width_scaled;
          fxaa_constants.size[1] = frontbuffer_height_scaled;
          fxaa_constants.size_inv[0] =
              1.0f / static_cast<float>(frontbuffer_width_scaled);
          fxaa_constants.size_inv[1] =
              1.0f / static_cast<float>(frontbuffer_height_scaled);
          deferred_command_buffer_.CmdVkPushConstants(
              fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(fxaa_constants), &fxaa_constants);

          // Bind FXAA descriptor sets.
          std::array<VkDescriptorSet, 2> fxaa_descriptor_sets = {
              fxaa_dest_descriptor, fxaa_source_descriptor};
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, fxaa_pipeline_layout_, 0,
              uint32_t(fxaa_descriptor_sets.size()),
              fxaa_descriptor_sets.data(), 0, nullptr);

          // Dispatch FXAA compute shader.
          ++submission_in_progress_.dispatch_count;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y,
                                                 1);

          PopDebugMarker();
        }

        // Insert the release barrier - transition from GENERAL to the
        // presenter's expected layout.
        PushImageMemoryBarrier(
            vulkan_context.image(),
            ui::vulkan::util::InitializeSubresourceRange(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
            VK_ACCESS_SHADER_WRITE_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
            VK_IMAGE_LAYOUT_GENERAL,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (cvars::submit_on_primary_buffer_end && submission_open_ &&
      !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE &&
         offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset +
                 other_buffer_memory_barrier.size <=
             offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset &&
        other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    VkImageLayout old_layout, VkImageLayout new_layout,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask && old_layout == new_layout &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask &
          subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount !=
             VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount !=
             VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask ==
            subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount ==
            subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount ==
            subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  assert_true(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass();
    }
    return false;
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend();
       ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask
                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() +
            it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() +
            it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass &&
        current_framebuffer_ == framebuffer) {
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is.
  EndRenderPass();

  // Clear released ZPD counter slots while no pass is open, so a hybrid query
  // opening inside this pass finds a clean slot without breaking it.
  if (zpd_host_query_pool_ && zpd_host_query_pool_->has_dirty_counters()) {
    zpd_host_query_pool_->FlushCounterClears(deferred_command_buffer_);
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  if (use_dynamic_rendering) {
    // Use dynamic rendering - construct VkRenderingInfo from render targets.
    // The PSI ("accuracy") path emulates EDRAM via SSBOs and uses no real
    // attachments (mirrors the zero-attachment fsi_render_pass_).
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};
    uint32_t color_attachment_count = 0;

    bool has_depth = false;
    bool has_stencil = false;
    if (render_target_cache_->GetPath() ==
        RenderTargetCache::Path::kHostRenderTargets) {
      render_target_cache_->GetLastUpdateRenderingAttachments(
          color_attachments, &color_attachment_count, &depth_attachment,
          &stencil_attachment);
      has_depth =
          depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      has_stencil = stencil_attachment.sType ==
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments =
        color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        has_stencil ? &stencil_attachment : nullptr;

    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
  ++render_passes_total_;

  // Resume any pending ZPD segment now that the pass is open.
  OpenQuerySegment(false);
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass &&
        current_framebuffer_ == framebuffer) {
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is.
  EndRenderPass();

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  if (use_dynamic_rendering) {
    // Use dynamic rendering for transfers - construct VkRenderingInfo from
    // the transfer destination.
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      // Stencil uses the same attachment.
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments =
        transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment =
        transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        transfer_dest_is_depth ? &stencil_attachment : nullptr;

    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
  ++render_passes_total_;

  OpenQuerySegment(false);
}

void VulkanCommandProcessor::EndRenderPass() {
  SCOPE_profile_cpu_f("gpu");
  assert_true(submission_open_);
  if (!in_render_pass_) {
    return;
  }
  // Close native Vulkan occlusion queries before ending the pass. FSI counter
  // segments don't use vkCmdBeginQuery / vkCmdEndQuery and can stay logically
  // open across render passes.
  if (zpd_active_segment_.segment_active && !zpd_active_query_is_fsi_) {
    CloseQuerySegment();
  }
  // Use current_render_pass_ to determine which end command to use.
  // VK_NULL_HANDLE means we used dynamic rendering, otherwise traditional.
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRendering();
  } else {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
}

void VulkanCommandProcessor::DestroyScratchImageWhenIdle(
    VkImage image, VkImageView image_view, VkDeviceMemory memory) {
  // GetCurrentSubmission() only grows, so the deques stay sorted by it.
  uint64_t submission_current = GetCurrentSubmission();
  if (image_view != VK_NULL_HANDLE) {
    destroy_image_views_.emplace_back(submission_current, image_view);
  }
  if (image != VK_NULL_HANDLE) {
    destroy_images_.emplace_back(submission_current, image);
  }
  if (memory != VK_NULL_HANDLE) {
    destroy_memory_.emplace_back(submission_current, memory);
  }
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::LinkedTypeDescriptorSetAllocator*
        transient_descriptor_allocator;
    VkDescriptorPoolSize descriptor_counts[2];
    uint32_t descriptor_count_count = 1;
    switch (transient_descriptor_layout) {
      case SingleTransientDescriptorLayout::kStorageBuffer:
        transient_descriptor_allocator =
            &transient_descriptor_allocator_storage_buffer_;
        descriptor_counts[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_counts[0].descriptorCount = 1;
        break;
      case SingleTransientDescriptorLayout::kStorageBufferAndStorageImage:
        transient_descriptor_allocator =
            &transient_descriptor_allocator_storage_buffer_and_image_;
        descriptor_counts[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_counts[0].descriptorCount = 1;
        descriptor_counts[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptor_counts[1].descriptorCount = 1;
        descriptor_count_count = 2;
        break;
      default:
        transient_descriptor_allocator =
            &transient_descriptor_allocator_uniform_buffer_;
        descriptor_counts[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptor_counts[0].descriptorCount = 1;
        break;
    }
    descriptor_set = transient_descriptor_allocator->Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout),
        descriptor_counts, descriptor_count_count);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(
    bool is_vertex, size_t texture_count, size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing =
      descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanCommandProcessor::GetPipelineLayout(size_t texture_count_pixel,
                                          size_t sampler_count_pixel,
                                          size_t texture_count_vertex,
                                          size_t sampler_count_vertex) {
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  // Called from the draw thread and from pipeline creation threads (deferred
  // translation), and reads/writes the shared layout maps +
  // GetTextureDescriptor SetLayout (only called from here), so serialize the
  // whole lookup+create.
  std::lock_guard<std::mutex> layouts_lock(pipeline_layouts_mutex_);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex,
                                    sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel,
                                    sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout
      descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts
      [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
          descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount =
      uint32_t(xe::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &pipeline_layout) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout,
                            descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition
VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
    VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE,
                            scratch_buffer_last_stage_mask_, initial_stage_mask,
                            scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    XELOGE(
        "VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer",
        size >> 20);
    return ScratchBufferAcquisition();
  }

  if (GetCompletedSubmission() >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_,
                                    scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_,
                                   scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_stage_mask;
  scratch_buffer_last_access_mask_ = initial_access_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(
    VkPipeline pipeline, bool keep_dynamic_depth_bias,
    bool keep_dynamic_blend_constants, bool keep_dynamic_stencil_mask_ref) {
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           const uint32_t* host_address,
                                           uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                       uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw. Matches the Direct3D 12 backend.
    // TODO(Triang3l): Do something so memexport still works in this case maybe?
    // Unlikely that zero would even really be legal though.
    return true;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  // TODO(Triang3l): If the shader uses memory export, but
  // vertexPipelineStoresAndAtomics is not supported, convert the vertex shader
  // to a compute shader and dispatch it after the draw if the draw doesn't use
  // tessellation.
  const bool memexport_used_vertex =
      vertex_shader->memexport_eM_written() != 0 &&
      device_properties.vertexPipelineStoresAndAtomics;
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  const bool memexport_used_pixel = pixel_shader &&
                                    pixel_shader->memexport_eM_written() != 0 &&
                                    device_properties.fragmentStoresAndAtomics;
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(
                          regs.Get<reg::SQ_PROGRAM_CNTL>(),
                          regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool use_interpreter = false;
  uint32_t normalized_color_mask;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
  bool apply_host_depth_polygon_offset = false;
  bool zpd_hybrid = false;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return false;
    }

    // Process primitives.
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return false;
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    // TODO(Triang3l): Geometry-type-specific vertex shader, vertex shader as
    // compute.
    // Skip unsupported host vertex shader types (but allow tessellation types
    // through - they will be handled in pipeline creation or rejected there if
    // not fully supported yet). Both AsTriangleStrip fallbacks are needed on
    // hosts without geometry shader support (MoltenVK / MoltenVK-like); the
    // SPIR-V translator and pipeline cache both handle them end-to-end.
    if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(
            primitive_processing_result.host_vertex_shader_type)) {
      return false;
    }

    normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

    // Compute which color render targets are used.
    normalized_color_mask =
        pixel_shader ? draw_util::GetNormalizedColorMask(
                           regs, pixel_shader->writes_color_targets())
                     : 0;
    apply_host_depth_polygon_offset =
        pixel_shader && !pixel_shader->writes_depth() &&
        render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        draw_util::GetHostDepthPolygonOffsetIfNeeded(
            regs, primitive_polygonal, normalized_depth_control,
            normalized_color_mask, host_depth_polygon_offset);

    // Shader modifications.
    vertex_shader_modification =
        pipeline_cache_->GetCurrentVertexShaderModification(
            *vertex_shader, primitive_processing_result.host_vertex_shader_type,
            interpolator_mask, ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification =
        pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                           *pixel_shader, interpolator_mask, ps_param_gen_pos,
                           normalized_depth_control, normalized_color_mask,
                           apply_host_depth_polygon_offset)
                     : SpirvShaderTranslator::Modification(0);
    // Hybrid occlusion query draw, counting coverage into the Total counter.
    // Only depth or stencil tested draws w/o depth writes, so scene geometry
    // keeps early depth rejection, and nothing can fail without a test anyway.
    zpd_hybrid = zpd_hybrid_supported_ &&
                 (zpd_active_segment_.segment_active ||
                  zpd_active_segment_.segment_pending_begin) &&
                 !normalized_depth_control.z_write_enable &&
                 (normalized_depth_control.z_enable ||
                  normalized_depth_control.stencil_enable);
    if (zpd_hybrid && pixel_shader) {
      pixel_shader_modification.pixel.set_zpd_total(true);
      // The counter buffer write disables early depth/stencil anyway.
      if (pixel_shader_modification.pixel.depth_stencil_mode ==
          SpirvShaderTranslator::Modification::DepthStencilMode::kEarlyHint) {
        pixel_shader_modification.pixel.depth_stencil_mode =
            SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
      }
    }

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(
            vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(
                               pixel_shader_modification.value))
                     : nullptr;
    // Decide whether the ucode interpreter can stand in for the real VS while
    // it translates+compiles in the background: a plain (non-tessellated,
    // non-expanded) vertex shader with static control flow and no texture fetch
    // or memory export, on a device matching the interpreter's assumptions
    // (single shared-memory binding, full 32-bit indices). When eligible, VS
    // translation is deferred to the background creation thread.
    use_interpreter =
        cvars::async_shader_vs_interpreter &&
        !vertex_shader_translation->is_translated() &&
        active_vertex_shader_ucode_address_ != 0 &&
        device_properties.fullDrawIndexUint32 &&
        SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
            device_properties.maxStorageBufferRange) == 0 &&
        primitive_processing_result.host_vertex_shader_type ==
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kPointList &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kRectangleList &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kQuadList &&
        vertex_shader->texture_bindings().empty() &&
        !vertex_shader->uses_subroutine_calls() &&
        vertex_shader->memexport_eM_written() == 0 &&
        vertex_shader->constant_register_map().loop_bitmap == 0;
    bool async_available =
        pipeline_cache_->CanCreatePipelineAsync(pixel_shader != nullptr);
    // A draw with no placeholder to render it now: the interpreter can't stand
    // in AND the real VS isn't translated yet (so no real-VS + no-op-PS
    // placeholder either). These are the draws async_shader_skip_draws governs.
    // Interpreter-eligible draws and draws whose VS is already translated
    // always have a placeholder and never translate on the draw thread.
    // Only decides whether to translate here. Whether the draw can render is
    // read off the pipeline itself, which may have gained a placeholder since.
    bool no_placeholder = async_available && !use_interpreter &&
                          !vertex_shader_translation->is_translated();
    // The draw thread translates only for a no-placeholder draw that isn't
    // being skipped (so it can render via a real-VS placeholder).
    bool translate_here =
        !async_available || (no_placeholder && !cvars::async_shader_skip_draws);
    if (translate_here) {
      if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                    pixel_shader_translation)) {
        return false;
      }
    }

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // TODO(Triang3l): Sampler caching and reuse for adjacent draws within one
    // submission.
    uint32_t samplers_overflowed_count = 0;
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>&
          shader_samplers =
              j ? current_samplers_pixel_ : current_samplers_vertex_;
      if (!i) {
        shader_samplers.clear();
      }
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        continue;
      }
      // Don't read a shader's sampler bindings while a creation thread is still
      // populating them (async draws don't translate here). A placeholder draw
      // that ends up using this shader binds no samplers for it anyway.
      if (!shader->bindings_ready()) {
        if (!i) {
          shader_samplers.clear();
        }
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i) {
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding :
             shader_sampler_bindings) {
          shader_samplers.emplace_back(
              texture_cache_->GetSamplerParameters(shader_sampler_binding),
              VK_NULL_HANDLE);
        }
      }
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
               shader_sampler_pair : shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler = texture_cache_->UseSampler(
            shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return false;
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(
            samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionCompletionAndDeviceLoss(sampler_overflow_await_submission);
  }

  // Set up the render targets - this may perform dispatches and draws.
  if (!render_target_cache_->Update(is_rasterization_done,
                                    normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return false;
  }

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VulkanPipelineCache::Pipeline* pipeline;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation,
          primitive_processing_result, normalized_depth_control,
          normalized_color_mask,
          render_target_cache_->last_update_render_pass_key(), use_interpreter,
          zpd_hybrid, &pipeline)) {
    XELOGE("IssueDraw: ConfigurePipeline failed for VS={:016X} PS={:016X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return false;
  }

  // If async mode is active, this may be a placeholder pipeline. The real
  // pipeline will be swapped in by the creation thread when ready.
  VkPipeline current_pipeline =
      pipeline->pipeline.load(std::memory_order_acquire);
  if (current_pipeline == VK_NULL_HANDLE) {
    // Nothing to draw with yet - no placeholder, real pipeline still building.
    // Skip rather than stall, a later frame renders it.
    XELOGI("Draw skipped (pipeline not ready yet): VS {:016X}, PS {:016X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return true;
  }
  // The interpreter reads the guest ucode from shared memory by its program
  // address. A cached interpreter placeholder reused for an inline
  // (IM_LOAD_IMMEDIATE, address 0) shader can't be fed, so skip until the real
  // pipeline is ready rather than interpret from address 0.
  if (active_vertex_shader_ucode_address_ == 0 &&
      current_pipeline ==
          pipeline->placeholder_pipeline.load(std::memory_order_acquire) &&
      pipeline->uses_interpreter.load(std::memory_order_acquire)) {
    return true;
  }

  // Push debug marker with Xbox 360 draw context for RenderDoc annotation.
  // Done early so texture loads appear nested under the draw that uses them.
  if (debug_markers_enabled_ || cvars::log_draws) {
    char label[draw_util::kDebugMarkerLabelMaxLength];
    draw_util::FormatDrawDebugMarker(
        label, sizeof(label), prim_type, primitive_processing_result,
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    PushDebugMarker("%s", label);
  }

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  // Only read after-translation state for shaders whose bindings are ready (not
  // being populated by a creation thread for an async draw).
  uint32_t used_texture_mask =
      (vertex_shader->bindings_ready()
           ? vertex_shader->GetUsedTextureMaskAfterTranslation()
           : 0) |
      (pixel_shader != nullptr && pixel_shader->bindings_ready()
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  // The pipeline may be not ready yet if created asynchronously.
  // EndSubmission must be called before submitting the command buffer to
  // await its creation.
  if (current_guest_graphics_pipeline_ != current_pipeline) {
    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               current_pipeline);
    current_guest_graphics_pipeline_ = current_pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }
  auto pipeline_layout = static_cast<const PipelineLayout*>(
      pipeline->pipeline_layout.load(std::memory_order_acquire));
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible).
      uint32_t descriptor_sets_kept =
          uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
      // Invalidate descriptor set bindings for incompatible sets.
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          (UINT32_C(1) << descriptor_sets_kept) - 1;
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  bool host_render_targets_used = render_target_cache_->GetPath() ==
                                  RenderTargetCache::Path::kHostRenderTargets;

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;

  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.

  // Resolution scale of this draw, which may be 1x1 because of
  // draw_resolution_scale_threshold, which the divisors have to match too.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();
  // ZPD segments can't mix scales or hybrid query Total counting.
  // The resolved sample count is divided by one scale area per segment.
  // Needs to be split before the counter index goes into system constants.
  UpdateZPDSegment(draw_resolution_scale_x * draw_resolution_scale_y,
                   zpd_hybrid);
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      draw_resolution_scale_x > 1
          ? texture_cache_->draw_resolution_scale_x_divisor()
          : divisors::MagicDiv(1),
      draw_resolution_scale_y > 1
          ? texture_cache_->draw_resolution_scale_y_divisor()
          : divisors::MagicDiv(1),
      false, device_properties.maxViewportDimensions[0],
      device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used &&
          render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);

  if (gviargs == previous_viewport_info_args_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(&gviargs, viewport_info);
    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info;
  }
  // Update dynamic graphics pipeline state.
  UpdateDynamicState(viewport_info, primitive_polygonal,
                     normalized_depth_control, draw_resolution_scale_x,
                     draw_resolution_scale_y, apply_host_depth_polygon_offset);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kVertex;

  // Update system constants before uploading them.
  UpdateSystemConstantValues(
      primitive_polygonal, primitive_processing_result, shader_32bit_index_dma,
      viewport_info, used_texture_mask, normalized_depth_control,
      normalized_color_mask,
      apply_host_depth_polygon_offset ? &host_depth_polygon_offset : nullptr);

  // Whether we bound the placeholder pipeline (vs the real one). Derived from
  // the handle actually bound, so it's consistent with current_pipeline even if
  // the creation thread swaps in the real pipeline mid-draw (the is_placeholder
  // flag is cleared a few instructions later, so reading it separately can
  // disagree).
  bool bound_is_placeholder =
      current_pipeline ==
      pipeline->placeholder_pipeline.load(std::memory_order_acquire);
  // The interpreter placeholder (interpreter VS + no-op PS) also needs its
  // ucode location + full float constants fed to it.
  bool interpreter_placeholder =
      bound_is_placeholder &&
      pipeline->uses_interpreter.load(std::memory_order_acquire);
  // Any placeholder draw binds the no-op placeholder pixel shader (which never
  // samples), so its pixel textures/samplers must not be bound - the
  // placeholder pipeline's layout has none.
  bool placeholder_pixel_shader = bound_is_placeholder;
  {
    uint32_t ucode_base_dwords = 0, cf_instr_count = 0;
    if (interpreter_placeholder) {
      uint32_t ucode_address = active_vertex_shader_ucode_address_;
      ucode_base_dwords = ucode_address >> 2;
      cf_instr_count = vertex_shader->cf_pair_index_bound() * 2;
      shared_memory_->RequestRange(
          ucode_address,
          uint32_t(vertex_shader->ucode_dword_count()) * sizeof(uint32_t));
    }
    if (system_constants_.interpreter_ucode_base_dwords != ucode_base_dwords ||
        system_constants_.interpreter_cf_instr_count != cf_instr_count) {
      system_constants_.interpreter_ucode_base_dwords = ucode_base_dwords;
      system_constants_.interpreter_cf_instr_count = cf_instr_count;
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
    }
  }

  // Two-buffer memexport routing: producer and geometry-consumer draws use the
  // host-imported buffer (aliasing guest RAM) so memexport output stays CPU
  // coherent (fixing the page false-sharing clobber) and consumers read it
  // directly. Only texture-sampled ranges are copied into the device buffer
  // (EnsureMemexportRangeInDeviceBuffer). Off without the host buffer, where
  // the staging readback carries the output instead, and when memexport_enable
  // asks for device-local output.
  bool route_to_host = false;
  if (cvars::memexport_enable &&
      shared_memory_host_and_edram_descriptor_set_ != VK_NULL_HANDLE) {
    route_to_host =
        memexport_used_vertex || memexport_used_pixel ||
        (any_memexport_pages_written_ &&
         ((primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
           IsMemexportRange(
               primitive_processing_result.guest_index_base,
               primitive_processing_result.guest_draw_vertex_count *
                   uint32_t(sizeof(uint32_t)))) ||
          VertexFetchInMemexportRange(regs, *vertex_shader)));
  }
  {
    // Select this draw's shared-memory set, forcing a re-bind only if it
    // changed. Both sets are pre-written, so the values-up-to-date bits stay
    // set (never cleared here - the flush asserts on them); only the bound-set
    // tracking is touched.
    VkDescriptorSet shared_memory_set =
        route_to_host ? shared_memory_host_and_edram_descriptor_set_
                      : shared_memory_and_edram_descriptor_set_;
    if (current_graphics_descriptor_sets_
            [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] !=
        shared_memory_set) {
      current_graphics_descriptor_sets_
          [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
              shared_memory_set;
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          ~(UINT32_C(1)
            << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram);
    }
  }

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader, interpreter_placeholder,
                      placeholder_pixel_shader)) {
    return false;
  }

  // Ensure vertex buffers are resident.
  //
  // Use the vertex_fetch_bitmap instead of vertex_bindings() to avoid using
  // cached/stale vertex binding indices. The bitmap is populated during shader
  // translation and represents which fetch constant indices the shader actually
  // references, allowing us to check the current register values at draw time.
  const Shader::ConstantRegisterMap& constant_map_vertex =
      vertex_shader->constant_register_map();
  {
    uint32_t vfetch_addresses[96];
    uint32_t vfetch_sizes[96];
    uint32_t vfetch_current_queued = 0;
    for (uint32_t i = 0;
         i < xe::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch_constant =
            regs.GetVertexFetch(vfetch_index);
        switch (vfetch_constant.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (cvars::gpu_allow_invalid_fetch_constants) {
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type! "
                "This is incorrect behavior, but you can try bypassing this by "
                "launching Xenia with "
                "--gpu_allow_invalid_fetch_constants=true.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
          default:
            // Type is kTexture (2) or kInvalidTexture (3) - completely wrong
            // for vertex data
            if (cvars::gpu_allow_invalid_fetch_constants) {
              XELOGW(
                  "Vertex fetch constant {} ({:08X} {:08X}) has wrong type {} "
                  "(texture fetch constant in vertex slot) - allowing due to "
                  "--gpu_allow_invalid_fetch_constants=true. This will likely "
                  "crash or produce garbage!",
                  vfetch_index, vfetch_constant.dword_0,
                  vfetch_constant.dword_1,
                  static_cast<uint32_t>(vfetch_constant.type));
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) is completely "
                "invalid! Type={} - this slot contains a texture fetch "
                "constant (type 2), not a vertex fetch constant (type 0). "
                "This may indicate the shader is reading from the wrong fetch "
                "constant index, or the game has a bug.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1,
                static_cast<uint32_t>(vfetch_constant.type));
            return false;
        }
        // Mask to physical like the shader - the guest may use a mirror window.
        vfetch_addresses[vfetch_current_queued] =
            xenos::CpuToGpu(vfetch_constant.address << 2) >> 2;
        vfetch_sizes[vfetch_current_queued++] = vfetch_constant.size;
      }
    }

    if (vfetch_current_queued) {
      // Pre-acquire the critical region so we're not repeatedly re-acquiring
      // it in RequestRange - SharedMemory tracks dirty pages and only uploads
      // what actually changed, making redundant calls cheap under a hoisted
      // lock.
      auto shared_memory_request_range_hoisted =
          global_critical_region::Acquire();

      for (uint32_t i = 0; i < vfetch_current_queued; ++i) {
        if (!shared_memory_->RequestRange(vfetch_addresses[i] << 2,
                                          vfetch_sizes[i] << 2)) {
          XELOGE(
              "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
              "shared memory",
              vfetch_addresses[i] << 2, vfetch_sizes[i] << 2);
          return false;
        }
      }
    }
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords
                                          << 2;
    // Host-routed producers write output to host_buffer_ (guest RAM), not the
    // device buffer, so this upload is redundant. It also drops the draw when
    // the guest committed only part of the declared capacity, so skip it.
    if (!route_to_host &&
        !shared_memory_->RequestRange(memexport_range_base_bytes,
                                      memexport_range.size_bytes)) {
      XELOGE(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start =
        std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end,
                 memexport_range_base_bytes + memexport_range.size_bytes);
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start,
                       memexport_extent_end - memexport_extent_start));
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // After all commands that may dispatch, copy or insert barriers, submit the
  // barriers (may end the render pass), and (re)enter the render pass before
  // drawing.
  uint32_t zpd_hybrid_index_before_render_pass =
      zpd_active_segment_.hybrid ? zpd_active_query_index_ : UINT32_MAX;
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());
  uint32_t zpd_hybrid_index_after_render_pass =
      zpd_active_segment_.hybrid ? zpd_active_query_index_ : UINT32_MAX;
  if (zpd_hybrid && zpd_hybrid_index_after_render_pass !=
                        zpd_hybrid_index_before_render_pass) {
    UpdateSystemConstantValues(
        primitive_polygonal, primitive_processing_result,
        shader_32bit_index_dma, viewport_info, used_texture_mask,
        normalized_depth_control, normalized_color_mask,
        apply_host_depth_polygon_offset ? &host_depth_polygon_offset : nullptr);
    if (!UpdateBindings(vertex_shader, pixel_shader)) {
      return false;
    }
  }

  // Track for device-lost diagnostics.
  ++submission_in_progress_.draw_count;
  submission_in_progress_.last_vs_hash = vertex_shader->ucode_data_hash();
  submission_in_progress_.last_ps_hash =
      pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  submission_in_progress_.last_render_pass_key =
      render_target_cache_->last_update_render_pass_key().key;

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer.first = route_to_host ? shared_memory_->host_buffer()
                                           : shared_memory_->buffer();
        index_buffer.second = primitive_processing_result.guest_index_base;
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer.first, index_buffer.second,
        primitive_processing_result.host_index_format ==
                xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32);
    deferred_command_buffer_.CmdVkDrawIndexed(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
  }

  // Pop debug marker for draw call.
  PopDebugMarker();

  // Invalidate textures in memexported memory and watch for changes.
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes,
                                      !route_to_host);
  }

  if (!memexport_ranges_.empty()) {
    if (route_to_host) {
      // Producer draw: output landed in host_buffer_ (guest RAM), already CPU
      // coherent, so no readback. Just record the written pages. Consumers then
      // route to host_buffer_ (geometry) or copy their own range into the
      // device buffer on demand (texture loads).
      for (const draw_util::MemExportRange& memexport_range :
           memexport_ranges_) {
        MarkMemexportPagesWritten(memexport_range.base_address_dwords << 2,
                                  memexport_range.size_bytes);
      }
    } else if (cvars::memexport_enable && !shared_memory_->is_zero_copy()) {
      // No host buffer to route to, and buffer_ is device-local, so the CPU
      // only sees the output if it is read back. Under zero-copy buffer_ is
      // guest RAM already, and a readback there would clobber it.
      StageMemexportReadback();
    }
  }

  return true;
}

// Records copies of this draw's export output into staging buffers. The copy
// out waits, so it is deferred to FlushMemexportStagingReadback, where one wait
// covers every producer since the last one.
void VulkanCommandProcessor::StageMemexportReadback() {
  if (memexport_ranges_.empty()) {
    return;
  }
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);
  VkBuffer device_buffer = shared_memory_->buffer();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    if (base_bytes >= SharedMemory::kBufferSize) {
      continue;
    }
    uint32_t size_bytes = std::min(memexport_range.size_bytes,
                                   SharedMemory::kBufferSize - base_bytes);
    // The declared capacity can run past what the guest committed, and the
    // copy out would write guest memory that isn't there.
    size_bytes = WritableGuestRangeLength(base_bytes, size_bytes);
    if (!size_bytes) {
      continue;
    }
    uint64_t key = MakeReadbackResolveKey(base_bytes, size_bytes);
    ReadbackStagingSlot* slot = AcquireReadbackStagingSlot(key, size_bytes);
    if (slot == nullptr) {
      continue;
    }
    VkBuffer staging_buffer = slot->buffer.buffer;
    OrderReadbackStagingWrite(staging_buffer);
    SubmitBarriers(true);
    InsertDebugMarker("Memexport Readback (staging): 0x%08X, %u bytes",
                      base_bytes, size_bytes);
    VkBufferCopy copy_region = {};
    copy_region.srcOffset = base_bytes;
    copy_region.dstOffset = 0;
    copy_region.size = size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(device_buffer, staging_buffer, 1,
                                             &copy_region);
    // Its buffer now holds the newer output, so keep one entry, at the back -
    // copy out order is what decides overlapping ranges.
    std::erase_if(memexport_staged_, [key](const MemexportStagedRange& staged) {
      return staged.key == key;
    });
    memexport_staged_.push_back({key, base_bytes, size_bytes});
    // The fence and coherency waits are driven by the page marks.
    MarkMemexportPagesWritten(base_bytes, size_bytes);
  }
}

void VulkanCommandProcessor::FlushMemexportStagingReadback() {
  if (memexport_staged_.empty()) {
    return;
  }
  // Staged output is about to reach guest RAM, so no fence need await it.
  memexport_await_pending_ = false;
  if (!AwaitAllQueueOperationsCompletion()) {
    XELOGE(
        "VulkanCommandProcessor: Failed to complete queue operations for "
        "memexport staging readback");
    memexport_staged_.clear();
    return;
  }
  for (const MemexportStagedRange& staged : memexport_staged_) {
    ReadbackStagingSlot* slot = FindReadbackStagingSlot(staged.key);
    if (slot == nullptr) {
      continue;
    }
    // The guest may have decommitted the range since the draw that staged it.
    uint32_t length = WritableGuestRangeLength(staged.address, staged.length);
    if (!length) {
      continue;
    }
    const ReadbackStagingBuffer& staging = slot->buffer;
    InvalidateReadbackStaging(staging);
    ReadbackStagingToGuestRam(staging, staged.address, length);
  }
  memexport_staged_.clear();
}

bool VulkanCommandProcessor::EnsureMemexportRangeInDeviceBuffer(
    uint32_t base_bytes, uint32_t size_bytes) {
  // Readers of the device buffer need memexport output copied across from
  // host_buffer_ (guest RAM), where it actually lives. Doing it on the GPU
  // keeps it ordered against the writes that produced it, which a CPU read
  // cannot be.
  if (!cvars::memexport_enable ||
      shared_memory_host_and_edram_descriptor_set_ == VK_NULL_HANDLE ||
      !size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  if (!IsMemexportRange(base_bytes, size_bytes)) {
    return false;
  }
  VkBuffer host_buffer = shared_memory_->host_buffer();
  VkBuffer device_buffer = shared_memory_->buffer();
  if (host_buffer == VK_NULL_HANDLE) {
    return false;
  }

  const VkPipelineStageFlags read_stages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkAccessFlags read_access = VK_ACCESS_INDEX_READ_BIT |
                                    VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT;

  // Order the writes on host_buffer_ before the transfer read (the producers
  // may have run several draws ago, so barrier host_buffer_ explicitly rather
  // than relying on Use's last-usage tracking), and prior device-buffer reads
  // before the transfer write into it. Resolves write host_buffer_ by transfer,
  // not just memexport shaders, so both source masks are needed.
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  PushBufferMemoryBarrier(
      host_buffer, VkDeviceSize(base_bytes), VkDeviceSize(size_bytes),
      guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes), read_stages,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_access,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
  SubmitBarriers(true);

  VkBufferCopy copy_region;
  copy_region.srcOffset = base_bytes;
  copy_region.dstOffset = base_bytes;
  copy_region.size = size_bytes;
  deferred_command_buffer_.CmdVkCopyBuffer(host_buffer, device_buffer, 1,
                                           &copy_region);

  // Make the copied data visible to the following read.
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes),
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_stages,
                          VK_ACCESS_TRANSFER_WRITE_BIT, read_access);
  return true;
}

void VulkanCommandProcessor::ResolveReadCallbackThunk(
    void* context, uint32_t physical_address, uint32_t length,
    Memory::PhysicalAccess access) {
  auto command_processor = static_cast<VulkanCommandProcessor*>(context);
  switch (access) {
    case Memory::PhysicalAccess::kRead:
      command_processor->MarkResolvePagesRead(physical_address, length);
      break;
    case Memory::PhysicalAccess::kWrite:
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      break;
    case Memory::PhysicalAccess::kDiscard:
      command_processor->DiscardResolvePages(physical_address, length);
      break;
  }
}

// Downscales a scaled resolve's output into resolve_downscale_buffer_, made
// readable by transfers, inside a debug marker scope the caller closes.
bool VulkanCommandProcessor::DownscaleScaledResolve(
    uint32_t written_address, const ScaledResolveReadbackInfo& scaled_info) {
  uint32_t pixel_size_log2 = scaled_info.pixel_size_log2;
  uint32_t tile_count = scaled_info.tile_count;
  uint32_t readback_length = scaled_info.readback_length;
  uint32_t scale_x = scaled_info.scale_x;
  uint32_t scale_y = scaled_info.scale_y;
  uint64_t scaled_start = scaled_info.scaled_start;
  uint64_t scaled_readback_length = scaled_info.scaled_readback_length;

  // Get scaled resolve buffer (works for both sparse and simple buffer modes)
  VkBuffer scaled_buffer = texture_cache_->GetCurrentScaledResolveBuffer();
  if (scaled_buffer == VK_NULL_HANDLE) {
    XELOGE("VulkanCommandProcessor: No scaled resolve buffer available");
    return false;
  }

  // Calculate offset within the buffer using the buffer's base address.
  // GetCurrentScaledResolveBufferBaseOffset() returns:
  // - For sparse buffers: buffer_index << 30 (same as D3D12's 1GB chunks)
  // - For simple buffers: the buffer's range_start_scaled
  uint64_t buffer_base =
      texture_cache_->GetCurrentScaledResolveBufferBaseOffset();
  if (scaled_start < buffer_base) {
    XELOGE(
        "VulkanCommandProcessor: Scaled address {} is before buffer start {}",
        scaled_start, buffer_base);
    return false;
  }
  uint64_t source_offset = scaled_start - buffer_base;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Ensure intermediate buffer for GPU downscaling is large enough
  uint32_t downscale_buffer_size = AlignReadbackBufferSize(readback_length);
  if (downscale_buffer_size > resolve_downscale_buffer_size_) {
    // Clean up old buffer
    if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE(
            "VulkanCommandProcessor: Failed to wait for GPU before "
            "destroying old downscale buffer");
        return false;
      }
      dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
      dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
      resolve_downscale_buffer_ = VK_NULL_HANDLE;
      resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
      resolve_downscale_buffer_size_ = 0;
    }

    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = downscale_buffer_size;
    buffer_info.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (dfn.vkCreateBuffer(device, &buffer_info, nullptr,
                           &resolve_downscale_buffer_) != VK_SUCCESS) {
      XELOGE("VulkanCommandProcessor: Failed to create {} MB downscale buffer",
             downscale_buffer_size >> 20);
      return false;
    }

    VkMemoryRequirements memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, resolve_downscale_buffer_,
                                      &memory_requirements);

    const uint32_t memory_type_index = ui::vulkan::util::ChooseMemoryType(
        vulkan_device->memory_types(), memory_requirements.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal);

    if (memory_type_index == UINT32_MAX) {
      XELOGE(
          "VulkanCommandProcessor: Failed to find memory type for downscale "
          "buffer");
      dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
      resolve_downscale_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryAllocateInfo memory_info = {};
    memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_info.allocationSize = memory_requirements.size;
    memory_info.memoryTypeIndex = memory_type_index;

    if (dfn.vkAllocateMemory(device, &memory_info, nullptr,
                             &resolve_downscale_buffer_memory_) != VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to allocate downscale buffer "
          "memory");
      dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
      resolve_downscale_buffer_ = VK_NULL_HANDLE;
      return false;
    }

    if (dfn.vkBindBufferMemory(device, resolve_downscale_buffer_,
                               resolve_downscale_buffer_memory_,
                               0) != VK_SUCCESS) {
      XELOGE("VulkanCommandProcessor: Failed to bind downscale buffer memory");
      dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
      dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
      resolve_downscale_buffer_ = VK_NULL_HANDLE;
      resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
      return false;
    }

    resolve_downscale_buffer_size_ = downscale_buffer_size;
  }

  // Allocate descriptor set for source and destination buffers.
  // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
  VkDescriptorSet descriptor_set =
      resolve_downscale_descriptor_pool_chain_->Allocate(
          GetCurrentSubmission());
  if (descriptor_set == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanCommandProcessor: Failed to allocate resolve downscale "
        "descriptor set from pool chain");
    return false;
  }

  // Ensure submission is open
  if (!BeginSubmission(true)) {
    XELOGE(
        "VulkanCommandProcessor: Failed to begin submission for scaled "
        "resolve readback");
    return false;
  }

  // Update descriptor set with buffer bindings
  // Bind source buffer at offset 0 to avoid alignment issues - offset is
  // passed via push constants and applied in the shader
  std::array<VkDescriptorBufferInfo, 2> buffer_infos;
  buffer_infos[0].buffer = scaled_buffer;
  buffer_infos[0].offset = 0;
  buffer_infos[0].range = VK_WHOLE_SIZE;
  // Destination buffer (intermediate device-local buffer)
  buffer_infos[1].buffer = resolve_downscale_buffer_;
  buffer_infos[1].offset = 0;
  buffer_infos[1].range = readback_length;

  std::array<VkWriteDescriptorSet, 2> descriptor_writes;
  descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_writes[0].pNext = nullptr;
  descriptor_writes[0].dstSet = descriptor_set;
  descriptor_writes[0].dstBinding = 0;
  descriptor_writes[0].dstArrayElement = 0;
  descriptor_writes[0].descriptorCount = 1;
  descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_writes[0].pImageInfo = nullptr;
  descriptor_writes[0].pBufferInfo = &buffer_infos[0];
  descriptor_writes[0].pTexelBufferView = nullptr;

  descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_writes[1].pNext = nullptr;
  descriptor_writes[1].dstSet = descriptor_set;
  descriptor_writes[1].dstBinding = 1;
  descriptor_writes[1].dstArrayElement = 0;
  descriptor_writes[1].descriptorCount = 1;
  descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_writes[1].pImageInfo = nullptr;
  descriptor_writes[1].pBufferInfo = &buffer_infos[1];
  descriptor_writes[1].pTexelBufferView = nullptr;

  dfn.vkUpdateDescriptorSets(device, uint32_t(descriptor_writes.size()),
                             descriptor_writes.data(), 0, nullptr);

  // End any active render pass and submit barriers
  SubmitBarriers(true);

  // Barrier for source buffer - ensure resolve copy compute shader writes
  // are complete before readback compute shader reads. And the copy out of the
  // previous downscale before this one overwrites it.
  VkBufferMemoryBarrier pre_dispatch_barriers[2] = {};
  pre_dispatch_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  pre_dispatch_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  pre_dispatch_barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  pre_dispatch_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre_dispatch_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre_dispatch_barriers[0].buffer = scaled_buffer;
  pre_dispatch_barriers[0].offset = 0;
  pre_dispatch_barriers[0].size = VK_WHOLE_SIZE;
  pre_dispatch_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  pre_dispatch_barriers[1].srcAccessMask = 0;
  pre_dispatch_barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  pre_dispatch_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre_dispatch_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre_dispatch_barriers[1].buffer = resolve_downscale_buffer_;
  pre_dispatch_barriers[1].offset = 0;
  pre_dispatch_barriers[1].size = readback_length;
  deferred_command_buffer_.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
      pre_dispatch_barriers, 0, nullptr);

  PushDebugMarker("Resolve Downscale: 0x%08X, %u bytes -> %u bytes",
                  written_address, uint32_t(scaled_readback_length),
                  readback_length);

  // Bind compute pipeline
  BindExternalComputePipeline(resolve_downscale_pipeline_);

  // Push constants
  ResolveDownscaleConstants constants;
  constants.scale_x = scale_x;
  constants.scale_y = scale_y;
  constants.pixel_size_log2 = pixel_size_log2;
  constants.tile_count = tile_count;
  constants.source_offset_bytes = static_cast<uint32_t>(source_offset);
  // Optionally sample from center of scaled block instead of top-left.
  constants.half_pixel_offset = (cvars::readback_resolve_half_pixel_offset &&
                                 (scale_x > 1 || scale_y > 1))
                                    ? 1
                                    : 0;
  deferred_command_buffer_.CmdVkPushConstants(
      resolve_downscale_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(constants), &constants);

  // Bind descriptor set
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, resolve_downscale_pipeline_layout_, 0, 1,
      &descriptor_set, 0, nullptr);

  // Dispatch compute shader - one thread group per 32x32 tile
  ++submission_in_progress_.dispatch_count;
  deferred_command_buffer_.CmdVkDispatch(tile_count, 1, 1);

  // Downscale compute-write -> transfer-read.
  VkBufferMemoryBarrier downscale_barrier = {};
  downscale_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  downscale_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  downscale_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  downscale_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  downscale_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  downscale_barrier.buffer = resolve_downscale_buffer_;
  downscale_barrier.offset = 0;
  downscale_barrier.size = readback_length;
  deferred_command_buffer_.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      0, nullptr, 1, &downscale_barrier, 0, nullptr);
  return true;
}

// Writes a scaled resolve's output, downscaled, into the shared memory buffer,
// where a native resolve would have written it, so it can be read back like
// one. The resolve has already marked the range as written by the GPU.
bool VulkanCommandProcessor::MirrorScaledResolveToSharedMemory(
    uint32_t written_address, uint32_t written_length,
    reg::RB_COPY_DEST_INFO copy_dest_info, uint32_t& mirrored_length_out) {
  if (resolve_downscale_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  ScaledResolveReadbackInfo scaled_info;
  if (!GetScaledResolveReadbackInfo(written_address, written_length,
                                    copy_dest_info, scaled_info)) {
    return false;
  }
  // Only for the memory backing the range, the pages are already valid.
  if (!shared_memory_->RequestRange(written_address,
                                    scaled_info.readback_length)) {
    return false;
  }
  if (!DownscaleScaledResolve(written_address, scaled_info)) {
    return false;
  }
  shared_memory_->Use(
      VulkanSharedMemory::Usage::kTransferDestination,
      std::make_pair(written_address, scaled_info.readback_length));
  SubmitBarriers(true);
  VkBufferCopy copy_region = {};
  copy_region.srcOffset = 0;
  copy_region.dstOffset = written_address;
  copy_region.size = scaled_info.readback_length;
  deferred_command_buffer_.CmdVkCopyBuffer(
      resolve_downscale_buffer_, shared_memory_->buffer(), 1, &copy_region);
  PopDebugMarker();
  texture_cache_->MarkScaledResolveMirrored(written_address,
                                            scaled_info.readback_length);
  mirrored_length_out = scaled_info.readback_length;
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }

  // Push debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PushDebugMarker("IssueCopy (Resolve)");
  }

  uint32_t written_address, written_length;
  reg::RB_COPY_DEST_INFO copy_dest_info;
  bool is_scaled;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                     written_address, written_length,
                                     &copy_dest_info, &is_scaled)) {
    if (debug_markers_enabled_) {
      PopDebugMarker();
    }
    return false;
  }
  ++submission_in_progress_.resolve_count;

  // The resolve wrote the device buffer. Drop any stale memexport marks so the
  // output isn't overwritten with guest RAM by a later texture load.
  ClearMemexportPages(written_address, written_length);

  if (IsReadbackResolveEnabled() && written_length > 0 &&
      IsResolveDestinationResident(written_address, written_length)) {
    // A CPU access copies the output into guest RAM out of the shared memory
    // buffer, so scaled output has to be there too. With zero-copy that buffer
    // is guest RAM, where native resolves land in place as well.
    if (is_scaled) {
      uint32_t mirrored_length;
      if (MirrorScaledResolveToSharedMemory(written_address, written_length,
                                            copy_dest_info, mirrored_length)) {
        is_scaled = false;
        written_length = mirrored_length;
      }
    }
    NoteResolveForReadback(written_address, written_length, is_scaled);
  }

  // Pop debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PopDebugMarker();
  }

  SubmitRequestedResolve();

  return true;
}

void VulkanCommandProcessor::EnsureZPDQueryResources() {
  if (zpd_mode_ == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  bool can_recreate = zpd_current_report_.handle == kInvalidReportHandle &&
                      !zpd_active_segment_.segment_active &&
                      zpd_active_query_index_ == UINT32_MAX &&
                      !zpd_active_query_is_fsi_ &&
                      !zpd_host_query_pool_->has_pending_resolve_batch() &&
                      zpd_resolves_in_flight_.empty();

  bool fsi_path = render_target_cache_ &&
                  render_target_cache_->GetPath() ==
                      RenderTargetCache::Path::kPixelShaderInterlock;
  zpd_fsi_path_ = fsi_path;

  // Hybrid queries need the counter too.
  if (zpd_host_query_pool_->EnsureInitialized(
          GetVulkanDevice(), kZPDQueryPoolCapacity, can_recreate,
          fsi_path || zpd_hybrid_supported_) &&
      zpd_host_query_pool_->counter_initialized()) {
    VkBuffer counter_buffer = zpd_host_query_pool_->counter_buffer();
    VkDeviceSize counter_range =
        XenosZPDReport::kCounterSizeBytes * zpd_host_query_pool_->capacity();
    if (zpd_counter_descriptor_buffer_ != counter_buffer ||
        zpd_counter_descriptor_range_ != counter_range) {
      VkDescriptorBufferInfo counter_descriptor_buffer_info;
      counter_descriptor_buffer_info.buffer = counter_buffer;
      counter_descriptor_buffer_info.offset = 0;
      counter_descriptor_buffer_info.range = counter_range;

      // Mirror binding 1 to the host-imported routing set too, so a routed
      // draw reads the same counter.
      VkWriteDescriptorSet counter_descriptor_writes[2];
      uint32_t counter_descriptor_write_count = 0;
      for (VkDescriptorSet set :
           {shared_memory_and_edram_descriptor_set_,
            shared_memory_host_and_edram_descriptor_set_}) {
        if (set == VK_NULL_HANDLE) {
          continue;
        }
        VkWriteDescriptorSet& counter_descriptor_write =
            counter_descriptor_writes[counter_descriptor_write_count++];
        counter_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        counter_descriptor_write.pNext = nullptr;
        counter_descriptor_write.dstSet = set;
        counter_descriptor_write.dstBinding = 1;
        counter_descriptor_write.dstArrayElement = 0;
        counter_descriptor_write.descriptorCount = 1;
        counter_descriptor_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        counter_descriptor_write.pImageInfo = nullptr;
        counter_descriptor_write.pBufferInfo = &counter_descriptor_buffer_info;
        counter_descriptor_write.pTexelBufferView = nullptr;
      }

      const ui::vulkan::VulkanDevice::Functions& dfn =
          GetVulkanDevice()->functions();
      dfn.vkUpdateDescriptorSets(GetVulkanDevice()->device(),
                                 counter_descriptor_write_count,
                                 counter_descriptor_writes, 0, nullptr);

      zpd_counter_descriptor_buffer_ = counter_buffer;
      zpd_counter_descriptor_range_ = counter_range;
    }
  } else if (fsi_path && !IsZPDQueryPoolReady()) {
    XELOGW(
        "ZPD/Vulkan: Counter resources unavailable; keeping counter "
        "index sentinel active");
  }
  zpd_counter_index_force_update_ = true;
}

bool VulkanCommandProcessor::IsZPDQueryPoolReady() const {
  if (!zpd_host_query_pool_ || !zpd_host_query_pool_->fbo_initialized()) {
    return false;
  }
  if (!zpd_fsi_path_) {
    return true;
  }
  VkDeviceSize counter_range =
      XenosZPDReport::kCounterSizeBytes * zpd_host_query_pool_->capacity();
  return zpd_host_query_pool_->counter_initialized() &&
         zpd_counter_descriptor_buffer_ ==
             zpd_host_query_pool_->counter_buffer() &&
         zpd_counter_descriptor_range_ == counter_range;
}

bool VulkanCommandProcessor::CanOpenZPDQuery() const {
  if (!submission_open_) {
    return false;
  }
  return (render_target_cache_ &&
          render_target_cache_->GetPath() ==
              RenderTargetCache::Path::kPixelShaderInterlock) ||
         in_render_pass_;
}

CommandProcessor::QueryOpenResult VulkanCommandProcessor::OpenZPDQuery(
    bool can_close_submission) {
  bool use_fsi_path =
      zpd_fsi_path_ && zpd_host_query_pool_->counter_initialized();

  if (!BeginSubmission(true)) {
    return QueryOpenResult::kFailed;
  }

  if (!use_fsi_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  // Strict mode can't guess, so wait for the oldest in-flight resolve to hand
  // a slot back. If it's still in the open submission, flip submissions once,
  // reentering the render pass, and try again.
  bool retried_after_submission_flip = false;
  while (true) {
    bool is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();

    if (is_pool_exhausted) {
      PumpQueryResolves();
      is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();
    }

    if (is_pool_exhausted && zpd_mode_ != ZPDMode::kStrict) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (is_pool_exhausted && !zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    if (wait_for == 0) {
      break;
    }

    if (submission_open_ && wait_for == GetCurrentSubmission()) {
      if (retried_after_submission_flip || !can_close_submission ||
          !CanEndSubmissionImmediately()) {
        return QueryOpenResult::kDeferred;
      }

      VkRenderPass saved_render_pass = VK_NULL_HANDLE;
      const VulkanRenderTargetCache::Framebuffer* saved_framebuffer = nullptr;
      if (in_render_pass_) {
        saved_render_pass = current_render_pass_;
        saved_framebuffer = current_framebuffer_;
        EndRenderPass();
      }
      if (!EndSubmission(false)) {
        return QueryOpenResult::kFailed;
      }
      if (!BeginSubmission(true)) {
        return QueryOpenResult::kFailed;
      }

      if (saved_framebuffer) {
        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        if (use_fsi_path) {
          zpd_active_segment_.segment_pending_begin = false;
        }
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
        if (!in_render_pass_) {
          return QueryOpenResult::kDeferred;
        }
      }

      retried_after_submission_flip = true;
      continue;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }
      completion_timeline_.AwaitSubmissionAndUpdateCompleted(wait_for);
      PumpQueryResolves();
    }

    break;
  }

  if (!use_fsi_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  if (!zpd_host_query_pool_->AcquireQueryIndex(zpd_active_query_index_,
                                               zpd_active_query_generation_)) {
    return QueryOpenResult::kFailed;
  }

  zpd_active_query_is_fsi_ = use_fsi_path;
  zpd_active_segment_.hybrid = !use_fsi_path && zpd_hybrid_supported_ &&
                               zpd_active_segment_.count_total &&
                               zpd_host_query_pool_->counter_initialized();

  // FSI queries don't use Vulkan occlusion queries at all.
  // While the segment is open, the translated pixel shader accumulates depth/
  // stencil outcomes into one counter slot selected via zpd_counter_index.
  // Clear the slot here if it's still dirty so a recycled index never inherits
  // old counts.
  if (zpd_active_query_is_fsi_) {
    if (zpd_host_query_pool_->IsCounterDirty(zpd_active_query_index_)) {
      if (in_render_pass_) {
        VkRenderPass saved_render_pass = current_render_pass_;
        const VulkanRenderTargetCache::Framebuffer* saved_framebuffer =
            current_framebuffer_;
        EndRenderPass();
        zpd_host_query_pool_->ClearCounter(deferred_command_buffer_,
                                           zpd_active_query_index_);
        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        zpd_active_segment_.segment_pending_begin = false;
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
      } else {
        zpd_host_query_pool_->ClearCounter(deferred_command_buffer_,
                                           zpd_active_query_index_);
      }
    }
    zpd_counter_index_force_update_ = true;
    return QueryOpenResult::kOpened;
  }

  if (zpd_active_segment_.hybrid) {
    // Dirty slots are cleared in bulk before the next render pass begins, so
    // wait for that rather than splitting this one.
    if (zpd_host_query_pool_->IsCounterDirty(zpd_active_query_index_)) {
      zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                              zpd_active_query_generation_);
      zpd_active_query_index_ = UINT32_MAX;
      zpd_active_query_generation_ = 0;
      zpd_active_segment_.hybrid = false;
      return QueryOpenResult::kDeferred;
    }
    zpd_counter_index_force_update_ = true;
  }

  zpd_host_query_pool_->BeginQuery(deferred_command_buffer_,
                                   zpd_active_query_index_);
  return QueryOpenResult::kOpened;
}

bool VulkanCommandProcessor::CloseZPDQuery(ReportHandle report_handle,
                                           uint64_t& out_submission) {
  if (!zpd_active_query_is_fsi_ && !in_render_pass_) {
    XELOGW("ZPD: Split segment requested outside render pass");
    return false;
  }

  if (zpd_active_query_is_fsi_) {
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
  } else {
    zpd_host_query_pool_->EndQuery(deferred_command_buffer_,
                                   zpd_active_query_index_);
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, false);
    if (zpd_active_segment_.hybrid) {
      zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
    }
  }

  PendingQueryResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.query_index = zpd_active_query_index_;
  resolve.query_generation = zpd_active_query_generation_;
  resolve.scale_area = GetZPDScaleArea();
  resolve.fsi = zpd_active_query_is_fsi_;
  resolve.hybrid = zpd_active_segment_.hybrid;
  resolve.report_handle = report_handle;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  if (zpd_active_query_is_fsi_ || zpd_active_segment_.hybrid) {
    zpd_counter_index_force_update_ = true;
  }
  zpd_active_query_is_fsi_ = false;
  return true;
}

void VulkanCommandProcessor::PumpQueryResolves() {
  if (!zpd_host_query_pool_ || zpd_resolves_in_flight_.empty()) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (zpd_resolves_in_flight_.front().submission > completed) {
    return;
  }

  // Invalidate CPU cache before reading results on non-coherent memory.
  zpd_host_query_pool_->InvalidateReadback();

  while (!zpd_resolves_in_flight_.empty() &&
         zpd_resolves_in_flight_.front().submission <= completed) {
    PendingQueryResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (zpd_host_query_pool_->GenerationMatches(resolve.query_index,
                                                resolve.query_generation)) {
      XenosZPDReport raw_counts = zpd_host_query_pool_->GetQueryReadbackValue(
          resolve.query_index, resolve.fsi, resolve.hybrid);
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD/Vulkan: Resolved {} segment index={} z_pass={}",
               resolve.fsi ? "FSI" : (resolve.hybrid ? "hybrid" : "native"),
               resolve.query_index, raw_counts.z_pass);
      }
      zpd_host_query_pool_->ReleaseQueryIndex(resolve.query_index,
                                              resolve.query_generation);
      OnZPDQueryResolved(resolve.report_handle, raw_counts, resolve.scale_area);
    }
  }
}

bool VulkanCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                               uint64_t wait_for_submission) {
  assert_not_zero(wait_for_submission);

  // The resolve is still in the open submission. Pipelines being created
  // asynchronously have to finish before it can be executed.
  if (wait_for_submission >= GetCurrentSubmission()) {
    if (!submission_open_) {
      return false;
    }
    if (!CanEndSubmissionImmediately()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Async: Draining Vulkan async pipeline creation for strict "
            "retirement");
      }
      pipeline_cache_->AwaitPipelineCompletion();
    }
    EndRenderPass();
    if (!EndSubmission(false)) {
      return false;
    }
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    completion_timeline_.AwaitSubmissionAndUpdateCompleted(wait_for_submission);
  }

  PumpQueryResolves();

  const ZPDReport* report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

void VulkanCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(true)) {
    return;
  }
  bool render_target_cache_submitted =
      render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_cache_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_cache_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool VulkanCommandProcessor::DumpEdramSnapshotToFile(
    const std::filesystem::path& path) {
  if (!BeginSubmission(true)) {
    return false;
  }
  if (!render_target_cache_->InitializeTraceSubmitDownloads()) {
    return false;
  }
  AwaitAllQueueOperationsCompletion();
  return render_target_cache_->WriteEdramSnapshotToFile(path);
}

void VulkanCommandProcessor::LogRecentSubmissions(const char* context) {
  XELOGE(
      "VulkanCommandProcessor: recent submission history ({}) - oldest first, "
      "current in-progress last:",
      context);
  // Walk the ring starting from the oldest entry.
  for (size_t i = 0; i < kSubmissionHistorySize; ++i) {
    const SubmissionSummary& s =
        submission_history_[(submission_history_next_ + i) %
                            kSubmissionHistorySize];
    if (!s.submission_index) {
      continue;
    }
    XELOGE(
        "  sub {:>5} frame {:>5}: draws={} dispatches={} resolves={} "
        "VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
  if (submission_open_) {
    const SubmissionSummary& s = submission_in_progress_;
    XELOGE(
        "  sub {:>5} frame {:>5} (in-progress): draws={} dispatches={} "
        "resolves={} VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
}

void VulkanCommandProcessor::CheckSubmissionCompletionAndDeviceLoss(
    uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  completion_timeline_.AwaitSubmissionAndUpdateCompleted(await_submission);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  if (vulkan_device->IsLost()) {
    XELOGE(
        "VulkanCommandProcessor: device lost observed in submission-check - "
        "awaiting submission {}, current {}, completed {}, in-flight {}, "
        "frame {} (frame_open: {}, submission_open: {})",
        await_submission, GetCurrentSubmission(), GetCompletedSubmission(),
        command_buffers_submitted_.size(), frame_current_, frame_open_,
        submission_open_);
    LogRecentSubmissions("submission-check");
    device_lost_ = true;
    graphics_system_->OnHostGpuLossFromAnyThread(true);
    return;
  }

  const uint64_t completed_submission = GetCompletedSubmission();

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission =
        submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > completed_submission) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > completed_submission) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(completed_submission);

  // Reclaim descriptor pools that the GPU has finished using.
  if (resolve_downscale_descriptor_pool_chain_) {
    resolve_downscale_descriptor_pool_chain_->Reclaim(completed_submission);
  }

  PumpQueryResolves();
  PumpPendingRetire();

  // Destroy objects scheduled for destruction.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission =
      is_opening_frame
          ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight]
          : 0;
  CheckSubmissionCompletionAndDeviceLoss(await_submission);
  const uint64_t completed_submission = GetCompletedSubmission();
  if (device_lost_ || completed_submission < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                       kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
          completed_submission) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    submission_in_progress_ = SubmissionSummary{};
    submission_in_progress_.submission_index = GetCurrentSubmission();
    submission_in_progress_.frame_index = frame_current_;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();

    // Reset cached state of the command buffer.
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Log guest ZPD report stats every 100 frames.
    if (GetZPDMode() != ZPDMode::kFake && cvars::occlusion_query_log &&
        zpd_host_query_pool_ && zpd_host_query_pool_->capacity() &&
        frame_current_ - zpd_stats_.last_log_frame >= 100) {
      XELOGI(
          "Occlusion Query Stats (last 100 frames): "
          "ReportsQueued={}, ReportsRetired={}, SegBegun={}, SegEnded={}, "
          "PoolExhausted={}, Failed={}, SpeculativeCorrections={}, "
          "RetiresAbandoned={}",
          zpd_stats_.reports_queued, zpd_stats_.reports_retired,
          zpd_stats_.segments_begun, zpd_stats_.segments_ended,
          zpd_stats_.pool_exhausted, zpd_stats_.failed,
          zpd_stats_.speculative_corrections, zpd_stats_.retires_abandoned);

      zpd_stats_.Reset(frame_current_);
    }

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0,
                sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0,
                sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
            shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1)
        << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(
                                             used_transient_descriptor.layout)]
          .push_back(used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(
          used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(
          used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it =
            texture_transient_descriptor_sets_free_
                .emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(used_transient_descriptor_set.layout),
                    std::forward_as_tuple())
                .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_ ||
         !pipeline_cache_->IsCreatingPipelines();
}

void VulkanCommandProcessor::CreateGpuTimeQueryPool() {
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  if (!vulkan_device->properties().timestampPeriod) {
    XELOGW("Vulkan: no timestamp support, GPU busy time will not be reported");
    return;
  }
  VkQueryPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  pool_info.queryCount = kGpuTimeQuerySubmissions * 2;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  if (dfn.vkCreateQueryPool(vulkan_device->device(), &pool_info, nullptr,
                            &gpu_time_query_pool_) != VK_SUCCESS) {
    XELOGE("Vulkan: failed to create the GPU time query pool");
    gpu_time_query_pool_ = VK_NULL_HANDLE;
  }
}

void VulkanCommandProcessor::DestroyGpuTimeQueryPool() {
  if (gpu_time_query_pool_ == VK_NULL_HANDLE) {
    return;
  }
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  vulkan_device->functions().vkDestroyQueryPool(vulkan_device->device(),
                                                gpu_time_query_pool_, nullptr);
  gpu_time_query_pool_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::CollectCompletedGpuTimeQueries() {
  if (gpu_time_query_pool_ == VK_NULL_HANDLE) {
    return;
  }
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const double period = vulkan_device->properties().timestampPeriod;
  const uint64_t completed = GetCompletedSubmission();
  for (uint32_t slot = 0; slot < kGpuTimeQuerySubmissions; ++slot) {
    uint64_t submission = gpu_time_query_submissions_[slot];
    if (!submission || submission > completed) {
      continue;
    }
    uint64_t timestamps[2] = {};
    if (dfn.vkGetQueryPoolResults(vulkan_device->device(), gpu_time_query_pool_,
                                  slot * 2, 2, sizeof(timestamps), timestamps,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        timestamps[1] > timestamps[0]) {
      gpu_time_total_ns_ += uint64_t((timestamps[1] - timestamps[0]) * period);
    }
    gpu_time_query_submissions_[slot] = 0;
  }
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr,
                                &semaphore) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex =
          vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        XELOGE("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    EndRenderPass();

    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount =
          uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty()
              ? sparse_buffer_bind_infos_temp_.data()
              : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(
                vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result = dfn.vkQueueBindSparse(
            queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        XELOGE("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(
          sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }
    // Can't cross command buffer boundaries. Close the active segment first.
    CloseQuerySegment();

    SubmitBarriers(true);

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer,
                                 &command_buffer_begin_info) != VK_SUCCESS) {
      XELOGE("Failed to begin a Vulkan command buffer");
      return false;
    }
    // Bracket the whole submission. Recycle finished slots first, or a frame
    // with more submissions than slots leaves the later ones untimed.
    CollectCompletedGpuTimeQueries();
    const bool gpu_time_pool_available = gpu_time_query_pool_ != VK_NULL_HANDLE;
    const uint32_t gpu_time_slot = gpu_time_query_slot_;
    const bool gpu_time_recorded =
        gpu_time_pool_available && !gpu_time_query_submissions_[gpu_time_slot];
    if (gpu_time_pool_available && !gpu_time_recorded) {
      ++gpu_time_untimed_submissions_;
    }
    if (gpu_time_recorded) {
      dfn.vkCmdResetQueryPool(command_buffer.buffer, gpu_time_query_pool_,
                              gpu_time_slot * 2, 2);
      dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              gpu_time_query_pool_, gpu_time_slot * 2);
    }

    deferred_command_buffer_.Execute(command_buffer.buffer);

    // Record ZPD resolves before submitting.
    if (zpd_host_query_pool_) {
      zpd_host_query_pool_->RecordResolveBatch(command_buffer.buffer);
    }
    if (gpu_time_recorded) {
      dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              gpu_time_query_pool_, gpu_time_slot * 2 + 1);
      gpu_time_query_submissions_[gpu_time_slot] = GetCurrentSubmission();
      gpu_time_query_slot_ =
          (gpu_time_query_slot_ + 1) % kGpuTimeQuerySubmissions;
    }
    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      XELOGE("Failed to end a Vulkan command buffer");
      return false;
    }

    const uint64_t submission_index = GetCurrentSubmission();

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount =
          uint32_t(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask =
          current_submission_wait_stage_masks_.data();
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.buffer;
    const VkResult submit_result = completion_timeline_.AcquireFenceAndSubmit(
        vulkan_device->queue_family_graphics_compute(), 0, 1, &submit_info);
    if (submit_result != VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to submit a Vulkan command buffer - "
          "VkResult: {} (0x{:08X}), submission: {} (completed: {}, in-flight: "
          "{}), frame: {} (frame_open: {}, is_closing_frame: {}), "
          "wait_semaphores: {}, draw_resolution_scale: {}x{}",
          static_cast<int32_t>(submit_result),
          static_cast<uint32_t>(submit_result), GetCurrentSubmission(),
          GetCompletedSubmission(), command_buffers_submitted_.size(),
          frame_current_, frame_open_, is_closing_frame,
          submit_info.waitSemaphoreCount,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_x()
                               : 0,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_y()
                               : 0);
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        XELOGE(
            "VK_ERROR_DEVICE_LOST - GPU crashed or hung. This may be caused by "
            "an invalid shader, out-of-bounds memory access, or driver bug.");
      }
      LogRecentSubmissions("submit-failure");
      if (vulkan_device->IsLost() && !device_lost_) {
        device_lost_ = true;
        graphics_system_->OnHostGpuLossFromAnyThread(true);
      }
      return false;
    }
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_index,
                                                     semaphore);
    }
    current_submission_wait_semaphores_.clear();
    resolve_submitted_through_.store(submission_index,
                                     std::memory_order_release);
    OnResolveSubmissionEnded();
    command_buffers_submitted_.emplace_back(submission_index, command_buffer);
    command_buffers_writable_.pop_back();

    submission_history_[submission_history_next_] = submission_in_progress_;
    submission_history_next_ =
        (submission_history_next_ + 1) % kSubmissionHistorySize;

    // Mark descriptor pool chains with submission index for reclaim tracking.
    if (resolve_downscale_descriptor_pool_chain_) {
      resolve_downscale_descriptor_pool_chain_->EndSubmission(submission_index);
    }

    submission_open_ = false;

    // Process any ZPD resolves that completed with this submission.
    PumpQueryResolves();
    PumpPendingRetire();
  }

  if (is_closing_frame) {
    if (cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }

    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] =
        GetCurrentSubmission() - 1;
    // Backstop for export output no fence or coherency request has asked for,
    // so it can't sit staged indefinitely.
    FlushMemexportStagingReadback();
    EvictOldReadbackStaging();

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_storage_buffer_and_image_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count =
      pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count =
      pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask &&
      !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >=
          pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset =
      pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset =
      pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         scratch_buffer_memory_);
}

void VulkanCommandProcessor::UpdateDynamicState(
    const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
    bool depth_bias_in_pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Scale the scissor to match the render target resolution scale
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    if (depth_bias_in_pixel_shader) {
      depth_bias_constant_factor = 0.0f;
      depth_bias_slope_factor = 0.0f;
    } else {
      draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal,
                                               depth_bias_slope_factor,
                                               depth_bias_constant_factor);
      depth_bias_constant_factor *=
          regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                  xenos::DepthRenderTargetFormat::kD24S8
              ? draw_util::kD3D10PolygonOffsetFactorUnorm24
              : draw_util::kD3D10PolygonOffsetFactorFloat24;
      // With non-square resolution scaling, make sure the worst-case impact is
      // reverted (slope only along the scaled axis), thus max. Per-draw scale
      // so native draws get the guest bias as is. More bias is better than less
      // bias; less bias means Z fighting with the background is more likely.
      depth_bias_slope_factor *=
          xenos::kPolygonOffsetScaleSubpixelUnit *
          float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    }
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_,
                    &depth_bias_constant_factor, sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_slope_factor_, &depth_bias_slope_factor,
                    sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(
          dynamic_depth_bias_constant_factor_, 0.0f,
          dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants,
                    sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
          stencil_ref_mask_front_reg =
              (pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back)
                  ? XE_GPU_REG_RB_STENCILREFMASK_BF
                  : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ !=
          stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ !=
          stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ !=
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ =
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ !=
          stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ ==
          dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(
            VK_STENCIL_FACE_FRONT_AND_BACK,
            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ ==
          dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // TODO(Triang3l): VK_EXT_extended_dynamic_state and
  // VK_EXT_extended_dynamic_state2.
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  // Resolution scale of this draw.
  // 1x1 with draw_resolution_scale_threshold (FSI only).
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  // The color info registers are also read by the non-FSI gamma flag and color
  // exponent bias below.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing. When gamma is stored as unorm16, the host render target
  // holds linear values (blended in linear space) and the linear -> gamma
  // encode happens on the EDRAM store, so the pixel shader must not pre-encode.
  if (!edram_fragment_shader_interlock &&
      !render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock) {
    // Fragment shader interlock (EDRAM ROP) flag bits and EDRAM constants. The
    // ZPD occlusion counter slot is selected here from Vulkan-specific query
    // state and passed into the shared helper.
    uint32_t zpd_fsi_counter_index = UINT32_MAX;
    if (zpd_active_query_index_ != UINT32_MAX && zpd_active_query_is_fsi_ &&
        zpd_host_query_pool_->counter_initialized()) {
      zpd_fsi_counter_index = zpd_active_query_index_;
    }
    dirty |= zpd_counter_index_force_update_;
    zpd_counter_index_force_update_ = false;
    WriteFragmentShaderInterlockSystemConstants(
        system_constants_, flags, dirty, regs, primitive_polygonal,
        normalized_depth_control, normalized_color_mask,
        draw_resolution_scale_x, draw_resolution_scale_y,
        zpd_fsi_counter_index);
  } else {
    // Hybrid queries on the host render target path count pre-test coverage
    // into the same counter slot from the pixel shader.
    uint32_t zpd_counter_index = UINT32_MAX;
    if (zpd_active_query_index_ != UINT32_MAX && zpd_active_segment_.hybrid &&
        zpd_host_query_pool_->counter_initialized()) {
      zpd_counter_index = zpd_active_query_index_;
    }
    dirty |= zpd_counter_index_force_update_ ||
             system_constants_.zpd_fsi_counter_index != zpd_counter_index;
    system_constants_.zpd_fsi_counter_index = zpd_counter_index;
    zpd_counter_index_force_update_ = false;
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |= system_constants_.vertex_index_load_address !=
             primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // Guest-side vertex index count, used for bounds-safe shared-memory loads in
  // VS expansion (kPointListAsTriangleStrip / kRectangleListAsTriangleStrip)
  // and in the full 32-bit index DMA load path. Out-of-bounds lanes read 0
  // instead of random memory, which otherwise produces scattered/skewed
  // geometry when expansion fans out past the guest draw count.
  const bool is_vs_expansion_draw =
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  const uint32_t vertex_index_count =
      is_vs_expansion_draw ? primitive_processing_result.guest_draw_vertex_count
                           : primitive_processing_result.host_draw_vertex_count;
  dirty |= system_constants_.vertex_index_count != vertex_index_count;
  system_constants_.vertex_index_count = vertex_index_count;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian !=
           primitive_processing_result.host_shader_index_endian;
  system_constants_.vertex_index_endian =
      primitive_processing_result.host_shader_index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // User clip planes, for vertex and domain shaders.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (xe::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining =
          xe::clear_lowest_bit(user_clip_planes_remaining);
      // Validate plane index is within bounds (0-5).
      assert(user_clip_plane_index < 6);
      if (user_clip_plane_index >= 6) {
        continue;
      }
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs,
                      4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                    4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  } else {
    constexpr float kZeroPlanes[6][4] = {};
    if (std::memcmp(system_constants_.user_clip_planes, kZeroPlanes,
                    sizeof(system_constants_.user_clip_planes))) {
      dirty = true;
      std::memset(system_constants_.user_clip_planes, 0,
                  sizeof(system_constants_.user_clip_planes));
    }
  }

  // Tessellation constants. The factor range has 1.0 added per Xbox 360 docs.
  // fractional_even partitioning needs a minimum of 2.0.
  {
    float tess_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    float tess_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    dirty |= system_constants_.tessellation_factor_range[0] != tess_min;
    dirty |= system_constants_.tessellation_factor_range[1] != tess_max;
    system_constants_.tessellation_factor_range[0] = tess_min;
    system_constants_.tessellation_factor_range[1] = tess_max;
    uint32_t tess_vie = static_cast<uint32_t>(
        primitive_processing_result.host_shader_index_endian);
    uint32_t tess_vio = regs[XE_GPU_REG_VGT_INDX_OFFSET];
    uint32_t tess_vmin = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
    uint32_t tess_vmax = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
    dirty |= system_constants_.tessellation_vertex_index_endian != tess_vie;
    dirty |= system_constants_.tessellation_vertex_index_offset != tess_vio;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[0] != tess_vmin;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[1] != tess_vmax;
    system_constants_.tessellation_vertex_index_endian = tess_vie;
    system_constants_.tessellation_vertex_index_offset = tess_vio;
    system_constants_.tessellation_vertex_index_min_max[0] = tess_vmin;
    system_constants_.tessellation_vertex_index_min_max[1] = tess_vmax;
  }

  // Point size, and the NDC size of a guest pixel, which the line geometry
  // shader also uses to expand resolution-scaled lines to 1 guest pixel wide.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineStrip ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineLoop) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min !=
             point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max !=
             point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] !=
             point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] !=
             point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1)
                                    << texture_signs_shift;
      dirty |=
          (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      uint32_t texture_integer_scale_bits =
          texture_cache_->GetActiveIntegerScaleBits(texture_index);
      dirty |= system_constants_.texture_integer_scale_bits[texture_index] !=
               texture_integer_scale_bits;
      system_constants_.texture_integer_scale_bits[texture_index] =
          texture_integer_scale_bits;
    }
  }

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint =
          system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle =
          texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle)
                                         << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1)
                                      << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) !=
               texture_swizzle_shifted;
      texture_swizzles_uint = (texture_swizzles_uint & ~texture_swizzle_mask) |
                              texture_swizzle_shifted;
    }
  }

  // Textures resolution scaled - which textures are from scaled resolve
  // operations.
  {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    dirty |= system_constants_.textures_resolved != textures_resolved;
    system_constants_.textures_resolved = textures_resolved;
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;

  // Alpha to coverage.
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format ==
                 xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  if (!edram_fragment_shader_interlock && host_depth_polygon_offset) {
    draw_util::HostDepthPolygonOffset polygon_offset =
        *host_depth_polygon_offset;
    float scale_factor =
        float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    polygon_offset.front_scale *= scale_factor;
    polygon_offset.back_scale *= scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale !=
             polygon_offset.front_scale;
    system_constants_.edram_poly_offset_front_scale =
        polygon_offset.front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset !=
             polygon_offset.front_offset;
    system_constants_.edram_poly_offset_front_offset =
        polygon_offset.front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale !=
             polygon_offset.back_scale;
    system_constants_.edram_poly_offset_back_scale = polygon_offset.back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset !=
             polygon_offset.back_offset;
    system_constants_.edram_poly_offset_back_offset =
        polygon_offset.back_offset;
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader,
                                            bool interpreter_placeholder,
                                            bool placeholder_pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  // The interpreter needs all 256 float constants unpacked; switching between
  // that and a shader's packed subset must re-upload.
  if (interpreter_placeholder != float_constants_vertex_are_full_) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
    float_constants_vertex_are_full_ = interpreter_placeholder;
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  // The bindings are dynamic, so a new suballocation only changes the offsets
  // passed when binding the set. The set itself has to be rewritten only when a
  // pool page or a size changes.
  bool constant_buffer_offsets_changed = false;
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) !=
      kAllConstantBuffersMask) {
    constant_buffer_offsets_changed = true;
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    auto request_constant_buffer = [&](uint32_t index,
                                       size_t size) -> uint8_t* {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceSize offset = 0;
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, size, uniform_buffer_alignment, buffer, offset);
      if (!mapping) {
        return nullptr;
      }
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[index];
      if (buffer_info.buffer != buffer ||
          buffer_info.range != VkDeviceSize(size)) {
        buffer_info.buffer = buffer;
        buffer_info.range = VkDeviceSize(size);
        current_graphics_descriptor_set_values_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
      }
      current_constant_buffer_dynamic_offsets_[index] = uint32_t(offset);
      current_constant_buffers_up_to_date_ |= UINT32_C(1) << index;
      return mapping;
    };
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferSystem,
          sizeof(SpirvShaderTranslator::SystemConstants));
      if (!mapping) {
        return false;
      }
      std::memcpy(mapping, &system_constants_,
                  sizeof(SpirvShaderTranslator::SystemConstants));
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      // The interpreter indexes all 256 float constants by raw index, so upload
      // the full contiguous register file rather than the packed subset.
      size_t float_constants_size =
          interpreter_placeholder
              ? sizeof(float) * 4 * 256
              : sizeof(float) * 4 *
                    std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferFloatVertex,
          float_constants_size);
      if (!mapping) {
        return false;
      }
      if (interpreter_placeholder) {
        std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X],
                    sizeof(float) * 4 * 256);
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t float_constant_map_entry =
              current_float_constant_map_vertex_[i];
          uint32_t float_constant_index;
          while (xe::bit_scan_forward(float_constant_map_entry,
                                      &float_constant_index)) {
            float_constant_map_entry &= ~(1ull << float_constant_index);
            std::memcpy(mapping,
                        &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                              (float_constant_index << 2)],
                        sizeof(float) * 4);
            mapping += sizeof(float) * 4;
          }
        }
      }
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferFloatPixel,
          float_constants_size);
      if (!mapping) {
        return false;
      }
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry =
            current_float_constant_map_pixel_[i];
        uint32_t float_constant_index;
        while (xe::bit_scan_forward(float_constant_map_entry,
                                    &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferBoolLoop,
          kBoolLoopConstantsSize);
      if (!mapping) {
        return false;
      }
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                  kBoolLoopConstantsSize);
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferFetch, kFetchConstantsSize);
      if (!mapping) {
        return false;
      }
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                  kFetchConstantsSize);
    }
  }

  // Textures and samplers. Only read a shader's binding lists when its bindings
  // are ready - an async draw doesn't translate here, so a creation thread may
  // still be populating them (reading size()/contents would race). Binding the
  // reference is fine; only size()/iteration touch the racing buffer.
  bool vertex_bindings_ready = vertex_shader->bindings_ready();
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex =
      vertex_bindings_ready ? uint32_t(samplers_vertex.size()) : 0;
  uint32_t texture_count_vertex =
      vertex_bindings_ready ? uint32_t(textures_vertex.size()) : 0;
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader && pixel_shader->bindings_ready()) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // Placeholder pipelines were built with a layout matching only what their
  // placeholder shaders access, so don't bind texture sets they lack (which
  // could otherwise happen if the real shaders finished translating mid-draw).
  // The no-op placeholder PS never samples; the interpreter VS never samples.
  if (placeholder_pixel_shader) {
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  if (interpreter_placeholder) {
    sampler_count_vertex = 0;
    texture_count_vertex = 0;
  }
  // Reuse the texture and sampler descriptor sets across draws while their
  // contents stay valid. A set becomes stale when the shader (and thus its
  // binding list) changes, when a used texture's host image view changes, or
  // when a sampler changes. The validity bits are cleared at the start of every
  // frame, and the transient sets aren't recycled until the frame completes, so
  // a kept set's handle is always still alive.
  uint32_t textures_changed = texture_cache_->texture_bindings_changed();
  uint32_t used_texture_mask_vertex =
      vertex_bindings_ready
          ? vertex_shader->GetUsedTextureMaskAfterTranslation()
          : 0;
  uint32_t used_texture_mask_pixel =
      (pixel_shader && pixel_shader->bindings_ready())
          ? pixel_shader->GetUsedTextureMaskAfterTranslation()
          : 0;
  if ((current_graphics_descriptor_set_values_up_to_date_ &
       (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex)) &&
      (current_textures_vertex_shader_ != vertex_shader ||
       (textures_changed & used_texture_mask_vertex) ||
       current_written_samplers_vertex_ != current_samplers_vertex_)) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if ((current_graphics_descriptor_set_values_up_to_date_ &
       (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel)) &&
      (current_textures_pixel_shader_ != pixel_shader ||
       (textures_changed & used_texture_mask_pixel) ||
       current_written_samplers_pixel_ != current_samplers_pixel_)) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  texture_cache_->ResetTextureBindingsChanged(used_texture_mask_vertex |
                                              used_texture_mask_pixel);

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex
                             : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding :
         textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding :
         *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet,
             SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  assert_not_zero(
      current_graphics_descriptor_set_values_up_to_date_ &
      (UINT32_C(1)
       << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type =
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      constants_descriptor_count.descriptorCount =
          SpirvShaderTranslator::kConstantBufferCount;
      constants_descriptor_set =
          transient_descriptor_allocator_uniform_buffer_.Allocate(
              descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(
        frame_current_, constants_descriptor_set);
    // Consecutive bindings updated via a single VkWriteDescriptorSet must have
    // identical stage flags, but for the constants they vary.
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      VkWriteDescriptorSet& write_constants =
          write_descriptor_sets[write_descriptor_set_count++];
      write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_constants.pNext = nullptr;
      write_constants.dstSet = constants_descriptor_set;
      write_constants.dstBinding = i;
      write_constants.dstArrayElement = 0;
      write_constants.descriptorCount = 1;
      write_constants.descriptorType =
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      write_constants.pImageInfo = nullptr;
      write_constants.pBufferInfo = &current_constant_buffer_infos_[i];
      write_constants.pTexelBufferView = nullptr;
    }
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetConstants] =
            constants_descriptor_set;
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
            write_textures[0].dstSet;
    // Remember what this set was written for, to reuse it in later draws.
    current_textures_vertex_shader_ = vertex_shader;
    current_written_samplers_vertex_ = current_samplers_vertex_;
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
            write_textures[0].dstSet;
    // Remember what this set was written for, to reuse it in later draws.
    current_textures_pixel_shader_ = pixel_shader;
    current_written_samplers_pixel_ = current_samplers_pixel_;
  }
  // Write.
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,
                               write_descriptor_sets.data(), 0, nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |=
      write_descriptor_set_bits;

  // Dynamic offsets are supplied when binding, so a moved suballocation needs a
  // rebind even though the set itself is unchanged.
  if (constant_buffer_offsets_changed) {
    current_graphics_descriptor_sets_bound_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
  }

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed =
      (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed &
      ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (
      xe::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        xe::tzcnt(~(descriptor_sets_remaining |
                    ((UINT32_C(1) << descriptor_set_index) - 1)));
    // The constants are the only dynamic bindings, so their offsets are passed
    // only by the command covering that set.
    bool binds_constants =
        descriptor_set_index <=
            uint32_t(SpirvShaderTranslator::kDescriptorSetConstants) &&
        uint32_t(SpirvShaderTranslator::kDescriptorSetConstants) <
            descriptor_set_mask_tzcnt;
    // TODO(Triang3l): Bind to compute for memexport emulation without vertex
    // shader memory stores.
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(),
        descriptor_set_index, descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index,
        binds_constants ? uint32_t(SpirvShaderTranslator::kConstantBufferCount)
                        : 0,
        binds_constants ? current_constant_buffer_dynamic_offsets_ : nullptr);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &=
        ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout,
    const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it = texture_transient_descriptor_sets_free_.find(
      texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(),
        texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

#define COMMAND_PROCESSOR VulkanCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
