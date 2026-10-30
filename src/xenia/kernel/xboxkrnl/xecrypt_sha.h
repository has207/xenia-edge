/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_SHA_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_SHA_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/mbedtls/include/mbedtls/sha512.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

struct XECRYPT_SHA_STATE {
  xe::be<uint32_t> count;     // 0x0
  xe::be<uint32_t> state[5];  // 0x4
  uint8_t buffer[64];         // 0x18
};
static_assert_size(XECRYPT_SHA_STATE, 0x58);

// TODO: Size of this struct hasn't been confirmed yet.
struct XECRYPT_SHA256_STATE {
  xe::be<uint32_t> count;     // 0x0
  xe::be<uint32_t> state[8];  // 0x4
  uint8_t buffer[64];         // 0x24
};

// TODO: Size of this struct hasn't been confirmed yet.
struct XECRYPT_SHA512_STATE {
  xe::be<uint64_t> count;     // 0x0
  xe::be<uint64_t> state[8];  // 0x8
  uint8_t buffer[128];        // 0x48
};

struct XECRYPT_HMACSHA_STATE {
  XECRYPT_SHA_STATE sha_state[2];
};
static_assert_size(XECRYPT_HMACSHA_STATE, 0xB0);

inline mbedtls_sha1_context LoadSha(const XECRYPT_SHA_STATE& state) {
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  context.MBEDTLS_PRIVATE(total)[0] = state.count;
  std::ranges::copy(state.state, context.MBEDTLS_PRIVATE(state));
  std::ranges::copy(state.buffer, context.MBEDTLS_PRIVATE(buffer));
  return context;
}

inline void StoreSha(const mbedtls_sha1_context& context,
                     XECRYPT_SHA_STATE& state) {
  state.count = context.MBEDTLS_PRIVATE(total)[0];
  std::ranges::copy(context.MBEDTLS_PRIVATE(state), state.state);
  std::ranges::copy(context.MBEDTLS_PRIVATE(buffer), state.buffer);
}

inline void XeCryptShaInit(XECRYPT_SHA_STATE* state) {
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  mbedtls_sha1_starts(&context);
  StoreSha(context, *state);
}

inline void XeCryptShaUpdate(XECRYPT_SHA_STATE* state, const uint8_t* input,
                             uint32_t input_size) {
  auto context = LoadSha(*state);
  mbedtls_sha1_update(&context, input, input_size);
  StoreSha(context, *state);
}

// Pads like the console, in place with a 32-bit bit count and the count kept.
// The last block it compressed stays in the context buffer, not the message
// tail. Only shows up when a title finalizes the same context twice and the
// padding needed a second block, where the tail no longer survives the first
// call.
inline void XeCryptShaFinal(XECRYPT_SHA_STATE* state, uint8_t* out,
                            uint32_t out_size) {
  const uint32_t count = state->count;
  const uint32_t used = count % 64;
  const uint32_t length_offset = (used < 56 ? 56 : 120) - used;
  uint8_t padding[72] = {0x80};
  const xe::be<uint32_t> bit_count = count << 3;
  std::memcpy(padding + length_offset + 4, &bit_count, sizeof(bit_count));

  auto context = LoadSha(*state);
  mbedtls_sha1_update(&context, padding, length_offset);
  // On its own the length completes the block in the buffer instead of being
  // compressed straight from the padding.
  mbedtls_sha1_update(&context, padding + length_offset, 8);
  StoreSha(context, *state);
  state->count = count;

  if (out_size) {
    std::memcpy(out, state->state, std::min<uint32_t>(out_size, 0x14));
  }
}

inline void XeCryptSha(const uint8_t* input_1, uint32_t input_1_size,
                       const uint8_t* input_2, uint32_t input_2_size,
                       const uint8_t* input_3, uint32_t input_3_size,
                       uint8_t* out, uint32_t out_size) {
  XECRYPT_SHA_STATE state;
  XeCryptShaInit(&state);
  if (input_1 && input_1_size) {
    XeCryptShaUpdate(&state, input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    XeCryptShaUpdate(&state, input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    XeCryptShaUpdate(&state, input_3, input_3_size);
  }
  XeCryptShaFinal(&state, out, out_size);
}

// Keys over a block are truncated, not hashed as in RFC 2104.
inline mbedtls_sha256_context LoadSha256(const XECRYPT_SHA256_STATE& state) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  context.MBEDTLS_PRIVATE(total)[0] = state.count;
  std::ranges::copy(state.state, context.MBEDTLS_PRIVATE(state));
  std::ranges::copy(state.buffer, context.MBEDTLS_PRIVATE(buffer));
  return context;
}

inline void StoreSha256(const mbedtls_sha256_context& context,
                        XECRYPT_SHA256_STATE& state) {
  state.count = context.MBEDTLS_PRIVATE(total)[0];
  std::ranges::copy(context.MBEDTLS_PRIVATE(state), state.state);
  std::ranges::copy(context.MBEDTLS_PRIVATE(buffer), state.buffer);
}

inline void XeCryptSha256Init(XECRYPT_SHA256_STATE* state) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  StoreSha256(context, *state);
}

inline void XeCryptSha256Update(XECRYPT_SHA256_STATE* state,
                                const uint8_t* input, uint32_t input_size) {
  auto context = LoadSha256(*state);
  mbedtls_sha256_update(&context, input, input_size);
  StoreSha256(context, *state);
}

