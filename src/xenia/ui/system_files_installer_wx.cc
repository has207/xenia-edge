/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/system_files_installer_wx.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <wx/log.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/xam/system_manifest.h"
#include "xenia/ui/http_download.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

#include "embedded_system_font.h"

namespace xe {
namespace ui {

namespace {

constexpr std::array<std::string_view, 3> kInstalledFolders = {
    "flash", "sysext", "sysaux"};
constexpr std::string_view kUpdateFolder = "$SystemUpdate/";
constexpr std::string_view kFlashPrefix = "$flash_";
constexpr size_t kFontSignatureOffset = 4;
constexpr size_t kFontSignatureSize = 0x100;
constexpr size_t kFontHeaderSize = 0x118;
// Far above the few hundred KiB the tables of a retail font unpack to.
constexpr size_t kMaxFontTablesSize = 16 * 1024 * 1024;

// The 17559 USB system update.
constexpr char kSystemUpdateUrl[] =
    "https://download.microsoft.com/download/8/f/4/"
    "8f456817-e264-4207-9b95-6efc990fee98/SystemUpdate_17559_USB.zip";
constexpr uint64_t kSystemUpdateSize = 115992188;
constexpr std::string_view kSystemUpdateSha256 =
    "38885a3ab6b9854fb4fa31d5ddf21a4fddfd4cfb25e219060cfb0c2fb31f208f";

// A flash font the update only patches, and the one built to stand in for it.
struct BaseFont {
  std::string_view name;
  std::span<const uint8_t> data;
};

// Whether |name| names a file in the folder it is written to.
bool IsPlainFileName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\:") == std::string_view::npos;
}

// The signature of the font a .xttp patch applies to, from its xttf table.
std::vector<uint8_t> GetPatchedFontSignature(std::span<const uint8_t> patch) {
  if (patch.size() < kFontHeaderSize || std::memcmp(patch.data(), "xttf", 4)) {
    return {};
  }
  const size_t packed_size = xe::load_and_swap<uint32_t>(patch.data() + 0x10C);
  size_t unpacked_size = xe::load_and_swap<uint32_t>(patch.data() + 0x110);
  if (packed_size > patch.size() - kFontHeaderSize ||
      unpacked_size > kMaxFontTablesSize) {
    return {};
  }
  std::vector<uint8_t> sfnt(unpacked_size);
  if (zng_uncompress(sfnt.data(), &unpacked_size,
                     patch.data() + kFontHeaderSize, packed_size) != Z_OK) {
    return {};
  }
  sfnt.resize(unpacked_size);
  if (sfnt.size() < 12) {
    return {};
  }
  const size_t table_count = xe::load_and_swap<uint16_t>(&sfnt[4]);
  for (size_t i = 0; i < table_count && 12 + 16 * i + 16 <= sfnt.size(); ++i) {
    const uint8_t* record = &sfnt[12 + 16 * i];
    const size_t offset = xe::load_and_swap<uint32_t>(record + 8);
    if (std::memcmp(record, "xttf", 4) == 0 &&
        offset + 8 + kFontSignatureSize <= sfnt.size()) {
      return {sfnt.begin() + offset + 8,
              sfnt.begin() + offset + 8 + kFontSignatureSize};
    }
  }
  return {};
}

// Writes the stand-ins for the flash fonts the update only patches.
bool WriteBaseFonts(const std::filesystem::path& flash_path) {
  const std::array fonts = {
      BaseFont{"xenonjklatin.xtt",
               {embedded_system_font::XenonJKLatin_xtt_data,
                embedded_system_font::XenonJKLatin_xtt_size}},
      BaseFont{"xenonclatin.xtt",
               {embedded_system_font::XenonCLatin_xtt_data,
                embedded_system_font::XenonCLatin_xtt_size}},
  };
  for (const auto& font : fonts) {
    const auto font_path = flash_path / font.name;
    std::error_code ec;
    if (std::filesystem::exists(font_path, ec)) {
      continue;
    }
    std::vector<uint8_t> data(font.data.begin(), font.data.end());
    // A patch only attaches to the font whose signature it holds.
    const auto signature = GetPatchedFontSignature(xe::filesystem::ReadAllBytes(
        flash_path / fmt::format("{}p1", font.name)));
    if (signature.size() == kFontSignatureSize) {
      std::ranges::copy(signature, data.begin() + kFontSignatureOffset);
    }
    if (!xe::filesystem::WriteAllBytes(font_path, data)) {
      return false;
    }
  }
  return true;
}

// Copies the flash files out of the update's own package under the names
// flash gives them, with patches in their first slot.
SystemFilesResult ExtractFlashFiles(const std::filesystem::path& package_path,
                                    const std::filesystem::path& flash_path) {
  auto device =
      vfs::XContentContainerDevice::CreateContentDevice("", package_path);
  if (!device || !device->Initialize()) {
    return SystemFilesResult::kReadFailed;
  }
  vfs::Device& package = *device;
  for (const auto& entry : package.ResolvePath("/")->children()) {
    std::string name = entry->name();
    // Only flash files go in, and not the drive firmware or the updater.
    if (!xe::utf8::starts_with_case(name, kFlashPrefix)) {
      continue;
    }
    name.erase(0, kFlashPrefix.size());
    if (xe::utf8::starts_with_case(name, "oddupd") ||
        xe::utf8::starts_with_case(name, "systemupdate")) {
      continue;
    }
    if (!IsPlainFileName(name)) {
      return SystemFilesResult::kNotSystemUpdate;
    }
    if (xe::utf8::ends_with_case(name, ".xexp") ||
        xe::utf8::ends_with_case(name, ".xttp")) {
      name += '1';
    }
    std::vector<uint8_t> data(entry->size());
    if (!data.empty()) {
      vfs::File* file = nullptr;
      if (XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
        return SystemFilesResult::kReadFailed;
      }
      size_t bytes_read = 0;
      const X_STATUS status = file->ReadSync(data, 0, &bytes_read);
      file->Destroy();
      if (XFAILED(status) || bytes_read != data.size()) {
        return SystemFilesResult::kReadFailed;
      }
    }
    if (!xe::filesystem::WriteAllBytes(flash_path / xe::to_path(name), data)) {
      return SystemFilesResult::kWriteFailed;
    }
  }
  return SystemFilesResult::kSuccess;
}

enum class SwapResult {
  kSwapped,
  kUndone,
  kUndoFailed,
};

// Puts the staged |folders| in place of the installed ones, which move into
// |previous_path|. It only renames, which lets a failure undo the moves and
// leave the previous install as it was.
SwapResult SwapInstalledFolders(const std::filesystem::path& system_path,
                                const std::filesystem::path& staging_path,
                                const std::filesystem::path& previous_path,
                                std::span<const std::string_view> folders) {
  std::error_code ec;
  std::filesystem::create_directories(previous_path, ec);
  if (ec) {
    return SwapResult::kUndone;
  }
  std::vector<std::string_view> moved_aside;
  std::vector<std::string_view> replaced;
  auto undo = [&]() {
    bool undone = true;
    std::error_code undo_ec;
    for (const auto folder : replaced) {
      std::filesystem::rename(system_path / folder, staging_path / folder,
                              undo_ec);
    }
    for (const auto folder : moved_aside) {
      std::filesystem::rename(previous_path / folder, system_path / folder,
                              undo_ec);
      if (undo_ec) {
        XELOGE("System files: failed to restore {}: {}", folder,
               undo_ec.message());
        undone = false;
      }
    }
    return undone ? SwapResult::kUndone : SwapResult::kUndoFailed;
  };
  for (const auto folder : folders) {
    const auto installed_path = system_path / folder;
    if (std::filesystem::exists(installed_path, ec)) {
      std::filesystem::rename(installed_path, previous_path / folder, ec);
      if (ec) {
        XELOGE("System files: failed to move {} aside: {}", folder,
               ec.message());
        return undo();
      }
      moved_aside.push_back(folder);
    }
    std::filesystem::rename(staging_path / folder, installed_path, ec);
    if (ec) {
      XELOGE("System files: failed to put {} in place: {}", folder,
             ec.message());
      return undo();
    }
    replaced.push_back(folder);
  }
  return SwapResult::kSwapped;
}

// Unzips the files of the update folder in |zip_path| into |update_path|.
SystemFilesResult UnzipUpdate(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& update_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  // Problems come back as results, not as wx error dialogs.
  wxLogNull no_wx_log;
  wxFFileInputStream input(wxString::FromUTF8(xe::path_to_utf8(zip_path)));
  if (!input.IsOk()) {
    return SystemFilesResult::kReadFailed;
  }
  wxZipInputStream zip(input);
  const int total = zip.GetTotalEntries();
  if (total <= 0) {
    return SystemFilesResult::kNotSystemUpdate;
  }
  size_t done = 0;
  for (std::unique_ptr<wxZipEntry> entry(zip.GetNextEntry()); entry;
       entry.reset(zip.GetNextEntry())) {
    if (!progress(done++, size_t(total))) {
      return SystemFilesResult::kCancelled;
    }
    const std::string name(entry->GetInternalName().utf8_str().data());
    if (entry->IsDir() || !xe::utf8::starts_with_case(name, kUpdateFolder)) {
      continue;
    }
    const std::string file_name = name.substr(kUpdateFolder.size());
    if (!IsPlainFileName(file_name)) {
      return SystemFilesResult::kNotSystemUpdate;
    }
    std::vector<uint8_t> data(size_t(entry->GetSize()));
    zip.Read(data.data(), data.size());
    // wx only checks the CRC once a read runs past the end of the entry.
    if (size_t(zip.LastRead()) != data.size() ||
        zng_crc32_z(0, data.data(), data.size()) != entry->GetCrc()) {
      return SystemFilesResult::kReadFailed;
    }
    if (!xe::filesystem::WriteAllBytes(update_path / xe::to_path(file_name),
                                       data)) {
      return SystemFilesResult::kWriteFailed;
    }
  }
  // The entries end at the end of the central directory, or else at an error.
  if (zip.GetLastError() != wxSTREAM_EOF) {
    return SystemFilesResult::kReadFailed;
  }
  return SystemFilesResult::kSuccess;
}

// Whether the SHA-256 of the file at |path| is |expected_hex|.
bool FileHasSha256(const std::filesystem::path& path,
                   std::string_view expected_hex) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  std::vector<uint8_t> buffer(1024 * 1024);
  size_t read;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) != 0) {
    mbedtls_sha256_update(&context, buffer.data(), read);
  }
  const bool read_failed = std::ferror(file) != 0;
  std::fclose(file);
  uint8_t digest[32];
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  if (read_failed) {
    return false;
  }
  std::string hex;
  for (const uint8_t byte : digest) {
    hex += fmt::format("{:02x}", byte);
  }
  return hex == expected_hex;
}

}  // namespace

