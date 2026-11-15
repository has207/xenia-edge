/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/null_device.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/null_entry.h"

namespace xe {
namespace vfs {

NullDevice::NullDevice(const std::string& mount_path,
                       const std::initializer_list<std::string>& null_paths)
    : Device(mount_path), name_("NullDevice"), null_paths_(null_paths) {}

NullDevice::~NullDevice() = default;

bool NullDevice::Initialize() {
  auto root_entry = new NullEntry(this, nullptr, mount_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  for (auto path : null_paths_) {
    auto child = NullEntry::Create(this, root_entry, path);
    root_entry->children_.push_back(std::unique_ptr<Entry>(child));
  }
  return true;
}

void NullDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* NullDevice::ResolvePath(const std::string_view path) {
  XELOGFS("NullDevice::ResolvePath({})", path);

  auto root = root_entry_.get();
  if (path.empty()) {
    return root_entry_.get();
  }

  for (auto& child : root->children()) {
    if (!xe_strcasecmp(child->path().c_str(), path.data())) {
      return child.get();
    }
  }

  return nullptr;
}

// Which cache partition an entry is, -1 for anything else.
static int CachePartitionIndex(const Entry* entry) {
  if (xe::utf8::equal_case(entry->name(), "Cache0")) {
    return 0;
  }
  if (xe::utf8::equal_case(entry->name(), "Cache1")) {
    return 1;
  }
  return -1;
}

void NullDevice::LoadCacheHeader(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  cache_header_path_ = path;
  cache_header_ = {};
  const auto bytes = xe::filesystem::ReadAllBytes(path);
  if (bytes.size() == sizeof(cache_header_)) {
    std::memcpy(&cache_header_, bytes.data(), sizeof(cache_header_));
  }
  for (auto& child : root_entry_->children()) {
    const int index = CachePartitionIndex(child.get());
    if (index >= 0 &&
        IsMountableCluster(cache_header_.sectors_per_cluster[index])) {
      static_cast<NullEntry*>(child.get())
          ->set_sectors_per_allocation_unit(
              cache_header_.sectors_per_cluster[index]);
    }
  }
}

void NullDevice::set_cache_format_handler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  cache_format_handler_ = std::move(handler);
}

bool NullDevice::ReadCacheTable(const Entry* entry, std::span<uint8_t> buffer,
                                size_t byte_offset) {
  if (!xe::utf8::equal_case(entry->name(), "Partition0")) {
    return false;
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  const size_t begin = std::max(byte_offset, kCacheTableOffset);
  const size_t end = std::min(byte_offset + buffer.size(),
                              kCacheTableOffset + kCacheTableSize);
  if (cache_header_path_.empty() || begin >= end) {
    return false;
  }
  std::fill(buffer.begin(), buffer.end(), uint8_t(0));
  std::memcpy(buffer.data() + (begin - byte_offset),
              cache_header_.table.data() + (begin - kCacheTableOffset),
              end - begin);
  return true;
}

void NullDevice::WriteCacheTable(const Entry* entry,
                                 std::span<const uint8_t> buffer,
                                 size_t byte_offset) {
  if (!xe::utf8::equal_case(entry->name(), "Partition0")) {
    return;
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  const size_t begin = std::max(byte_offset, kCacheTableOffset);
  const size_t end = std::min(byte_offset + buffer.size(),
                              kCacheTableOffset + kCacheTableSize);
  if (cache_header_path_.empty() || begin >= end) {
    return;
  }
  std::memcpy(cache_header_.table.data() + (begin - kCacheTableOffset),
              buffer.data() + (begin - byte_offset), end - begin);
  SaveCacheHeader();
}

bool NullDevice::IsMountableCluster(uint32_t sectors_per_cluster) const {
  return xe::is_pow2(sectors_per_cluster) && sectors_per_cluster <= 0x80 &&
         uint64_t(sectors_per_cluster) * bytes_per_sector() >= 0x1000;
}

void NullDevice::FormatCachePartition(const Entry* entry,
                                      uint32_t sectors_per_cluster) {
  const int index = CachePartitionIndex(entry);
  if (index < 0) {
    return;
  }
  std::function<void()> handler;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (cache_header_path_.empty()) {
      return;
    }
    XELOGI("The title formatted {} with {} sectors per cluster", entry->name(),
           sectors_per_cluster);
    if (sectors_per_cluster) {
      cache_header_.sectors_per_cluster[index] = sectors_per_cluster;
      SaveCacheHeader();
    }
    handler = cache_format_handler_;
  }
  // The handler empties the volume through the file system, which takes its
  // own lock.
  if (handler) {
    handler();
  }
}

void NullDevice::SaveCacheHeader() {
  std::error_code ec;
  std::filesystem::create_directories(cache_header_path_.parent_path(), ec);
  if (!xe::filesystem::WriteAllBytes(
          cache_header_path_, {reinterpret_cast<const uint8_t*>(&cache_header_),
                               sizeof(cache_header_)})) {
    XELOGE("Unable to write the cache header {}",
           xe::strip_pii(xe::path_to_utf8(cache_header_path_)));
  }
}

}  // namespace vfs
}  // namespace xe
