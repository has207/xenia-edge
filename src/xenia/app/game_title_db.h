/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_TITLE_DB_H_
#define XENIA_APP_GAME_TITLE_DB_H_

#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace app {

// Title metadata from the embedded title databases: x360db's games.json for
// the 360 and assets/xdb/games.json, made from xemu's xdb, for the original
// Xbox.
struct GameTitleInfo {
  std::string name;    // Display title, e.g. "007 Legends".
  std::string boxart;  // Cover art URL; may be empty. Not downloaded here.
};

// Looks up title metadata by title id (also resolves alternative ids).
// Returns nullptr when the id is unknown. The pointer is stable for the
// lifetime of the process.
const GameTitleInfo* GetGameTitleInfo(uint32_t title_id);

// Returns every title id the game is known by (primary id first, then its
// alternative ids), or nullptr if the id is unknown. The pointer is stable
// for the lifetime of the process. Useful for resolving data recorded under a
// sibling id, e.g. compat state stored under a different release's id.
const std::vector<uint32_t>* GetTitleIdGroup(uint32_t title_id);

// Where the game's box art can be downloaded, best first. For a 360 game that
// is x360db's own copy on its GitHub Pages site, then on GitHub, then the
// marketplace URL it was taken from. For an original Xbox game it is xdb's
// cover. Empty when the id is unknown.
std::vector<std::string> GetBoxArtUrls(uint32_t title_id);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_TITLE_DB_H_
