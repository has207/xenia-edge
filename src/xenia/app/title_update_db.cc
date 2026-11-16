/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_update_db.h"

#include <charconv>

#include "rapidjson/document.h"
#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"

#include "embedded_bundle_title_update_db.h"

namespace xe {
namespace app {

namespace {

using UpdateIndex = std::unordered_map<uint32_t, TitleUpdateDownloads>;

std::optional<uint32_t> ParseHex32(std::string_view hex) {
  uint32_t value = 0;
  const auto result =
      std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
  if (hex.size() != 8 || result.ec != std::errc() ||
      result.ptr != hex.data() + hex.size()) {
    return std::nullopt;
  }
  return value;
}

// "a.b.c.d" in xex2_version's bit layout.
std::optional<uint32_t> ParseVersion(std::string_view text) {
  constexpr uint32_t kLimits[] = {0xF, 0xF, 0xFFFF, 0xFF};
  constexpr uint32_t kShifts[] = {28, 24, 8, 0};
  uint32_t version = 0;
  const char* p = text.data();
  const char* end = text.data() + text.size();
  for (int i = 0; i < 4; ++i) {
    if (i && (p == end || *p++ != '.')) {
      return std::nullopt;
    }
    uint32_t part = 0;
    const auto result = std::from_chars(p, end, part);
    if (result.ec != std::errc() || part > kLimits[i]) {
      return std::nullopt;
    }
    version |= part << kShifts[i];
    p = result.ptr;
  }
  if (p != end) {
    return std::nullopt;
  }
  return version;
}

std::optional<uint64_t> ParseDecimal(std::string_view text) {
  uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() ||
      result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string_view> StringMember(const rapidjson::Value& object,
                                             const char* name) {
  if (!object.IsObject()) {
    return std::nullopt;
  }
  const auto member = object.FindMember(name);
  if (member == object.MemberEnd() || !member->value.IsString()) {
    return std::nullopt;
  }
  return std::string_view(member->value.GetString(),
                          member->value.GetStringLength());
}

// A decimal number given either as a JSON number or as a string.
std::optional<uint64_t> NumberMember(const rapidjson::Value& object,
                                     const char* name) {
  if (!object.IsObject()) {
    return std::nullopt;
  }
  const auto member = object.FindMember(name);
  if (member == object.MemberEnd()) {
    return std::nullopt;
  }
  if (member->value.IsUint64()) {
    return member->value.GetUint64();
  }
  if (!member->value.IsString()) {
    return std::nullopt;
  }
  return ParseDecimal(std::string_view(member->value.GetString(),
                                       member->value.GetStringLength()));
}

std::optional<TitleUpdateDownload> ParseUpdate(const rapidjson::Value& entry) {
  if (!entry.IsObject()) {
    return std::nullopt;
  }
  auto base_version = entry.FindMember("base_version");
  auto size = entry.FindMember("size");
  auto url = entry.FindMember("url");
  if (base_version == entry.MemberEnd() || !base_version->value.IsString() ||
      size == entry.MemberEnd() || !size->value.IsUint64() ||
      url == entry.MemberEnd() || !url->value.IsString()) {
    return std::nullopt;
  }
  TitleUpdateDownload update;
  const auto version = ParseVersion(std::string_view(
      base_version->value.GetString(), base_version->value.GetStringLength()));
  if (!version) {
    return std::nullopt;
  }
  update.base_version = *version;
  update.size = size->value.GetUint64();
  update.url.assign(url->value.GetString(), url->value.GetStringLength());
  auto tu = entry.FindMember("tu");
  if (tu != entry.MemberEnd() && tu->value.IsUint()) {
    update.tu = tu->value.GetUint();
  }
  return update;
}

const UpdateIndex& GetUpdateIndex() {
  static const UpdateIndex index = []() {
    UpdateIndex idx;
    xe::EmbeddedBundle bundle(xe::embedded_bundle_title_update_db::kBundleData,
                              xe::embedded_bundle_title_update_db::kBundleSize);
    if (!bundle.ok()) {
      XELOGE("TitleUpdateDb: bundle decompress failed");
      return idx;
    }
    bundle.ForEach([&](std::string_view name, std::string_view data) {
      if (name != "title_updates.json") {
        return;
      }
      rapidjson::Document doc;
      doc.Parse(data.data(), data.size());
      if (doc.HasParseError() || !doc.IsObject()) {
        XELOGE("TitleUpdateDb: {} parse failed", name);
        return;
      }
      for (const auto& title : doc.GetObject()) {
        const auto title_id = ParseHex32(std::string_view(
            title.name.GetString(), title.name.GetStringLength()));
        if (!title_id || !title.value.IsObject()) {
          continue;
        }
        for (const auto& media : title.value.GetObject()) {
          const auto media_id = ParseHex32(std::string_view(
              media.name.GetString(), media.name.GetStringLength()));
          if (!media_id || !media.value.IsArray()) {
            continue;
          }
          for (const auto& entry : media.value.GetArray()) {
            if (auto update = ParseUpdate(entry)) {
              idx[*title_id][*media_id].push_back(std::move(*update));
            }
          }
        }
      }
    });
    return idx;
  }();
  return index;
}

}  // namespace

const TitleUpdateDownloads& GetTitleUpdateDownloads(uint32_t title_id) {
  static const TitleUpdateDownloads none;
  const auto& index = GetUpdateIndex();
  const auto it = index.find(title_id);
  return it == index.end() ? none : it->second;
}

std::string XboxUnityTitleUpdatesUrl(uint32_t title_id) {
  return fmt::format(
      "https://xboxunity.net/Resources/Lib/TitleUpdateInfo.php?titleid={:08X}",
      title_id);
}

std::optional<TitleUpdateDownloads> ParseXboxUnityTitleUpdates(
    std::string_view json) {
  rapidjson::Document doc;
  // Iterative, since the answer comes off the network and nesting deep enough
  // would overflow the stack of the recursive parser.
  doc.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  TitleUpdateDownloads downloads;
  const auto medias = doc.FindMember("MediaIDS");
  if (medias == doc.MemberEnd()) {
    // What a title without updates gets.
    const auto updates = doc.FindMember("Updates");
    if (updates != doc.MemberEnd() && updates->value.IsArray() &&
        updates->value.Empty()) {
      return downloads;
    }
    return std::nullopt;
  }
  if (!medias->value.IsArray()) {
    return std::nullopt;
  }
  size_t listed = 0;
  for (const auto& media : medias->value.GetArray()) {
    if (!media.IsObject()) {
      continue;
    }
    const auto updates = media.FindMember("Updates");
    if (updates == media.MemberEnd() || !updates->value.IsArray()) {
      continue;
    }
    listed += updates->value.Size();
    const auto media_id = StringMember(media, "MediaID");
    const auto parsed_media_id =
        media_id ? ParseHex32(*media_id) : std::nullopt;
    if (!parsed_media_id) {
      continue;
    }
    for (const auto& entry : updates->value.GetArray()) {
      const auto id = NumberMember(entry, "TitleUpdateID");
      const auto tu = NumberMember(entry, "Version");
      const auto base_version = StringMember(entry, "BaseVersion");
      // In KiB, which a package's 4 KiB blocks make exact.
      const auto size_kib = NumberMember(entry, "Size");
      const auto base = base_version ? ParseHex32(*base_version) : std::nullopt;
      if (!id || !tu || *tu > UINT32_MAX || !base || !size_kib ||
          *size_kib > UINT64_MAX / 1024) {
        continue;
      }
      TitleUpdateDownload update;
      update.tu = uint32_t(*tu);
      update.base_version = *base;
      update.size = *size_kib * 1024;
      update.url = fmt::format(
          "https://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid={}", *id);
      downloads[*parsed_media_id].push_back(std::move(update));
    }
  }
  // Updates listed in a shape this doesn't read aren't the same as none.
  if (listed && downloads.empty()) {
    return std::nullopt;
  }
  return downloads;
}

}  // namespace app
}  // namespace xe