bool HasSystemFiles(const std::filesystem::path& system_path) {
  std::error_code ec;
  for (const auto folder : kInstalledFolders) {
    if (std::filesystem::exists(system_path / folder, ec)) {
      return true;
    }
  }
  return false;
}

std::filesystem::path GetUnrestoredSystemFilesPath(
    const std::filesystem::path& system_path) {
  return system_path / "unrestored";
}

std::filesystem::path GetSystemUpdateDownloadPath(
    const std::filesystem::path& system_path) {
  return system_path / "download" / "SystemUpdate_17559_USB.zip";
}

SystemUpdateDownload DownloadSystemUpdate(
    const std::filesystem::path& system_path,
    const std::function<bool(uint64_t received, uint64_t size)>& progress) {
  const auto zip_path = GetSystemUpdateDownloadPath(system_path);
  std::error_code ec;
  // Only a verified download is given the zip's name.
  if (std::filesystem::file_size(zip_path, ec) == kSystemUpdateSize) {
    return SystemUpdateDownload::kDownloaded;
  }
  auto partial_path = zip_path;
  partial_path += ".partial";
  switch (DownloadFile(kSystemUpdateUrl, partial_path,
                       [&progress](uint64_t received, uint64_t) {
                         return progress(std::min(received, kSystemUpdateSize),
                                         kSystemUpdateSize);
                       })) {
    case DownloadResult::kSuccess:
      break;
    case DownloadResult::kCancelled:
      return SystemUpdateDownload::kCancelled;
    case DownloadResult::kFailed:
      return SystemUpdateDownload::kFailed;
    case DownloadResult::kWriteFailed:
      return SystemUpdateDownload::kWriteFailed;
  }
  if (!FileHasSha256(partial_path, kSystemUpdateSha256)) {
    XELOGE("System files: the downloaded update doesn't match its SHA-256");
    std::filesystem::remove(partial_path, ec);
    return SystemUpdateDownload::kNotVerified;
  }
  std::filesystem::rename(partial_path, zip_path, ec);
  if (ec) {
    std::filesystem::remove(partial_path, ec);
    return SystemUpdateDownload::kWriteFailed;
  }
  return SystemUpdateDownload::kDownloaded;
}

