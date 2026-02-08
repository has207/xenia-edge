/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/gpu/texture_conversion.h"
#include "xenia/gpu/xenos.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::gpu::texture_conversion::test {

TEST_CASE("ConvertTexelCTX1ToR8G8_BasicColors", "[texture_conversion]") {
  // Test basic color interpolation
  // CTX1 format: 2 base colors (R,G) + 32-bit index bitmap

  // Test case 1: Black to white gradient
  // r0=0, r1=255, g0=0, g1=255
  // Expected interpolated values:
  //   index 0: (0, 0)
  //   index 1: (255, 255)
  //   index 2: (2*0 + 1*255 + 1)/3 = (0 + 255 + 1)/3 = 256/3 = 85
  //   index 3: (1*0 + 2*255 + 1)/3 = (0 + 510 + 1)/3 = 511/3 = 170

  uint8_t input[8];
  input[0] = 0;    // g0
  input[1] = 0;    // r0
  input[2] = 255;  // g1
  input[3] = 255;  // r1
  // Index pattern: all zeros (use color 0 for all pixels)
  input[4] = 0x00;
  input[5] = 0x00;
  input[6] = 0x00;
  input[7] = 0x00;

  uint8_t output[32];  // 4x4 pixels, 2 bytes each (R,G)
  std::memset(output, 0xFF, sizeof(output));

  ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, output, input, 8);

  // All pixels should be (0, 0) since all indices are 0
  for (int i = 0; i < 16; i++) {
    REQUIRE(output[i * 2 + 0] == 0);  // R channel
    REQUIRE(output[i * 2 + 1] == 0);  // G channel
  }
}

TEST_CASE("ConvertTexelCTX1ToR8G8_InterpolatedColors", "[texture_conversion]") {
  // Test interpolation correctness
  // r0=128, r1=64, g0=192, g1=96

  uint8_t input[8];
  input[0] = 192;  // g0
  input[1] = 128;  // r0
  input[2] = 96;   // g1
  input[3] = 64;   // r1
  // Index pattern: 0x1B = 0b00011011
  // Pixel indices (2 bits each, LSB first):
  // pixels 0-3 (row 0): 3, 2, 1, 0
  input[4] = 0x1B;
  input[5] = 0x00;
  input[6] = 0x00;
  input[7] = 0x00;

  uint8_t output[32];
  std::memset(output, 0xFF, sizeof(output));

  ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, output, input, 8);

  // Calculate expected values with proper rounding
  uint8_t expected_r[4] = {
      128,                                               // index 0: r0
      64,                                                // index 1: r1
      static_cast<uint8_t>((2 * 128 + 1 * 64 + 1) / 3),  // index 2: 107
      static_cast<uint8_t>((1 * 128 + 2 * 64 + 1) / 3)   // index 3: 85
  };
  uint8_t expected_g[4] = {
      192,                                               // index 0: g0
      96,                                                // index 1: g1
      static_cast<uint8_t>((2 * 192 + 1 * 96 + 1) / 3),  // index 2: 160
      static_cast<uint8_t>((1 * 192 + 2 * 96 + 1) / 3)   // index 3: 128
  };

  // Check first row
  // Pixel indices are extracted LSB-first from 0x1B:
  // Pixel 0: bits [1:0] = 0b11 = 3
  // Pixel 1: bits [3:2] = 0b10 = 2
  // Pixel 2: bits [5:4] = 0b01 = 1
  // Pixel 3: bits [7:6] = 0b00 = 0
  REQUIRE(output[0] == expected_r[3]);  // pixel 0 uses index 3
  REQUIRE(output[1] == expected_g[3]);
  REQUIRE(output[2] == expected_r[2]);  // pixel 1 uses index 2
  REQUIRE(output[3] == expected_g[2]);
  REQUIRE(output[4] == expected_r[1]);  // pixel 2 uses index 1
  REQUIRE(output[5] == expected_g[1]);
  REQUIRE(output[6] == expected_r[0]);  // pixel 3 uses index 0
  REQUIRE(output[7] == expected_g[0]);
}

