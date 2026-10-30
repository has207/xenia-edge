/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/null_file.h"

#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/null_entry.h"

namespace xe {
namespace vfs {

NullFile::NullFile(uint32_t file_access, NullEntry* entry)
    : File(file_access, entry) {}

NullFile::~NullFile() = default;

void NullFile::Destroy() { delete this; }

X_STATUS NullFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                            size_t* out_bytes_read) {
  if (!(file_access_ & FileAccess::kFileReadData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS NullFile::WriteSync(std::span<const uint8_t> buffer,
                             size_t byte_offset, size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // The XDK cache drive formats a partition by writing a FATX volume header
  // ('XTAF', serial, sectors per cluster) at its start. FATX mounts only
  // clusters of a power of two sectors and at least 0x1000 bytes.
  constexpr uint32_t kFatxMagic = 0x58544146;
  if (byte_offset == 0 && buffer.size() >= 12 &&
      xe::load_and_swap<uint32_t>(buffer.data()) == kFatxMagic) {
    const uint32_t sectors = xe::load_and_swap<uint32_t>(buffer.data() + 8);
    const uint64_t cluster_size =
        uint64_t(sectors) * entry()->device()->bytes_per_sector();
    if (xe::is_pow2(sectors) && sectors <= 0x80 && cluster_size >= 0x1000) {
      static_cast<NullEntry*>(entry())->set_sectors_per_allocation_unit(
          sectors);
    }
  }

  return X_STATUS_SUCCESS;
}

X_STATUS NullFile::SetLength(size_t length) {
  if (!(file_access_ & FileAccess::kFileWriteData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
