/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_
#define XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_

#include <memory>
#include <mutex>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/devices/xcontent_container_entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {
// One handle per data fragment, read at an explicit offset. No shared cursor,
// so two of this device's files can be read at once. Each opens on its first
// read, as walking the directories touches only a few fragments.
class MultiFileHandles {
 public:
  void Add(std::filesystem::path path);
  // Null when the fragment does not open.
  xe::filesystem::FileHandle* Get(size_t index);

 private:
  struct Fragment {
    std::filesystem::path path;
    std::unique_ptr<xe::filesystem::FileHandle> handle;
  };
  std::mutex mutex_;
  std::vector<Fragment> fragments_;
};

class XContentContainerDevice;

class SvodContainerEntry : public XContentContainerEntry {
 public:
  SvodContainerEntry(Device* device, Entry* parent, const std::string_view path,
                     MultiFileHandles* files);
  ~SvodContainerEntry() override;

  static std::unique_ptr<SvodContainerEntry> Create(Device* device,
                                                    Entry* parent,
                                                    const std::string_view name,
                                                    MultiFileHandles* files);

  MultiFileHandles* files() const { return files_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

 private:
  bool DeleteEntryInternal(Entry* entry) override;

  MultiFileHandles* files_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_
