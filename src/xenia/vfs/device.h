/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICE_H_
#define XENIA_VFS_DEVICE_H_

#include <memory>
#include <mutex>
#include <string>

#include "xenia/base/mutex.h"
#include "xenia/base/string_buffer.h"
#include "xenia/vfs/entry.h"

namespace xe {
namespace vfs {

// Models the time storage needs to service a request, since a completion that
// lands sooner than hardware could deliver it breaks some titles' loaders.
// A request costs its transfer, plus a seek unless it continues the last one.
class DriveTiming {
 public:
  // Enables the model, which is inert on a device that never calls this.
  void Configure();

  // Returns the host uptime millisecond it completes at, or 0 when untimed.
  uint64_t Reserve(const Entry* entry, uint64_t offset, size_t length);

 private:
  bool enabled_ = false;
  std::mutex lock_;
  // When the medium frees up, fractional so short transfers accumulate.
  double free_at_ms_ = 0.0;
  // Where the previous request ended.
  const Entry* head_entry_ = nullptr;
  uint64_t head_offset_ = 0;
};

class Device {
 public:
  explicit Device(const std::string_view mount_path);
  virtual ~Device();

  virtual bool Initialize() = 0;

  const std::string& mount_path() const { return mount_path_; }
  virtual bool is_read_only() const { return true; }

  // True when two of this device's files can be read at the same time. False
  // for a shared file cursor, or a reader that serializes internally.
  virtual bool supports_concurrent_io() const { return false; }

  // True when the file system queues every request, which then always pends on
  // an async handle. FATX and GDFX complete a buffered transfer inline.
  virtual bool queues_all_requests() const { return false; }

  // True when the files come from a disc's GDFX file system.
  virtual bool is_gdfx() const { return name() == "GDFX"; }

  virtual void Dump(StringBuffer* string_buffer) = 0;
  virtual Entry* ResolvePath(const std::string_view path) = 0;

  virtual const std::string& name() const = 0;
  virtual uint32_t attributes() const = 0;
  virtual uint32_t component_name_max_length() const = 0;

  virtual uint32_t total_allocation_units() const = 0;
  virtual uint32_t available_allocation_units() const = 0;
  virtual uint32_t sectors_per_allocation_unit() const = 0;
  virtual uint32_t bytes_per_sector() const = 0;

  // Request timing for this device, configured where the device is mounted.
  DriveTiming& drive_timing() { return drive_timing_; }

 protected:
  xe::global_critical_region global_critical_region_;
  std::string mount_path_;
  DriveTiming drive_timing_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICE_H_
