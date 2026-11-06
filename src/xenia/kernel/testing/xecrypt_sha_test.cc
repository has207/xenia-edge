/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::xboxkrnl::XECRYPT_SHA256_STATE;
using xe::kernel::xboxkrnl::XECRYPT_SHA512_STATE;
using xe::kernel::xboxkrnl::XECRYPT_SHA_STATE;
using xe::kernel::xboxkrnl::XeCryptSha;
using xe::kernel::xboxkrnl::XeCryptSha256Final;
using xe::kernel::xboxkrnl::XeCryptSha256Init;
using xe::kernel::xboxkrnl::XeCryptSha256Update;
using xe::kernel::xboxkrnl::XeCryptSha512Final;
using xe::kernel::xboxkrnl::XeCryptSha512Init;
using xe::kernel::xboxkrnl::XeCryptSha512Update;
using xe::kernel::xboxkrnl::XeCryptShaFinal;
using xe::kernel::xboxkrnl::XeCryptShaInit;
using xe::kernel::xboxkrnl::XeCryptShaUpdate;

using Digest = std::array<uint8_t, 20>;
using Digest256 = std::array<uint8_t, 32>;
using Digest512 = std::array<uint8_t, 64>;

template <typename T = Digest>
T FromHex(std::string_view hex) {
  T digest{};
  for (size_t i = 0; i < digest.size(); i++) {
    digest[i] = static_cast<uint8_t>(
        std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
  }
  return digest;
}

const uint8_t* Bytes(std::string_view text) {
  return reinterpret_cast<const uint8_t*>(text.data());
}

Digest Sha(std::string_view message) {
  Digest digest;
  XeCryptSha(Bytes(message), static_cast<uint32_t>(message.size()), nullptr, 0,
             nullptr, 0, digest.data(), static_cast<uint32_t>(digest.size()));
  return digest;
}

Digest Final(XECRYPT_SHA_STATE& state) {
  Digest digest;
  XeCryptShaFinal(&state, digest.data(), static_cast<uint32_t>(digest.size()));
  return digest;
}

Digest512 Final512(XECRYPT_SHA512_STATE& state) {
  Digest512 digest;
  XeCryptSha512Final(&state, digest.data(),
                     static_cast<uint32_t>(digest.size()));
  return digest;
}

Digest512 Sha512(std::string_view message) {
  XECRYPT_SHA512_STATE state;
  XeCryptSha512Init(&state);
  XeCryptSha512Update(&state, Bytes(message),
                      static_cast<uint32_t>(message.size()));
  return Final512(state);
}

Digest256 Sha256(std::string_view message) {
  XECRYPT_SHA256_STATE state;
  XeCryptSha256Init(&state);
  XeCryptSha256Update(&state, Bytes(message),
                      static_cast<uint32_t>(message.size()));
  Digest256 digest;
  XeCryptSha256Final(&state, digest.data(),
                     static_cast<uint32_t>(digest.size()));
  return digest;
}

}  // namespace

