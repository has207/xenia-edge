/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TITLE_UPDATE_CHECK_H_
#define XENIA_APP_TITLE_UPDATE_CHECK_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

#include "xenia/app/title_update_db.h"

namespace xe {
namespace app {

// The release a title update would be made for.
struct TitleUpdateRelease {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t version = 0;
};

// The release a 360 game file holds, from its executable when a package's
// header doesn't say. Empty for what title updates don't apply to, indie and
// original Xbox games and system titles, and for a file that can't be read.
std::optional<TitleUpdateRelease> ReadTitleUpdateRelease(
    const std::filesystem::path& path);

// An update's number is the build field of the version its default.xexp
// patches to, 0.0.3.4 for TU3 of 0.0.0.4. 0 when |xexp| isn't a patch of
// |base_version|.
uint32_t XexpTitleUpdateNumber(std::span<const uint8_t> xexp,
                               uint32_t base_version);

// The number of the title update package at |package| when it's made for
// |version|, else 0.
uint32_t PackageTitleUpdateNumber(const std::filesystem::path& package,
                                  uint32_t version);

// Whether the title update package at |package| fits the game file at |game|
// by the patch's encryption key, the check the kernel picks updates by, which
// an update made for another release of the same code passes. Empty when the
// game's executable or the package's patch can't be read.
std::optional<bool> TitleUpdateFitsGame(const std::filesystem::path& package,
                                        const std::filesystem::path& game);

// Whether every file in the package at |package| reads back whole. Some
// archived packages were uploaded cut short and a patch cut short fails part
// way through patching the executable.
bool IsPackageComplete(const std::filesystem::path& package);

enum class TitleUpdateFit {
  kFits,
  kDamaged,
  // Its patch's key doesn't fit the game's executable.
  kWrongKey,
  // Made for another release, which decides when the game can't be read.
  kOtherRelease,
};

// Whether the title update package at |package|, made for |package_version|,
// can be installed for |version| of the game at |game|. A version that isn't
// known, 0, doesn't rule it out.
TitleUpdateFit CheckTitleUpdateFit(const std::filesystem::path& package,
                                   uint32_t package_version,
                                   const std::filesystem::path& game,
                                   uint32_t version);

// The highest number of the updates in |folder| made for |version|, packages
// and extracted ones alike, 0 when none is. Only the patches are read, since an
// older install's header file has no version in it.
uint32_t InstalledTitleUpdateNumber(const std::filesystem::path& folder,
                                    uint32_t version);

// The newest of |downloads|, a title's, for |release| when it's newer than the
// update numbered |installed|, from those listed for its media id and for any
// media. An unnumbered one can't be compared so it's only offered when
// |installed| is 0. Null when there's none.
const TitleUpdateDownload* FindNewerTitleUpdate(
    const TitleUpdateDownloads& downloads, const TitleUpdateRelease& release,
    uint32_t installed);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_TITLE_UPDATE_CHECK_H_
