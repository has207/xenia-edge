/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_COMPAT_DISPLAY_WX_H_
#define XENIA_UI_COMPAT_DISPLAY_WX_H_

#include <wx/colour.h>
#include <wx/string.h>

#include "xenia/app/game_compat_db.h"

namespace xe {
namespace app {

// Localized label for a compatibility state.
wxString CompatStateName(CompatState state);

// Badge color for a compatibility state.
wxColour CompatColor(CompatState state);

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_COMPAT_DISPLAY_WX_H_