TEST_CASE("XeCryptSha_known_answers", "[crypt]") {
  REQUIRE(Sha("") == FromHex("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  REQUIRE(Sha("abc") == FromHex("a9993e364706816aba3e25717850c26c9cd0d89d"));
  REQUIRE(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          FromHex("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST_CASE("XeCryptSha_joins_inputs_and_truncates_output", "[crypt]") {
  const std::string_view inputs[] = {
      "abcdbcdecdefdefgefgh", "fghighijhijkijkljklmklmn", "lmnomnopnopq"};
  std::array<uint8_t, 24> out;
  out.fill(0xCC);
  XeCryptSha(Bytes(inputs[0]), static_cast<uint32_t>(inputs[0].size()),
             Bytes(inputs[1]), static_cast<uint32_t>(inputs[1].size()),
             Bytes(inputs[2]), static_cast<uint32_t>(inputs[2].size()),
             out.data(), 8);
  const Digest expected = FromHex("84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  REQUIRE(std::equal(out.begin(), out.begin() + 8, expected.begin()));
  REQUIRE(std::all_of(out.begin() + 8, out.end(),
                      [](uint8_t b) { return b == 0xCC; }));
}

TEST_CASE("XeCryptShaUpdate_streams_across_blocks", "[crypt]") {
  // Uneven chunks start and end at every offset within a block.
  const std::vector<uint8_t> message(1000000, 'a');
  XECRYPT_SHA_STATE state;
  XeCryptShaInit(&state);
  size_t offset = 0;
  size_t chunk = 1;
  while (offset < message.size()) {
    const size_t size = std::min(chunk, message.size() - offset);
    XeCryptShaUpdate(&state, message.data() + offset,
                     static_cast<uint32_t>(size));
    offset += size;
    chunk = chunk * 3 % 1000 + 1;
  }
  REQUIRE(state.count == 1000000);
  REQUIRE(Final(state) == FromHex("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

TEST_CASE("XeCryptShaFinal_keeps_count_and_last_block", "[crypt]") {
  XECRYPT_SHA_STATE state;
  XeCryptShaInit(&state);
  XeCryptShaUpdate(&state, Bytes("abc"), 3);
  const Digest digest = Final(state);

  REQUIRE(state.count == 3);
  REQUIRE(std::memcmp(state.state, digest.data(), digest.size()) == 0);
  std::array<uint8_t, 64> block{'a', 'b', 'c', 0x80};
  block[63] = 0x18;
  REQUIRE(std::memcmp(state.buffer, block.data(), block.size()) == 0);
}

TEST_CASE("XeCryptShaFinal_twice_after_two_padding_blocks", "[crypt]") {
  // 60 bytes need a second padding block, which overwrites the message tail
  // the second Final pads again.
  std::array<uint8_t, 60> message;
  std::iota(message.begin(), message.end(), uint8_t(0));
  XECRYPT_SHA_STATE state;
  XeCryptShaInit(&state);
  XeCryptShaUpdate(&state, message.data(), 60);

  REQUIRE(Final(state) == FromHex("c98714b16f92c8a770e9fc229df834d1688e282f"));
  std::array<uint8_t, 64> block{};
  block[62] = 0x01;
  block[63] = 0xE0;
  REQUIRE(std::memcmp(state.buffer, block.data(), block.size()) == 0);
  REQUIRE(Final(state) == FromHex("59c10a548f4ef60095e57190d6d17b48c54c90c0"));
}

TEST_CASE("XeCryptShaFinal_32bit_bit_count", "[crypt]") {
  // Only the low 32 bits of the bit count are padded in, so 0x20000003 bytes
  // pad like 3.
  XECRYPT_SHA_STATE state;
  XeCryptShaInit(&state);
  XeCryptShaUpdate(&state, Bytes("abc"), 3);
  state.count = 0x20000003;
  REQUIRE(Final(state) == FromHex("a9993e364706816aba3e25717850c26c9cd0d89d"));
}

TEST_CASE("XeCryptSha256_known_answers", "[crypt]") {
  REQUIRE(Sha256("") ==
          FromHex<Digest256>("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934c"
                             "a495991b7852b855"));
  REQUIRE(Sha256("abc") ==
          FromHex<Digest256>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
                             "b410ff61f20015ad"));
  REQUIRE(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          FromHex<Digest256>("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167"
                             "f6ecedd419db06c1"));
}

TEST_CASE("XeCryptSha256Update_streams_across_blocks", "[crypt]") {
  const std::vector<uint8_t> message(1000000, 'a');
  XECRYPT_SHA256_STATE state;
  XeCryptSha256Init(&state);
  size_t offset = 0;
  size_t chunk = 1;
  while (offset < message.size()) {
    const size_t size = std::min(chunk, message.size() - offset);
    XeCryptSha256Update(&state, message.data() + offset,
                        static_cast<uint32_t>(size));
    offset += size;
    chunk = chunk * 3 % 1000 + 1;
  }
  REQUIRE(state.count == 1000000);
  Digest256 digest;
  XeCryptSha256Final(&state, digest.data(),
                     static_cast<uint32_t>(digest.size()));
  REQUIRE(digest ==
          FromHex<Digest256>("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e"
                             "046d39ccc7112cd0"));
}

TEST_CASE("XeCryptSha256Final_leaves_digest_in_state", "[crypt]") {
  XECRYPT_SHA256_STATE state;
  XeCryptSha256Init(&state);
  XeCryptSha256Update(&state, Bytes("abc"), 3);
  Digest256 digest;
  XeCryptSha256Final(&state, digest.data(),
                     static_cast<uint32_t>(digest.size()));

  REQUIRE(state.count == 3);
  REQUIRE(std::memcmp(state.state, digest.data(), digest.size()) == 0);
  REQUIRE(std::memcmp(state.buffer, digest.data(), digest.size()) == 0);
}

TEST_CASE("XeCryptSha512_known_answers", "[crypt]") {
  REQUIRE(
      Sha512("") ==
      FromHex<Digest512>(
          "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
          "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
  REQUIRE(
      Sha512("abc") ==
      FromHex<Digest512>(
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
  REQUIRE(
      Sha512("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijkl"
             "mnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
      FromHex<Digest512>(
          "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
          "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
}

TEST_CASE("XeCryptSha512Update_streams_across_blocks", "[crypt]") {
  const std::vector<uint8_t> message(1000000, 'a');
  XECRYPT_SHA512_STATE state;
  XeCryptSha512Init(&state);
  size_t offset = 0;
  size_t chunk = 1;
  while (offset < message.size()) {
    const size_t size = std::min(chunk, message.size() - offset);
    XeCryptSha512Update(&state, message.data() + offset,
                        static_cast<uint32_t>(size));
    offset += size;
    chunk = chunk * 3 % 1000 + 1;
  }
  REQUIRE(state.count == 1000000);
  REQUIRE(
      Final512(state) ==
      FromHex<Digest512>(
          "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
          "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"));
}

TEST_CASE("XeCryptSha512Final_leaves_digest_in_state", "[crypt]") {
  XECRYPT_SHA512_STATE state;
  XeCryptSha512Init(&state);
  XeCryptSha512Update(&state, Bytes("abc"), 3);
  const Digest512 digest = Final512(state);

  REQUIRE(state.count == 3);
  REQUIRE(std::memcmp(state.state, digest.data(), digest.size()) == 0);
  REQUIRE(std::memcmp(state.buffer, digest.data(), digest.size()) == 0);
}
