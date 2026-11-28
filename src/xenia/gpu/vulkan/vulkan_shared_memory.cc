/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shared_memory.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(gpu_allow_invalid_upload_range);
DECLARE_bool(enable_host_buffer);
DECLARE_bool(shared_memory_zero_copy);

DEFINE_bool(vulkan_sparse_shared_memory, true,
            "Enable sparse binding for shared memory emulation. Disabling it "
            "increases video memory usage - a 512 MB buffer is created - but "
            "allows graphics debuggers that don't support sparse binding to "
            "work.",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

VulkanSharedMemory::VulkanSharedMemory(
    VulkanCommandProcessor& command_processor, Memory& memory,
    TraceWriter& trace_writer,
    VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() { Shutdown(true); }

bool VulkanSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Zero-copy: alias guest RAM as the buffer. On success the buffer is a single
  // fully-resident non-sparse allocation backed by guest RAM, so the normal
  // sparse/device-local paths below are skipped.
  if (cvars::shared_memory_zero_copy && TryInitializeZeroCopy()) {
    last_usage_ = Usage::kTransferDestination;
    last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);
    upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                  size_t(1) << page_size_log2()));
    return true;
  }

  // Zero-copy has no uploads to order.
  TrackSubmissionUsedPages();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
      VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // When a dedicated transfer queue is used for resolve readback, the buffer is
  // read from both the graphics/compute and transfer families, so it must be
  // shared concurrently between them (no compression penalty for a buffer).
  const uint32_t transfer_family = vulkan_device->queue_family_transfer();
  const uint32_t concurrent_queue_families[2] = {
      vulkan_device->queue_family_graphics_compute(), transfer_family};

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (transfer_family != UINT32_MAX) {
    buffer_create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_create_info.queueFamilyIndexCount = 2;
    buffer_create_info.pQueueFamilyIndices = concurrent_queue_families;
  } else {
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices = nullptr;
  }
  if (cvars::vulkan_sparse_shared_memory &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) ==
        VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                        &buffer_memory_requirements);
      if (xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                   vulkan_device->memory_types().device_local,
                               &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        xe::bit_scan_forward(
            std::max(uint64_t(buffer_memory_requirements.alignment),
                     uint64_t(1)),
            &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(
              std::max(allocation_size_log2,
                       std::max(kHostGpuMemoryOptimalSparseAllocationLog2,
                                kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        XELOGE(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
             kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    XELOGGPU(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan buffer",
             kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                      &buffer_memory_requirements);
    if (!xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                  vulkan_device->memory_types().device_local,
                              &buffer_memory_type_)) {
      XELOGE(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last =
        &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize =
        buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext =
          &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(
              &buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr,
                             &buffer_memory) != VK_SUCCESS) {
      XELOGE(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                size_t(1) << page_size_log2()));

  // Second (host-imported) buffer for memexport-touching draws and resolve
  // readback on the device-local path, so their output stays coherent with the
  // CPU.
  TryInitializeHostBuffer();

  return true;
}

bool VulkanSharedMemory::CreateImportedGuestRamBuffer(
    VkBuffer& out_buffer, VkDeviceMemory& out_memory) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties =
      vulkan_device->vkGetMemoryHostPointerPropertiesEXT();
  if (!vulkan_device->extensions().ext_EXT_external_memory_host ||
      !get_host_pointer_properties) {
    XELOGI(
        "Shared memory host import: VK_EXT_external_memory_host not available");
    return false;
  }

  void* const guest_ram = memory().TranslatePhysical(0);
  if (!guest_ram) {
    XELOGE("Shared memory host import: guest RAM is null");
    return false;
  }

  // The host pointer and the imported size must both be aligned to the import
  // granularity.
  VkDeviceSize import_alignment =
      vulkan_device->properties().minImportedHostPointerAlignment;
  if (import_alignment == 0) {
    import_alignment = 1;
  }
  if ((reinterpret_cast<uintptr_t>(guest_ram) % import_alignment) != 0 ||
      (VkDeviceSize(kBufferSize) % import_alignment) != 0) {
    XELOGI(
        "Shared memory host import: guest RAM 0x{:X} not aligned to import "
        "granularity {}",
        reinterpret_cast<uintptr_t>(guest_ram), import_alignment);
    return false;
  }

  // Which memory types can back this exact host pointer.
  VkMemoryHostPointerPropertiesEXT host_pointer_properties = {
      VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (get_host_pointer_properties(
          device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
          guest_ram, &host_pointer_properties) != VK_SUCCESS) {
    XELOGI(
        "Shared memory host import: vkGetMemoryHostPointerPropertiesEXT "
        "failed");
    return false;
  }

  // A plain non-sparse buffer to bind the imported memory to. It must declare
  // the external handle type it will be bound to, or vkBindBufferMemory is
  // invalid (VUID-vkBindBufferMemory-memory-02985).
  const uint32_t transfer_family = vulkan_device->queue_family_transfer();
  const uint32_t concurrent_queue_families[2] = {
      vulkan_device->queue_family_graphics_compute(), transfer_family};
  VkExternalMemoryBufferCreateInfo external_memory_buffer_create_info;
  external_memory_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external_memory_buffer_create_info.pNext = nullptr;
  external_memory_buffer_create_info.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = &external_memory_buffer_create_info;
  buffer_create_info.flags = 0;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (transfer_family != UINT32_MAX) {
    buffer_create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_create_info.queueFamilyIndexCount = 2;
    buffer_create_info.pQueueFamilyIndices = concurrent_queue_families;
  } else {
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices = nullptr;
  }
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to create the buffer");
    return false;
  }

  VkMemoryRequirements buffer_memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer,
                                    &buffer_memory_requirements);

  // The memory type must satisfy the buffer, accept the host pointer, and be
  // host-visible + host-coherent so guest and GPU observe the same bytes.
  const uint32_t memory_type_bits = buffer_memory_requirements.memoryTypeBits &
                                    host_pointer_properties.memoryTypeBits &
                                    vulkan_device->memory_types().host_visible &
                                    vulkan_device->memory_types().host_coherent;
  uint32_t memory_type;
  if (!xe::bit_scan_forward(memory_type_bits, &memory_type)) {
    XELOGI(
        "Shared memory host import: no host-visible coherent memory type "
        "accepts the guest RAM pointer");
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  VkImportMemoryHostPointerInfoEXT import_info;
  import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
  import_info.pNext = nullptr;
  import_info.handleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  import_info.pHostPointer = guest_ram;

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = &import_info;
  memory_allocate_info.allocationSize = kBufferSize;
  memory_allocate_info.memoryTypeIndex = memory_type;

  VkDeviceMemory imported_memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr,
                           &imported_memory) != VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to import {} MB of guest RAM",
           kBufferSize >> 20);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  if (dfn.vkBindBufferMemory(device, buffer, imported_memory, 0) !=
      VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to bind imported memory");
    dfn.vkFreeMemory(device, imported_memory, nullptr);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  out_buffer = buffer;
  out_memory = imported_memory;
  // The import pins guest RAM - a later mprotect on the alias would fail the
  // next submit with EFAULT.
  memory().SetPhysicalAliasSkipHostProtect(true);
  return true;
}

bool VulkanSharedMemory::TryInitializeZeroCopy() {
  VkDeviceMemory buffer_memory;
  if (!CreateImportedGuestRamBuffer(buffer_, buffer_memory)) {
    buffer_ = VK_NULL_HANDLE;
    return false;
  }
  buffer_memory_.push_back(buffer_memory);
  zero_copy_ = true;
  XELOGI("Shared memory: using zero-copy guest RAM aliasing");
  return true;
}

void VulkanSharedMemory::TryInitializeHostBuffer() {
  if (!cvars::enable_host_buffer) {
    return;
  }
  // A second, host-imported (guest RAM) buffer written by memexport-touching
  // draws and resolve readback while the main buffer stays fast device-local.
  // Non-sparse, so it can accept host memory where the sparse buffer can't.
  if (!CreateImportedGuestRamBuffer(host_buffer_, host_buffer_memory_)) {
    // Without it memexport and resolve output stays device-local, so both fall
    // back to reading back through a staging buffer.
    XELOGW(
        "Shared memory: guest RAM import failed - memexport and resolve "
        "readback fall back to staging copies");
    host_buffer_ = VK_NULL_HANDLE;
    host_buffer_memory_ = VK_NULL_HANDLE;
    return;
  }
  XELOGI("Shared memory: guest RAM host buffer ready");
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();
  zero_copy_ = false;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         host_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         host_buffer_memory_);
  // No import left to pin the alias.
  memory().SetPhysicalAliasSkipHostProtect(false);

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  SCOPE_profile_cpu_f("gpu");
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  if (early_uploads_recorded_) {
    early_uploads_recorded_ = false;
    RecordEarlyUploadBarrier(Usage::kTransferDestination,
                             submission_start_usage_);
  }
  submission_start_usage_ = last_usage_;
  upload_buffer_pool_->FlushWrites();
  ClearSubmissionUsedPages();
}

void VulkanSharedMemory::RecordEarlyUploadBarrier(Usage old_usage,
                                                  Usage new_usage) {
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkBufferMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  GetUsageMasks(old_usage, src_stage_mask, barrier.srcAccessMask);
  GetUsageMasks(new_usage, dst_stage_mask, barrier.dstAccessMask);
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer_;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;
  command_processor_.early_upload_command_buffer().CmdVkPipelineBarrier(
      src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void VulkanSharedMemory::Use(Usage usage,
                             std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second =
      std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask,
        dst_access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        false);
    // Memexport-touching draws are routed to the host-imported buffer, so their
    // writes and the reads that consume them go through host_buffer_, not
    // buffer_. Mirror the same ordering barrier onto it so those GPU
    // write->read dependencies are synchronized too.
    if (host_buffer_ != VK_NULL_HANDLE) {
      command_processor_.PushBufferMemoryBarrier(
          host_buffer_, offset, size, src_stage_mask, dst_stage_mask,
          src_access_mask, dst_access_mask, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
    }
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(),
          download_page_count << page_size_log2(),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, trace_download_buffer_,
          trace_download_buffer_memory_)) {
    XELOGE(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }

  Use(Usage::kRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  command_processor_.InsertDebugMarker(
      "Trace Download: %u KB, %zu ranges",
      download_page_count << page_size_log2() >> 10,
      trace_download_ranges().size());

  size_t download_range_count = trace_download_ranges().size();
  VkBufferCopy* download_regions = command_buffer.CmdCopyBufferEmplace(
      buffer_, trace_download_buffer_, uint32_t(download_range_count));
  VkDeviceSize download_buffer_offset = 0;
  for (size_t i = 0; i < download_range_count; ++i) {
    VkBufferCopy& download_region = download_regions[i];
    const std::pair<uint32_t, uint32_t>& download_range =
        trace_download_ranges()[i];
    download_region.srcOffset = download_range.first;
    download_region.dstOffset = download_buffer_offset;
    download_region.size = download_range.second;
    download_buffer_offset += download_range.second;
  }

  command_processor_.PushBufferMemoryBarrier(
      trace_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);

  return true;
}

void VulkanSharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_memory_) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* download_mapping;
  if (dfn.vkMapMemory(device, trace_download_buffer_memory_, 0, VK_WHOLE_SIZE,
                      0, &download_mapping) == VK_SUCCESS) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) +
              download_buffer_offset);
      download_buffer_offset += download_range.second;
    }
    dfn.vkUnmapMemory(device, trace_download_buffer_memory_);
  } else {
    XELOGE(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize =
      length_allocations << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) !=
      VK_SUCCESS) {
    XELOGE("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations
                        << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  SCOPE_profile_cpu_f("gpu");
  if (!num_upload_ranges) {
    return true;
  }

  if (zero_copy_) {
    // The buffer aliases guest RAM - nothing to copy, just mark the pages valid
    // so RequestRange stops asking to upload them.
    for (uint32_t i = 0; i < num_upload_ranges; ++i) {
      trace_writer_.WriteMemoryRead(
          upload_page_ranges[i].first << page_size_log2(),
          upload_page_ranges[i].second << page_size_log2());
      MakeRangeValid(upload_page_ranges[i].first << page_size_log2(),
                     upload_page_ranges[i].second << page_size_log2(), false);
    }
    return true;
  }

  // Ranges holding memexport output live in host_buffer_ (guest RAM) and may
  // still be being written by the GPU, so reading them with the CPU below races
  // those writes. Refresh those on the GPU and upload only what is left.
  cpu_upload_ranges_.clear();
  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    uint32_t range_base = upload_page_ranges[i].first << page_size_log2();
    uint32_t range_size = upload_page_ranges[i].second << page_size_log2();
    if (command_processor_.EnsureMemexportRangeInDeviceBuffer(
            range_base, range_size, true)) {
      MakeRangeValid(range_base, range_size, false);
      continue;
    }
    cpu_upload_ranges_.push_back(upload_page_ranges[i]);
  }
  if (cpu_upload_ranges_.empty()) {
    return true;
  }
  // Ranges refreshed from memexport output were copied in order. What is left
  // has to follow them.
  const bool early = !upload_pages_used_in_submission() &&
                     cpu_upload_ranges_.size() == num_upload_ranges;
  upload_page_ranges = cpu_upload_ranges_.data();
  num_upload_ranges = uint32_t(cpu_upload_ranges_.size());

  auto& range_front = upload_page_ranges[0];
  auto& range_back = upload_page_ranges[num_upload_ranges - 1];

  if (early) {
    if (!early_uploads_recorded_) {
      early_uploads_recorded_ = true;
      RecordEarlyUploadBarrier(submission_start_usage_,
                               Usage::kTransferDestination);
    }
  } else {
    // upload_page_ranges are sorted, use them to determine the range for the
    // ordering barrier.
    Use(Usage::kTransferDestination,
        std::make_pair(
            range_front.first << page_size_log2(),
            (range_back.first + range_back.second - range_front.first)
                << page_size_log2()));
    // Submit barriers (may end render pass) before pushing debug marker so
    // EndRenderPass is not inside the SharedMem Upload marker.
    command_processor_.SubmitBarriers(true);

    // Calculate total upload size for debug marker.
    uint32_t total_upload_bytes =
        (range_back.first + range_back.second - range_front.first)
        << page_size_log2();
    command_processor_.PushDebugMarker(
        "UploadRanges (SharedMem): 0x%08X-0x%08X (%u KB, %u ranges)",
        range_front.first << page_size_log2(),
        (range_back.first + range_back.second) << page_size_log2(),
        total_upload_bytes / 1024, num_upload_ranges);
  }
  DeferredCommandBuffer& command_buffer =
      early ? command_processor_.early_upload_command_buffer()
            : command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;

  // for (auto upload_range : upload_page_ranges) {
  for (unsigned int i = 0; i < num_upload_ranges; ++i) {
    uint32_t upload_range_start = upload_page_ranges[i].first;
    uint32_t upload_range_length = upload_page_ranges[i].second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());

    if (upload_range_length > 0 && !cvars::gpu_allow_invalid_upload_range) {
      const uint32_t range_start_addr = upload_range_start << page_size_log2();
      const uint32_t upload_range_last_page =
          upload_range_start + upload_range_length - 1;
      const uint32_t range_end_addr = upload_range_last_page
                                      << page_size_log2();

      const memory::PageAccess start_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_start_addr,
                                                       range_start_addr);
      const memory::PageAccess end_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_end_addr,
                                                       range_end_addr);
      if (start_access == xe::memory::PageAccess::kNoAccess ||
          end_access == xe::memory::PageAccess::kNoAccess) {
        XELOGE(
            "Vulkan shared memory: Invalid upload range {:08X} length {:08X}",
            upload_range_start, upload_range_length);
        successful = false;
        break;
      }
    }

    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset,
          upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        XELOGE("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(),
                     uint32_t(upload_buffer_size), false);

      if (upload_buffer_size < (1ULL << 32) && upload_buffer_size > 8192) {
        memory::vastcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            static_cast<uint32_t>(upload_buffer_size));
        swcache::WriteFence();
      } else {
        std::memcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            upload_buffer_size);
      }
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()),
                                       upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset =
          VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages =
          uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()),
                                   upload_regions_.data());
    upload_regions_.clear();
  }
  if (!early) {
    command_processor_.PopDebugMarker();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage,
                                       VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |=
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanSharedMemory::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         trace_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         trace_download_buffer_memory_);
  ReleaseTraceDownloadRanges();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
