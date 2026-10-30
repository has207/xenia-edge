/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_NULL_ENTRY_H_
#define XENIA_VFS_DEVICES_NULL_ENTRY_H_

#include <atomic>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/entry.h"

namespace xe {
namespace vfs {

class NullDevice;

class NullEntry : public Entry {
 public:
  NullEntry(Device* device, Entry* parent, std::string path);
  ~NullEntry() override;

  static NullEntry* Create(Device* device, Entry* parent,
                           const std::string& path);

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  bool can_map() const override { return false; }

  // A raw partition takes the cluster size of the FATX header last written to
  // its start, once there is one.
  uint32_t sectors_per_allocation_unit() const override;
  void set_sectors_per_allocation_unit(uint32_t sectors) {
    sectors_per_allocation_unit_ = sectors;
  }

 private:
  friend class NullDevice;

  bool DeleteEntryInternal(Entry* entry) override;

  std::atomic<uint32_t> sectors_per_allocation_unit_ = 0;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_NULL_ENTRY_H_
