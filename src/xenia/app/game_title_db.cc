/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_title_db.h"

#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rapidjson/document.h"

#include "xenia/app/title_id_util.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"

#include "embedded_bundle_game_db.h"

namespace xe {
namespace app {

namespace {

// One x360db game: its metadata plus every title id it is known by.
struct Record {
  GameTitleInfo info;
  std::vector<uint32_t> ids;  // Primary id first, then alternative ids.
};

struct TitleIndex {
  // Owns the records; pointers into it stay valid (deque never reallocates).
  std::deque<Record> records;
  std::unordered_map<uint32_t, const Record*> by_id;
};

std::string GetMemberString(const rapidjson::Value& entry, const char* key) {
  auto it = entry.FindMember(key);
  if (it == entry.MemberEnd() || !it->value.IsString()) {
    return std::string();
  }
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

const TitleIndex& GetTitleIndex() {
  static const TitleIndex index = []() {
    TitleIndex idx;
    xe::EmbeddedBundle bundle(xe::embedded_bundle_game_db::kBundleData,
                              xe::embedded_bundle_game_db::kBundleSize);
    if (!bundle.ok()) {
      XELOGE("TitleDb: bundle decompress failed");
      return idx;
    }
    bundle.ForEach([&](std::string_view name, std::string_view data) {
      if (name != "games.json") {
        return;
      }
      rapidjson::Document doc;
      doc.Parse(data.data(), data.size());
      if (doc.HasParseError() || !doc.IsArray()) {
        XELOGE("TitleDb: {} parse failed", name);
        return;
      }
      for (const auto& entry : doc.GetArray()) {
        if (!entry.IsObject()) {
          continue;
        }
        auto id_it = entry.FindMember("id");
        if (id_it == entry.MemberEnd() || !id_it->value.IsString()) {
          continue;
        }
        uint32_t title_id = ParseHexTitleId(id_it->value.GetString(),
                                            id_it->value.GetStringLength());
        if (title_id == 0) {
          continue;
        }
        Record& rec = idx.records.emplace_back();
        rec.info.name = GetMemberString(entry, "title");
        rec.info.boxart = GetMemberString(entry, "boxart");
        rec.ids.push_back(title_id);
        // First writer wins so a primary id is never shadowed by another
        // game's alternative id.
        idx.by_id.emplace(title_id, &rec);
        auto alt_it = entry.FindMember("alternative_id");
        if (alt_it != entry.MemberEnd() && alt_it->value.IsArray()) {
          for (const auto& alt : alt_it->value.GetArray()) {
            if (!alt.IsString()) {
              continue;
            }
            uint32_t alt_id =
                ParseHexTitleId(alt.GetString(), alt.GetStringLength());
            if (alt_id != 0) {
              rec.ids.push_back(alt_id);
              idx.by_id.emplace(alt_id, &rec);
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

const GameTitleInfo* GetGameTitleInfo(uint32_t title_id) {
  if (title_id == 0) {
    return nullptr;
  }
  const auto& idx = GetTitleIndex();
  auto it = idx.by_id.find(title_id);
  if (it == idx.by_id.end()) {
    return nullptr;
  }
  return &it->second->info;
}

const std::vector<uint32_t>* GetTitleIdGroup(uint32_t title_id) {
  if (title_id == 0) {
    return nullptr;
  }
  const auto& idx = GetTitleIndex();
  auto it = idx.by_id.find(title_id);
  if (it == idx.by_id.end()) {
    return nullptr;
  }
  return &it->second->ids;
}

std::vector<std::string> GetBoxArtUrls(uint32_t title_id) {
  const GameTitleInfo* info = GetGameTitleInfo(title_id);
  const std::vector<uint32_t>* ids = GetTitleIdGroup(title_id);
  if (!info || !ids) {
    return {};
  }
  // x360db keeps the artwork under the game's primary id.
  const uint32_t id = ids->front();
  std::vector<std::string> urls = {
      fmt::format("https://xenia-manager.github.io/x360db/titles/{:08X}/"
                  "artwork/boxart.jpg",
                  id),
      fmt::format("https://raw.githubusercontent.com/xenia-manager/x360db/"
                  "main/titles/{:08X}/artwork/boxart.jpg",
                  id),
  };
  if (!info->boxart.empty()) {
    urls.push_back(info->boxart);
  }
  return urls;
}

}  // namespace app
}  // namespace xe
