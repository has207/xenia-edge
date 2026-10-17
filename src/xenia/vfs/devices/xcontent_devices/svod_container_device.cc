/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_entry.h"
#include "xenia/vfs/gdfx_util.h"

namespace xe {
namespace vfs {

// Reads exactly |length| bytes at |offset|. False on a short or failed read.
static bool ReadAt(xe::filesystem::FileHandle* file, size_t offset,
                   void* buffer, size_t length) {
  size_t bytes_read = 0;
  return file->Read(offset, buffer, length, &bytes_read) &&
         bytes_read == length;
}

SvodContainerDevice::SvodContainerDevice(const std::string_view mount_path,
                                         const std::filesystem::path& host_path)
    : XContentContainerDevice(mount_path, host_path),
      svod_base_offset_(),
      svod_layout_() {
  SetName("FATX");
}

SvodContainerDevice::~SvodContainerDevice() {}

SvodContainerDevice::Result SvodContainerDevice::LoadHostFiles() {
  std::filesystem::path data_fragment_path = host_path_;
  data_fragment_path += ".data";
  if (!std::filesystem::exists(data_fragment_path)) {
    XELOGE("STFS container is multi-file, but path {} does not exist.",
           data_fragment_path);
    return Result::kFileMismatch;
  }

  auto fragment_files = filesystem::ListFiles(data_fragment_path);

  if (fragment_files.size() != header_->content_metadata.data_file_count) {
    XELOGE("SVOD expecting {} data fragments, but {} are present.",
           header_->content_metadata.data_file_count.get(),
           fragment_files.size());
    return Result::kFileMismatch;
  }

  for (size_t i = 0; i < fragment_files.size(); i++) {
    auto& fragment = fragment_files.at(i);
    // A handle opens on a directory too, so reject one here rather than let it
    // sit in the map and fail every read routed to its fragment index.
    if (fragment.type != xe::filesystem::FileInfo::Type::kFile) {
      XELOGE("SVOD data fragment {} is not a file.", fragment.name);
      return Result::kFileMismatch;
    }
    auto path = fragment.path / fragment.name;
    auto file = xe::filesystem::FileHandle::OpenExisting(
        path, xe::filesystem::FileAccess::kGenericRead);
    if (!file) {
      XELOGI("Failed to map SVOD file {}.", path);
      return Result::kReadError;
    }

    // Size the path, not the listing entry, which reports 0 for a symlinked
    // fragment on Windows where the target size is what counts.
    std::error_code ec;
    const uintmax_t fragment_size = std::filesystem::file_size(path, ec);
    if (ec) {
      XELOGI("Failed to size SVOD file {}.", path);
      return Result::kReadError;
    }

    files_total_size_ += fragment_size;
    files_.emplace(i, std::move(file));
  }
  XELOGI("SVOD successfully mapped {} files.", fragment_files.size());
  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::Read() {
  // SVOD Systems can have different layouts. The root block is
  // denoted by the magic "MICROSOFT*XBOX*MEDIA" and is always in
  // the first "actual" data fragment of the system.
  if (!files_.contains(0)) {
    XELOGE("SVOD has no data files");
    return Result::kFileMismatch;
  }
  auto* svod_header = files_.at(0).get();

  size_t magic_offset = 0;
  const Result layout_result = SetLayout(svod_header, magic_offset);
  if (layout_result != Result::kSuccess) {
    return layout_result;
  }

  // Parse the root directory
  struct {
    uint32_t block;
    uint32_t size;
    uint32_t creation_date;
    uint32_t creation_time;
  } root_data;
  static_assert_size(root_data, 0x10);

  if (!ReadAt(svod_header, magic_offset + 0x14, &root_data,
              sizeof(root_data))) {
    XELOGE("ReadSVOD failed to read root block data at 0x{:016X}",
           magic_offset + 0x14);
    return Result::kReadError;
  }

  const uint64_t root_creation_timestamp =
      decode_fat_timestamp(root_data.creation_date, root_data.creation_time);

  auto root_entry = new SvodContainerEntry(this, nullptr, "", &files_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry->access_timestamp_ = root_creation_timestamp;
  root_entry->create_timestamp_ = root_creation_timestamp;
  root_entry->write_timestamp_ = root_creation_timestamp;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  // Traverse all child entries
  return ReadEntry(root_data.block, 0, root_entry);
}

SvodContainerDevice::Result SvodContainerDevice::ReadEntry(
    uint32_t block, uint32_t ordinal, SvodContainerEntry* parent) {
  // For games with a large amount of files, the ordinal offset can overrun
  // the current block and potentially hit a hash block.
  size_t ordinal_offset = ordinal * 0x4;
  size_t block_offset = ordinal_offset / 0x800;
  size_t true_ordinal_offset = ordinal_offset % 0x800;

  // Calculate the file & address of the block
  size_t entry_address, entry_file;
  BlockToOffset(block + block_offset, &entry_address, &entry_file);
  entry_address += true_ordinal_offset;

  // Read directory entry
  auto* file = files_.at(entry_file).get();

#pragma pack(push, 1)
  struct {
    uint16_t node_l;
    uint16_t node_r;
    uint32_t data_block;
    uint32_t length;
    uint8_t attributes;
    uint8_t name_length;
  } dir_entry;
  static_assert_size(dir_entry, 0xE);
#pragma pack(pop)

  if (!ReadAt(file, entry_address, &dir_entry, sizeof(dir_entry))) {
    XELOGE("ReadEntrySVOD failed to read directory entry at {:016X}",
           entry_address);
    return Result::kReadError;
  }

  if (dir_entry.node_l == kGdfxPaddingOffset) {
    const uint32_t padded_ordinal = GdfxPaddedEntryOrdinal(ordinal);
    return padded_ordinal ? ReadEntry(block, padded_ordinal, parent)
                          : Result::kSuccess;
  }

  auto name_buffer = std::make_unique<char[]>(dir_entry.name_length);
  if (!ReadAt(file, entry_address + sizeof(dir_entry), name_buffer.get(),
              dir_entry.name_length)) {
    XELOGE("ReadEntrySVOD failed to read directory entry name at {:016X}",
           entry_address);
    return Result::kReadError;
  }

  // Filename is stored as Windows-1252, convert it to UTF-8.
  auto ansi_name = std::string(name_buffer.get(), dir_entry.name_length);
  auto name = xe::win1252_to_utf8(ansi_name);

  // Fallback to normal name if for whatever reason conversion from 1252 code
  // page failed.
  if (name.empty()) {
    name = ansi_name;
  }

  // Read the left node
  if (dir_entry.node_l) {
    auto node_result = ReadEntry(block, dir_entry.node_l, parent);
    if (node_result != Result::kSuccess) {
      return node_result;
    }
  }

  // Read file & address of block's data
  size_t data_address, data_file;
  BlockToOffset(dir_entry.data_block, &data_address, &data_file);

  // Create the entry
  // NOTE: SVOD entries don't have timestamps for individual files, which can
  //       cause issues when decrypting games. Using the root entry's timestamp
  //       solves this issues.
  auto entry = SvodContainerEntry::Create(this, parent, name, &files_);
  if (dir_entry.attributes & kFileAttributeDirectory) {
    // Entry is a directory
    entry->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    entry->block_ = block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    if (dir_entry.length) {
      // If length is greater than 0, traverse the directory's children
      auto directory_result = ReadEntry(dir_entry.data_block, 0, entry.get());
      if (directory_result != Result::kSuccess) {
        return directory_result;
      }
    }
  } else {
    // Entry is a file
    entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
    entry->size_ = dir_entry.length;
    entry->allocation_size_ = xe::round_up(dir_entry.length, kBlockSize);
    entry->data_offset_ = data_address;
    entry->data_size_ = dir_entry.length;
    entry->block_ = dir_entry.data_block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    // Fill in all block records, sector by sector.
    if (entry->attributes() & X_FILE_ATTRIBUTE_NORMAL) {
      uint32_t block_index = dir_entry.data_block;
      size_t remaining_size = xe::round_up(dir_entry.length, 0x800);

      size_t last_record = -1;
      size_t last_offset = -1;
      while (remaining_size) {
        constexpr size_t BLOCK_SIZE = 0x800;

        size_t offset, file_index;
        BlockToOffset(block_index, &offset, &file_index);

        block_index++;
        remaining_size -= BLOCK_SIZE;

        if (offset - last_offset == BLOCK_SIZE) {
          // Consecutive, so append to last entry.
          entry->block_list_[last_record].length += BLOCK_SIZE;
          last_offset = offset;
          continue;
        }

        entry->block_list_.push_back({file_index, offset, BLOCK_SIZE});
        last_record = entry->block_list_.size() - 1;
        last_offset = offset;
      }
    }
  }

  parent->children_.emplace_back(std::move(entry));

  // Read the right node.
  if (dir_entry.node_r) {
    auto node_result = ReadEntry(block, dir_entry.node_r, parent);
    if (node_result != Result::kSuccess) {
      return node_result;
    }
  }

  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::SetLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  if (IsEDGFLayout()) {
    return SetEDGFLayout(header, magic_offset);
  }

  if (IsXSFLayout(header)) {
    return SetXSFLayout(header, magic_offset);
  }

  return SetNormalLayout(header, magic_offset);
}

XContentContainerDevice::Result SvodContainerDevice::SetEDGFLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];
  if (!ReadAt(header, 0x2000, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x2000");
    return Result::kReadError;
  }

  if (std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) != 0) {
    XELOGE("SVOD uses an EGDF layout, but the magic block was not found.");
    return Result::kFileMismatch;
  }

  svod_base_offset_ = 0x0000;
  magic_offset = 0x2000;
  svod_layout_ = SvodLayoutType::kEnhancedGDF;
  XELOGI("SVOD uses an EGDF layout. Magic block present at 0x2000.");
  return Result::kSuccess;
}

const bool SvodContainerDevice::IsXSFLayout(
    xe::filesystem::FileHandle* header) const {
  uint8_t magic_buf[20];
  if (!ReadAt(header, 0x12000, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x12000");
    return false;
  }

  return std::memcmp(magic_buf, MEDIA_MAGIC, countof(magic_buf)) == 0;
}

XContentContainerDevice::Result SvodContainerDevice::SetXSFLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];
  const char* XSF_MAGIC = "XSF";

