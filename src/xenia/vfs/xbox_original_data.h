/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_XBOX_ORIGINAL_DATA_H_
#define XENIA_VFS_XBOX_ORIGINAL_DATA_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/xbox.h"

namespace xe {
namespace vfs {

// An original Xbox saved game, one folder under UDATA\<title id>.
struct XboxOriginalSave {
  std::filesystem::path path;
  // From its SaveMeta.xbx.
  std::string name;
};

// Downloaded content, one folder under TDATA\<title id>\$c.
struct XboxOriginalContent {
  std::filesystem::path path;
  // From its ContentMeta.xbx, empty when that names nothing.
  std::string name;
};

// `name` in `dir`, in any case since a copied tree can spell it differently.
// Empty when there is none.
std::filesystem::path FindXboxOriginalPath(const std::filesystem::path& dir,
                                           std::string_view name);

// The saves in a title's UDATA folder. One without a name is logged and left
// out.
std::vector<XboxOriginalSave> ListXboxOriginalSaves(
    const std::filesystem::path& title_dir);

// The packages in a title's $c folder, named in `language` where they can be.
std::vector<XboxOriginalContent> ListXboxOriginalContent(
    const std::filesystem::path& content_dir, XLanguage language);

// The Name in a SaveMeta.xbx, empty when it has none.
std::string ReadSaveMetaName(std::span<const uint8_t> data);

// The name a ContentMeta.xbx gives in `language`, else its default one.
std::string ReadContentMetaName(std::span<const uint8_t> data,
                                XLanguage language);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_XBOX_ORIGINAL_DATA_H_
