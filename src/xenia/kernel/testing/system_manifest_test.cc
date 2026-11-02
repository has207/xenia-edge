/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/kernel/xam/system_manifest.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xam::FindSystemManifestFile;
using xe::kernel::xam::GetSystemManifestVersion;
using xe::kernel::xam::SystemExtFolderName;

void AppendName(std::vector<uint8_t>& manifest, std::string_view name) {
  const size_t offset = manifest.size();
  manifest.resize(offset + 2 + name.size() + 1);
  xe::store_and_swap<uint16_t>(&manifest[offset], uint16_t(name.size() + 1));
  std::memcpy(&manifest[offset + 2], name.data(), name.size());
}

void AppendFile(std::vector<uint8_t>& manifest, std::string_view name,
                uint32_t version) {
  AppendName(manifest, name);
  const size_t offset = manifest.size();
  manifest.resize(offset + 4 + 0x10);
  xe::store_and_swap<uint32_t>(&manifest[offset], version);
}

// Laid out like the 2.0.17559 manifest, which names each file again later
// without its version. Its records start with binary data that can look like a
// name, as at 0x2F0 there.
std::vector<uint8_t> MakeManifest() {
  std::vector<uint8_t> manifest(0x150);
  std::memcpy(manifest.data(), "XMNP", 4);
  xe::store_and_swap<uint32_t>(&manifest[0x14C], 0x20449700);
  for (const uint8_t byte :
       {0x00, 0x09, 0x90, 0x00, 0x00, 0x00, 0x3A, 0x95, 0x00, 0x00, 0x00}) {
    manifest.push_back(byte);
  }
  AppendFile(manifest, "dash.xex", 0x20447600);
  AppendName(manifest, "dash.xex");
  AppendFile(manifest, "Title.Zune.xex", 0x20417F00);
  AppendName(manifest, "Title.Zune.xex");
  return manifest;
}

TEST_CASE("System manifest", "[system_manifest]") {
  const auto manifest = MakeManifest();

  SECTION("It describes a system version") {
    REQUIRE(GetSystemManifestVersion(manifest) == 0x20449700);
  }

  SECTION("A file has the version it is listed with") {
    REQUIRE(FindSystemManifestFile(manifest, "dash.xex") == 0x20447600);
    REQUIRE(FindSystemManifestFile(manifest, "Title.Zune.xex") == 0x20417F00);
  }

  SECTION("Names match without case") {
    REQUIRE(FindSystemManifestFile(manifest, "title.zune.xex") == 0x20417F00);
  }

  SECTION("Only whole names match") {
    REQUIRE_FALSE(FindSystemManifestFile(manifest, "dash"));
    REQUIRE_FALSE(FindSystemManifestFile(manifest, "Zune.xex"));
    REQUIRE_FALSE(FindSystemManifestFile(manifest, "PlayReady.xex"));
  }

  SECTION("Other files aren't manifests") {
    auto other = manifest;
    other[0] = 'Y';
    REQUIRE_FALSE(GetSystemManifestVersion(other));
    REQUIRE_FALSE(FindSystemManifestFile(other, "dash.xex"));
    REQUIRE_FALSE(GetSystemManifestVersion(
        std::span<const uint8_t>(manifest.data(), 0x14C)));
  }

  SECTION("A name with no room for a version after it isn't listed") {
    auto cut = manifest;
    AppendName(cut, "last.xex");
    REQUIRE_FALSE(FindSystemManifestFile(cut, "last.xex"));
  }

  SECTION("A version names its folder") {
    REQUIRE(SystemExtFolderName(0x20417F00) == "20417f00");
  }
}

}  // namespace
