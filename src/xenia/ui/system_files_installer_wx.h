/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_
#define XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_

#include <cstddef>
#include <filesystem>
#include <functional>

namespace xe {
namespace ui {

enum class SystemFilesResult {
  kSuccess,
  kCancelled,
  // The zip isn't a USB system update.
  kNotSystemUpdate,
  kReadFailed,
  kWriteFailed,
  // The install failed and the previous one couldn't be put back. Its folders
  // are left in the unrestored folder. Installs are refused until it's gone.
  kRestoreFailed,
};

// Installs the files of a USB system update zip in |system_path| as a console
// holds them: flash, sysext (the system extended partition) and an empty
// sysaux, replacing what is there. A flash font the update only patches is
// filled in with one built from the bundled fonts. |progress| gets the files
// done out of a total and cancels the install by returning false.
SystemFilesResult InstallSystemFiles(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& system_path,
    const std::function<bool(size_t done, size_t total)>& progress);

// Where InstallSystemFiles leaves a previous install it couldn't put back.
std::filesystem::path GetUnrestoredSystemFilesPath(
    const std::filesystem::path& system_path);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SYSTEM_FILES_INSTALLER_WX_H_
