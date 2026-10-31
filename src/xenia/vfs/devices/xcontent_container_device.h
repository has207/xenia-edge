/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_
#define XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_

#include <filesystem>
#include <map>
#include <string_view>

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

constexpr fourcc_t kLIVESignature = make_fourcc("LIVE");
constexpr fourcc_t kCONSignature = make_fourcc("CON ");
constexpr fourcc_t kPIRSSignature = make_fourcc("PIRS");

class XContentContainerDevice : public Device {
 public:
  constexpr static uint32_t kBlockSize = 0x1000;

  static std::unique_ptr<XContentContainerDevice> CreateContentDevice(
      const std::string_view mount_path,
      const std::filesystem::path& host_path);

  // A device for the STFS package stored in |entry|, whose device has to
  // outlive it.
  static std::unique_ptr<XContentContainerDevice> CreateContentDevice(
      const std::string_view mount_path, Entry* entry);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      const std::filesystem::path& file_path);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      FILE* host_file);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      File* file);

  ~XContentContainerDevice() override;

  bool Initialize() override;

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }

  uint32_t sectors_per_allocation_unit() const override { return 8; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  size_t data_size() const {
    if (header_->content_header.header_size) {
      return files_total_size_ -
             xe::round_up(header_->content_header.header_size, kBlockSize);
    }
    return files_total_size_ - sizeof(XContentContainerHeader);
  }

  uint64_t xuid() const { return header_->content_metadata.profile_id; }

  uint32_t title_id() const {
    return header_->content_metadata.execution_info.title_id;
  }

  uint32_t content_type() const {
    return static_cast<uint32_t>(header_->content_metadata.content_type.get());
  }

  static kernel::xam::XCONTENT_AGGREGATE_DATA ContentDataFromHeader(
      const XContentContainerHeader& header);

  static uint32_t LicenseMaskFromHeader(const XContentContainerHeader& header);

  kernel::xam::XCONTENT_AGGREGATE_DATA content_header() const;
  uint32_t license_mask() const { return LicenseMaskFromHeader(*header_); }

  const std::filesystem::path& GetHostPath() const { return host_path_; }
  // The package's file on another device, null for a host file.
  const Entry* package_entry() const {
    return package_file_ ? package_file_->entry() : nullptr;
  }

  const XContentContainerHeader* GetContainerHeader() const {
    return header_.get();
  }

 protected:
  XContentContainerDevice(const std::string_view mount_path,
                          const std::filesystem::path& host_path);
  // Takes ownership of |package_file|.
  XContentContainerDevice(const std::string_view mount_path,
                          File* package_file);
  enum class Result {
    kSuccess = 0,
    kOutOfMemory = -1,
    kReadError = -10,
    kFileMismatch = -30,
    kDamagedFile = -31,
    kTooSmall = -32,
  };

  virtual Result Read() = 0;
  // Load all host files. Usually STFS is only 1 file, meanwhile SVOD is usually
  // multiple file.
  virtual Result LoadHostFiles() = 0;
  // Initialize container specific fields.
  virtual void SetupContainer() {};

  Entry* ResolvePath(const std::string_view path) override;
  void Dump(StringBuffer* string_buffer) override;
  Result VerifyHeader(std::unique_ptr<XContentContainerHeader> header);

  void SetName(std::string name) { name_ = name; }
  const std::string& GetName() const { return name_; }

  void SetFilesSize(uint64_t files_size) { files_total_size_ = files_size; }
  const uint64_t GetFilesSize() const { return files_total_size_; }

  std::string name_;
  std::filesystem::path host_path_;
  // The package on another device, read in place of a host file.
  File* package_file_ = nullptr;

  size_t files_total_size_;
  std::unique_ptr<Entry> root_entry_;
  std::unique_ptr<XContentContainerHeader> header_;
};

}  // namespace vfs
}  // namespace xe

#endif
