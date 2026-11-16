/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_update_check.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/memory.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"

namespace xe {
namespace app {

namespace {

constexpr fourcc_t kXex2Magic = make_fourcc("XEX2");
// Enough for the headers of any executable or patch, which come first.
constexpr size_t kMaxXexHeaderSize = 1024 * 1024;

std::vector<uint8_t> ReadXexHeaders(vfs::Entry* entry) {
  std::vector<uint8_t> data(std::min<size_t>(entry->size(), kMaxXexHeaderSize));
  vfs::File* file = nullptr;
  if (data.empty() ||
      XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
    return {};
  }
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(data, 0, &bytes_read);
  file->Destroy();
  if (XFAILED(status)) {
    return {};
  }
  data.resize(bytes_read);
  return data;
}

// The update's default.xexp or, for a set, a disc's, which all patch to the
// same version.
vfs::Entry* FindXexp(vfs::Device& device) {
  if (vfs::Entry* xexp = device.ResolvePath("default.xexp")) {
    return xexp;
  }
  if (vfs::Entry* root = device.ResolvePath("")) {
    for (const auto& child : root->children()) {
      if (xe::utf8::starts_with_case(child->name(), "disc")) {
        if (vfs::Entry* xexp = child->GetChild("default.xexp")) {
          return xexp;
        }
      }
    }
  }
  return nullptr;
}

uint32_t DeviceTitleUpdateNumber(vfs::Device& device, uint32_t base_version) {
  vfs::Entry* xexp = FindXexp(device);
  return xexp ? XexpTitleUpdateNumber(ReadXexHeaders(xexp), base_version) : 0;
}

// |data| cut to the headers its XEX header says it has, empty when it isn't
// one.
std::vector<uint8_t> XexHeadersOf(std::vector<uint8_t> data) {
  if (data.size() < sizeof(xex2_header) ||
      xe::load_and_swap<uint32_t>(data.data()) != kXex2Magic) {
    return {};
  }
  const uint32_t header_size = xe::load_and_swap<uint32_t>(
      data.data() + offsetof(xex2_header, header_size));
  if (header_size > data.size()) {
    return {};
  }
  data.resize(header_size);
  return data;
}

// The headers of the game file's default.xex, or of the file when it's an
// executable.
std::vector<uint8_t> ReadGameXexHeaders(const std::filesystem::path& path) {
  const auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xex") {
    // Read directly, since a device on its folder would scan the whole tree.
    FILE* file = xe::filesystem::OpenFile(path, "rb");
    if (!file) {
      return {};
    }
    std::vector<uint8_t> data(kMaxXexHeaderSize);
    data.resize(std::fread(data.data(), 1, data.size(), file));
    std::fclose(file);
    return XexHeadersOf(std::move(data));
  }
  std::unique_ptr<vfs::Device> device;
  if (extension == ".iso") {
    device = std::make_unique<vfs::DiscImageDevice>("", path);
  } else if (extension == ".zar") {
    device = std::make_unique<vfs::DiscZarchiveDevice>("", path);
  } else if (vfs::XContentContainerDevice::IsContainerFile(path)) {
    device = vfs::XContentContainerDevice::CreateContentDevice("", path);
  }
  if (!device || !device->Initialize()) {
    return {};
  }
  vfs::Entry* xex = device->ResolvePath("default.xex");
  return xex ? XexHeadersOf(ReadXexHeaders(xex)) : std::vector<uint8_t>();
}

// Whether every file under |entry| reads back whole.
bool FilesReadWhole(vfs::Entry* entry, std::vector<uint8_t>& buffer) {
  for (const auto& child : entry->children()) {
    if (child->attributes() & vfs::kFileAttributeDirectory) {
      if (!FilesReadWhole(child.get(), buffer)) {
        return false;
      }
      continue;
    }
    vfs::File* file = nullptr;
    if (XFAILED(child->Open(vfs::FileAccess::kFileReadData, &file))) {
      return false;
    }
    bool whole = true;
    for (size_t offset = 0; whole && offset < child->size();) {
      size_t bytes_read = 0;
      const size_t length = std::min(buffer.size(), child->size() - offset);
      whole = XSUCCEEDED(file->ReadSync({buffer.data(), length}, offset,
                                        &bytes_read)) &&
              bytes_read == length;
      offset += length;
    }
    file->Destroy();
    if (!whole) {
      return false;
    }
  }
  return true;
}

// What the default.xex in the package at |path| says about itself.
std::optional<vfs::XexMetadata> ReadPackageXexMetadata(
    const std::filesystem::path& path) {
  std::unique_ptr<vfs::Device> device =
      vfs::XContentContainerDevice::CreateContentDevice("", path);
  if (!device || !device->Initialize()) {
    return std::nullopt;
  }
  vfs::Entry* xex = device->ResolvePath("default.xex");
  if (!xex) {
    return std::nullopt;
  }
  const auto data = ReadXexHeaders(xex);
  return vfs::ExtractXexMetadata(data.data(), data.size());
}

}  // namespace

std::optional<TitleUpdateRelease> ReadTitleUpdateRelease(
    const std::filesystem::path& path) {
  std::error_code ec;
  // An indie game installed as a folder.
  if (std::filesystem::is_directory(path, ec)) {
    return std::nullopt;
  }
  const auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  std::optional<vfs::XexMetadata> xex;
  TitleUpdateRelease release;
  if (extension == ".xex") {
    xex = vfs::ExtractXexMetadata(path);
  } else if (extension == ".iso") {
    xex = vfs::ExtractIsoMetadata(path);
  } else if (extension == ".zar") {
    xex = vfs::ExtractZarMetadata(path);
  } else if (extension == ".xbe") {
    return std::nullopt;
  } else {
    const auto stfs = vfs::ExtractStfsMetadata(path);
    if (!stfs) {
      return std::nullopt;
    }
    switch (XContentType(stfs->content_type)) {
      case XContentType::kXbox360Title:
      case XContentType::kInstalledGame:
      case XContentType::kArcadeTitle:
        break;
      default:
        return std::nullopt;
    }
    release = {stfs->title_id, stfs->media_id, stfs->version};
    // Early Arcade packages leave their header's execution info zero so only
    // the executable says which release it is.
    if (!release.version) {
      xex = ReadPackageXexMetadata(path);
    }
  }
  if (xex) {
    release = {xex->title_id, xex->media_id, xex->version.value()};
  } else if (!release.title_id) {
    return std::nullopt;
  }
  if (kernel::IsSystemTitle(release.title_id)) {
    return std::nullopt;
  }
  return release;
}

uint32_t XexpTitleUpdateNumber(std::span<const uint8_t> xexp,
                               uint32_t base_version) {
  constexpr size_t kHeadersOffset = offsetof(xex2_header, headers);
  if (xexp.size() < kHeadersOffset ||
      xe::load_and_swap<uint32_t>(xexp.data()) != kXex2Magic) {
    return 0;
  }
  const size_t count = xe::load_and_swap<uint32_t>(
      xexp.data() + offsetof(xex2_header, header_count));
  if (count > (xexp.size() - kHeadersOffset) / sizeof(xex2_opt_header)) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* opt_header =
        xexp.data() + kHeadersOffset + i * sizeof(xex2_opt_header);
    if (xe::load_and_swap<uint32_t>(opt_header) !=
        XEX_HEADER_DELTA_PATCH_DESCRIPTOR) {
      continue;
    }
    const size_t offset = xe::load_and_swap<uint32_t>(
        opt_header + offsetof(xex2_opt_header, offset));
    constexpr size_t kTarget =
        offsetof(xex2_opt_delta_patch_descriptor, target_version_value);
    constexpr size_t kSource =
        offsetof(xex2_opt_delta_patch_descriptor, source_version_value);
    if (offset > xexp.size() || xexp.size() - offset < kSource + 4) {
      return 0;
    }
    const uint32_t source =
        xe::load_and_swap<uint32_t>(xexp.data() + offset + kSource);
    const uint32_t target =
        xe::load_and_swap<uint32_t>(xexp.data() + offset + kTarget);
    // Early updates leave the source zero and their target is the release's
    // version but for the build field.
    if (source ? source != base_version
               : (target & 0xFF0000FF) != base_version) {
      return 0;
    }
    return (target >> 8) & 0xFFFF;
  }
  return 0;
}

