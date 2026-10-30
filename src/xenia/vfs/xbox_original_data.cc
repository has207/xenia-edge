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
  // Half of a surrogate pair on its own would make the conversion throw.
  for (size_t i = 0; i < text.size(); ++i) {
    const bool high = text[i] >= 0xD800 && text[i] <= 0xDBFF;
    const bool low = text[i] >= 0xDC00 && text[i] <= 0xDFFF;
    if (high && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      ++i;
    } else if (high || low) {
      text[i] = char16_t(0xFFFD);
    }
  }
  return xe::to_utf8(text);
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
