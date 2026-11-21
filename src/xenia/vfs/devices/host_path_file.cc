/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_file.h"

#include <algorithm>

#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathFile::HostPathFile(
    uint32_t file_access, HostPathEntry* entry,
    std::unique_ptr<xe::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {
  entry->RetainFile();
}

HostPathFile::~HostPathFile() {
  // Closed first, or a delete on Windows can leave the name in place until
  // this handle closes.
  file_handle_.reset();
  entry_->ReleaseFile();
}

void HostPathFile::Destroy() { delete this; }

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ &
        (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // An overwritten file still holds its old bytes past its size.
  size_t length = buffer.size();
  auto entry = static_cast<HostPathEntry*>(entry_);
  if (entry->overwritten_) {
    length = byte_offset < entry->size_
                 ? std::min(length, entry->size_ - byte_offset)
                 : 0;
  }
  if (file_handle_->Read(byte_offset, buffer.data(), length, out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer,
                                 size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(),
                          out_bytes_written)) {
    // A write past the end grows the size the entry reports, which a
    // directory listing reads without asking the host.
    auto entry = static_cast<HostPathEntry*>(entry_);
    const size_t end = byte_offset + *out_bytes_written;
    if (end > entry->size_ && !entry->reported_size_fixed_) {
      entry->SetSize(end);
    }
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    // Shrinking the host file to the new size keeps the old bytes below it.
    auto entry = static_cast<HostPathEntry*>(entry_);
    if (entry->overwritten_) {
      entry->SetSize(length);
    }
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetTimes(uint64_t create_timestamp,
                                uint64_t access_timestamp,
                                uint64_t write_timestamp) {
  // Set through this handle, since a host file system can restamp a file when
  // a handle that wrote it closes, unless the times were set through it.
  if (static_cast<HostPathEntry*>(entry_)->SetTimes(
          *file_handle_, create_timestamp, access_timestamp, write_timestamp)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_UNSUCCESSFUL;
  }
}

}  // namespace vfs
}  // namespace xe