  if (!ReadAt(header, 0x2000, magic_buf, 3)) {
    XELOGE("ReadSVOD failed to read SVOD XSF magic at 0x2000");
    return Result::kReadError;
  }

  svod_base_offset_ = 0x10000;
  magic_offset = 0x12000;

  if (std::memcmp(magic_buf, XSF_MAGIC, 3) != 0) {
    svod_layout_ = SvodLayoutType::kUnknown;
    XELOGI("SVOD appears to use an XSF layout, but no header is present.");
    XELOGI("SVOD magic block found at 0x12000");
    return Result::kSuccess;
  }

  svod_layout_ = SvodLayoutType::kXSF;
  XELOGI("SVOD uses an XSF layout. Magic block present at 0x12000.");
  XELOGI("Game was likely converted using a third-party tool.");
  return Result::kSuccess;
}

XContentContainerDevice::Result SvodContainerDevice::SetNormalLayout(
    xe::filesystem::FileHandle* header, size_t& magic_offset) {
  uint8_t magic_buf[20];

  const uint32_t magic_pos =
      header_->content_metadata.data_file_count == 1 ? 0xD000 : 0x2000;
  if (!ReadAt(header, magic_pos, magic_buf, countof(magic_buf))) {
    XELOGE("ReadSVOD failed to read SVOD magic at 0x{:04X}", magic_pos);
    return Result::kReadError;
  }

  if (std::memcmp(magic_buf, MEDIA_MAGIC, 20) != 0) {
    XELOGE("Could not locate SVOD magic block.");
    return Result::kReadError;
  }

  // If the SVOD's magic block is at 0xD000, it most likely means that it
  // is a single-file system. The STFS Header is 0xB000 bytes and the
  // remaining 0x2000 is from hash tables. In most cases, these will be
  // STFS, not SVOD.
  if (header_->content_metadata.data_file_count == 1) {
    svod_base_offset_ = 0xB000;
    svod_layout_ = SvodLayoutType::kSingleFile;
    XELOGI("SVOD is a single file. Magic block present at 0xD000.");
    magic_offset = 0xD000;
  } else {
    svod_base_offset_ = 0x0000;
    svod_layout_ = SvodLayoutType::kMultipleFiles;
    XELOGI("SVOD is a multiple files. Magic block present at 0x2000.");
    magic_offset = 0x2000;
  }
  return Result::kSuccess;
}

void SvodContainerDevice::BlockToOffset(size_t block, size_t* out_address,
                                        size_t* out_file_index) const {
  // SVOD Systems use hash blocks for integrity checks. These hash blocks
  // cause blocks to be discontinuous in memory, and must be accounted for.
  //  - Each data block is 0x800 bytes in length
  //  - Every group of 0x198 data blocks is preceded a Level0 hash table.
  //    Level0 tables contain 0xCC hashes, each representing two data blocks.
  //    The total size of each Level0 hash table is 0x1000 bytes in length.
  //  - Every 0xA1C4 Level0 hash tables is preceded by a Level1 hash table.
  //    Level1 tables contain 0xCB hashes, each representing two Level0 hashes.
  //    The total size of each Level1 hash table is 0x1000 bytes in length.
  //  - Files are split into fragments of 0xA290000 bytes in length,
  //    consisting of 0x14388 data blocks, 0xCB Level0 hash tables, and 0x1
  //    Level1 hash table.

  constexpr size_t BLOCK_SIZE = 0x800;
  constexpr size_t HASH_BLOCK_SIZE = 0x1000;
  constexpr size_t BLOCKS_PER_L0_HASH = 0x198;
  constexpr size_t HASHES_PER_L1_HASH = 0xA1C4;
  constexpr size_t BLOCKS_PER_FILE = 0x14388;
  constexpr size_t MAX_FILE_SIZE = 0xA290000;
  const size_t BLOCK_OFFSET =
      header_->content_metadata.volume_descriptor.svod.start_data_block();

  // Resolve the true block address and file index
  size_t true_block = block - (BLOCK_OFFSET * 2);
  if (svod_layout_ == SvodLayoutType::kEnhancedGDF) {
    // EGDF has an 0x1000 byte offset, which is two blocks
    true_block += 0x2;
  }

  size_t file_block = true_block % BLOCKS_PER_FILE;
  size_t file_index = true_block / BLOCKS_PER_FILE;
  size_t offset = 0;

  // Calculate offset caused by Level0 Hash Tables
  size_t level0_table_count = (file_block / BLOCKS_PER_L0_HASH) + 1;
  offset += level0_table_count * HASH_BLOCK_SIZE;

  // Calculate offset caused by Level1 Hash Tables
  size_t level1_table_count = (level0_table_count / HASHES_PER_L1_HASH) + 1;
  offset += level1_table_count * HASH_BLOCK_SIZE;

  // For single-file SVOD layouts, include the size of the header in the offset.
  if (svod_layout_ == SvodLayoutType::kSingleFile) {
    offset += svod_base_offset_;
  }

  size_t block_address = (file_block * BLOCK_SIZE) + offset;

  // If the offset causes the block address to overrun the file, round it.
  if (block_address >= MAX_FILE_SIZE) {
    file_index += 1;
    block_address %= MAX_FILE_SIZE;
    block_address += 0x2000;
  }

  *out_address = block_address;
  *out_file_index = file_index;
}

}  // namespace vfs
}  // namespace xe
