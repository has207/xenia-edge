/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/xbox_original_data.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

namespace xe {
namespace vfs {

namespace {

// Metadata files hold a few hundred bytes, so a far bigger one isn't one.
constexpr uintmax_t kMaxMetaSize = 0x10000;

// ContentMeta.xbx header fields. The first section holds the names.
constexpr size_t kContentMetaSignature = 0x14;
constexpr size_t kContentMetaNamesOffset = 0x34;
constexpr size_t kContentMetaNamesSize = 0x38;

std::vector<uint8_t> ReadMetaFile(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxMetaSize) {
    return {};
  }
  return xe::filesystem::ReadAllBytes(path);
}

// The names and keys looked up here are ASCII. Folding only ASCII needs no
// decoding, which a host name that isn't valid Unicode would fail.
template <typename Char>
Char FoldAscii(Char c) {
  return c >= 'A' && c <= 'Z' ? Char(c - 'A' + 'a') : c;
}

template <typename Char>
bool EqualsAsciiCase(std::basic_string_view<Char> text,
                     std::string_view ascii) {
  return std::equal(
      text.begin(), text.end(), ascii.begin(), ascii.end(), [](Char a, char b) {
        return FoldAscii(a) == FoldAscii(Char(static_cast<unsigned char>(b)));
      });
}

bool LessAsciiCase(std::string_view a, std::string_view b) {
  return std::lexicographical_compare(
      a.begin(), a.end(), b.begin(), b.end(),
      [](char x, char y) { return FoldAscii(x) < FoldAscii(y); });
}

// Little-endian UTF-16 after a byte order mark. The title skips the first two
// bytes without checking them, and so does this.
std::string DecodeMetaText(std::span<const uint8_t> data) {
  std::u16string text;
  for (size_t i = 2; i + 1 < data.size(); i += 2) {
    const char16_t unit = char16_t(data[i] | (data[i + 1] << 8));
    // What follows the text is padding.
    if (!unit) {
      break;
    }
    text.push_back(unit);
  }
  return xe::to_utf8_lossy(text);
}

// Calls `visit(section, key, value)` for each key=value line.
template <typename Visitor>
void ForEachMetaValue(std::string_view text, Visitor&& visit) {
  std::string_view section;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    std::string_view value = line.substr(equals + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    visit(section, line.substr(0, equals), value);
  }
}

// The section a ContentMeta.xbx keeps `language`'s name under.
std::string_view ContentMetaSection(XLanguage language) {
  switch (language) {
    case XLanguage::kEnglish:
      return "EN";
    case XLanguage::kJapanese:
      return "JA";
    case XLanguage::kGerman:
      return "DE";
    case XLanguage::kFrench:
      return "FR";
    case XLanguage::kSpanish:
      return "ES";
    case XLanguage::kItalian:
      return "IT";
    case XLanguage::kKorean:
      return "KO";
    case XLanguage::kTChinese:
      return "TW";
    default:
      return {};
  }
}

uint32_t LoadLE32(std::span<const uint8_t> data, size_t offset) {
  return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
         (uint32_t(data[offset + 2]) << 16) |
         (uint32_t(data[offset + 3]) << 24);
}

}  // namespace

std::filesystem::path FindXboxOriginalPath(const std::filesystem::path& dir,
                                           std::string_view name) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto file_name = it->path().filename();
    if (EqualsAsciiCase(
            std::basic_string_view<std::filesystem::path::value_type>(
                file_name.native()),
            name)) {
      return it->path();
    }
  }
  return {};
}

