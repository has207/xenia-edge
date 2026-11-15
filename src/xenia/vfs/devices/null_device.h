/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_NULL_DEVICE_H_
#define XENIA_VFS_DEVICES_NULL_DEVICE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class NullEntry;

class NullDevice : public Device {
 public:
  NullDevice(const std::string& mount_path,
             const std::initializer_list<std::string>& null_paths);
  ~NullDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  // Keeps the cache table the title writes to Partition0 and the cluster size
  // of each cache partition it formats in the file at |path|.
  void LoadCacheHeader(const std::filesystem::path& path);
  // Runs when the title formats a cache partition.
  void set_cache_format_handler(std::function<void()> handler);

  // Fills a read of Partition0 that reaches the cache table, false for any
  // other read.
  bool ReadCacheTable(const Entry* entry, std::span<uint8_t> buffer,
                      size_t byte_offset);
  void WriteCacheTable(const Entry* entry, std::span<const uint8_t> buffer,
                       size_t byte_offset);
  // FATX mounts only clusters of a power of two sectors and at least 0x1000
  // bytes.
  bool IsMountableCluster(uint32_t sectors_per_cluster) const;
  // |sectors_per_cluster| is 0 for a FATX header that can't be mounted.
  void FormatCachePartition(const Entry* entry, uint32_t sectors_per_cluster);

  bool is_read_only() const override { return false; }

  bool supports_concurrent_io() const override { return true; }

  // Raw partitions, served by the disk driver.
  bool queues_all_requests() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }

  uint32_t total_allocation_units() const override { return 0x10; }
  uint32_t available_allocation_units() const override { return 0x10; }

  // STFC/cache code seems to require the product of the next two to equal
  // 0x10000
  uint32_t sectors_per_allocation_unit() const override { return 0x80; }

  // STFC requires <= 0x1000
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  static constexpr size_t kCacheTableOffset = 0x800;
  static constexpr size_t kCacheTableSize = 0x400;

  // The layout of the header file.
  struct CacheHeader {
    std::array<uint8_t, kCacheTableSize> table;
    std::array<uint32_t, 2> sectors_per_cluster;
  };
  static_assert(sizeof(CacheHeader) == kCacheTableSize + 8);

  // Called with cache_mutex_ held.
  void SaveCacheHeader();

  std::string name_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<std::string> null_paths_;

  std::mutex cache_mutex_;
  std::filesystem::path cache_header_path_;
  CacheHeader cache_header_ = {};
  std::function<void()> cache_format_handler_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_NULL_DEVICE_H_
