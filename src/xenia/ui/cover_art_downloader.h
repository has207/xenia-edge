/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_COVER_ART_DOWNLOADER_H_
#define XENIA_UI_COVER_ART_DOWNLOADER_H_

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xe {
namespace ui {

// One game's cover, saved as each of |files|.
struct CoverArtDownload {
  // Tried in order until one gives an image.
  std::vector<std::string> urls;
  std::vector<std::filesystem::path> files;
};

struct CoverArtResult {
  size_t downloaded = 0;
  // Games none of whose URLs gave an image, which includes being offline.
  size_t not_downloaded = 0;
  // Games whose cover couldn't be written to one of its files.
  size_t not_saved = 0;
  bool cancelled = false;
};

// Downloads each game's cover. |progress| gets the games done out of a total
// and cancels by returning false.
CoverArtResult DownloadCoverArt(
    const std::vector<CoverArtDownload>& downloads,
    const std::function<bool(size_t done, size_t total)>& progress);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_COVER_ART_DOWNLOADER_H_