TEST_CASE("ConvertTexelCTX1ToR8G8_AllIndices", "[texture_conversion]") {
  // Test all 4 possible indices in a single block

  uint8_t input[8];
  input[0] = 100;  // g0
  input[1] = 200;  // r0
  input[2] = 50;   // g1
  input[3] = 150;  // r1

  // Pattern that uses all 4 indices:
  // Row 0: 0x1B = 0b00011011
  //   Pixel 0: bits [1:0] = 0b11 = 3
  //   Pixel 1: bits [3:2] = 0b10 = 2
  //   Pixel 2: bits [5:4] = 0b01 = 1
  //   Pixel 3: bits [7:6] = 0b00 = 0
  // Row 1: 0xE4 = 0b11100100
  //   Pixel 0: bits [1:0] = 0b00 = 0
  //   Pixel 1: bits [3:2] = 0b01 = 1
  //   Pixel 2: bits [5:4] = 0b10 = 2
  //   Pixel 3: bits [7:6] = 0b11 = 3
  // Row 2: 0x00 = all indices 0
  // Row 3: 0x55 = 0b01010101 = all indices 1
  input[4] = 0x1B;
  input[5] = 0xE4;
  input[6] = 0x00;
  input[7] = 0x55;

  uint8_t output[32];
  ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, output, input, 8);

  // Calculate expected palette
  uint8_t palette_r[4] = {
      200,                                                // r0
      150,                                                // r1
      static_cast<uint8_t>((2 * 200 + 1 * 150 + 1) / 3),  // 183
      static_cast<uint8_t>((1 * 200 + 2 * 150 + 1) / 3)   // 167
  };
  uint8_t palette_g[4] = {
      100,                                               // g0
      50,                                                // g1
      static_cast<uint8_t>((2 * 100 + 1 * 50 + 1) / 3),  // 83
      static_cast<uint8_t>((1 * 100 + 2 * 50 + 1) / 3)   // 67
  };

  // Row 0: pixel indices 3, 2, 1, 0 (extracted from 0x1B)
  REQUIRE(output[0] == palette_r[3]);
  REQUIRE(output[1] == palette_g[3]);
  REQUIRE(output[2] == palette_r[2]);
  REQUIRE(output[3] == palette_g[2]);
  REQUIRE(output[4] == palette_r[1]);
  REQUIRE(output[5] == palette_g[1]);
  REQUIRE(output[6] == palette_r[0]);
  REQUIRE(output[7] == palette_g[0]);

  // Row 1: pixel indices 0, 1, 2, 3 (extracted from 0xE4)
  REQUIRE(output[8] == palette_r[0]);
  REQUIRE(output[9] == palette_g[0]);
  REQUIRE(output[10] == palette_r[1]);
  REQUIRE(output[11] == palette_g[1]);
  REQUIRE(output[12] == palette_r[2]);
  REQUIRE(output[13] == palette_g[2]);
  REQUIRE(output[14] == palette_r[3]);
  REQUIRE(output[15] == palette_g[3]);

  // Row 2: all index 0
  for (int i = 0; i < 4; i++) {
    REQUIRE(output[16 + i * 2] == palette_r[0]);
    REQUIRE(output[17 + i * 2] == palette_g[0]);
  }

  // Row 3: all index 1
  for (int i = 0; i < 4; i++) {
    REQUIRE(output[24 + i * 2] == palette_r[1]);
    REQUIRE(output[25 + i * 2] == palette_g[1]);
  }
}

TEST_CASE("ConvertTexelCTX1ToR8G8_RoundingEdgeCases", "[texture_conversion]") {
  // Test rounding behavior with values that would differ between
  // floating-point and integer arithmetic

  uint8_t input[8];
  input[0] = 1;     // g0
  input[1] = 128;   // r0
  input[2] = 2;     // g1
  input[3] = 64;    // r1
  input[4] = 0x0A;  // Use index 2 for pixels 0 and 1
  input[5] = 0x00;
  input[6] = 0x00;
  input[7] = 0x00;

  uint8_t output[32];
  ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, output, input, 8);

  // index 2: (2 * r0 + 1 * r1 + 1) / 3
  // Floating-point (WRONG): (2.0/3.0 * 128 + 1.0/3.0 * 64) = 85.333...
  // truncates to 85 Integer (CORRECT): (2 * 128 + 1 * 64 + 1) / 3 = 321 / 3 =
  // 107

  uint8_t expected_r = static_cast<uint8_t>((2 * 128 + 1 * 64 + 1) / 3);
  uint8_t expected_g = static_cast<uint8_t>((2 * 1 + 1 * 2 + 1) / 3);

  REQUIRE(expected_r == 107);  // Not 85!
  REQUIRE(expected_g == 1);

  REQUIRE(output[2] == expected_r);  // pixel 1, R
  REQUIRE(output[3] == expected_g);  // pixel 1, G
}

TEST_CASE("ConvertTexelCTX1ToR8G8_Endianness", "[texture_conversion]") {
  // Test endian swapping

  uint8_t input_be[8] = {0x64, 0xC8,  // g0=100, r0=200 in big-endian
                         0x32, 0x96,  // g1=50, r1=150 in big-endian
                         0x1B, 0x00, 0x00, 0x00};

  uint8_t output[32];

  // Test with k8in16 endian (swap each 16-bit word)
  ConvertTexelCTX1ToR8G8(xenos::Endian::k8in16, output, input_be, 8);

  // After k8in16 swap, the r0/g0 pair is swapped, r1/g1 pair is swapped
  // So we get: g0=0xC8 (200), r0=0x64 (100), g1=0x96 (150), r1=0x32 (50)
  uint8_t palette_r[4] = {100,  // r0 after swap
                          50,   // r1 after swap
                          static_cast<uint8_t>((2 * 100 + 1 * 50 + 1) / 3),
                          static_cast<uint8_t>((1 * 100 + 2 * 50 + 1) / 3)};

  // First pixel (index 0) should use palette entry 0
  REQUIRE(output[0] == palette_r[0]);
}

}  // namespace xe::gpu::texture_conversion::test
