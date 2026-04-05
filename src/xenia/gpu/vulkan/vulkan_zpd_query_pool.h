/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_
#define XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_

#include <cstdint>
#include <vector>

#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}
}  // namespace ui

namespace gpu {
namespace vulkan {

class DeferredCommandBuffer;

// Vulkan occlusion query pool for ZPD reports. Queries live in VkQueryPool,
// results are copied to a persistent buffer via vkCmdCopyQueryPoolResults.
// vkCmdBeginQuery is only valid inside a render pass, queries get deferred
// when no pass is open and segments split at pass boundaries.
// Requires VK_EXT_host_query_reset (1.2 core) so slots can be reset on the
// CPU at release time, no paired vkCmdEndQuery needed, and also allows
// DiscardHostZPDQuery work outside a pass.
//
// VK_QUERY_RESULT_WAIT_BIT in the copy removes the need for a separate
// availability check. Transfer barrier before InvalidateReadback covers non-
// coherent memory.
//
// Per-slot generation counter has same purpose as D3D12 pool.
class VulkanZPDQueryPool {
 public:
  VulkanZPDQueryPool() = default;
  VulkanZPDQueryPool(const VulkanZPDQueryPool&) = delete;
  VulkanZPDQueryPool& operator=(const VulkanZPDQueryPool&) = delete;
  ~VulkanZPDQueryPool() { Shutdown(); }

  bool EnsureInitialized(const ui::vulkan::VulkanDevice* vulkan_device,
                         uint32_t requested_capacity, bool can_recreate);
  void Shutdown();

  bool is_initialized() const {
    return query_pool_ != VK_NULL_HANDLE &&
           readback_buffer_ != VK_NULL_HANDLE && readback_mapping_ != nullptr &&
           capacity_ != 0;
  }

  uint32_t capacity() const { return capacity_; }

  bool has_pending_resolve_batch() const {
    return resolve_batch_index_count_ != 0;
  }

  bool has_free_indices() const { return !free_indices_.empty(); }

  bool AcquireQueryIndex(uint32_t& query_index, uint32_t& query_generation);
  void ReleaseQueryIndex(uint32_t query_index, uint32_t query_generation);
  bool GenerationMatches(uint32_t query_index, uint32_t query_generation) const;

  void BeginQuery(DeferredCommandBuffer& deferred_command_buffer,
                  uint32_t query_index) const;
  void EndQuery(DeferredCommandBuffer& deferred_command_buffer,
                uint32_t query_index) const;
  void QueueQueryResolve(uint32_t query_index);
  void RecordResolveBatch(VkCommandBuffer command_buffer);

  void InvalidateReadback();

  uint64_t GetQueryReadbackValue(uint32_t query_index) const;

 private:
  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  VkQueryPool query_pool_ = VK_NULL_HANDLE;

  VkBuffer readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
  uint64_t* readback_mapping_ = nullptr;
  // If not HOST_COHERENT, call InvalidateReadback before reading.
  bool readback_is_coherent_ = true;

  uint32_t capacity_ = 0;
  std::vector<uint32_t> free_indices_;

  // Bumped on each acquire so stale copies from recycled slots get dropped.
  std::vector<uint32_t> index_generations_;

  std::vector<uint8_t> resolve_batch_pending_;
  uint32_t resolve_batch_index_count_ = 0;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_ZPD_QUERY_POOL_H_