SystemFilesResult InstallSystemFiles(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& system_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  const auto& folders = kInstalledFolders;
  const auto staging_path = system_path / "staging";
  const auto unrestored_path = GetUnrestoredSystemFilesPath(system_path);
  const auto update_path = staging_path / "update";
  const auto previous_path = staging_path / "previous";
  const auto flash_path = staging_path / "flash";
  const auto sysext_path = staging_path / "sysext";

  std::error_code ec;
  // A previous install that couldn't be put back stays until it is dealt with.
  if (std::filesystem::exists(unrestored_path, ec)) {
    return SystemFilesResult::kRestoreFailed;
  }
  std::filesystem::remove_all(staging_path, ec);
  if (ec) {
    return SystemFilesResult::kWriteFailed;
  }
  for (const auto folder : folders) {
    if (!std::filesystem::create_directories(staging_path / folder, ec)) {
      return SystemFilesResult::kWriteFailed;
    }
  }
  if (!std::filesystem::create_directories(update_path, ec)) {
    return SystemFilesResult::kWriteFailed;
  }
  auto fail = [&staging_path](SystemFilesResult result) {
    std::error_code remove_ec;
    std::filesystem::remove_all(staging_path, remove_ec);
    return result;
  };

  if (const auto result = UnzipUpdate(zip_path, update_path, progress);
      result != SystemFilesResult::kSuccess) {
    return fail(result);
  }

  const auto manifest = xe::filesystem::ReadAllBytes(
      update_path / kernel::xam::kSystemManifestName);
  const auto system_version = kernel::xam::GetSystemManifestVersion(manifest);
  if (!system_version) {
    return fail(SystemFilesResult::kNotSystemUpdate);
  }

  std::vector<std::filesystem::path> files;
  for (std::filesystem::directory_iterator it(update_path, ec), end;
       !ec && it != end; it.increment(ec)) {
    files.push_back(it->path());
  }
  if (ec) {
    return fail(SystemFilesResult::kReadFailed);
  }
  bool has_flash = false;
  for (const auto& source : files) {
    const std::string name = xe::path_to_utf8(source.filename());
    std::filesystem::path target;
    const auto header =
        vfs::XContentContainerDevice::ReadContainerHeader(source);
    if (xe::utf8::equal_case(name, kernel::xam::kSystemManifestName)) {
      target = sysext_path / source.filename();
    } else if (header && header->content_header.is_magic_valid()) {
      const XContentType type = header->content_metadata.content_type;
      if (type == XContentType::kInstaller) {
        if (const auto result = ExtractFlashFiles(source, flash_path);
            result != SystemFilesResult::kSuccess) {
          return fail(result);
        }
        has_flash = true;
        continue;
      }
      // Shared content of the system, such as the avatar assets.
      const uint32_t title_id =
          header->content_metadata.execution_info.title_id;
      target = sysext_path / "Content" / "0000000000000000" /
               fmt::format("{:08X}", title_id) /
               fmt::format("{:08X}", uint32_t(type)) / source.filename();
    } else {
      const uint32_t version =
          kernel::xam::FindSystemManifestFile(manifest, name)
              .value_or(*system_version);
      target = sysext_path / kernel::xam::SystemExtFolderName(version) /
               source.filename();
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::rename(source, target, ec);
    if (ec) {
      return fail(SystemFilesResult::kWriteFailed);
    }
  }
  if (!has_flash) {
    return fail(SystemFilesResult::kNotSystemUpdate);
  }
  if (!WriteBaseFonts(flash_path)) {
    return fail(SystemFilesResult::kWriteFailed);
  }

  switch (
      SwapInstalledFolders(system_path, staging_path, previous_path, folders)) {
    case SwapResult::kSwapped:
      break;
    case SwapResult::kUndone:
      return fail(SystemFilesResult::kWriteFailed);
    case SwapResult::kUndoFailed:
      std::filesystem::rename(previous_path, unrestored_path, ec);
      XELOGE("System files: the previous install is left in {}",
             xe::strip_pii(
                 xe::path_to_utf8(ec ? previous_path : unrestored_path)));
      return SystemFilesResult::kRestoreFailed;
  }
  // The previous install is in staging now.
  std::filesystem::remove_all(staging_path, ec);
  return SystemFilesResult::kSuccess;
}

}  // namespace ui
}  // namespace xe
