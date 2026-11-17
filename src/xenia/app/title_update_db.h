/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TITLE_UPDATE_DB_H_
#define XENIA_APP_TITLE_UPDATE_DB_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xe {
namespace app {

// A title update a source lists: the embedded assets/title_updates/
// title_updates.json made by tools/tu-index.py, which only says it exists, or
// xboxunity.net, which has it to download.
struct TitleUpdateDownload {
  // The TU number, 0 when it's unknown.
  uint32_t tu = 0;
  // The version of the release the update applies to, as an xex2_version.
  uint32_t base_version = 0;
  // xboxunity.net's only.
  uint64_t size = 0;
  std::string url;
};

// A title's updates by the media id of the release they're for.
using TitleUpdateDownloads =
    std::unordered_map<uint32_t, std::vector<TitleUpdateDownload>>;

// The updates known to exist for the title, empty when there are none. The
// reference is stable for the lifetime of the process.
const TitleUpdateDownloads& GetKnownTitleUpdates(uint32_t title_id);

// Where xboxunity.net lists the updates for a title.
std::string XboxUnityTitleUpdatesUrl(uint32_t title_id);

// The updates in |json|, xboxunity.net's list for a title. Empty when it lists
// none, nullopt when |json| isn't such a list.
std::optional<TitleUpdateDownloads> ParseXboxUnityTitleUpdates(
    std::string_view json);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_TITLE_UPDATE_DB_H_
