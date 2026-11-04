/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_TITLE_IMAGE_DEVICE_H_
#define XENIA_KERNEL_XAM_TITLE_IMAGE_DEVICE_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

#include "xenia/vfs/devices/host_path_device.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

inline constexpr std::string_view kTitleImageMountPath =
    "\\Device\\TitleImages";
inline constexpr std::string_view kTitleImageSymbolicLink = "TitleImages:";
// The dashboard loads box art from <root>/<title id>/<locale>/<image>.
inline constexpr std::u16string_view kTitleImageUriRoot =
    u"file://TitleImages:";

// Title images as <title id>\<locale>\<image> or <title id>\<image>, made for
// this launch from the downloaded box art of the title's game library release,
// or else its NXEArt, or its icon for box art.
class TitleImageDevice : public vfs::HostPathDevice {
 public:
  TitleImageDevice(const std::string_view mount_path,
                   const std::filesystem::path& cache_root, Emulator* emulator);

  bool Initialize() override;
  vfs::Entry* ResolvePath(const std::string_view path) override;

 private:
  std::filesystem::path cache_root_;
  Emulator* emulator_;
  // Lowercase title folders made this launch, empty ones included.
  std::unordered_set<std::string> made_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_TITLE_IMAGE_DEVICE_H_
