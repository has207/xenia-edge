/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_compat_db.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rapidjson/document.h"

#include "xenia/app/game_title_db.h"
#include "xenia/app/title_id_util.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"

#include "embedded_bundle_game_compat.h"

namespace xe {
namespace app {

namespace {

CompatState ParseState(std::string_view s) {
  if (s == "Playable") {
    return CompatState::kPlayable;
  }
  if (s == "Gameplay") {
    return CompatState::kGameplay;
  }
  if (s == "Loads") {
    return CompatState::kLoads;
  }
  if (s == "Unplayable") {
    return CompatState::kUnplayable;
  }
  return CompatState::kUnknown;
}

const std::unordered_map<uint32_t, CompatEntry>& GetCompatIndex() {
  static const std::unordered_map<uint32_t, CompatEntry> index = []() {
    std::unordered_map<uint32_t, CompatEntry> map;
    xe::EmbeddedBundle bundle(xe::embedded_bundle_game_compat::kBundleData,
                              xe::embedded_bundle_game_compat::kBundleSize);
    if (!bundle.ok()) {
      XELOGE("CompatDb: bundle decompress failed");
      return map;
    }
    // Merge sources with the most-optimistic state across all of them winning.
    bundle.ForEach([&](std::string_view name, std::string_view data) {
      if (name != "compatibility_data.json" && name != "master.json") {
        return;
      }
      const bool is_master = name == "master.json";
      rapidjson::Document doc;
      doc.Parse(data.data(), data.size());
      if (doc.HasParseError() || !doc.IsArray()) {
        XELOGE("CompatDb: {} parse failed", name);
        return;
      }
      for (const auto& entry : doc.GetArray()) {
        if (!entry.IsObject()) {
          continue;
        }
        auto id_it = entry.FindMember("id");
        auto state_it = entry.FindMember("state");
        if (id_it == entry.MemberEnd() || state_it == entry.MemberEnd()) {
          continue;
        }
        if (!id_it->value.IsString() || !state_it->value.IsString()) {
          continue;
        }
        uint32_t title_id = ParseHexTitleId(id_it->value.GetString(),
                                            id_it->value.GetStringLength());
        if (title_id == 0) {
          continue;
        }
        CompatState s = ParseState(std::string_view(
            state_it->value.GetString(), state_it->value.GetStringLength()));
        auto& slot = map[title_id];
        if (static_cast<uint8_t>(s) > static_cast<uint8_t>(slot.state)) {
          slot.state = s;
        }
        std::string& url = is_master ? slot.urls.master : slot.urls.canary;
        if (url.empty()) {
          auto url_it = entry.FindMember("url");
          if (url_it != entry.MemberEnd() && url_it->value.IsString()) {
            url.assign(url_it->value.GetString(),
                       url_it->value.GetStringLength());
          }
        }
      }
    });
    return map;
  }();
  return index;
}

}  // namespace

CompatState GetCompatState(uint32_t title_id) {
  if (title_id == 0) {
    return CompatState::kUnknown;
  }
  const auto& idx = GetCompatIndex();
  auto it = idx.find(title_id);
  if (it != idx.end()) {
    return it->second.state;
  }
  // No direct entry: the game may be tracked under a different release's id.
  // Fall back to the most optimistic compat state among the x360db sibling
  // ids of the same game.
  const std::vector<uint32_t>* group = GetTitleIdGroup(title_id);
  if (group) {
    CompatState best = CompatState::kUnknown;
    for (uint32_t alias : *group) {
      auto alias_it = idx.find(alias);
      if (alias_it != idx.end() &&
          static_cast<uint8_t>(alias_it->second.state) >
              static_cast<uint8_t>(best)) {
        best = alias_it->second.state;
      }
    }
    return best;
  }
  return CompatState::kUnknown;
}

CompatUrls GetCompatUrls(uint32_t title_id) {
  if (title_id == 0) {
    return {};
  }
  const auto& idx = GetCompatIndex();
  auto it = idx.find(title_id);
  if (it != idx.end() && !it->second.urls.empty()) {
    return it->second.urls;
  }
  // No direct entry: the game may be tracked under a different release's id.
  const std::vector<uint32_t>* group = GetTitleIdGroup(title_id);
  if (group) {
    for (uint32_t alias : *group) {
      auto alias_it = idx.find(alias);
      if (alias_it != idx.end() && !alias_it->second.urls.empty()) {
        return alias_it->second.urls;
      }
    }
  }
  return {};
}

std::string CompatSearchQuery(uint32_t title_id) {
  std::string query = fmt::format("{:08X}", title_id);
  const std::vector<uint32_t>* group = GetTitleIdGroup(title_id);
  if (group && group->size() > 1) {
    std::string ids;
    for (uint32_t id : *group) {
      if (!ids.empty()) {
        ids += "+OR+";
      }
      ids += fmt::format("{:08X}", id);
    }
    // Parenthesize the OR group, percent-encoded for the search URL.
    query = "%28" + ids + "%29";
  }
  return query;
}

}  // namespace app
}  // namespace xe
