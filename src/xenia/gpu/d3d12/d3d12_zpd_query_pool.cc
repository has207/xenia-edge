/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_zpd_query_pool.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/deferred_command_list.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace xe {
namespace gpu {
namespace d3d12 {
namespace {

struct ResolveRange {
  uint32_t start;
  uint32_t count;
};
}  // namespace

bool D3D12ZPDQueryPool::EnsureInitialized(
    const ui::d3d12::D3D12Provider& provider, uint32_t requested_capacity,
    bool can_recreate) {
  if (is_initialized() && (capacity_ == requested_capacity || !can_recreate)) {
    return true;
  }

  // Can't recreate while resolves are in-flight, that would destroy the heap
  // under a live ResolveQueryData call.
  assert_true(!is_initialized() || !has_pending_resolve_batch());
  Shutdown();

  ID3D12Device* device = provider.GetDevice();

  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = requested_capacity;
  heap_desc.NodeMask = 0;

  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&query_heap_)))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to create the ZPD query "
        "heap, falling back to fake sample counts.");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc,
                                          sizeof(uint64_t) * requested_capacity,
                                          D3D12_RESOURCE_FLAG_NONE);

  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback_buffer_)))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to allocate the ZPD query "
        "readback buffer, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  D3D12_RANGE read_range = {};
  read_range.Begin = 0;
  read_range.End = sizeof(uint64_t) * requested_capacity;

  void* mapping = nullptr;
  if (FAILED(readback_buffer_->Map(0, &read_range, &mapping))) {
    XELOGW(
        "D3D12ZPDQueryPool: Failed to map the ZPD query "
        "readback buffer, falling back to fake sample counts.");
    Shutdown();
    return false;
  }

  readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  capacity_ = requested_capacity;

  resolve_batch_pending_.assign(requested_capacity, 0);
  resolve_batch_index_count_ = 0;

  free_indices_.clear();
  free_indices_.reserve(requested_capacity);
  for (uint32_t i = requested_capacity; i > 0; --i) {
    free_indices_.push_back(i - 1);
  }
  index_generations_.assign(requested_capacity, 0);

  return true;
}

void D3D12ZPDQueryPool::Shutdown() {
  resolve_batch_pending_.clear();
  resolve_batch_index_count_ = 0;
  free_indices_.clear();
  index_generations_.clear();

  capacity_ = 0;

  if (readback_mapping_ && readback_buffer_) {
    readback_buffer_->Unmap(0, nullptr);
  }
  readback_mapping_ = nullptr;

  readback_buffer_.Reset();
  query_heap_.Reset();
}

bool D3D12ZPDQueryPool::AcquireQueryIndex(uint32_t& query_index,
                                          uint32_t& query_generation) {
  if (free_indices_.empty()) {
    query_index = UINT32_MAX;
    query_generation = 0;
    return false;
  }

  query_index = free_indices_.back();
  free_indices_.pop_back();

  assert_true(query_index < index_generations_.size());
  // Bump the generation. Any in-flight readbacks for the slot's previous
  // occupants are ignored.
  query_generation = ++index_generations_[query_index];
  return true;
}

void D3D12ZPDQueryPool::ReleaseQueryIndex(uint32_t query_index,
                                          uint32_t query_generation) {
  if (query_index >= capacity_) {
    return;
  }

  if (!GenerationMatches(query_index, query_generation)) {
    XELOGW("D3D12ZPDQueryPool: stale release index={} gen={}", query_index,
           query_generation);
    return;
  }

  free_indices_.push_back(query_index);
}

bool D3D12ZPDQueryPool::GenerationMatches(uint32_t query_index,
                                          uint32_t query_generation) const {
  return query_index < index_generations_.size() &&
         index_generations_[query_index] == query_generation;
}

void D3D12ZPDQueryPool::BeginQuery(DeferredCommandList& deferred_command_list,
                                   uint32_t query_index) const {
  if (!query_heap_ || query_index >= capacity_) {
    return;
  }

  deferred_command_list.D3DBeginQuery(query_heap_.Get(),
                                      D3D12_QUERY_TYPE_OCCLUSION, query_index);
}

void D3D12ZPDQueryPool::EndQuery(DeferredCommandList& deferred_command_list,
                                 uint32_t query_index) const {
  if (!query_heap_ || query_index >= capacity_) {
    return;
  }

  deferred_command_list.D3DEndQuery(query_heap_.Get(),
                                    D3D12_QUERY_TYPE_OCCLUSION, query_index);
}

void D3D12ZPDQueryPool::QueueQueryResolve(uint32_t query_index) {
  if (query_index >= capacity_) {
    return;
  }

  // Guard against duplicates. Split paths can touch the same index twice before
  // the batch drains at EndSubmission.
  if (!resolve_batch_pending_[query_index]) {
    resolve_batch_pending_[query_index] = 1;
    ++resolve_batch_index_count_;
  }
}

void D3D12ZPDQueryPool::FlushResolveBatch(
    DeferredCommandList& deferred_command_list, bool submission_open) {
  if (!submission_open) {
    return;
  }

  if (!resolve_batch_index_count_) {
    return;
  }

  if (!is_initialized()) {
    std::fill(resolve_batch_pending_.begin(), resolve_batch_pending_.end(), 0);
    resolve_batch_index_count_ = 0;
    return;
  }

  std::vector<ResolveRange> ranges;

  // Coalesce into contiguous ranges to cut down on ResolveQueryData calls,
  // which have considerable overhead.
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

  for (const ResolveRange& range : ranges) {
    deferred_command_list.D3DResolveQueryData(
        query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, range.start, range.count,
        readback_buffer_.Get(), range.start * sizeof(uint64_t));
  }
}

uint64_t D3D12ZPDQueryPool::GetQueryReadbackValue(uint32_t query_index) const {
  if (!readback_mapping_ || query_index >= capacity_) {
    return 0;
  }

  return readback_mapping_[query_index];
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
