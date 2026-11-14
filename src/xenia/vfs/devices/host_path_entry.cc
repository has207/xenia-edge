/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_entry.h"

#include <algorithm>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_file.h"

namespace xe {
namespace vfs {

HostPathEntry::HostPathEntry(Device* device, Entry* parent,
                             const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() = default;

bool HostPathEntry::has_open_files() const {
  auto global_lock = global_critical_region_.Acquire();
  // Deleting a directory frees its children, so their handles count too.
  return open_files_.load() != 0 ||
         std::ranges::any_of(children_, [](const auto& child) {
           return child->has_open_files();
         });
}

void HostPathEntry::ReleaseFile() {
  auto global_lock = global_critical_region_.Acquire();
  if (open_files_.fetch_sub(1) != 1) {
    return;
  }
  if (delete_on_close() && parent()) {
    Delete();
  } else if (overwritten_) {
    // The old bytes past the size go with the last handle. A host file
    // already that size is left alone so its times stay.
    overwritten_ = false;
    auto file_info = xe::filesystem::GetInfo(host_path_);
    if (file_info && file_info->total_size != size_) {
      std::error_code ec;
      std::filesystem::resize_file(host_path_, size_, ec);
      if (ec) {
        XELOGE("{}: Cannot trim {}: {}", __func__,
               xe::strip_pii(xe::path_to_utf8(host_path_)), ec.message());
      }
    }
  }
}

bool HostPathEntry::Overwrite() {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only() || (attributes_ & kFileAttributeDirectory) ||
      reported_size_fixed_ || has_open_files()) {
    return false;
  }
  // FATX frees the clusters without clearing them and hands the same ones back
  // when the file grows again.
  overwritten_ = true;
  size_ = 0;
  allocation_size_ = 0;
  return true;
}

void HostPathEntry::SetOverwrittenSize(size_t size) {
  size_ = size;
  allocation_size_ = xe::round_up(size, device()->bytes_per_sector());
}

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     xe::filesystem::FileInfo file_info) {
  auto path = xe::utf8::join_guest_paths(parent->path(),
                                         xe::path_to_utf8(file_info.name));
  auto entry = new HostPathEntry(device, parent, path, full_path);
  entry->SetInfo(file_info);
  return entry;
}

void HostPathEntry::SetInfo(const xe::filesystem::FileInfo& file_info) {
  create_timestamp_ = file_info.create_timestamp;
  access_timestamp_ = file_info.access_timestamp;
  write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
    attributes_ = kFileAttributeDirectory;
  } else {
    attributes_ = kFileAttributeNormal;
    if (device_->is_read_only()) {
      attributes_ |= kFileAttributeReadOnly;
    }
    size_ = file_info.total_size;
    allocation_size_ =
        xe::round_up(file_info.total_size, device_->bytes_per_sector());
  }
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  if (is_read_only() && (desired_access & (FileAccess::kFileWriteData |
                                           FileAccess::kFileAppendData))) {
    XELOGE("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }
  // FATX sets times through any handle, even one opened only to read.
  uint32_t host_access = desired_access & ~FileAccess::kFileWriteAttributes;
  if (!is_read_only()) {
    host_access |= FileAccess::kFileWriteAttributes;
  }
  auto file_handle =
      xe::filesystem::FileHandle::OpenExisting(host_path_, host_access);
  if (!file_handle) {
    // TODO(benvanik): pick correct response.
    return X_STATUS_NO_SUCH_FILE;
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

std::unique_ptr<MappedMemory> HostPathEntry::OpenMapped(MappedMemory::Mode mode,
                                                        size_t offset,
                                                        size_t length) {
  return MappedMemory::Open(host_path_, mode, offset, length);
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(
    const std::string_view name, uint32_t attributes) {
  auto full_path = host_path_ / xe::to_path(name);
  if (attributes & kFileAttributeDirectory) {
    if (!std::filesystem::create_directories(full_path)) {
      return nullptr;
    }
  } else {
    auto file = xe::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
  auto file_info = xe::filesystem::GetInfo(full_path);
  if (!file_info) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(
      HostPathEntry::Create(device_, this, full_path, file_info.value()));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  auto full_path = host_path_ / xe::to_path(entry->name());
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Skip directories, they we're handled above.
    if (std::filesystem::is_directory(full_path)) {
      return false;
    }

    if (std::filesystem::exists(full_path)) {
      const auto result = std::filesystem::remove(full_path, ec);
      if (ec) {
        XELOGE("{}: Cannot remove file entry. File: {} Error: {}", __func__,
               xe::strip_pii(xe::path_to_utf8(full_path)), ec.message());
        return false;
      }
    }
    return true;
  }
}

bool HostPathEntry::RenameEntryInternal(Entry* new_parent,
                                        const std::string_view name) {
  const std::filesystem::path new_host_path =
      static_cast<HostPathEntry*>(new_parent)->host_path_ / xe::to_path(name);

  std::error_code ec;
  std::filesystem::rename(host_path_, new_host_path, ec);
  if (ec) {
    XELOGE("RenameEntryInternal: Failed to rename '{}' to '{}': {}",
           xe::strip_pii(xe::path_to_utf8(host_path_)),
           xe::strip_pii(xe::path_to_utf8(new_host_path)), ec.message());
    return false;
  }
  host_path_ = new_host_path;
  return true;
}

void HostPathEntry::update() {
  auto file_info = xe::filesystem::GetInfo(host_path_);
  if (!file_info) {
    return;
  }
  // Creation time never changes, and posix reports st_ctime here, which
  // moves on every write.
  access_timestamp_ = file_info->access_timestamp;
  write_timestamp_ = file_info->write_timestamp;
  // A synthesized size, such as a collapsed package container, must stand, as
  // must an overwritten file's.
  if (file_info->type == xe::filesystem::FileInfo::Type::kFile &&
      !reported_size_fixed_ && !overwritten_) {
    size_ = file_info->total_size;
    allocation_size_ =
        xe::round_up(file_info->total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetTimes(xe::filesystem::FileHandle& file_handle,
                             uint64_t create_timestamp,
                             uint64_t access_timestamp,
                             uint64_t write_timestamp) {
  if (device_->is_read_only()) {
    XELOGW("{}: Tried to set the times of read-only file {}", __func__, name_);
    return false;
  }
  // FATX keeps times in 2 s units, rounding up.
  constexpr uint64_t kTimeUnit = 2 * 10000000;
  create_timestamp = xe::round_up(create_timestamp, kTimeUnit, false);
  access_timestamp = xe::round_up(access_timestamp, kTimeUnit, false);
  write_timestamp = xe::round_up(write_timestamp, kTimeUnit, false);
  if (!file_handle.SetTimes(create_timestamp, access_timestamp,
                            write_timestamp)) {
    XELOGE("{}: Cannot set the times of {}", __func__,
           xe::strip_pii(xe::path_to_utf8(host_path_)));
    return false;
  }
  // update() never rereads the creation time, which a POSIX host can't keep.
  if (create_timestamp) {
    create_timestamp_ = create_timestamp;
  }
  if (access_timestamp) {
    access_timestamp_ = access_timestamp;
  }
  if (write_timestamp) {
    write_timestamp_ = write_timestamp;
  }
  return true;
}

}  // namespace vfs
}  // namespace xe
