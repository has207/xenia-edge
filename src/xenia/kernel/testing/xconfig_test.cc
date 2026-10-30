/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstdint>

#include "xenia/kernel/xconfig.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::ConsoleMacAddress;

TEST_CASE("Console MAC address", "[xconfig]") {
  const auto mac = ConsoleMacAddress("E000000000000001");

  SECTION("It is in a console vendor's block") {
    REQUIRE(mac[0] == 0x00);
    REQUIRE(mac[1] == 0x1D);
    REQUIRE(mac[2] == 0xD8);
  }

  SECTION("A console keeps its address") {
    // Titles encrypt saves with it, so a new derivation would lose them.
    REQUIRE(mac == std::array<uint8_t, 6>{0x00, 0x1D, 0xD8, 0x78, 0x02, 0xF7});
  }

  SECTION("Another profile or machine is another console") {
    REQUIRE(ConsoleMacAddress("E000000000000002") != mac);
    REQUIRE(ConsoleMacAddress("machine") != mac);
  }
}

}  // namespace
