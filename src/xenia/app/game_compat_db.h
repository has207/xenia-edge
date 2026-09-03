/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_COMPAT_DB_H_
#define XENIA_APP_GAME_COMPAT_DB_H_

#include <cstdint>
#include <string>

namespace xe {
namespace app {

// Order encodes "better compat" — higher value sorts above.
enum class CompatState : uint8_t {
  kUnknown = 0,
  kUnplayable,
  kLoads,
  kGameplay,
  kPlayable,
};

// Both trackers carry a report for some titles, so their links are kept
// apart rather than merged: a search of the other tracker is not a substitute
// for a report on it.
struct CompatUrls {
  std::string canary;
  std::string master;

  bool empty() const { return canary.empty() && master.empty(); }
};

struct CompatEntry {
  CompatState state = CompatState::kUnknown;
  CompatUrls urls;
};

CompatState GetCompatState(uint32_t title_id);

// Issue URLs from the compatibility database for a title, each empty when
// that tracker has no report for it.
CompatUrls GetCompatUrls(uint32_t title_id);

// GitHub issue-search query fragment for a title. Returns the title id, or all
// sibling ids (x360db alternative ids) OR'd in parentheses when present.
// URL-encoded for use after the "is:open " term.
std::string CompatSearchQuery(uint32_t title_id);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_COMPAT_DB_H_
