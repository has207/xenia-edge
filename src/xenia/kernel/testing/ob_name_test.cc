/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::xeObStripDosDevicesPrefix;
using xe::kernel::xboxkrnl::xeObSymbolicLinkName;

std::string Stripped(std::string path) {
  xeObStripDosDevicesPrefix(path);
  return path;
}

TEST_CASE("DOS devices prefix", "[ob]") {
  SECTION("\\??\\ is dropped") {
    REQUIRE(Stripped("\\??\\game:\\file") == "game:\\file");
  }

  SECTION("\\System??\\ is dropped in any case") {
    REQUIRE(Stripped("\\System??\\game:\\file") == "game:\\file");
    REQUIRE(Stripped("\\system??\\game:\\file") == "game:\\file");
    REQUIRE(Stripped("\\SYSTEM??\\game:\\file") == "game:\\file");
  }

  SECTION("Other names are kept") {
    REQUIRE(Stripped("game:\\file") == "game:\\file");
    REQUIRE(Stripped("\\Device\\Harddisk0\\Partition1") ==
            "\\Device\\Harddisk0\\Partition1");
    REQUIRE(Stripped("\\System??") == "\\System??");
    REQUIRE(Stripped("\\System?\\game:") == "\\System?\\game:");
  }

  SECTION("Bytes after the prefix are kept as they are") {
    REQUIRE(Stripped("\\System??\\\xFF\xFE") == "\xFF\xFE");
  }
}

TEST_CASE("Symbolic link name", "[ob]") {
  REQUIRE(xeObSymbolicLinkName("\\??\\D:\\") == "D:");
  REQUIRE(xeObSymbolicLinkName("\\system??\\media:") == "media:");
  REQUIRE(xeObSymbolicLinkName("\\SYSTEM??\\XSYSLAUNCH") == "XSYSLAUNCH");
}

}  // namespace
