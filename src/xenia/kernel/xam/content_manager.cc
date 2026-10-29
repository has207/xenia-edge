/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_manager.h"

#include <algorithm>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

static const char* kGameContentHeaderDirName = "Headers";
static const char* kSpaFilename = "spa.bin";

static int content_device_id_ = 0;

ContentPackage::ContentPackage(KernelState* kernel_state,
                               const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path,
                               const std::string_view root_path)
    : kernel_state_(kernel_state),
      root_name_(root_name),
      package_path_(package_path),
      license_(cvars::license_mask) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
  content_data_ = data;

  auto fs = kernel_state_->file_system();
  if (std::filesystem::is_directory(package_path)) {
    auto device = std::make_unique<vfs::HostPathDevice>(
        device_path_, package_path, false, false, "STFS");
    mounted_ = device->Initialize();
    fs->RegisterDevice(std::move(device));
  } else {
    // A title can install content by copying the package file itself, so a
    // package is either an extracted directory or a container.
    auto device = vfs::XContentContainerDevice::CreateContentDevice(
        device_path_, package_path);
    if (device && device->Initialize()) {
      license_ = device->license_mask();
      mounted_ = true;
      fs->RegisterDevice(std::move(device));
    } else {
      XELOGE("{}: Cannot mount content container {}", __func__,
             xe::strip_pii(xe::path_to_utf8(package_path)));
    }
  }
  fs->RegisterSymbolicLink(root_name_ + ":",
                           xe::utf8::join_guest_paths(device_path_, root_path));
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

