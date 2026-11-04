/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/title_image_device.h"

#include <algorithm>
#include <charconv>
#include <span>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe {
namespace kernel {
namespace xam {

// What the patched My Games list asks for under <title id>.
static constexpr std::string_view kLibraryImages[] = {"largeboxart.jpg"};
// What the dashboard asks for under <title id>\<locale>.
static constexpr std::string_view kDashboardImages[] = {
    "smallboxart.jpg", "largeboxart.jpg", "background.jpg"};
// What a title's releases give, kept in its title_art folder.
static constexpr std::string_view kReleaseImages[] = {"largeboxart.jpg",
                                                      "background.jpg"};
// Bigger than any NXEArt image, so a larger entry is corrupt.
constexpr uint64_t kMaxImageSize = 16 * 1024 * 1024;

static bool IsBoxArt(const std::string_view image) {
  return xe::utf8::equal_case(image, "smallboxart.jpg") ||
         xe::utf8::equal_case(image, "largeboxart.jpg");
}

// The NXEArt image that stands in for a title image.
static std::string_view TitleArtName(const std::string_view image) {
  if (IsBoxArt(image)) {
    return "nxeslot.jpg";
  }
  if (xe::utf8::equal_case(image, "background.jpg")) {
    return "nxebg.jpg";
  }
  return {};
}

static bool IsTitleImage(const std::string_view name) {
  return !TitleArtName(name).empty();
}

static bool ReadEntry(vfs::Entry* entry, std::vector<uint8_t>* data) {
  vfs::File* file = nullptr;
  if (!entry || entry->size() > kMaxImageSize ||
      XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
    return false;
  }
  data->resize(entry->size());
  size_t bytes_read = 0;
  const X_STATUS result = file->ReadSync(*data, 0, &bytes_read);
  file->Destroy();
  return XSUCCEEDED(result) && bytes_read == data->size();
}

static bool IsExecutable(const Emulator::FileSignatureType signature) {
  using Signature = Emulator::FileSignatureType;
  switch (signature) {
    case Signature::XEX0:
    case Signature::XEXQ:
    case Signature::XEXH:
    case Signature::XEX25:
    case Signature::XEX1:
    case Signature::XEX2:
    case Signature::ELF:
    case Signature::XBE:
      return true;
    default:
      return false;
  }
}

// Fills the empty |images| from the NXEArt package |art|.
static void ReadArt(vfs::XContentContainerDevice* art,
                    std::vector<std::vector<uint8_t>>* images) {
  if (!art || !art->Initialize()) {
    return;
  }
  for (size_t i = 0; i < images->size(); ++i) {
    if ((*images)[i].empty()) {
      ReadEntry(static_cast<vfs::Device&>(*art).ResolvePath(
                    TitleArtName(kReleaseImages[i])),
                &(*images)[i]);
    }
  }
}

// Fills the empty |images| from the NXEArt package of the release at |path|.
// False when the release couldn't be opened to look.
static bool ReadReleaseArt(Emulator* emulator,
                           const std::filesystem::path& path,
                           std::vector<std::vector<uint8_t>>* images) {
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(path, ec);
  if (folder || IsExecutable(emulator->GetFileSignature(path))) {
    // An unpacked release keeps it beside its executable. Mounting the folder
    // would list every file in the game.
    const auto dir = folder ? path : path.parent_path();
    if (!std::filesystem::is_directory(dir, ec)) {
      return false;
    }
    const auto package = xe::filesystem::FindChildIgnoringCase(dir, "NXEArt");
    if (!package.empty()) {
      ReadArt(
          vfs::XContentContainerDevice::CreateContentDevice("", package).get(),
          images);
    }
    return true;
  }
  const auto release = emulator->CreateVfsDevice(path, "");
  if (!release || !release->Initialize()) {
    return false;
  }
  if (vfs::Entry* package = release->ResolvePath("NXEArt")) {
    ReadArt(
        vfs::XContentContainerDevice::CreateContentDevice("", package).get(),
        images);
  }
  return true;
}

// Keeps the NXEArt images of |title_id|'s game library releases in |folder|,
// which is left empty when they have none. It isn't made while a release
// can't be opened or an image can't be written, so the next launch looks
// again.
static void MakeReleaseImages(Emulator* emulator, const uint32_t title_id,
                              const std::filesystem::path& folder) {
  std::error_code ec;
  if (std::filesystem::exists(folder, ec)) {
    return;
  }
  const auto titles = emulator->LibraryTitles();
  if (!titles) {
    return;
  }
  std::vector<std::vector<uint8_t>> images(std::size(kReleaseImages));
  bool looked = false;
  for (const auto& title : *titles) {
    if (title.title_id == title_id) {
      if (!ReadReleaseArt(emulator, title.path, &images)) {
        return;
      }
      looked = true;
    }
  }
  if (!looked) {
    return;
  }
  // Written beside |folder| and renamed into place, so a write cut short isn't
  // kept as the title's images.
  auto partial = folder;
  partial += ".partial";
  std::filesystem::remove_all(partial, ec);
  std::filesystem::create_directories(partial, ec);
  for (size_t i = 0; i < images.size(); ++i) {
    if (!images[i].empty() && !xe::filesystem::WriteAllBytes(
                                  partial / kReleaseImages[i], images[i])) {
      std::filesystem::remove_all(partial, ec);
      return;
    }
  }
  std::filesystem::rename(partial, folder, ec);
  if (ec) {
    std::filesystem::remove_all(partial, ec);
  }
}

// The first existing |file| of |title_id|'s game library releases, empty when
// none has one.
static std::filesystem::path FindReleaseFile(
    Emulator* emulator, const uint32_t title_id,
    std::filesystem::path Emulator::LibraryTitle::* file) {
  const auto titles = emulator->LibraryTitles();
  if (!titles) {
    return {};
  }
  std::error_code ec;
  for (const auto& title : *titles) {
    const std::filesystem::path& path = title.*file;
    if (title.title_id == title_id && !path.empty() &&
        std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return {};
}

TitleImageDevice::TitleImageDevice(const std::string_view mount_path,
                                   const std::filesystem::path& cache_root,
                                   Emulator* emulator)
    : HostPathDevice(mount_path, cache_root / "title_images", false),
      cache_root_(cache_root),
      emulator_(emulator) {}

bool TitleImageDevice::Initialize() {
  // Made again each launch, so box art downloaded since replaces what stood
  // in for it.
  std::error_code ec;
  std::filesystem::remove_all(host_path(), ec);
  return HostPathDevice::Initialize();
}

vfs::Entry* TitleImageDevice::ResolvePath(const std::string_view path) {
  auto parts = xe::utf8::split_path(path);
  // An image is in its title's folder, which is made with it.
  if (parts.size() >= 2 && IsTitleImage(parts.back())) {
    vfs::Entry* folder = ResolvePath(xe::utf8::find_base_guest_path(path));
    return folder ? folder->GetChild(parts.back()) : nullptr;
  }
  uint32_t title_id = 0;
  const bool title_folder =
      !parts.empty() && parts.size() <= 2 && parts[0].size() == 8 &&
      std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(),
                      title_id, 16)
              .ptr == parts[0].data() + parts[0].size();
  // The file system resolves under its global lock, so making a folder holds
  // up its other calls.
  auto global_lock = global_critical_region_.Acquire();
  if (!title_folder || !made_.insert(xe::utf8::lower_ascii(path)).second) {
    return HostPathDevice::ResolvePath(path);
  }

  // Opens resolve the folder before the image in it, so a title's folder is
  // made with every image its form is asked for.
  const std::span<const std::string_view> names =
      parts.size() == 1 ? std::span<const std::string_view>(kLibraryImages)
                        : std::span<const std::string_view>(kDashboardImages);
  const auto cover = FindReleaseFile(emulator_, title_id,
                                     &Emulator::LibraryTitle::boxart_path);
  const auto release_images =
      cache_root_ / "title_art" / fmt::format("{:08X}", title_id);
  if (cover.empty() ||
      std::ranges::any_of(names, [](auto name) { return !IsBoxArt(name); })) {
    MakeReleaseImages(emulator_, title_id, release_images);
  }
  std::filesystem::path icon;
  std::error_code ec;
  std::vector<std::filesystem::path> sources(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    if (!IsBoxArt(names[i])) {
      sources[i] = release_images / names[i];
    } else if (!cover.empty()) {
      sources[i] = cover;
      continue;
    } else {
      sources[i] = release_images / kReleaseImages[0];
    }
    if (std::filesystem::exists(sources[i], ec)) {
      continue;
    }
    sources[i].clear();
    if (IsBoxArt(names[i])) {
      if (icon.empty()) {
        icon = FindReleaseFile(emulator_, title_id,
                               &Emulator::LibraryTitle::icon_path);
      }
      sources[i] = icon;
    }
  }

  if (std::ranges::all_of(sources,
                          [](const auto& source) { return source.empty(); })) {
    return HostPathDevice::ResolvePath(path);
  }
  vfs::Entry* folder = HostPathDevice::ResolvePath("");
  for (size_t i = 0; folder && i < parts.size(); ++i) {
    vfs::Entry* child = folder->GetChild(parts[i]);
    folder = child
                 ? child
                 : folder->CreateEntry(parts[i], vfs::kFileAttributeDirectory);
  }
  if (!folder) {
    return nullptr;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (sources[i].empty() || folder->GetChild(names[i])) {
      continue;
    }
    vfs::Entry* entry =
        folder->CreateEntry(names[i], vfs::kFileAttributeNormal);
    if (!entry) {
      continue;
    }
    if (!std::filesystem::copy_file(
            sources[i], static_cast<vfs::HostPathEntry*>(entry)->host_path(),
            std::filesystem::copy_options::overwrite_existing, ec)) {
      folder->Delete(entry);
      continue;
    }
    entry->update();
  }
  XELOGI("TitleImageDevice: made {} for {:08X}", path, title_id);
  return folder;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