// Leaves the count and state words alone and copies the digest into the
// buffer.
inline void XeCryptSha256Final(XECRYPT_SHA256_STATE* state, uint8_t* out,
                               uint32_t out_size) {
  auto context = LoadSha256(*state);
  uint8_t digest[32];
  mbedtls_sha256_finish(&context, digest);
  if (out_size) {
    std::memcpy(out, digest, std::min<uint32_t>(out_size, sizeof(digest)));
  }
  std::memcpy(state->buffer, digest, sizeof(digest));
}

inline void XeCryptSha256(const uint8_t* input_1, uint32_t input_1_size,
                          const uint8_t* input_2, uint32_t input_2_size,
                          const uint8_t* input_3, uint32_t input_3_size,
                          uint8_t* out, uint32_t out_size) {
  XECRYPT_SHA256_STATE state;
  XeCryptSha256Init(&state);
  if (input_1 && input_1_size) {
    XeCryptSha256Update(&state, input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    XeCryptSha256Update(&state, input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    XeCryptSha256Update(&state, input_3, input_3_size);
  }
  XeCryptSha256Final(&state, out, out_size);
}

inline mbedtls_sha512_context LoadSha512(const XECRYPT_SHA512_STATE& state) {
  mbedtls_sha512_context context;
  mbedtls_sha512_init(&context);
  context.MBEDTLS_PRIVATE(total)[0] = state.count;
  std::ranges::copy(state.state, context.MBEDTLS_PRIVATE(state));
  std::ranges::copy(state.buffer, context.MBEDTLS_PRIVATE(buffer));
  return context;
}

inline void StoreSha512(const mbedtls_sha512_context& context,
                        XECRYPT_SHA512_STATE& state) {
  state.count = context.MBEDTLS_PRIVATE(total)[0];
  std::ranges::copy(context.MBEDTLS_PRIVATE(state), state.state);
  std::ranges::copy(context.MBEDTLS_PRIVATE(buffer), state.buffer);
}

inline void XeCryptSha512Init(XECRYPT_SHA512_STATE* state) {
  mbedtls_sha512_context context;
  mbedtls_sha512_init(&context);
  mbedtls_sha512_starts(&context, 0);
  StoreSha512(context, *state);
}

inline void XeCryptSha512Update(XECRYPT_SHA512_STATE* state,
                                const uint8_t* input, uint32_t input_size) {
  auto context = LoadSha512(*state);
  mbedtls_sha512_update(&context, input, input_size);
  StoreSha512(context, *state);
}

// Leaves the count and state words alone and copies the digest into the
// buffer.
inline void XeCryptSha512Final(XECRYPT_SHA512_STATE* state, uint8_t* out,
                               uint32_t out_size) {
  auto context = LoadSha512(*state);
  uint8_t digest[64];
  mbedtls_sha512_finish(&context, digest);
  if (out_size) {
    std::memcpy(out, digest, std::min<uint32_t>(out_size, sizeof(digest)));
  }
  std::memcpy(state->buffer, digest, sizeof(digest));
}

inline void XeCryptHmacShaInit(XECRYPT_HMACSHA_STATE* state, const uint8_t* key,
                               uint32_t key_size) {
  key_size = std::min<uint32_t>(key_size, 64);
  std::array<uint8_t, 64> inner_pad;
  std::array<uint8_t, 64> outer_pad;
  inner_pad.fill(0x36);
  outer_pad.fill(0x5C);
  for (uint32_t i = 0; i < key_size; i++) {
    inner_pad[i] ^= key[i];
    outer_pad[i] ^= key[i];
  }

  XeCryptShaInit(&state->sha_state[0]);
  XeCryptShaUpdate(&state->sha_state[0], inner_pad.data(), 64);
  XeCryptShaInit(&state->sha_state[1]);
  XeCryptShaUpdate(&state->sha_state[1], outer_pad.data(), 64);
}

inline void XeCryptHmacShaUpdate(XECRYPT_HMACSHA_STATE* state,
                                 const uint8_t* input, uint32_t input_size) {
  XeCryptShaUpdate(&state->sha_state[0], input, input_size);
}

inline void XeCryptHmacShaFinal(XECRYPT_HMACSHA_STATE* state, uint8_t* out,
                                uint32_t out_size) {
  XeCryptShaFinal(&state->sha_state[0], nullptr, 0);
  XeCryptShaUpdate(&state->sha_state[1],
                   reinterpret_cast<const uint8_t*>(state->sha_state[0].state),
                   sizeof(XECRYPT_SHA_STATE::state));
  XeCryptShaFinal(&state->sha_state[1], out, out_size);
}

inline void XeCryptHmacSha(const uint8_t* key, uint32_t key_size,
                           const uint8_t* inp_1, uint32_t inp_1_size,
                           const uint8_t* inp_2, uint32_t inp_2_size,
                           const uint8_t* inp_3, uint32_t inp_3_size,
                           uint8_t* out, uint32_t out_size) {
  XECRYPT_HMACSHA_STATE state;
  XeCryptHmacShaInit(&state, key, key_size);
  if (inp_1_size) {
    XeCryptHmacShaUpdate(&state, inp_1, inp_1_size);
  }
  if (inp_2_size) {
    XeCryptHmacShaUpdate(&state, inp_2, inp_2_size);
  }
  if (inp_3_size) {
    XeCryptHmacShaUpdate(&state, inp_3, inp_3_size);
  }
  XeCryptHmacShaFinal(&state, out, out_size);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XECRYPT_SHA_H_
