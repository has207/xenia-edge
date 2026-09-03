/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_XEX_METADATA_H_
#define XENIA_VFS_XEX_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "third_party/tomlplusplus/toml.hpp"

namespace xe {
namespace vfs {

enum class XexFormat {
  kUnknown,
  kXex1,
  kXex2,
};

struct XexVersion {
  uint8_t major = 0;
  uint8_t minor = 0;
  uint16_t build = 0;
  uint8_t qfe = 0;

  // "major.minor.build.qfe".
  std::string ToString() const;
  // Packed as it appears in the XEX execution info.
  uint32_t value() const;
  static XexVersion FromValue(uint32_t value);
  // Parses what ToString() produces. Empty unless the text is exactly that,
  // so a version can never be spelled two ways.
  static std::optional<XexVersion> FromString(std::string_view text);

  bool operator==(const XexVersion& other) const = default;
};

struct XexMetadata {
  XexFormat format = XexFormat::kUnknown;
  std::string module_name;

  // From XEX_HEADER_EXECUTION_INFO
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t savegame_id = 0;
  XexVersion version = {};
  XexVersion base_version = {};
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;

  // From XEX_HEADER_GAME_RATINGS
  struct {
    bool present = false;
    uint8_t esrb = 0xFF;
    uint8_t pegi = 0xFF;
    uint8_t cero = 0xFF;
    uint8_t usk = 0xFF;
    uint8_t oflc_au = 0xFF;
    uint8_t oflc_nz = 0xFF;
  } ratings;

  toml::table ToToml() const;
};

std::optional<XexMetadata> ExtractXexMetadata(
    const std::filesystem::path& path);
std::optional<XexMetadata> ExtractXexMetadata(const uint8_t* data, size_t size);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_XEX_METADATA_H_