uint32_t PackageTitleUpdateNumber(const std::filesystem::path& package,
                                  uint32_t version) {
  if (!vfs::XContentContainerDevice::IsContainerFile(package)) {
    return 0;
  }
  std::unique_ptr<vfs::Device> device =
      vfs::XContentContainerDevice::CreateContentDevice("", package);
  return device && device->Initialize()
             ? DeviceTitleUpdateNumber(*device, version)
             : 0;
}

std::optional<bool> TitleUpdateFitsGame(const std::filesystem::path& package,
                                        const std::filesystem::path& game) {
  const std::vector<uint8_t> base = ReadGameXexHeaders(game);
  if (base.empty() || !vfs::XContentContainerDevice::IsContainerFile(package)) {
    return std::nullopt;
  }
  std::unique_ptr<vfs::Device> device =
      vfs::XContentContainerDevice::CreateContentDevice("", package);
  vfs::Entry* xexp =
      device && device->Initialize() ? FindXexp(*device) : nullptr;
  const std::vector<uint8_t> patch =
      xexp ? XexHeadersOf(ReadXexHeaders(xexp)) : std::vector<uint8_t>();
  if (patch.empty()) {
    return std::nullopt;
  }
  // The base's key decides which of the two it's encrypted with.
  return cpu::XexModule::PatchKeyFits(base, patch, false) ||
         cpu::XexModule::PatchKeyFits(base, patch, true);
}