namespace {

// A profile's own Compatibility folder is named by its XUID.
std::string ProfileFolderName(uint64_t xuid) {
  return fmt::format("{:016X}", xuid);
}

std::filesystem::path ProfileSaves(const std::filesystem::path& compatibility,
                                   uint64_t xuid) {
  return compatibility / ProfileFolderName(xuid) / "Xbox1" / "UDATA";
}

std::filesystem::path SharedSaves(const std::filesystem::path& compatibility) {
  const auto xbox1 = FindXboxOriginalPath(compatibility, "Xbox1");
  const auto saves = xbox1.empty() ? std::filesystem::path()
                                   : FindXboxOriginalPath(xbox1, "UDATA");
  return saves.empty() ? compatibility / "Xbox1" / "UDATA" : saves;
}

bool AnyProfileHasSaves(const std::filesystem::path& compatibility) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(compatibility, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = xe::path_to_utf8(it->path().filename());
    std::error_code type_ec;
    if (it->is_directory(type_ec) && name.size() == 16 &&
        std::all_of(name.begin(), name.end(),
                    [](char c) { return std::isxdigit(uint8_t(c)); })) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::filesystem::path FindXboxOriginalSaves(
    const std::filesystem::path& compatibility, uint64_t xuid) {
  if (!xuid) {
    return SharedSaves(compatibility);
  }
  const auto own = ProfileSaves(compatibility, xuid);
  std::error_code ec;
  if (std::filesystem::exists(own, ec) || AnyProfileHasSaves(compatibility)) {
    return own;
  }
  return SharedSaves(compatibility);
}

std::filesystem::path PrepareXboxOriginalSaves(
    const std::filesystem::path& compatibility, uint64_t xuid) {
  if (!xuid) {
    return SharedSaves(compatibility);
  }
  // An open of the mount's own path needs the folder it is mounted in.
  std::error_code ec;
  if (FindXboxOriginalPath(compatibility, "Xbox1").empty()) {
    std::filesystem::create_directories(compatibility / "Xbox1", ec);
  }
  const auto own = ProfileSaves(compatibility, xuid);
  if (std::filesystem::exists(own, ec)) {
    return own;
  }
  const auto shared = SharedSaves(compatibility);
  if (!AnyProfileHasSaves(compatibility) &&
      std::filesystem::exists(shared, ec)) {
    // Copied beside it and moved in whole, so a copy that fails leaves no
    // profile folder and the next launch tries again.
    const auto copying = compatibility / (ProfileFolderName(xuid) + ".copying");
    std::filesystem::remove_all(copying, ec);
    if (!ec) {
      std::filesystem::create_directories(copying / "Xbox1", ec);
    }
    if (!ec) {
      std::filesystem::copy(shared, copying / "Xbox1" / "UDATA",
                            std::filesystem::copy_options::recursive, ec);
    }
    if (!ec) {
      std::filesystem::rename(copying, compatibility / ProfileFolderName(xuid),
                              ec);
    }
    if (ec) {
      XELOGE("Copying the shared original Xbox saves for {:016X} failed: {}",
             xuid, ec.message());
      std::error_code cleanup_ec;
      std::filesystem::remove_all(copying, cleanup_ec);
      return {};
    }
    XELOGI(
        "Profile {:016X} starts with a copy of the shared original Xbox saves",
        xuid);
    return own;
  }
  std::filesystem::create_directories(own, ec);
  return ec ? std::filesystem::path() : own;
}

std::vector<XboxOriginalSave> ListXboxOriginalSaves(
    const std::filesystem::path& title_dir) {
  std::vector<XboxOriginalSave> saves;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(title_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) {
      continue;
    }
    XboxOriginalSave save;
    save.path = it->path();
    const auto meta = FindXboxOriginalPath(save.path, "SaveMeta.xbx");
    if (!meta.empty()) {
      save.name = ReadSaveMetaName(ReadMetaFile(meta));
    }
    if (save.name.empty()) {
      XELOGW(
          "Original Xbox save folder {} has no name in SaveMeta.xbx so the "
          "game can't save to it until it is deleted",
          xe::strip_pii(xe::path_to_utf8(save.path)));
      continue;
    }
    saves.push_back(std::move(save));
  }
  std::sort(saves.begin(), saves.end(),
            [](const XboxOriginalSave& a, const XboxOriginalSave& b) {
              if (LessAsciiCase(a.name, b.name)) {
                return true;
              }
              if (LessAsciiCase(b.name, a.name)) {
                return false;
              }
              return a.path < b.path;
            });
  return saves;
}

std::vector<XboxOriginalContent> ListXboxOriginalContent(
    const std::filesystem::path& content_dir, XLanguage language) {
  std::vector<XboxOriginalContent> packages;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(content_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) {
      continue;
    }
    XboxOriginalContent package;
    package.path = it->path();
    const auto meta = FindXboxOriginalPath(package.path, "ContentMeta.xbx");
    if (!meta.empty()) {
      package.name = ReadContentMetaName(ReadMetaFile(meta), language);
    }
    packages.push_back(std::move(package));
  }
  std::sort(packages.begin(), packages.end(),
            [](const XboxOriginalContent& a, const XboxOriginalContent& b) {
              return a.path < b.path;
            });
  return packages;
}

std::string ReadSaveMetaName(std::span<const uint8_t> data) {
  std::string name;
  ForEachMetaValue(
      DecodeMetaText(data),
      [&](std::string_view, std::string_view key, std::string_view value) {
        if (name.empty() && EqualsAsciiCase(key, "Name")) {
          name = value;
        }
      });
  return name;
}

std::string ReadContentMetaName(std::span<const uint8_t> data,
                                XLanguage language) {
  if (data.size() < kContentMetaNamesSize + 4 ||
      std::memcmp(data.data() + kContentMetaSignature, "XCSF", 4) != 0) {
    return {};
  }
  const size_t offset = LoadLE32(data, kContentMetaNamesOffset);
  const size_t size = LoadLE32(data, kContentMetaNamesSize);
  if (offset > data.size() || size > data.size() - offset) {
    return {};
  }

  // The language asked for, then the default, then English, then any.
  const std::string_view preferred[] = {ContentMetaSection(language), "Default",
                                        "EN"};
  std::string name;
  size_t name_rank = std::size(preferred) + 1;
  ForEachMetaValue(DecodeMetaText(data.subspan(offset, size)),
                   [&](std::string_view section, std::string_view key,
                       std::string_view value) {
                     if (value.empty() || !EqualsAsciiCase(key, "Name")) {
                       return;
                     }
                     size_t rank = 0;
                     while (rank < std::size(preferred) &&
                            (preferred[rank].empty() ||
                             !EqualsAsciiCase(section, preferred[rank]))) {
                       ++rank;
                     }
                     if (rank < name_rank) {
                       name = value;
                       name_rank = rank;
                     }
                   });
  return name;
}

}  // namespace vfs
}  // namespace xe
