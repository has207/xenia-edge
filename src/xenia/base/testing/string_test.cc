/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/base/string.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

TEST_CASE("Windows-1252 To UTF-8", "[string]") {
  REQUIRE(win1252_to_utf8("204_\xE9p.dds") == "204_\xC3\xA9p.dds");
  REQUIRE(win1252_to_utf8("") == "");

  const char* const expected[32] = {
      "\xE2\x82\xAC", "\xC2\x81",     "\xE2\x80\x9A", "\xC6\x92",
      "\xE2\x80\x9E", "\xE2\x80\xA6", "\xE2\x80\xA0", "\xE2\x80\xA1",
      "\xCB\x86",     "\xE2\x80\xB0", "\xC5\xA0",     "\xE2\x80\xB9",
      "\xC5\x92",     "\xC2\x8D",     "\xC5\xBD",     "\xC2\x8F",
      "\xC2\x90",     "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9C",
      "\xE2\x80\x9D", "\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x80\x94",
      "\xCB\x9C",     "\xE2\x84\xA2", "\xC5\xA1",     "\xE2\x80\xBA",
      "\xC5\x93",     "\xC2\x9D",     "\xC5\xBE",     "\xC5\xB8",
  };
  for (int i = 0; i < 32; ++i) {
    REQUIRE(win1252_to_utf8(std::string(1, char(0x80 + i))) == expected[i]);
  }
}

TEST_CASE("UTF-8 To Windows-1252", "[string]") {
  REQUIRE(utf8_to_win1252("caf\xC3\xA9") == "caf\xE9");
  REQUIRE(utf8_to_win1252("\xE2\x82\xAC") == "\x80");
  REQUIRE(utf8_to_win1252("x\xE3\x81\x82y") == "x_y");
  REQUIRE(utf8_to_win1252("ab\xE3\x81") == "ab_");
}

TEST_CASE("Windows-1252 Round Trip", "[string]") {
  std::string all;
  for (int b = 0; b < 256; ++b) {
    all.push_back(char(b));
  }
  REQUIRE(utf8_to_win1252(win1252_to_utf8(all)) == all);
}

}  // namespace xe::base::test
