/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/system_manifest.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace kernel {
namespace xam {

constexpr size_t kSystemVersionOffset = 0x14C;

constexpr char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? char(c | 0x20) : c;
}

std::optional<uint32_t> GetSystemManifestVersion(
    std::span<const uint8_t> manifest) {
  if (manifest.size() < kSystemVersionOffset + sizeof(uint32_t) ||
      std::memcmp(manifest.data(), "XMNP", 4) != 0) {
    return std::nullopt;
  }
  return xe::load_and_swap<uint32_t>(&manifest[kSystemVersionOffset]);
}

std::optional<uint32_t> FindSystemManifestFile(
    std::span<const uint8_t> manifest, std::string_view name) {
  if (name.empty() || !GetSystemManifestVersion(manifest)) {
    return std::nullopt;
  }
  // The first copy of a file's name, a big-endian length that counts the
  // terminator and then the string, is followed by its version. It compares
  // bytes rather than decoding UTF-8, as the scan also meets binary data.
  const size_t name_size = name.size() + 1;
  for (size_t i = 0; i + 2 + name_size + 4 <= manifest.size(); ++i) {
    if (xe::load_and_swap<uint16_t>(&manifest[i]) != name_size ||
        manifest[i + 2 + name.size()]) {
      continue;
    }
    const std::string_view listed(
        reinterpret_cast<const char*>(&manifest[i + 2]), name.size());
    if (std::ranges::equal(listed, name, [](char a, char b) {
          return LowerAscii(a) == LowerAscii(b);
        })) {
      return xe::load_and_swap<uint32_t>(&manifest[i + 2 + name_size]);
    }
  }
  return std::nullopt;
}

std::string SystemExtFolderName(uint32_t version) {
  return fmt::format("{:08x}", version);
}

std::string FindSystemExtFile(std::string_view name) {
  auto entry = kernel_state()->file_system()->ResolvePath(
      fmt::format("{}\\{}", kSystemExtPartition, kSystemManifestName));
  if (!entry) {
    return {};
  }
  vfs::File* file = nullptr;
  if (XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
    return {};
  }
  std::vector<uint8_t> manifest(entry->size());
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(manifest, 0, &bytes_read);
  file->Destroy();
  if (XFAILED(status) || bytes_read != manifest.size()) {
    return {};
  }
  const auto version = FindSystemManifestFile(manifest, name);
  if (!version) {
    return {};
  }
  return fmt::format("{}\\{}\\{}", kSystemExtPartition,
                     SystemExtFolderName(*version), name);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
