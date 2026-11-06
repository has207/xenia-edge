/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_INDIE_GAME_H_
#define XENIA_KERNEL_XAM_INDIE_GAME_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {

// An indie game, installed as content of the XNA title launcher that runs it.
struct IndieGame {
  // The game's own, not the launcher's.
  uint32_t title_id = 0;
  std::string name;
  std::filesystem::path package_path;
  // Empty when the package has none.
  std::vector<uint8_t> icon;
  // Starts the names of the game's saves, which are the launcher's XNA content.
  std::string virtual_title_id;
};

// The indie game in the installed package at |package_path|, a folder or the
// package file, from its GameInfo.xml.
std::optional<IndieGame> ReadIndieGame(
    const std::filesystem::path& package_path);

// The launch data that has the XNA title launcher run the indie game in the
// package named |file_name| on |device_id|, as the dashboard starts it. Empty
// when the name doesn't fit.
std::vector<uint8_t> MakeIndieGameLaunchData(std::string_view file_name,
                                             uint32_t device_id);

// The file name of the package |launch_data| has the launcher run, empty when
// it names none.
std::string IndieGameLaunchFileName(std::span<const uint8_t> launch_data);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_INDIE_GAME_H_
