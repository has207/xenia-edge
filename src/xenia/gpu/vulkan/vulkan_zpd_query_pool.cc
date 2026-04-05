/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {
namespace {

struct ResolveRange {
  uint32_t start;
  uint32_t count;
};
}  // namespace

bool VulkanZPDQueryPool::EnsureInitialized(
    const ui::vulkan::VulkanDevice* vulkan_device, uint32_t requested_capacity,
    bool can_recreate) {
  vulkan_device_ = vulkan_device;
  if (!vulkan_device_) {
    return false;
  }

  if (is_initialized() && (capacity_ == requested_capacity || !can_recreate)) {
    return true;
  }

  Shutdown();

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Need VK_EXT_host_query_reset (1.2 core) to reset slots on the CPU without
  // a paired vkCmdEndQuery. May be unavailable on older drivers or devices.
  if (!vulkan_device_->properties().hostQueryReset ||
      dfn.vkResetQueryPool == nullptr) {
    return false;
  }

  VkQueryPoolCreateInfo pool_info;
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.pNext = nullptr;
  pool_info.flags = 0;
  pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
  pool_info.queryCount = requested_capacity;
  pool_info.pipelineStatistics = 0;
  if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &query_pool_) !=
      VK_SUCCESS) {
    XELOGW(
        "VulkanZPDQueryPool: Failed to create the ZPD query "
        "pool, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  VkBufferCreateInfo readback_buffer_info;
  readback_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  readback_buffer_info.pNext = nullptr;
  readback_buffer_info.flags = 0;
  readback_buffer_info.size = sizeof(uint64_t) * requested_capacity;
  readback_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  readback_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  readback_buffer_info.queueFamilyIndexCount = 0;
  readback_buffer_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &readback_buffer_info, nullptr,
                         &readback_buffer_) != VK_SUCCESS) {
    XELOGW(
        "VulkanZPDQueryPool: Failed to create the ZPD query "
        "readback buffer, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  VkMemoryRequirements readback_mem_reqs;
  dfn.vkGetBufferMemoryRequirements(device, readback_buffer_,
                                    &readback_mem_reqs);

  VkMemoryAllocateInfo readback_alloc_info;
  readback_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  readback_alloc_info.pNext = nullptr;
  readback_alloc_info.allocationSize = readback_mem_reqs.size;
  readback_alloc_info.memoryTypeIndex = ui::vulkan::util::ChooseMemoryType(
      vulkan_device_->memory_types(), readback_mem_reqs.memoryTypeBits,
      ui::vulkan::util::MemoryPurpose::kReadback);
  if (readback_alloc_info.memoryTypeIndex == UINT32_MAX ||
      dfn.vkAllocateMemory(device, &readback_alloc_info, nullptr,
                           &readback_memory_) != VK_SUCCESS) {
    XELOGW(
        "VulkanZPDQueryPool: Failed to allocate ZPD query "
        "readback memory, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  readback_is_coherent_ = (vulkan_device_->memory_types().host_coherent &
                           (1u << readback_alloc_info.memoryTypeIndex)) != 0;

  if (dfn.vkBindBufferMemory(device, readback_buffer_, readback_memory_, 0) !=
      VK_SUCCESS) {
    XELOGW(
        "VulkanZPDQueryPool: Failed to bind ZPD query readback "
        "buffer memory, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapping) != VK_SUCCESS) {
    XELOGW(
        "VulkanZPDQueryPool: Failed to map ZPD query readback "
        "memory, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  capacity_ = requested_capacity;

  dfn.vkResetQueryPool(device, query_pool_, 0, requested_capacity);

  free_indices_.clear();
  free_indices_.reserve(requested_capacity);
  for (uint32_t i = 0; i < requested_capacity; ++i) {
    free_indices_.push_back(requested_capacity - 1 - i);
  }
  index_generations_.assign(requested_capacity, 0);

  resolve_batch_pending_.assign(requested_capacity, 0);
  resolve_batch_index_count_ = 0;

  return true;
}

void VulkanZPDQueryPool::Shutdown() {
  if (!vulkan_device_) {
    query_pool_ = VK_NULL_HANDLE;
    readback_buffer_ = VK_NULL_HANDLE;
    readback_memory_ = VK_NULL_HANDLE;
    readback_mapping_ = nullptr;
    readback_is_coherent_ = true;
    capacity_ = 0;
    free_indices_.clear();
    index_generations_.clear();
    resolve_batch_pending_.clear();
    resolve_batch_index_count_ = 0;
    return;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  free_indices_.clear();
  index_generations_.clear();
  resolve_batch_pending_.clear();
  resolve_batch_index_count_ = 0;

  capacity_ = 0;
  readback_is_coherent_ = true;

  if (readback_mapping_ && readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, readback_memory_);
  }
  readback_mapping_ = nullptr;

  if (readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, readback_buffer_, nullptr);
  }
  readback_buffer_ = VK_NULL_HANDLE;

  if (readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, readback_memory_, nullptr);
  }
  readback_memory_ = VK_NULL_HANDLE;

  if (query_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, query_pool_, nullptr);
  }
  query_pool_ = VK_NULL_HANDLE;
}

bool VulkanZPDQueryPool::AcquireQueryIndex(uint32_t& query_index,
                                           uint32_t& query_generation) {
  if (free_indices_.empty()) {
    query_index = UINT32_MAX;
    query_generation = 0;
    return false;
  }

  query_index = free_indices_.back();
  free_indices_.pop_back();

  assert_true(query_index < index_generations_.size());
  // Bump before returning - invalidates in-flight copies from prior occupant.
  query_generation = ++index_generations_[query_index];
  return true;
}

void VulkanZPDQueryPool::ReleaseQueryIndex(uint32_t query_index,
                                           uint32_t query_generation) {
  if (!vulkan_device_ || query_index >= capacity_) {
    return;
  }

  if (!GenerationMatches(query_index, query_generation)) {
    XELOGW("VulkanZPDQueryPool: stale release index={} gen={}", query_index,
           query_generation);
    return;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Immediately reset the slot on the CPU so it's ready for the next
  // AcquireQueryIndex without requiring a paired EndQuery.
  dfn.vkResetQueryPool(device, query_pool_, query_index, 1);
  free_indices_.push_back(query_index);
}

bool VulkanZPDQueryPool::GenerationMatches(uint32_t query_index,
                                           uint32_t query_generation) const {
  return query_index < index_generations_.size() &&
         index_generations_[query_index] == query_generation;
}

void VulkanZPDQueryPool::BeginQuery(
    DeferredCommandBuffer& deferred_command_buffer,
    uint32_t query_index) const {
  if (query_pool_ == VK_NULL_HANDLE || query_index >= capacity_) {
    return;
  }

  // Precise bit is crucial. Most titles tested actually care about the sample
  // counts, not just 0 vs non-zero.
  deferred_command_buffer.CmdVkBeginQuery(query_pool_, query_index,
                                          VK_QUERY_CONTROL_PRECISE_BIT);
}

void VulkanZPDQueryPool::EndQuery(
    DeferredCommandBuffer& deferred_command_buffer,
    uint32_t query_index) const {
  if (query_pool_ == VK_NULL_HANDLE || query_index >= capacity_) {
    return;
  }

  deferred_command_buffer.CmdVkEndQuery(query_pool_, query_index);
}

void VulkanZPDQueryPool::QueueQueryResolve(uint32_t query_index) {
  if (query_index >= capacity_) {
    return;
  }

  // Guard against duplicates within the same submission.
  if (!resolve_batch_pending_[query_index]) {
    resolve_batch_pending_[query_index] = 1;
    ++resolve_batch_index_count_;
  }
}

void VulkanZPDQueryPool::RecordResolveBatch(VkCommandBuffer command_buffer) {
  if (!resolve_batch_index_count_) {
    return;
  }

  if (!is_initialized()) {
    std::fill(resolve_batch_pending_.begin(), resolve_batch_pending_.end(), 0);
    resolve_batch_index_count_ = 0;
    return;
  }

  std::vector<ResolveRange> ranges;

  // Coalesce into contiguous ranges - minimize vkCmdCopyQueryPoolResults calls.
  uint32_t range_start = 0;
  uint32_t range_count = 0;
  for (uint32_t index = 0; index < capacity_; ++index) {
    if (!resolve_batch_pending_[index]) {
      continue;
    }

    if (range_count == 0) {
      range_start = index;
      range_count = 1;
      continue;
    }

    if (index == range_start + range_count) {
      ++range_count;
      continue;
    }

    ranges.push_back({range_start, range_count});
    range_start = index;
    range_count = 1;
  }

  if (range_count != 0) {
    ranges.push_back({range_start, range_count});
  }

  // Reset the batch. ENDs from later in this submission belong to the next.
  std::fill(resolve_batch_pending_.begin(), resolve_batch_pending_.end(), 0);
  resolve_batch_index_count_ = 0;

  if (ranges.empty()) {
    return;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();

  VkDeviceSize barrier_offset = VK_WHOLE_SIZE;
  VkDeviceSize barrier_end = 0;
  for (const ResolveRange& range : ranges) {
    if (range.start >= capacity_) {
      continue;
    }

    uint32_t count = std::min(range.count, capacity_ - range.start);
    VkDeviceSize offset =
        static_cast<VkDeviceSize>(range.start) * sizeof(uint64_t);
    VkDeviceSize size = static_cast<VkDeviceSize>(count) * sizeof(uint64_t);

    // WAIT_BIT blocks until available. No separate availability check needed.
    dfn.vkCmdCopyQueryPoolResults(
        command_buffer, query_pool_, range.start, count, readback_buffer_,
        offset, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    barrier_offset = std::min(barrier_offset, offset);
    barrier_end = std::max(barrier_end, offset + size);
  }

  if (barrier_offset == VK_WHOLE_SIZE || barrier_end <= barrier_offset) {
    return;
  }

  VkBufferMemoryBarrier readback_barrier;
  readback_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  readback_barrier.pNext = nullptr;
  readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  readback_barrier.buffer = readback_buffer_;
  readback_barrier.offset = barrier_offset;
  readback_barrier.size = barrier_end - barrier_offset;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                           &readback_barrier, 0, nullptr);
}

void VulkanZPDQueryPool::InvalidateReadback() {
  if (readback_is_coherent_ || !vulkan_device_ ||
      readback_memory_ == VK_NULL_HANDLE || !readback_mapping_) {
    return;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Flushes the CPU-side cache so the persistent mapping reflects the GPU
  // writes made by vkCmdCopyQueryPoolResults. Not needed on HOST_COHERENT.
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = readback_memory_;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
}

uint64_t VulkanZPDQueryPool::GetQueryReadbackValue(uint32_t query_index) const {
  if (!readback_mapping_ || query_index >= capacity_) {
    return 0;
  }

  return readback_mapping_[query_index];
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