void ContentPackage::LoadPackageLicenseMask(
    const std::filesystem::path header_path) {
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(header_path);
  if (header && header->content_header.is_magic_valid()) {
    license_ = vfs::XContentContainerDevice::LicenseMaskFromHeader(*header);
    return;
  }

  if (!std::filesystem::exists(header_path)) {
    return;
  }

  auto file = xe::filesystem::OpenFile(header_path, "rb");
  auto file_size = std::filesystem::file_size(header_path);
  if (file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  size_t result = fread(&license_, 1, sizeof(license_), file);
  fclose(file);
}

ContentManager::ContentManager(KernelState* kernel_state,
                               const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(
    uint64_t xuid, uint32_t title_id, XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str =
      fmt::format("{:08X}", static_cast<uint32_t>(content_type));

  // Package root path:
  // content_root/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

// FATX is case-insensitive, so on a case-sensitive host filesystem (Linux)
// fall back to a package directory that differs only in case.
static std::filesystem::path ResolveNameInsensitive(
    const std::filesystem::path& parent, const std::string_view name) {
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(parent, ec)) {
    if (xe::utf8::equal_case(xe::path_to_utf8(child.path().filename()), name)) {
      return child.path();
    }
  }
  return {};
}

// The error a package's file name fails with in FATX, if any.
static X_RESULT CheckFileName(const std::string_view name) {
  // FATX refuses "." and ".." and these characters in any component.
  constexpr std::string_view kInvalidCharacters = "\"*+,/:;<=>?|";
  size_t component_start = 0;
  for (size_t i = 0; i <= name.size(); ++i) {
    if (i < name.size() && name[i] != '\\') {
      if (uint8_t(name[i]) < 0x20 ||
          kInvalidCharacters.find(name[i]) != std::string_view::npos) {
        return X_ERROR_INVALID_NAME;
      }
      continue;
    }
    const std::string_view component =
        name.substr(component_start, i - component_start);
    if (component == "." || component == "..") {
      return X_ERROR_INVALID_NAME;
    }
    component_start = i + 1;
  }
  // A package is a file in FATX so a name with a separator in it names none.
  // Only the first byte may be non-ASCII so a name with any other doesn't
  // either.
  if (name.empty() || name.find('\\') != std::string_view::npos ||
      std::any_of(name.begin(), name.end(),
                  [](const char c) { return uint8_t(c) >= 0x80; })) {
    return X_ERROR_PATH_NOT_FOUND;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ResolvePackagePath(const uint64_t xuid,
                                            const XCONTENT_AGGREGATE_DATA& data,
                                            std::filesystem::path& path) {
  std::string file_name = data.file_name();
  const bool shared =
      !file_name.empty() && (uint8_t(file_name[0]) & kSharedContentFileNameBit);
  if (shared) {
    file_name[0] = char(uint8_t(file_name[0]) & ~kSharedContentFileNameBit);
  }
  file_name = xe::string_util::trim(file_name);
  if (const X_RESULT result = CheckFileName(file_name)) {
    return result;
  }
  // The host has to keep the name as it is, which Windows doesn't for one
  // ending in a dot.
  const std::filesystem::path folder_name = xe::to_path(file_name);
  std::error_code ec;
  if (std::filesystem::absolute(root_path_ / folder_name, ec) !=
      std::filesystem::absolute(root_path_, ec) / folder_name) {
    return X_ERROR_INVALID_NAME;
  }

  // Content path:
  // content_root/title_id/content_type/data_file_name/
  auto get_package_path = [&, data](const uint32_t title_id) {
    uint64_t used_xuid =
        (data.xuid != -1 && data.xuid != 0) ? data.xuid.get() : xuid;

    // All DLCs are stored in common directory, so we need to override xuid for
    // them and probably some other types.
    if (shared || data.content_type == XContentType::kMarketplaceContent) {
      used_xuid = 0;
    }

    auto package_root =
        ResolvePackageRoot(used_xuid, title_id, data.content_type);
    std::filesystem::path package_path = package_root / folder_name;

    if (!std::filesystem::exists(package_path)) {
      auto existing = ResolveNameInsensitive(package_root, file_name);
      if (!existing.empty()) {
        package_path = existing;
      }
    }

    return package_path;
  };

  if (data.content_type == XContentType::kPublisher) {
    const std::unordered_set<uint32_t> title_ids =
        FindPublisherTitleIds(xuid, data.title_id);

    for (const auto& title_id : title_ids) {
      auto package_path = get_package_path(title_id);

      if (!std::filesystem::exists(package_path)) {
        continue;
      }
      path = package_path;
      return X_ERROR_SUCCESS;
    }
  }

  // Default handling for current title
  path = get_package_path(data.title_id);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CheckPackageName(const uint64_t xuid,
                                          const XCONTENT_AGGREGATE_DATA& data) {
  std::filesystem::path path;
  return ResolvePackagePath(xuid, data, path);
}

static std::filesystem::path PackageHeaderPath(
    const std::filesystem::path& package_path) {
  // Header path:
  // content_root/xuid/title_id/Headers/content_type/package_name.header
  const auto content_type_path = package_path.parent_path();
  return content_type_path.parent_path() / kGameContentHeaderDirName /
         content_type_path.filename() /
         package_path.filename().concat(".header");
}

std::unordered_set<uint32_t> ContentManager::FindPublisherTitleIds(
    const uint64_t xuid, uint32_t base_title_id) const {
  if (base_title_id == kCurrentlyRunningTitleId) {
    base_title_id = kernel_state_->title_id();
  }
  std::unordered_set<uint32_t> title_ids = {};

  std::string publisher_id_regex =
      fmt::format("^{:04X}.*", static_cast<uint16_t>(base_title_id >> 16));
  // Get all publisher entries
  auto publisher_entries = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(root_path_ /
                                      fmt::format("{:016X}", xuid)),
      std::regex(publisher_id_regex));

  for (const auto& entry : publisher_entries) {
    std::filesystem::path path_to_publisher_dir =
        entry.path / entry.name /
        fmt::format("{:08X}", static_cast<uint32_t>(XContentType::kPublisher));

    if (!std::filesystem::exists(path_to_publisher_dir)) {
      continue;
    }

    title_ids.insert(xe::string_util::from_string<uint32_t>(
        xe::path_to_utf8(entry.name), true));
  }

  // Always remove current title. It will be handled differently
  if (title_ids.count(base_title_id)) {
    title_ids.erase(base_title_id);
  }
  return title_ids;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  std::unordered_set<uint32_t> title_ids = {title_id};

  if (content_type == XContentType::kPublisher) {
    title_ids = FindPublisherTitleIds(xuid, title_id);
  }

  for (const uint32_t& title_id : title_ids) {
    // Search path:
    // content_root/xuid/title_id/type_name/*
    auto package_root = ResolvePackageRoot(xuid, title_id, content_type);
    auto file_infos = xe::filesystem::ListFiles(package_root);

    for (const auto& file_info : file_infos) {
      XCONTENT_AGGREGATE_DATA content_data;
      if (XSUCCEEDED(ReadContentHeaderFile(file_info.path / file_info.name,
                                           xuid, title_id, content_type,
                                           content_data))) {
        result.emplace_back(std::move(content_data));
        continue;
      }

      std::u16string display_name = xe::path_to_utf16(file_info.name);
      if (file_info.type != xe::filesystem::FileInfo::Type::kDirectory) {
        // A title can install content by copying the package file itself, so
        // take the name a container carries instead of the extracted layout's
        // directory name.
        auto header = vfs::XContentContainerDevice::ReadContainerHeader(
            file_info.path / file_info.name);
        if (!header || !header->content_header.is_magic_valid()) {
          continue;
        }
        auto container_name =
            vfs::XContentContainerDevice::ContentDataFromHeader(*header)
                .display_name();
        if (!container_name.empty()) {
          display_name = container_name;
        }
      }

      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(display_name);
      content_data.set_file_name(xe::path_to_utf8(file_info.name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(std::move(content_data));
    }
  }
  return result;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContentODD(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str =
      fmt::format("{:08X}", static_cast<uint32_t>(content_type));

  const std::string game_content_path =
      fmt::format("{}\\content\\{}\\{}\\{}", kernel_state_->title_mount_path_,
                  xuid_str, title_id_str, content_type_str);

  auto entry = kernel_state_->file_system()->ResolvePath(game_content_path);

  if (!entry) {
    return {};
  }

  for (const auto& child : entry->children()) {
    XCONTENT_AGGREGATE_DATA content_data;

    content_data.device_id = device_id;
    content_data.content_type = content_type;
    content_data.set_display_name(xe::path_to_utf16(child->name()));
    content_data.set_file_name(xe::path_to_utf8(child->name()));
    content_data.title_id = title_id;
    content_data.xuid = xuid;
    result.emplace_back(std::move(content_data));
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, const uint64_t xuid,
    const XCONTENT_AGGREGATE_DATA& data, const uint32_t disc_number) {
  std::filesystem::path package_path;
  if (ResolvePackagePath(xuid, data, package_path) ||
      !std::filesystem::exists(package_path)) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  // A multi-disc title's update ships as one package holding a discNNN
  // directory per disc; mount the one this disc needs.
  const std::string root_path =
      disc_number != -1 ? fmt::format("disc{:03}", disc_number) : "";

  auto package = std::make_unique<ContentPackage>(
      kernel_state_, root_name, data, package_path, root_path);
  if (!package->is_mounted()) {
    return nullptr;
  }
  return package;
}

bool ContentManager::ContentExists(const uint64_t xuid,
                                   const XCONTENT_AGGREGATE_DATA& data) {
  std::filesystem::path path;
  return !ResolvePackagePath(xuid, data, path) && std::filesystem::exists(path);
}

X_RESULT ContentManager::WriteContentHeaderFile(const uint64_t xuid,
                                                XCONTENT_AGGREGATE_DATA data) {
  if (data.title_id == -1) {
    data.title_id = kernel_state_->title_id();
  }
  if (data.xuid == -1) {
    data.xuid = xuid;
  }

  std::filesystem::path package_path;
  if (ResolvePackagePath(xuid, data, package_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }
  // The header names the package as it's stored.
  data.file_name_raw[0] =
      char(uint8_t(data.file_name_raw[0]) & ~kSharedContentFileNameBit);
  auto header_path = PackageHeaderPath(package_path);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  xe::filesystem::CreateEmptyFile(header_path);

  if (std::filesystem::exists(header_path)) {
    auto file = xe::filesystem::OpenFile(header_path, "wb");
    fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file);
    fclose(file);
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_NO_SUCH_FILE;
}

X_RESULT ContentManager::ReadContentHeaderFile(
    const std::filesystem::path& package_path, const uint64_t xuid,
    const uint32_t title_id, XContentType content_type,
    XCONTENT_AGGREGATE_DATA& data) const {
  auto header_file_path = PackageHeaderPath(package_path);
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  if (std::filesystem::exists(header_file_path)) {
    auto file = xe::filesystem::OpenFile(header_file_path, "rb");
    if (!file) {
      return X_STATUS_NO_SUCH_FILE;
    }

    // Upstream canary rewrites the header file as the whole container header,
    // followed by the file name the save was created with.
    auto header = vfs::XContentContainerDevice::ReadContainerHeader(file);
    if (header && header->content_header.is_magic_valid()) {
      char saved_name[sizeof(XCONTENT_DATA::file_name_raw)] = {};
      fread(saved_name, 1, sizeof(saved_name), file);
      fclose(file);
      data = vfs::XContentContainerDevice::ContentDataFromHeader(*header);
      // Identify the package by its location, as listing without a header does.
      data.content_type = content_type;
      data.title_id = title_id;
      data.xuid = xuid;
      const std::string file_name = xe::path_to_utf8(package_path.filename());
      // Only a save has its name there, an extracted package has other data.
      data.set_file_name(
          content_type == XContentType::kSavedGame && saved_name[0]
              ? std::string_view(saved_name,
                                 strnlen(saved_name, sizeof(saved_name)))
              : std::string_view(file_name));
      return X_STATUS_SUCCESS;
    }
    fseek(file, 0, SEEK_SET);

    std::array<uint8_t, header_size> buffer;

    auto file_size = std::filesystem::file_size(header_file_path);
    if (file_size < header_size) {
      fclose(file);
      return X_STATUS_END_OF_FILE;
    }

    size_t result = fread(buffer.data(), 1, header_size, file);
    if (result != header_size) {
      fclose(file);
      return X_STATUS_END_OF_FILE;
    }

    fclose(file);
    std::memcpy(&data, buffer.data(), buffer.size());
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_NO_SUCH_FILE;
}

X_RESULT ContentManager::CreatePackage(const uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  if (std::filesystem::exists(package_path)) {
    // Exists, must not!
    return X_ERROR_ALREADY_EXISTS;
  }

  if (!std::filesystem::create_directories(package_path)) {
    return X_ERROR_ACCESS_DENIED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name,
                                       const uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key_insensitive(root_name))) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  if (const X_RESULT result = CreatePackage(xuid, data)) {
    return result;
  }

  auto package = ResolvePackage(root_name, xuid, data);
  if (!package) {
    return X_ERROR_ACCESS_DENIED;
  }

  open_packages_.insert(
      {string_key_insensitive::create(root_name), package.release()});

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name,
                                     const uint64_t xuid,
                                     const XCONTENT_AGGREGATE_DATA& data,
                                     uint32_t& content_license,
                                     const uint32_t disc_number) {
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key_insensitive(root_name))) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  if (!std::filesystem::exists(package_path)) {
    // Does not exist, must be created.
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Open package.
  auto package = ResolvePackage(root_name, xuid, data, disc_number);
  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  package->LoadPackageLicenseMask(PackageHeaderPath(package_path));

  content_license = package->GetPackageLicense();
  if (static_cast<uint32_t>(cvars::license_mask) > 0) {
    content_license |= cvars::license_mask;
  }

  // Check for SPA file in package. Check it only for DLCs
  if (data.content_type == XContentType::kMarketplaceContent) {
    std::string spa_path = fmt::format("{}:\\{}", root_name, kSpaFilename);
    auto spa_update = kernel_state_->file_system()->ResolvePath(spa_path);
    if (spa_update) {
      UpdateSpaData(spa_update);
    }
  }

  open_packages_.insert(
      {string_key_insensitive::create(root_name), package.release()});

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContentFile(
    const std::string_view root_name,
    const std::filesystem::path& package_path) {
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key_insensitive(root_name))) {
    return X_ERROR_ALREADY_EXISTS;
  }
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto package = std::make_unique<ContentPackage>(
      kernel_state_, root_name, XCONTENT_AGGREGATE_DATA(), package_path);
  if (!package->is_mounted()) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  open_packages_.insert(
      {string_key_insensitive::create(root_name), package.release()});
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  auto global_lock = global_critical_region_.Acquire();

  // 415607D6 - Uses XamContentCreate with name "save", but XamContentClose with
  // "SAVE".
  auto it = open_packages_.find(string_key_insensitive(root_name));
  if (it == open_packages_.end()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  auto package = it->second;
  CloseOpenedFilesFromContent(*package);

  open_packages_.erase(it);
  delete package;

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(
    const uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();

  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  auto thumb_path = package_path / vfs::kPackageThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = xe::filesystem::OpenFile(thumb_path, "rb");
    size_t file_len = std::filesystem::file_size(thumb_path);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  }

  // A deleted package leaves its header behind.
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_PATH_NOT_FOUND;
  }

  // Upstream canary moves the thumbnail into the header file.
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(
      PackageHeaderPath(package_path));
  if (!header || !header->content_header.is_magic_valid()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  const auto& metadata = header->content_metadata;
  const uint32_t max_thumbnail_size =
      metadata.metadata_version >= 2 ? vfs::XContentMetadata::kThumbLengthV2
                                     : vfs::XContentMetadata::kThumbLengthV1;
  if (metadata.thumbnail_size == 0 ||
      metadata.thumbnail_size > max_thumbnail_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  buffer->assign(metadata.thumbnail,
                 metadata.thumbnail + metadata.thumbnail_size);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::SetContentThumbnail(
    const uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
    std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_PATH_NOT_FOUND;
  }
  auto file = xe::filesystem::OpenFile(
      package_path / vfs::kPackageThumbnailFileName, "wb");
  if (!file) {
    return X_ERROR_ACCESS_DENIED;
  }
  fwrite(buffer.data(), 1, buffer.size(), file);
  fclose(file);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::TruncateContent(const uint64_t xuid,
                                         const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  if (IsContentOpen(package_path)) {
    return X_ERROR_SHARING_VIOLATION;
  }
  std::error_code ec;
  // A package stored as a container file becomes an empty directory.
  if (!std::filesystem::is_directory(package_path, ec)) {
    if (!std::filesystem::remove(package_path, ec) ||
        !std::filesystem::create_directory(package_path, ec)) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }
  std::vector<std::filesystem::path> entries;
  for (const auto& entry :
       std::filesystem::directory_iterator(package_path, ec)) {
    if (entry.path().filename() != vfs::kPackageThumbnailFileName) {
      entries.push_back(entry.path());
    }
  }
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }
  for (const auto& entry : entries) {
    if (std::filesystem::remove_all(entry, ec) == static_cast<uintmax_t>(-1)) {
      return X_ERROR_ACCESS_DENIED;
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::DeleteContent(const uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  std::filesystem::path package_path;
  if (const X_RESULT result = ResolvePackagePath(xuid, data, package_path)) {
    return result;
  }
  if (IsContentOpen(package_path)) {
    return X_ERROR_SHARING_VIOLATION;
  }
  if (std::filesystem::remove_all(package_path) > 0) {
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_PATH_NOT_FOUND;
  }
}

std::filesystem::path ContentManager::ResolveGameUserContentPath(
    const uint64_t xuid) {
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id = fmt::format("{:08X}", kernel_state_->title_id());

  return root_path_ / xuid_str / kDashboardStringID / "00010000" / xuid_str /
         title_id;
}

bool ContentManager::IsContentOpen(
    const std::filesystem::path& package_path) const {
  // A name in another case, with the shared bit or for another user can reach
  // the same package so open ones are matched by their host files.
  return std::ranges::any_of(open_packages_, [&](const auto& content) {
    std::error_code ec;
    return std::filesystem::equivalent(package_path,
                                       content.second->package_path(), ec);
  });
}

void ContentManager::CloseOpenedFilesFromContent(
    const ContentPackage& package) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(
          XObject::Type::File);

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content =
        utf8::starts_with(file_path, package.device_path());

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

uint64_t ContentManager::GetContentTotalSpace() const {
  std::error_code ec;
  const auto drive_stats = std::filesystem::space(root_path_, ec);
  if (ec) {
    XELOGW("{}: {} (:08X)", __func__, ec.message(), ec.value());
    return 0;
  }

  return drive_stats.capacity;
}

uint64_t ContentManager::GetContentFreeSpace() const {
  std::error_code ec;
  const auto drive_stats = std::filesystem::space(root_path_, ec);
  if (ec) {
    XELOGW("{}: {} (:08X)", __func__, ec.message(), ec.value());
    return 0;
  }

  return drive_stats.free;
}

bool ContentManager::UpdateSpaData(vfs::Entry* spa_file_update) {
  vfs::File* file;
  if (spa_file_update->Open(vfs::FileAccess::kFileReadData, &file) !=
      X_STATUS_SUCCESS) {
    return false;
  }

  std::vector<uint8_t> data(spa_file_update->size());

  size_t read_bytes = 0;
  if (file->ReadSync(std::span<uint8_t>(data.data(), spa_file_update->size()),
                     0, &read_bytes) != X_STATUS_SUCCESS) {
    return false;
  }

  xam::SpaInfo new_spa_data(std::span<uint8_t>(data.data(), data.size()));
  kernel_state_->xam_state()->LoadSpaInfo(&new_spa_data);
  kernel_state_->emulator()->game_info_database()->Update(&new_spa_data);
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
