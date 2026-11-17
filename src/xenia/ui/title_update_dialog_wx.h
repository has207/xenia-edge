/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_TITLE_UPDATE_DIALOG_WX_H_
#define XENIA_UI_TITLE_UPDATE_DIALOG_WX_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include <wx/string.h>

#include "xenia/app/game_library.h"
#include "xenia/app/title_update_check.h"

class wxWindow;

namespace xe {
namespace app {

// The release on |disc| of |entry|, read from its file when the library has no
// version for it. Empty for what title updates don't apply to.
std::optional<TitleUpdateRelease> LibraryTitleUpdateRelease(
    const LibraryEntry& entry, const LibraryPath& disc);

// Why the title update package at |package| called |name|, made for
// |package_version|, can't be installed for |version| of the game at |game|,
// empty when it fits. It reads the whole package under a progress dialog.
wxString CheckTitleUpdatePackage(wxWindow* parent,
                                 const std::filesystem::path& package,
                                 const wxString& name, uint32_t package_version,
                                 const std::filesystem::path& game,
                                 uint32_t version);

// Offers to install a title update for |release|, the game at |game| called
// |name|, into |folder|, which holds the update numbered |installed|, saying
// whether a newer one is known to exist: xboxunity.net's, looked up only when
// asked, or a package from disk. At launch it can also stop the offers for the
// game. True when one got installed.
bool ShowTitleUpdateDialog(wxWindow* parent, const TitleUpdateRelease& release,
                           uint32_t installed,
                           const std::filesystem::path& game,
                           const wxString& name,
                           const std::filesystem::path& folder, bool at_launch);

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_TITLE_UPDATE_DIALOG_WX_H_