bool IsPackageComplete(const std::filesystem::path& package) {
  if (!vfs::XContentContainerDevice::IsContainerFile(package)) {
    return false;
  }
  std::unique_ptr<vfs::Device> device =
      vfs::XContentContainerDevice::CreateContentDevice("", package);
  vfs::Entry* root =
      device && device->Initialize() ? device->ResolvePath("") : nullptr;
  std::vector<uint8_t> buffer(1024 * 1024);
  return root && FilesReadWhole(root, buffer);
}

TitleUpdateFit CheckTitleUpdateFit(const std::filesystem::path& package,
                                   uint32_t package_version,
                                   const std::filesystem::path& game,
                                   uint32_t version) {
  // A patch cut short can't apply, which the key check can't tell.
  if (!IsPackageComplete(package)) {
    return TitleUpdateFit::kDamaged;
  }
  // An update made for another release fits when the two share their code,
  // which the patch's key tells, the same check the kernel picks updates by.
  if (const auto fits = TitleUpdateFitsGame(package, game)) {
    return *fits ? TitleUpdateFit::kFits : TitleUpdateFit::kWrongKey;
  }
  return version && package_version && package_version != version
             ? TitleUpdateFit::kOtherRelease
             : TitleUpdateFit::kFits;
}

uint32_t InstalledTitleUpdateNumber(const std::filesystem::path& folder,
                                    uint32_t version) {
  uint32_t newest = 0;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end;
       !ec && it != end; it.increment(ec)) {
    const auto& path = it->path();
    if (!it->is_directory(ec)) {
      newest = std::max(newest, PackageTitleUpdateNumber(path, version));
      continue;
    }
    vfs::HostPathDevice device("", path, true);
    if (device.Initialize()) {
      newest = std::max(newest, DeviceTitleUpdateNumber(device, version));
    }
  }
  return newest;
}

const TitleUpdateDownload* FindNewerTitleUpdate(
    const TitleUpdateDownloads& downloads, const TitleUpdateRelease& release,
    uint32_t installed) {
  const TitleUpdateDownload* newest = nullptr;
  auto consider = [&](uint32_t media_id) {
    const auto updates = downloads.find(media_id);
    if (updates == downloads.end()) {
      return;
    }
    for (const auto& update : updates->second) {
      if (update.base_version == release.version &&
          (!newest || update.tu >= newest->tu)) {
        newest = &update;
      }
    }
  };
  consider(release.media_id);
  // Early updates were made for any media of the release.
  if (release.media_id) {
    consider(0);
  }
  if (!newest || (newest->tu ? newest->tu <= installed : installed != 0)) {
    return nullptr;
  }
  return newest;
}

}  // namespace app
}  // namespace xe
