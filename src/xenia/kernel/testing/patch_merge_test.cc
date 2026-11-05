/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/patcher/patch_db.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::patcher::MergePatchFiles;
using xe::patcher::PatchDB;
using xe::patcher::PatchFileEntry;

constexpr std::string_view kHeader =
    "title_name = \"Test Title\"\n"
    "title_id = \"12345678\"\n"
    "hash = \"0123456789ABCDEF\"\n";

std::string Patch(std::string_view name, bool enabled, uint32_t value) {
  return fmt::format(
      "\n[[patch]]\n"
      "    name = \"{}\"\n"
      "    is_enabled = {}\n"
      "\n"
      "    [[patch.be32]]\n"
      "        address = 0x82000000\n"
      "        value = 0x{:08X}\n",
      name, enabled, value);
}

std::string PatchWithoutEnabled(std::string_view name, uint32_t value) {
  return fmt::format(
      "\n[[patch]]\n"
      "    name = \"{}\"\n"
      "\n"
      "    [[patch.be32]]\n"
      "        address = 0x82000000\n"
      "        value = 0x{:08X}\n",
      name, value);
}

std::string Crlf(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '\n') {
      out += '\r';
    }
    out += c;
  }
  return out;
}

PatchFileEntry Parse(std::string_view text) {
  return PatchDB(std::filesystem::path())
      .ReadPatchFromString("12345678 - Test Title.patch.toml", text);
}

// Each patch's name and whether it's enabled, in file order.
std::vector<std::pair<std::string, bool>> Patches(std::string_view text) {
  std::vector<std::pair<std::string, bool>> patches;
  for (const auto& patch : Parse(text).patch_info) {
    patches.emplace_back(patch.patch_name, patch.is_enabled);
  }
  return patches;
}

// The value the named patch writes, as the guest reads it.
uint32_t Value(std::string_view text, std::string_view name) {
  for (const auto& patch : Parse(text).patch_info) {
    if (patch.patch_name == name && patch.patch_data.size() == 1 &&
        patch.patch_data[0].data.patch_data.size() == 4) {
      const auto& bytes = patch.patch_data[0].data.patch_data;
      return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
             uint32_t(bytes[2]) << 8 | uint32_t(bytes[3]);
    }
  }
  return 0;
}

// The merge of two files that are expected to merge.
std::string Merge(std::string_view base, std::string_view extra,
                  bool keep_extra_enabled) {
  auto merged = MergePatchFiles(base, extra, keep_extra_enabled);
  REQUIRE(merged);
  return *merged;
}

using Enabled = std::vector<std::pair<std::string, bool>>;

TEST_CASE("Patch file merging", "[patcher]") {
  const std::string bundled =
      std::string(kHeader) + Patch("A", true, 1) + Patch("B", true, 2);

  SECTION("A saved patch keeps its enabled state") {
    const std::string saved = std::string(kHeader) + Patch("A", false, 1);
    const auto merged = Merge(bundled, saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", false}, {"B", true}});
  }

  SECTION("A saved patch takes the bundled data") {
    const std::string saved = std::string(kHeader) + Patch("A", false, 9);
    const auto merged = Merge(bundled, saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", false}, {"B", true}});
    REQUIRE(Value(merged, "A") == 1);
  }

  SECTION("Patches only the saved copy has are kept after the bundled ones") {
    const std::string saved =
        std::string(kHeader) + Patch("C", false, 3) + Patch("A", true, 1);
    const auto merged = Merge(bundled, saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", true}, {"B", true}, {"C", false}});
    REQUIRE(Value(merged, "C") == 3);
  }

  SECTION("Without keep_extra_enabled the base's state stays") {
    const std::string other = std::string(kHeader) + Patch("A", false, 1);
    const auto merged = Merge(bundled, other, false);
    REQUIRE(Patches(merged) == Enabled{{"A", true}, {"B", true}});
  }

  SECTION("A patch with no is_enabled gets one") {
    const std::string base = std::string(kHeader) + PatchWithoutEnabled("A", 1);
    const std::string saved = std::string(kHeader) + Patch("A", true, 1);
    const auto merged = Merge(base, saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", true}});
  }

  SECTION("Patches of the same name pair up in order") {
    const std::string base =
        std::string(kHeader) + Patch("A", true, 1) + Patch("A", true, 2);
    const std::string saved =
        std::string(kHeader) + Patch("A", true, 1) + Patch("A", false, 2);
    const auto merged = Merge(base, saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", true}, {"A", false}});
  }

  SECTION("CRLF files merge") {
    const std::string saved = std::string(kHeader) + Patch("B", false, 2);
    const auto merged = Merge(Crlf(bundled), Crlf(saved), true);
    REQUIRE(Patches(merged) == Enabled{{"A", true}, {"B", false}});
    REQUIRE(merged.find("is_enabled = false\r\n") != std::string::npos);
  }

  SECTION("A patch header with a comment starts a patch") {
    std::string commented = bundled;
    commented.replace(commented.rfind("[[patch]]"), 9, "[[patch]] # note");
    const std::string saved = std::string(kHeader) + Patch("B", false, 2);
    const auto merged = Merge(Crlf(commented), saved, true);
    REQUIRE(Patches(merged) == Enabled{{"A", true}, {"B", false}});
  }

  SECTION("Merging a merged file again changes nothing") {
    const std::string saved =
        std::string(kHeader) + Patch("A", false, 1) + Patch("C", true, 3);
    const auto merged = Merge(bundled, saved, true);
    REQUIRE(Merge(bundled, merged, true) == merged);
  }

  SECTION("A saved copy that doesn't parse doesn't merge") {
    REQUIRE_FALSE(MergePatchFiles(bundled, "[[patch", true));
  }
}

}  // namespace
