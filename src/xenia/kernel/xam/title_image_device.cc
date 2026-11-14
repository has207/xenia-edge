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
#include <memory>
#include <span>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
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
  // kept as the title's images. Named for the thread, as another opener can be
  // making the same folder.
  auto partial = folder;
  partial +=
      fmt::format(".{:X}.partial", xe::threading::current_thread_system_id());
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

static bool HasRelease(Emulator* emulator, const uint32_t title_id) {
  const auto titles = emulator->LibraryTitles();
  return titles && std::ranges::any_of(*titles, [&](const auto& title) {
           return title.title_id == title_id;
         });
}

// What stands in for |image| once the releases were looked at, their image or
// else the icon for box art. Empty when there's none.
static std::filesystem::path LookedImageSource(
    Emulator* emulator, const uint32_t title_id,
    const std::filesystem::path& release_images, const std::string_view image) {
  std::error_code ec;
  auto source = release_images / (IsBoxArt(image) ? kReleaseImages[0] : image);
  if (std::filesystem::exists(source, ec)) {
    return source;
  }
  if (IsBoxArt(image)) {
    return FindReleaseFile(emulator, title_id,
                           &Emulator::LibraryTitle::icon_path);
  }
  return {};
}

// An image only the title's releases can give, read from them when it's first
// opened, which is outside the file system lock.
class TitleImageEntry final : public vfs::HostPathEntry {
 public:
  TitleImageEntry(TitleImageDevice* device, vfs::Entry* parent,
                  const std::string_view path,
                  const std::filesystem::path& host_path,
                  const xe::filesystem::FileInfo& file_info,
                  const uint32_t title_id)
      : HostPathEntry(device, parent, path, host_path), title_id_(title_id) {
    SetInfo(file_info);
  }

  uint32_t title_id() const { return title_id_; }

  // Held open while the release is read, so a delete can't free the entry.
  X_STATUS Open(uint32_t desired_access, vfs::File** out_file) override {
    RetainFile();
    X_STATUS status = X_STATUS_OBJECT_NAME_NOT_FOUND;
    if (static_cast<TitleImageDevice*>(device())->MakeImage(*this)) {
      status = HostPathEntry::Open(desired_access, out_file);
    }
    ReleaseFile();
    return status;
  }

  // A query reports the image's size, so it's made first.
  void update() override {
    RetainFile();
    static_cast<TitleImageDevice*>(device())->MakeImage(*this);
    HostPathEntry::update();
    ReleaseFile();
  }

 private:
  friend class TitleImageDevice;

  const uint32_t title_id_;
  // Guarded by the device's images_mutex_.
  bool filled_ = false;
};

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
  // A launch cut short leaves its half-written release images behind.
  std::vector<std::filesystem::path> partials;
  for (std::filesystem::directory_iterator it(cache_root_ / "title_art", ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".partial") {
      partials.push_back(it->path());
    }
  }
  for (const auto& partial : partials) {
    std::filesystem::remove_all(partial, ec);
  }
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
  const auto release_images = ReleaseImagesFolder(title_id);
  std::error_code ec;
  // Releases not looked at yet are read when their image is opened.
  const bool pending = !std::filesystem::exists(release_images, ec) &&
                       HasRelease(emulator_, title_id);
  std::vector<std::filesystem::path> sources(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    if (IsBoxArt(names[i]) && !cover.empty()) {
      sources[i] = cover;
    } else if (!pending) {
      sources[i] =
          LookedImageSource(emulator_, title_id, release_images, names[i]);
    }
  }

  if (!pending && std::ranges::all_of(sources, [](const auto& source) {
        return source.empty();
      })) {
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
    if (folder->GetChild(names[i])) {
      continue;
    }
    if (sources[i].empty()) {
      if (!pending) {
        continue;
      }
      auto* host_folder = static_cast<vfs::HostPathEntry*>(folder);
      const auto host_path = host_folder->host_path() / names[i];
      if (!xe::filesystem::CreateEmptyFile(host_path)) {
        continue;
      }
      const auto file_info = xe::filesystem::GetInfo(host_path);
      if (file_info) {
        AddEntry(host_folder,
                 std::make_unique<TitleImageEntry>(
                     this, folder,
                     xe::utf8::join_guest_paths(folder->path(), names[i]),
                     host_path, *file_info, title_id));
      }
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

std::filesystem::path TitleImageDevice::ReleaseImagesFolder(
    const uint32_t title_id) const {
  return cache_root_ / "title_art" / fmt::format("{:08X}", title_id);
}

bool TitleImageDevice::MakeImage(TitleImageEntry& entry) {
  const uint32_t title_id = entry.title_id();
  bool read_releases;
  {
    std::lock_guard lock(images_mutex_);
    if (entry.filled_) {
      return true;
    }
    read_releases = !unreadable_.contains(title_id);
  }
  const auto release_images = ReleaseImagesFolder(title_id);
  if (read_releases) {
    MakeReleaseImages(emulator_, title_id, release_images);
  }
  std::error_code ec;
  const bool looked = std::filesystem::exists(release_images, ec);
  const auto source =
      LookedImageSource(emulator_, title_id, release_images, entry.name());
  std::lock_guard lock(images_mutex_);
  if (!looked) {
    // Read again next launch, not on every open of this one.
    unreadable_.insert(title_id);
  }
  if (!entry.filled_ && !source.empty() &&
      std::filesystem::copy_file(
          source, entry.host_path(),
          std::filesystem::copy_options::overwrite_existing, ec)) {
    // The plain refresh, as its own update() comes back here.
    entry.HostPathEntry::update();
    entry.filled_ = true;
    XELOGI("TitleImageDevice: made {} for {:08X}", entry.path(), title_id);
  }
  return entry.filled_;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
