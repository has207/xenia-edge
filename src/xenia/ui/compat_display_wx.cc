/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/compat_display_wx.h"

#include <wx/translation.h>

namespace xe {
namespace app {

wxString CompatStateName(CompatState state) {
  switch (state) {
    case CompatState::kPlayable:
      return _("Playable");
    case CompatState::kGameplay:
      return _("Gameplay");
    case CompatState::kLoads:
      return _("Loads");
    case CompatState::kUnplayable:
      return _("Unplayable");
    case CompatState::kUnknown:
    default:
      return _("Unknown");
  }
}

wxColour CompatColor(CompatState state) {
  switch (state) {
    case CompatState::kPlayable:
      return wxColour(80, 200, 90);
    case CompatState::kGameplay:
      return wxColour(230, 200, 60);
    case CompatState::kLoads:
    case CompatState::kUnplayable:
      return wxColour(220, 80, 80);
    case CompatState::kUnknown:
    default:
      return wxColour(140, 140, 140);
  }
}

}  // namespace app
}  // namespace xe
