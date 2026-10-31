/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path) {
  if (!std::filesystem::exists(host_path)) {
    XELOGE("Path to XContent container does not exist: {}",
           xe::strip_pii(xe::path_to_utf8(host_path)));
    return nullptr;
  }

  if (std::filesystem::is_directory(host_path)) {
    return nullptr;
  }

  FILE* host_file = xe::filesystem::OpenFile(host_path, "rb");
  if (!host_file) {
    XELOGE("Error opening XContent file.");
    return nullptr;
  }

  const uint64_t package_size = std::filesystem::file_size(host_path);
  if (package_size < sizeof(XContentContainerHeader)) {
    return nullptr;
  }

  const auto header = XContentContainerDevice::ReadContainerHeader(host_file);
  if (header == nullptr) {
    return nullptr;
  }

  fclose(host_file);

  if (!header->content_header.is_magic_valid()) {
    return nullptr;
  }

  switch (header->content_metadata.volume_type) {
    case XContentVolumeType::kStfs:
      return std::make_unique<StfsContainerDevice>(mount_path, host_path);
      break;
    case XContentVolumeType::kSvod:
      return std::make_unique<SvodContainerDevice>(mount_path, host_path);
      break;
    default:
      break;
  }

  return nullptr;
}

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(const std::string_view mount_path,
                                             Entry* entry) {
  if (entry->attributes() & kFileAttributeDirectory) {
    return nullptr;
  }

  File* file = nullptr;
  if (entry->Open(FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS) {
    XELOGE("Error opening XContent file {}", entry->absolute_path());
    return nullptr;
  }

  // An SVOD package keeps its data in fragments beside it.
  const auto header = ReadContainerHeader(file);
  if (!header || !header->content_header.is_magic_valid() ||
      header->content_metadata.volume_type != XContentVolumeType::kStfs) {
    file->Destroy();
    return nullptr;
  }

  return std::make_unique<StfsContainerDevice>(mount_path, file);
}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path)
    : Device(mount_path),
      name_("XContent"),
      host_path_(host_path),
      files_total_size_(0),
      header_(std::make_unique<XContentContainerHeader>()) {}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, File* package_file)
    : XContentContainerDevice(mount_path, std::filesystem::path()) {
  package_file_ = package_file;
}

XContentContainerDevice::~XContentContainerDevice() {
  if (package_file_) {
    package_file_->Destroy();
  }
}

bool XContentContainerDevice::Initialize() {
  std::unique_ptr<XContentContainerHeader> header;
  if (package_file_) {
    XELOGI("Loading XContent package: {}",
           package_file_->entry()->absolute_path());
    files_total_size_ = package_file_->entry()->size();
    header = ReadContainerHeader(package_file_);
  } else {
    if (!std::filesystem::exists(host_path_)) {
      XELOGE("Path to XContent container does not exist: {}",
             xe::strip_pii(xe::path_to_utf8(host_path_)));
      return false;
    }

    if (std::filesystem::is_directory(host_path_)) {
      return false;
    }

    XELOGI("Loading XContent header file: {}",
           xe::strip_pii(xe::path_to_utf8(host_path_)));
    auto header_file = xe::filesystem::OpenFile(host_path_, "rb");
    if (!header_file) {
      XELOGE("Error opening XContent header file.");
      return false;
    }
    files_total_size_ = std::filesystem::file_size(host_path_);
    header = ReadContainerHeader(header_file);
    fclose(header_file);
  }

  auto header_loading_result = VerifyHeader(std::move(header));
  if (header_loading_result != Result::kSuccess) {
    XELOGE("Error reading XContent header: {}",
           static_cast<int32_t>(header_loading_result));
    return false;
  }

  SetupContainer();

  if (LoadHostFiles() != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  return Read() == Result::kSuccess;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(
    const std::filesystem::path& file_path) {
  if (!std::filesystem::exists(file_path)) {
    return {};
  }

  if (std::filesystem::file_size(file_path) < sizeof(XContentContainerHeader)) {
    return {};
  }

  auto header_file = xe::filesystem::OpenFile(file_path, "rb");
  if (!header_file) {
    return {};
  }

  auto header = ReadContainerHeader(header_file);
  fclose(header_file);
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(FILE* host_file) {
  std::unique_ptr<XContentContainerHeader> header =
      std::make_unique<XContentContainerHeader>();

  // Read header & check signature
  if (fread(header.get(), sizeof(XContentContainerHeader), 1, host_file) != 1) {
    return nullptr;
  }
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(File* file) {
  auto header = std::make_unique<XContentContainerHeader>();
  size_t bytes_read = 0;
  if (file->ReadSync({reinterpret_cast<uint8_t*>(header.get()),
                      sizeof(XContentContainerHeader)},
                     0, &bytes_read) != X_STATUS_SUCCESS ||
      bytes_read != sizeof(XContentContainerHeader)) {
    return nullptr;
  }
  return header;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

kernel::xam::XCONTENT_AGGREGATE_DATA
XContentContainerDevice::ContentDataFromHeader(
    const XContentContainerHeader& header) {
  kernel::xam::XCONTENT_AGGREGATE_DATA data;

  std::memset(&data, 0, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA));

  data.device_id = 1;
  data.title_id = header.content_metadata.execution_info.title_id;
  data.content_type = header.content_metadata.content_type;

  auto name = header.content_metadata.display_name(XLanguage::kEnglish);
  if (name.empty()) {
    // Find first filled language and use it. It might be incorrect, but meh
    // until stfs support is done.
    for (uint32_t i = 1; i <= header.content_metadata.kNumLanguagesV2; i++) {
      name = header.content_metadata.display_name((XLanguage)i);
      if (!name.empty()) {
        break;
      }
    }
  }

  data.set_display_name(name);

  return data;
}

uint32_t XContentContainerDevice::LicenseMaskFromHeader(
    const XContentContainerHeader& header) {
  uint32_t final_license = 0;
  for (uint8_t i = 0; i < license_count; i++) {
    final_license |= header.content_header.licenses[i].license_bits;
  }
  return final_license;
}

kernel::xam::XCONTENT_AGGREGATE_DATA XContentContainerDevice::content_header()
    const {
  return ContentDataFromHeader(*header_);
}

XContentContainerDevice::Result XContentContainerDevice::VerifyHeader(
    std::unique_ptr<XContentContainerHeader> header) {
  if (files_total_size_ < sizeof(XContentContainerHeader)) {
    return Result::kTooSmall;
  }

  if (header == nullptr) {
    return Result::kReadError;
  }

  if (!header->content_header.is_magic_valid()) {
    // Unexpected format.
    return Result::kFileMismatch;
  }

  header_ = std::move(header);
  return Result::kSuccess;
}

}  // namespace vfs
}  // namespace xe
