/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_device.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path,
                               bool read_only, bool collapse_content_packages)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      collapse_content_packages_(collapse_content_packages) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      XELOGE("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("HostPathDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

namespace {

// Content-type folders are named with an 8-digit hex value (e.g. 00000001).
bool IsContentTypeName(const std::string& name) {
  if (name.size() != 8) {
    return false;
  }
  for (char c : name) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

// A single-file content package is a directory holding one payload file. The
// thumbnail is Xenia bookkeeping, not game data, so it is ignored. Returns that
// payload's info so the package can be presented as the file itself.
bool ResolveSinglePackagePayload(const std::filesystem::path& package_dir,
                                 xe::filesystem::FileInfo& out_payload) {
  xe::filesystem::FileInfo* payload = nullptr;
  auto entries = xe::filesystem::ListFiles(package_dir);
  for (auto& entry : entries) {
    if (entry.type == xe::filesystem::FileInfo::Type::kDirectory) {
      return false;
    }
    if (entry.name == kPackageThumbnailFileName) {
      continue;
    }
    if (payload) {
      return false;
    }
    payload = &entry;
  }
  if (!payload) {
    return false;
  }
  out_payload = *payload;
  return true;
}

// Size the 360 STFS container would have for a single-file saved-game package.
// Games that stat the raw content path validate this container size, not the
// raw payload. Mirrors the block layout in stfs_container_device.cc.
uint64_t StfsSingleFileContainerSize(uint64_t payload_size) {
  constexpr uint64_t kBlockSize = 0x1000;
  constexpr uint32_t kBlocksPerHash = 2;    // read-write (CON) packages
  constexpr uint64_t kHeaderSize = 0xA000;  // round_up(0x971A metadata, block)
  constexpr uint32_t kLevel[3] = {0xAA, 0xAA * 0xAA, 0xAA * 0xAA * 0xAA};
  const uint32_t data_blocks =
      uint32_t((payload_size + kBlockSize - 1) / kBlockSize);
  // One file-table block, the data blocks, and one spare, matching containers.
  const uint32_t content_blocks = 1 + data_blocks + 1;
  uint64_t block = content_blocks;
  for (int i = 0; i < 3; i++) {
    block += ((content_blocks + kLevel[i]) / kLevel[i]) * kBlocksPerHash;
    if (content_blocks < kLevel[i]) {
      break;
    }
  }
  return kHeaderSize + block * kBlockSize;
}

}  // namespace

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  std::vector<std::filesystem::path> ancestors;
  PopulateEntry(parent_entry, ancestors);
}

void HostPathDevice::PopulateEntry(
    HostPathEntry* parent_entry,
    std::vector<std::filesystem::path>& ancestors) {
  std::error_code ec;
  auto canonical_self =
      std::filesystem::canonical(parent_entry->host_path(), ec);
  if (!ec) {
    for (const auto& seen : ancestors) {
      if (seen == canonical_self) {
        XELOGW(
            "HostPathDevice: {} is already on the path being walked; not "
            "following it again",
            xe::path_to_utf8(parent_entry->host_path()));
        return;
      }
    }
    ancestors.push_back(canonical_self);
  }

  auto child_infos = xe::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    // Xenia bookkeeping, never part of the package the console hands out.
    if (child_info.type == xe::filesystem::FileInfo::Type::kFile &&
        child_info.name == kPackageThumbnailFileName) {
      continue;
    }

    // On the content partition, present a single-file package (a directory
    // under a content-type folder holding one payload file) as that file, the
    // way the console stores it. A game that stats the raw content path then
    // sees the save instead of an empty directory.
    if (collapse_content_packages_ &&
        child_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
        IsContentTypeName(parent_entry->name())) {
      const auto package_dir = parent_entry->host_path() / child_info.name;
      xe::filesystem::FileInfo payload;
      if (ResolveSinglePackagePayload(package_dir, payload)) {
        const auto payload_host = package_dir / payload.name;
        const uint64_t container_size =
            StfsSingleFileContainerSize(payload.total_size);
        payload.name = child_info.name;
        auto file_entry =
            HostPathEntry::Create(this, parent_entry, payload_host, payload);
        // Report the STFS container size, not the raw payload size, and keep it
        // pinned so update() does not reset it to the backing file's size.
        file_entry->size_ = container_size;
        file_entry->allocation_size_ =
            xe::round_up(container_size, bytes_per_sector());
        file_entry->reported_size_fixed_ = true;
        parent_entry->children_.push_back(std::unique_ptr<Entry>(file_entry));
        continue;
      }
    }

    auto child = HostPathEntry::Create(
        this, parent_entry, parent_entry->host_path() / child_info.name,
        child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child, ancestors);
    }
  }

  if (!ec) {
    ancestors.pop_back();
  }
}

}  // namespace vfs
}  // namespace xe
