/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_PNG_UTILS_H_
#define XENIA_BASE_PNG_UTILS_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace xe {

bool IsFilePngImage(const std::filesystem::path& file_path);
std::pair<uint16_t, uint16_t> GetImageResolution(
    const std::filesystem::path& file_path);

std::vector<uint8_t> ReadPngFromFile(const std::filesystem::path& file_path);

// The image resized to width x height as a PNG, each pixel the average of the
// source pixels it covers. Empty if the image doesn't decode.
std::vector<uint8_t> ResizePng(std::span<const uint8_t> image, uint32_t width,
                               uint32_t height);

}  // namespace xe

#endif  // XENIA_BASE_PNG_UTILS_H_
