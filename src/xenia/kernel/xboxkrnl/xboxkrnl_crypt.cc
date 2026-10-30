/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2022 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include <algorithm>
#include <tuple>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_crypt.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

#include "xenia/kernel/xboxkrnl/xecrypt_aes.h"
#include "xenia/kernel/xboxkrnl/xecrypt_des.h"
#include "xenia/kernel/xboxkrnl/xecrypt_md5.h"
#include "xenia/kernel/xboxkrnl/xecrypt_rsa.h"
#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

struct XECRYPT_RC4_STATE {
  uint8_t S[256];  // 0x0
  uint8_t i;       // 0x100
  uint8_t j;       // 0x101
};
static_assert_size(XECRYPT_RC4_STATE, 0x102);

// TODO: Size of this struct hasn't been confirmed yet.
struct XECRYPT_RSA {
  xe::be<uint32_t> size;  // size of modulus in 8 byte units
  xe::be<uint32_t> public_exponent;
  xe::be<uint64_t> pad_8;

  // followed by modulus, followed by any private-key data
};
static_assert_size(XECRYPT_RSA, 0x10);

struct XECRYPT_SIG {
  xe::be<uint64_t> aqwPad[0x1C];
  xe::be<uint8_t> One;
  uint8_t Salt[0xA];
  uint8_t Hash[0x14];
  xe::be<uint8_t> End;
};
static_assert_size(XECRYPT_SIG, 0x100);  // size 256

// Keys
// TODO: Array of keys we need

// Console Serial Number Key - unimplemented
static constexpr uint8_t key14[] = {'0', '1', '2', '3', '4', '5',
                                    '6', '7', '8', '9', '1', '0'};  // string

// Motherboard Serial Number Key, XeFu builds the Xbox 1 HD key from it
static constexpr uint8_t key15[] = {'0', '1', '2', '3', '4', '5',
                                    '6', '7', '8', '9', '1', '0'};  // string

// Retail key 0x19
static constexpr uint8_t key19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1,
                                    0xEA, 0xE9, 0xAB, 0x31, 0x70, 0xF3,
                                    0xAD, 0x47, 0xEB, 0xF3};

// Pirs Public Key
struct XECRYPT_RSAPUB_2048 {
  XECRYPT_RSA Rsa;
  xe::be<uint64_t> aqwM[0x20];
};
static_assert_size(XECRYPT_RSAPUB_2048, 0x110);

static constexpr uint8_t key39[] = {                 // pirs_retail_public
    0x00, 0x00, 0x00, 0x20,                          // cqw
    0x00, 0x00, 0x00, 0x03,                          // dwPubExp
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // qwReserved
    // aqwM
    0xE6, 0x3B, 0x32, 0xB2, 0x8D, 0x9E, 0x9E, 0xE7, 0x9D, 0xFC, 0x5C, 0x72,
    0x41, 0x94, 0x58, 0x47, 0xDE, 0x0D, 0x18, 0x40, 0x72, 0xD6, 0xE3, 0x46,
    0x8E, 0xBA, 0x8E, 0xBC, 0x1A, 0x90, 0xAC, 0x20, 0xBA, 0x03, 0x85, 0xB5,
    0x1A, 0x3E, 0x25, 0xF9, 0xA6, 0x58, 0xEB, 0xB6, 0xA3, 0xC4, 0xA3, 0xEE,
    0xB2, 0xB0, 0xAE, 0x97, 0x69, 0xEB, 0xFE, 0x71, 0xFC, 0x02, 0xAB, 0x77,
    0xBA, 0xC8, 0xE6, 0x74, 0xE6, 0x7C, 0x63, 0x0E, 0xAF, 0x4C, 0xF7, 0xE7,
    0x11, 0x4A, 0x80, 0x24, 0x72, 0x05, 0x7A, 0x63, 0xD0, 0xF8, 0x91, 0x02,
    0xA6, 0xE7, 0x7D, 0x77, 0xC5, 0xA7, 0x9B, 0x08, 0x11, 0x2E, 0xA0, 0x64,
    0x45, 0x60, 0x46, 0xBC, 0x36, 0xE1, 0x17, 0x71, 0xBE, 0x66, 0x49, 0x2F,
    0xAE, 0x20, 0xA4, 0x76, 0x9C, 0x27, 0x51, 0xCF, 0x4B, 0x34, 0x7A, 0x35,
    0xBC, 0xA4, 0xAA, 0x1C, 0x47, 0x4B, 0xF4, 0x97, 0x22, 0x4E, 0x13, 0x24,
    0xD3, 0xC1, 0x57, 0xDF, 0x4D, 0x84, 0xB9, 0x18, 0x97, 0x99, 0xAC, 0x00,
    0xB3, 0x3D, 0x03, 0x25, 0x60, 0xC8, 0x7A, 0x59, 0xFE, 0x48, 0xFF, 0x28,
    0x3D, 0x10, 0xBB, 0x9E, 0x09, 0x06, 0x2A, 0x61, 0x20, 0x2C, 0xF8, 0x72,
    0xEB, 0x87, 0xE6, 0xD1, 0xFB, 0xB3, 0x66, 0xFC, 0x4A, 0x02, 0xAE, 0xD4,
    0xD8, 0x37, 0xCF, 0xA6, 0x32, 0x25, 0x79, 0x36, 0x0E, 0xF4, 0xED, 0x19,
    0xA2, 0x10, 0x27, 0x96, 0x2F, 0x9F, 0xA9, 0x3D, 0xA4, 0x37, 0x30, 0x11,
    0x51, 0x83, 0xBD, 0xF7, 0xC7, 0xE5, 0xCE, 0xAA, 0xEC, 0xDE, 0x48, 0xA0,
    0x84, 0xF7, 0xB0, 0xF6, 0x4B, 0x8E, 0xF0, 0x89, 0xBD, 0x47, 0x7C, 0x90,
    0xDD, 0x88, 0x12, 0x17, 0x40, 0xD2, 0x4E, 0xA6, 0xC6, 0x11, 0x04, 0x1B,
    0x57, 0xA8, 0x68, 0xB4, 0x61, 0xF4, 0x1B, 0xC6, 0x8B, 0xE8, 0xD9, 0x20,
    0xF2, 0x05, 0xE0, 0x70};

const static std::map<uint16_t, std::span<const uint8_t>> key_vault = {
    {0x14, key14},
    {0x15, key15},
    {0x39, key39},
};

enum X_KEY_INDEX : uint32_t {
  MANUFACTURING_MODE = 0x0,
  ALTERNATE_KEY_VAULT = 0x1,
  RESTRICTED_PRIVILEGES_FLAGS = 0x2,
  RESERVED_BYTE3 = 0x3,
  ODD_FEATURES = 0x4,
  ODD_AUTHTYPE = 0x5,
  RESTRICTED_HVEXT_LOADER = 0x6,
  POLICY_FLASH_SIZE = 0x7,
  POLICY_BUILTIN_USBMU_SIZE = 0x8,
  RESERVED_DWORD4 = 0x9,
  RESTRICTED_PRIVILEGES = 0xA,
  RESERVED_QWORD2 = 0xB,
  RESERVED_QWORD3 = 0xC,
  RESERVED_QWORD4 = 0xD,
  RESERVED_KEY1 = 0xE,
  RESERVED_KEY2 = 0xF,
  RESERVED_KEY3 = 0x10,
  RESERVED_KEY4 = 0x11,
  RESERVED_RANDOM_KEY1 = 0x12,
  RESERVED_RANDOM_KEY2 = 0x13,
  CONSOLE_SERIAL_NUMBER = 0x14,
  MOBO_SERIAL_NUMBER = 0x15,
  GAME_REGION = 0x16,
  CONSOLE_OBFUSCATION_KEY = 0x17,
  KEY_OBFUSCATION_KEY = 0x18,
  ROAMABLE_OBFUSCATION_KEY = 0x19,
  DVD_KEY = 0x1A,
  PRIMARY_ACTIVATION_KEY = 0x1B,
  SECONDARY_ACTIVATION_KEY = 0x1C,
  GLOBAL_DEVICE_2DES_KEY1 = 0x1D,
  GLOBAL_DEVICE_2DES_KEY2 = 0x1E,
  WIRELESS_CONTROLLER_MS_2DES_KEY1 = 0x1F,
  WIRELESS_CONTROLLER_MS_2DES_KEY2 = 0x20,
  WIRED_WEBCAM_MS_2DES_KEY1 = 0x21,
  WIRED_WEBCAM_MS_2DES_KEY2 = 0x22,
  WIRED_CONTROLLER_MS_2DES_KEY1 = 0x23,
  WIRED_CONTROLLER_MS_2DES_KEY2 = 0x24,
  MEMORY_UNIT_MS_2DES_KEY1 = 0x25,
  MEMORY_UNIT_MS_2DES_KEY2 = 0x26,
  OTHER_XSM3_DEVICE_MS_2DES_KEY1 = 0x27,
  OTHER_XSM3_DEVICE_MS_2DES_KEY2 = 0x28,
  WIRELESS_CONTROLLER_3P_2DES_KEY1 = 0x29,
  WIRELESS_CONTROLLER_3P_2DES_KEY2 = 0x2A,
  WIRED_WEBCAM_3P_2DES_KEY1 = 0x2B,
  WIRED_WEBCAM_3P_2DES_KEY2 = 0x2C,
  WIRED_CONTROLLER_3P_2DES_KEY1 = 0x2D,
  WIRED_CONTROLLER_3P_2DES_KEY2 = 0x2E,
  MEMORY_UNIT_3P_2DES_KEY1 = 0x2F,
  MEMORY_UNIT_3P_2DES_KEY2 = 0x30,
  OTHER_XSM3_DEVICE_3P_2DES_KEY1 = 0x31,
  OTHER_XSM3_DEVICE_3P_2DES_KEY2 = 0x32,
  CONSOLE_PRIVATE_KEY = 0x33,
  XEIKA_PRIVATE_KEY = 0x34,
  CARDEA_PRIVATE_KEY = 0x35,
  CONSOLE_CERTIFICATE = 0x36,
  XEIKA_CERTIFICATE = 0x37,
  CARDEA_CERTIFICATE = 0x38,
  MAX_KEY_INDEX = 0x39,
  // constant keys
  CONSTANT_PIRS_KEY = 0x39,
  CONSTANT_ALT_MASTER_KEY = 0x3A,
  CONSTANT_ALT_LIVE_KEY = 0x3B,
  CONSTANT_MASTER_KEY = 0x3C,
  CONSTANT_LIVE_KEY = 0x3D,
  CONSTANT_XB1_GREEN_KEY = 0x3E,
  CONSTANT_SATA_DISK_SECURITY_KEY = 0x3F,
  CONSTANT_DEVICE_REVOCATION_KEY = 0x40,
  CONSTANT_XMACS_KEY = 0x41,
  CONSTANT_REVOCATION_LIST_NONCE = 0x42,
  CONSTANT_CROSS_PLATFORM_SYSLINK_KEY = 0x43,
  // special keys
  SPECIAL_KEY_VAULT_SIGNATURE = 0x44,
  SPECIAL_SECROM_DIGEST = 0x45,
  SPECIAL_SECDATA = 0x46,
  SPECIAL_DVD_FIRMWARE_KEY = 0x47,
  SPECIAL_DEBUG_UNLOCK = 0x48,
  MAX_CONSTANT_INDEX = 0x49,
  // title keys
  TITLE_KEYS_BASE = 0xE0,
  TITLE_KEYS_LIMIT = 0xE8,
  TITLE_KEYS_RESET = 0xF0,
  // secured keys
  SECURED_DATA_BASE = 0x1000,
  SECURED_DATA_LIMIT = 0x2000,
};

// https://github.com/emoose/ExCrypt/blob/master/src/exkeys.cpp#L7
std::map<uint32_t, std::tuple<uint32_t, uint32_t>> X_Key_Properties = {
    {MANUFACTURING_MODE, {0x8, 0x1}},
    {ALTERNATE_KEY_VAULT, {0x9, 0x1}},
    {RESTRICTED_PRIVILEGES_FLAGS, {0xA, 0x1}},
    {RESERVED_BYTE3, {0xB, 0x1}},
    {ODD_FEATURES, {0xC, 0x2}},
    {ODD_AUTHTYPE, {0xE, 0x2}},
    {RESTRICTED_HVEXT_LOADER, {0x10, 0x4}},
    {POLICY_FLASH_SIZE, {0x14, 0x4}},
    {POLICY_BUILTIN_USBMU_SIZE, {0x18, 0x4}},
    {RESERVED_DWORD4, {0x1C, 0x4}},
    {RESTRICTED_PRIVILEGES, {0x20, 0x8}},
    {RESERVED_QWORD2, {0x28, 0x8}},
    {RESERVED_QWORD3, {0x30, 0x8}},
    {RESERVED_QWORD4, {0x38, 0x8}},
    {RESERVED_KEY1, {0x40, 0x10}},
    {RESERVED_KEY2, {0x50, 0x10}},
    {RESERVED_KEY3, {0x60, 0x10}},
    {RESERVED_KEY4, {0x70, 0x10}},
    {RESERVED_RANDOM_KEY1, {0x80, 0x10}},
    {RESERVED_RANDOM_KEY2, {0x90, 0x10}},
    {CONSOLE_SERIAL_NUMBER, {0xA0, 0xC}},
    {MOBO_SERIAL_NUMBER, {0xAC, 0xC}},
    {GAME_REGION, {0xB8, 0x2}},
    // 6 bytes padding
    {CONSOLE_OBFUSCATION_KEY, {0xC0, 0x10}},
    {KEY_OBFUSCATION_KEY, {0xD0, 0x10}},
    {ROAMABLE_OBFUSCATION_KEY, {0xE0, 0x10}},
    {DVD_KEY, {0xF0, 0x10}},
    {PRIMARY_ACTIVATION_KEY, {0x100, 0x18}},
    {SECONDARY_ACTIVATION_KEY, {0x118, 0x10}},
    {GLOBAL_DEVICE_2DES_KEY1, {0x128, 0x10}},
    {GLOBAL_DEVICE_2DES_KEY2, {0x138, 0x10}},
    {WIRELESS_CONTROLLER_MS_2DES_KEY1, {0x148, 0x10}},
    {WIRELESS_CONTROLLER_MS_2DES_KEY2, {0x158, 0x10}},
    {WIRED_WEBCAM_MS_2DES_KEY1, {0x168, 0x10}},
    {WIRED_WEBCAM_MS_2DES_KEY2, {0x178, 0x10}},
    {WIRED_CONTROLLER_MS_2DES_KEY1, {0x188, 0x10}},
    {WIRED_CONTROLLER_MS_2DES_KEY2, {0x198, 0x10}},
    {MEMORY_UNIT_MS_2DES_KEY1, {0x1A8, 0x10}},
    {MEMORY_UNIT_MS_2DES_KEY2, {0x1B8, 0x10}},
    {OTHER_XSM3_DEVICE_MS_2DES_KEY1, {0x1C8, 0x10}},
    {OTHER_XSM3_DEVICE_MS_2DES_KEY2, {0x1D8, 0x10}},
    {WIRELESS_CONTROLLER_3P_2DES_KEY1, {0x1E8, 0x10}},
    {WIRELESS_CONTROLLER_3P_2DES_KEY2, {0x1F8, 0x10}},
    {WIRED_WEBCAM_3P_2DES_KEY1, {0x208, 0x10}},
    {WIRED_WEBCAM_3P_2DES_KEY2, {0x218, 0x10}},
    {WIRED_CONTROLLER_3P_2DES_KEY1, {0x228, 0x10}},
    {WIRED_CONTROLLER_3P_2DES_KEY2, {0x238, 0x10}},
    {MEMORY_UNIT_3P_2DES_KEY1, {0x248, 0x10}},
    {MEMORY_UNIT_3P_2DES_KEY2, {0x258, 0x10}},
    {OTHER_XSM3_DEVICE_3P_2DES_KEY1, {0x268, 0x10}},
    {OTHER_XSM3_DEVICE_3P_2DES_KEY2, {0x278, 0x10}},
    {CONSOLE_PRIVATE_KEY, {0x288, 0x1D0}},
    {XEIKA_PRIVATE_KEY, {0x458, 0x390}},
    {CARDEA_PRIVATE_KEY, {0x7E8, 0x1D0}},
    {CONSOLE_CERTIFICATE, {0x9B8, 0x1A8}},
    {XEIKA_CERTIFICATE, {0xB60, 0x1288}},
    {SPECIAL_KEY_VAULT_SIGNATURE, {0x1DF8, 0x100}},
    {CARDEA_CERTIFICATE, {0x1EE8, 0x2108}},
};

static const uint8_t xe_key_obfuscation_key[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#pragma pack(push, 1)
struct XE_CONSOLE_ID {
  union {
    struct {
      uint8_t RefurbBits : 4;
      uint8_t ManufactureMonth : 4;
      uint32_t ManufactureYear : 4;
      uint32_t MacIndex3 : 8;
      uint32_t MacIndex4 : 8;
      uint32_t MacIndex5 : 8;
      uint32_t Crc : 4;
    };
    uint8_t Data[5];
  };
};
#pragma pack(pop)

struct X_CONSOLE_PUBLIC_KEY {
  uint8_t public_exponent[0x4];  // 0x0 sz:0x4
  uint8_t modulus[0x80];         // 0x4 sz:0x80
};  // size 132
static_assert_size(X_CONSOLE_PUBLIC_KEY, 0x84);

enum XConsoleType : uint32_t {
  Invalid = 0,
  Devkit = 1,
  Retail = 2,
  Testkit = 0x40000001,
  RecoveredDevkit = 0x80000001,
  Pre1888Devkit = 0x80000002
};

struct XE_CONSOLE_CERTIFICATE {
  xe::be<uint16_t> cert_size;               // 0x0 sz:0x2
  XE_CONSOLE_ID console_id;                 // 0x2 sz:0x5
  uint8_t console_part_number[11];          // 0x7 sz:0xB
  uint8_t reserved[4];                      // 0x12 sz:0x4
  xe::be<uint16_t> privileges;              // 0x16 sz:0x2
  xe::be<XConsoleType> console_type;        // 0x18 sz:0x4
  uint8_t manufacture_date[8];              // 0x1C sz:0x8
  X_CONSOLE_PUBLIC_KEY console_public_key;  // 0x24 sz:0x84
  XECRYPT_SIG signature;                    // 0xA8 sz:0x100
};  // size 424
static_assert_size(XE_CONSOLE_CERTIFICATE, 0x1A8);

struct XE_CONSOLE_SIGNATURE {
  XE_CONSOLE_CERTIFICATE console_certificate;
  uint8_t signature[0x80];
};
static_assert_size(XE_CONSOLE_SIGNATURE, 0x228);

void XeCryptRc4Key_entry(pointer_t<XECRYPT_RC4_STATE> rc4_ctx, lpvoid_t key,
                         dword_t key_size) {
  // Setup RC4 state
  rc4_ctx->i = rc4_ctx->j = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    rc4_ctx->S[x] = (uint8_t)x;
  }

  // A key of no bytes has nothing to schedule and would divide by zero.
  if (!key_size) {
    return;
  }
  uint32_t idx = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    idx = (idx + rc4_ctx->S[x] + key[x % key_size]) % 0x100;
    uint8_t temp = rc4_ctx->S[idx];
    rc4_ctx->S[idx] = rc4_ctx->S[x];
    rc4_ctx->S[x] = temp;
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4Key, kNone, kImplemented);

void XeCryptRc4Ecb_entry(pointer_t<XECRYPT_RC4_STATE> rc4_ctx, lpvoid_t data,
                         dword_t size) {
  // Crypt data
  for (uint32_t idx = 0; idx < size; idx++) {
    rc4_ctx->i = (rc4_ctx->i + 1) % 0x100;
    rc4_ctx->j = (rc4_ctx->j + rc4_ctx->S[rc4_ctx->i]) % 0x100;
    uint8_t temp = rc4_ctx->S[rc4_ctx->i];
    rc4_ctx->S[rc4_ctx->i] = rc4_ctx->S[rc4_ctx->j];
    rc4_ctx->S[rc4_ctx->j] = temp;

    uint8_t a = data[idx];
    uint8_t b =
        rc4_ctx->S[(rc4_ctx->S[rc4_ctx->i] + rc4_ctx->S[rc4_ctx->j]) % 0x100];
    data[idx] = (uint8_t)(a ^ b);
  }
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4Ecb, kNone, kImplemented);

void XeCryptRc4_entry(lpvoid_t key, dword_t key_size, lpvoid_t data,
                      dword_t size) {
  XECRYPT_RC4_STATE rc4_ctx;
  XeCryptRc4Key_entry(&rc4_ctx, key, key_size);
  XeCryptRc4Ecb_entry(&rc4_ctx, data, size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRc4, kNone, kImplemented);

void XeCryptShaInit_entry(pointer_t<XECRYPT_SHA_STATE> sha_state) {
  XeCryptShaInit(sha_state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaInit, kNone, kImplemented);

void XeCryptShaUpdate_entry(pointer_t<XECRYPT_SHA_STATE> sha_state,
                            lpvoid_t input, dword_t input_size) {
  XeCryptShaUpdate(sha_state, input.as<uint8_t*>(), input_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaUpdate, kNone, kImplemented);

void XeCryptShaFinal_entry(pointer_t<XECRYPT_SHA_STATE> sha_state,
                           pointer_t<uint8_t> out, dword_t out_size) {
  XeCryptShaFinal(sha_state, out, out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptShaFinal, kNone, kImplemented);

void XeCryptSha_entry(lpvoid_t input_1, dword_t input_1_size, lpvoid_t input_2,
                      dword_t input_2_size, lpvoid_t input_3,
                      dword_t input_3_size, lpvoid_t output,
                      dword_t output_size) {
  XeCryptSha(input_1.as<uint8_t*>(), input_1_size, input_2.as<uint8_t*>(),
             input_2_size, input_3.as<uint8_t*>(), input_3_size,
             output.as<uint8_t*>(), output_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha, kNone, kImplemented);

void XeCryptHmacShaInit_entry(pointer_t<XECRYPT_HMACSHA_STATE> sha_state_ptr,
                              lpvoid_t key_ptr, dword_t key_size) {
  XeCryptHmacShaInit(sha_state_ptr, key_ptr.as<uint8_t*>(), key_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptHmacShaInit, kNone, kImplemented);

void XeCryptHmacShaUpdate_entry(pointer_t<XECRYPT_HMACSHA_STATE> sha_state_ptr,
                                lpvoid_t input, dword_t input_size) {
  XeCryptHmacShaUpdate(sha_state_ptr, input.as<uint8_t*>(), input_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptHmacShaUpdate, kNone, kImplemented);

void XeCryptHmacShaFinal_entry(pointer_t<XECRYPT_HMACSHA_STATE> sha_state_ptr,
                               pointer_t<uint8_t> out, dword_t out_size) {
  XeCryptHmacShaFinal(sha_state_ptr, out, out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptHmacShaFinal, kNone, kImplemented);

void XeCryptMd5_entry(lpvoid_t input_1, dword_t input_1_size, lpvoid_t input_2,
                      dword_t input_2_size, lpvoid_t input_3,
                      dword_t input_3_size, lpvoid_t output,
                      dword_t output_size) {
  XeCryptMd5(input_1.as<uint8_t*>(), input_1_size, input_2.as<uint8_t*>(),
             input_2_size, input_3.as<uint8_t*>(), input_3_size,
             output.as<uint8_t*>(), output_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptMd5, kNone, kImplemented);

void XeCryptSha256Init_entry(pointer_t<XECRYPT_SHA256_STATE> sha_state) {
  XeCryptSha256Init(sha_state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Init, kNone, kImplemented);

void XeCryptSha256Update_entry(pointer_t<XECRYPT_SHA256_STATE> sha_state,
                               lpvoid_t input, dword_t input_size) {
  XeCryptSha256Update(sha_state, input.as<uint8_t*>(), input_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Update, kNone, kImplemented);

void XeCryptSha256Final_entry(pointer_t<XECRYPT_SHA256_STATE> sha_state,
                              pointer_t<uint8_t> out, dword_t out_size) {
  XeCryptSha256Final(sha_state, out, out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256Final, kNone, kImplemented);

void XeCryptSha256_entry(lpvoid_t input_1, dword_t input_1_size,
                         lpvoid_t input_2, dword_t input_2_size,
                         lpvoid_t input_3, dword_t input_3_size,
                         lpvoid_t output, dword_t output_size) {
  XeCryptSha256(input_1.as<uint8_t*>(), input_1_size, input_2.as<uint8_t*>(),
                input_2_size, input_3.as<uint8_t*>(), input_3_size,
                output.as<uint8_t*>(), output ? uint32_t(output_size) : 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha256, kNone, kImplemented);

void XeCryptSha512Init_entry(pointer_t<XECRYPT_SHA512_STATE> sha_state) {
  XeCryptSha512Init(sha_state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha512Init, kNone, kImplemented);

void XeCryptSha512Update_entry(pointer_t<XECRYPT_SHA512_STATE> sha_state,
                               lpvoid_t input, dword_t input_size) {
  XeCryptSha512Update(sha_state, input.as<uint8_t*>(), input_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha512Update, kNone, kImplemented);

void XeCryptSha512Final_entry(pointer_t<XECRYPT_SHA512_STATE> sha_state,
                              pointer_t<uint8_t> out, dword_t out_size) {
  XeCryptSha512Final(sha_state, out, out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptSha512Final, kNone, kImplemented);

void XeCryptMd5Init_entry(pointer_t<XECRYPT_MD5_STATE> md5_state) {
  XeCryptMd5Init(md5_state);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptMd5Init, kNone, kImplemented);

void XeCryptMd5Update_entry(pointer_t<XECRYPT_MD5_STATE> md5_state,
                            lpvoid_t input, dword_t input_size) {
  XeCryptMd5Update(md5_state, input.as<uint8_t*>(), input_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptMd5Update, kNone, kImplemented);

void XeCryptMd5Final_entry(pointer_t<XECRYPT_MD5_STATE> md5_state,
                           pointer_t<uint8_t> out, dword_t out_size) {
  XeCryptMd5Final(md5_state, out, out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptMd5Final, kNone, kImplemented);

// Byteswaps each 8 bytes
void XeCryptBnQw_SwapDwQwLeBe_entry(pointer_t<uint64_t> qw_inp,
                                    pointer_t<uint64_t> qw_out, dword_t size) {
  xe::copy_and_swap<uint64_t>(qw_out, qw_inp, size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQw_SwapDwQwLeBe, kNone, kImplemented);

dword_result_t XeCryptBnQwNeRsaPubCrypt_entry(pointer_t<uint64_t> qw_a,
                                              pointer_t<uint64_t> qw_b,
                                              pointer_t<XECRYPT_RSA> rsa) {
  uint32_t num_qwords = rsa->size;
  uint32_t exponent = rsa->public_exponent;
  const uint8_t* input_bytes = reinterpret_cast<const uint8_t*>(&qw_a[0]);
  uint8_t* output_bytes = reinterpret_cast<uint8_t*>(&qw_b[0]);
  const uint8_t* mod_bytes =
      reinterpret_cast<const uint8_t*>(&rsa[1]);  // modulus follows header

  return XeCryptBnQwNeRsaPubCrypt(input_bytes, output_bytes, mod_bytes,
                                  num_qwords, exponent);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwNeRsaPubCrypt, kNone, kImplemented);

dword_result_t XeCryptBnQwBeSigVerify_entry(pointer_t<XECRYPT_SIG> sig,
                                            lpvoid_t hash, lpstring_t salt,
                                            pointer_t<XECRYPT_RSA> rsa) {
  // hash from XeCryptRotSumSha
  // known salts are "XBOX360SVC", "XBOX360XTT", "TDBXBOX36O"
  return true;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnQwBeSigVerify, kNone, kStub);

dword_result_t XeCryptBnDwLePkcs1Verify_entry(lpvoid_t hash, lpvoid_t sig,
                                              dword_t size) {
  // BOOL return value
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptBnDwLePkcs1Verify, kNone, kStub);

void XeCryptRandom_entry(lpvoid_t buf, dword_t buf_size) {
  std::memset(buf, 0xFD, buf_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRandom, kNone, kStub);

void XeCryptDesKey_entry(pointer_t<XECRYPT_DES_STATE> state_ptr, lpvoid_t key) {
  XeCryptDesKey(state_ptr, key.as<uint8_t*>());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDesKey, kNone, kImplemented);

void XeCryptDesEcb_entry(pointer_t<XECRYPT_DES_STATE> state_ptr, lpvoid_t inp,
                         lpvoid_t out, dword_t encrypt) {
  XeCryptDesEcb(state_ptr, inp.as<uint8_t*>(), out.as<uint8_t*>(),
                encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDesEcb, kNone, kImplemented);

void XeCryptDesCbc_entry(pointer_t<XECRYPT_DES_STATE> state_ptr, lpvoid_t inp,
                         dword_t inp_size, lpvoid_t out, lpvoid_t feed,
                         dword_t encrypt) {
  XeCryptDesCbc(state_ptr, inp.as<uint8_t*>(), inp_size, out.as<uint8_t*>(),
                feed.as<uint8_t*>(), encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDesCbc, kNone, kImplemented);

// Sets bit 0 to make the parity odd
void XeCryptDesParity_entry(lpvoid_t inp, dword_t inp_size, lpvoid_t out_ptr) {
  XeCryptDesParity(inp.as<uint8_t*>(), inp_size, out_ptr.as<uint8_t*>());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDesParity, kNone, kImplemented);

void XeCryptDes3Key_entry(pointer_t<XECRYPT_DES3_STATE> state_ptr,
                          lpvoid_t key) {
  XeCryptDes3Key(state_ptr, key.as<uint8_t*>());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Key, kNone, kImplemented);

void XeCryptDes3Ecb_entry(pointer_t<XECRYPT_DES3_STATE> state_ptr, lpvoid_t inp,
                          lpvoid_t out, dword_t encrypt) {
  XeCryptDes3Ecb(state_ptr, inp.as<uint8_t*>(), out.as<uint8_t*>(),
                 encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Ecb, kNone, kImplemented);

void XeCryptDes3Cbc_entry(pointer_t<XECRYPT_DES3_STATE> state_ptr, lpvoid_t inp,
                          dword_t inp_size, lpvoid_t out, lpvoid_t feed,
                          dword_t encrypt) {
  // DES can only do 8-byte chunks at a time!
  assert_true(inp_size % 8 == 0);

  XeCryptDes3Cbc(state_ptr, inp.as<uint8_t*>(), inp_size, out.as<uint8_t*>(),
                 feed.as<uint8_t*>(), encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptDes3Cbc, kNone, kImplemented);

void XeCryptAesKey_entry(pointer_t<XECRYPT_AES_STATE> state_ptr, lpvoid_t key) {
  XeCryptAesKey(state_ptr, key.as<uint8_t*>());
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesKey, kNone, kImplemented);

void XeCryptAesEcb_entry(pointer_t<XECRYPT_AES_STATE> state_ptr,
                         lpvoid_t inp_ptr, lpvoid_t out_ptr, dword_t encrypt) {
  XeCryptAesEcb(state_ptr, inp_ptr.as<uint8_t*>(), out_ptr.as<uint8_t*>(),
                encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesEcb, kNone, kImplemented);

void XeCryptAesCbc_entry(pointer_t<XECRYPT_AES_STATE> state_ptr,
                         lpvoid_t inp_ptr, dword_t inp_size, lpvoid_t out_ptr,
                         lpvoid_t feed_ptr, dword_t encrypt) {
  XeCryptAesCbc(state_ptr, inp_ptr.as<uint8_t*>(), inp_size,
                out_ptr.as<uint8_t*>(), feed_ptr.as<uint8_t*>(), encrypt != 0);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptAesCbc, kNone, kImplemented);

void XeCryptHmacSha_entry(lpvoid_t key, dword_t key_size_in, lpvoid_t inp_1,
                          dword_t inp_1_size, lpvoid_t inp_2,
                          dword_t inp_2_size, lpvoid_t inp_3,
                          dword_t inp_3_size, lpvoid_t out, dword_t out_size) {
  XeCryptHmacSha(key.as<uint8_t*>(), key_size_in, inp_1.as<uint8_t*>(),
                 inp_1_size, inp_2.as<uint8_t*>(), inp_2_size,
                 inp_3.as<uint8_t*>(), inp_3_size, out.as<uint8_t*>(),
                 out_size);
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptHmacSha, kNone, kImplemented);

dword_result_t XeKeysGetKeyProperties_entry(dword_t key) {
  if (X_Key_Properties.contains(key)) {
    auto& key_info = X_Key_Properties.at(key);
    return std::get<1>(key_info);
  }
  XELOGW("Key 0x{:04X} not implemented", static_cast<uint16_t>(key));
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysGetKeyProperties, kNone, kImplemented);

dword_result_t XeKeysGetKey_entry(word_t key, lpvoid_t key_buffer,
                                  lpdword_t key_length) {
  if (key_vault.contains(key)) {
    std::span<const uint8_t> key_needed = key_vault.at(key);
    // The hypervisor returns a fixed size key only into a buffer of its size.
    if (*key_length != key_needed.size()) {
      return 0xC8000004;
    }
    std::memcpy(key_buffer, key_needed.data(), key_needed.size());
    *key_length = static_cast<uint32_t>(key_needed.size());
  } else {
    XELOGW("Key 0x{:04X} not implemented", static_cast<uint16_t>(key));
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysGetKey, kNone, kSketchy);

// The per-console obfuscation key (0x17) differs on every console. Like the key
// obfuscation key above it is one of our own. Data a title protects with it
// only has to come back the same on this emulator.
static const uint8_t xe_console_obfuscation_key[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// The keys the key numbered calls (XeKeysHmacSha, XeKeysAesCbc) can use, or
// nullptr for one we don't have.
static const uint8_t* GetKeysKey(uint32_t key_num) {
  switch (key_num) {
    case CONSOLE_OBFUSCATION_KEY:
      return xe_console_obfuscation_key;
    case ROAMABLE_OBFUSCATION_KEY:
      return key19;
    default:
      return nullptr;
  }
}

bool XeKeysHmacSha(uint32_t key_num, const uint8_t* inp_1, uint32_t inp_1_size,
                   const uint8_t* inp_2, uint32_t inp_2_size,
                   const uint8_t* inp_3, uint32_t inp_3_size, uint8_t* out,
                   uint32_t out_size) {
  const uint8_t* key = GetKeysKey(key_num);
  if (!key) {
    return false;
  }
  XeCryptHmacSha(key, 16, inp_1, inp_1_size, inp_2, inp_2_size, inp_3,
                 inp_3_size, out, out_size);
  return true;
}

dword_result_t XeKeysHmacSha_entry(dword_t key_num, lpvoid_t inp_1,
                                   dword_t inp_1_size, lpvoid_t inp_2,
                                   dword_t inp_2_size, lpvoid_t inp_3,
                                   dword_t inp_3_size, lpvoid_t out,
                                   dword_t out_size) {
  return XeKeysHmacSha(key_num, inp_1.as<uint8_t*>(), inp_1_size,
                       inp_2.as<uint8_t*>(), inp_2_size, inp_3.as<uint8_t*>(),
                       inp_3_size, out.as<uint8_t*>(), out_size)
             ? X_STATUS_SUCCESS
             : X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysHmacSha, kNone, kImplemented);

// AES with one of the keys above: CBC through feed, or ECB block by block
// without one. Silverlight's XamlHost wraps each storage file's key with the
// console obfuscation key through this.
dword_result_t XeKeysAesCbc_entry(dword_t key_num, lpvoid_t inp_ptr,
                                  dword_t inp_size, lpvoid_t out_ptr,
                                  lpvoid_t feed_ptr, dword_t encrypt) {
  const uint8_t* key = GetKeysKey(key_num);
  if (!key) {
    XELOGW("XeKeysAesCbc: key 0x{:X} not implemented", uint32_t(key_num));
    return X_STATUS_UNSUCCESSFUL;
  }
  // The hypervisor takes whole blocks only.
  if (inp_size % 16) {
    return 0xC8000004;
  }
  if (inp_size && (!inp_ptr || !out_ptr)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  XECRYPT_AES_STATE aes;
  XeCryptAesKey(&aes, key);
  const uint8_t* input = inp_ptr.as<uint8_t*>();
  uint8_t* output = out_ptr.as<uint8_t*>();
  if (feed_ptr) {
    XeCryptAesCbc(&aes, input, inp_size, output, feed_ptr.as<uint8_t*>(),
                  encrypt != 0);
  } else {
    for (uint32_t offset = 0; offset < inp_size; offset += 16) {
      XeCryptAesEcb(&aes, input + offset, output + offset, encrypt != 0);
    }
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysAesCbc, kNone, kImplemented);

void XeCryptRotSumSha_entry(lpvoid_t inp_1, dword_t inp_1_size, lpvoid_t inp_2,
                            dword_t inp_2_size, lpvoid_t out,
                            dword_t out_size) {
  // out used by XeCryptBnQwBeSigVerify
}
DECLARE_XBOXKRNL_EXPORT1(XeCryptRotSumSha, kNone, kStub);

dword_result_t XeKeysAesCbcUsingKey_entry(lpvoid_t obscured_key,
                                          lpvoid_t inp_ptr, dword_t inp_size,
                                          lpvoid_t out_ptr, lpvoid_t feed_ptr,
                                          dword_t encrypt) {
  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(&aes, (uint8_t*)xe_key_obfuscation_key);
  XeCryptAesEcb_entry(&aes, obscured_key, key, 0);

  // Run CBC using deobscured key
  XeCryptAesKey_entry(&aes, key);
  XeCryptAesCbc_entry(&aes, inp_ptr, inp_size, out_ptr, feed_ptr, encrypt);

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysAesCbcUsingKey, kNone, kImplemented);

dword_result_t XeKeysObscureKey_entry(lpvoid_t input, lpvoid_t output) {
  // Based on HvxKeysObscureKey
  // Seems to encrypt input with per-console KEY_OBFUSCATION_KEY (key 0x18)

  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(&aes, (uint8_t*)xe_key_obfuscation_key);
  XeCryptAesEcb_entry(&aes, input, output, 1);

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysObscureKey, kNone, kImplemented);

dword_result_t XeKeysHmacShaUsingKey_entry(lpvoid_t obscured_key,
                                           lpvoid_t inp_1, dword_t inp_1_size,
                                           lpvoid_t inp_2, dword_t inp_2_size,
                                           lpvoid_t inp_3, dword_t inp_3_size,
                                           lpvoid_t out, dword_t out_size) {
  if (!obscured_key) {
    return X_STATUS_INVALID_PARAMETER;
  }

  uint8_t key[16];

  // Deobscure key
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(&aes, (uint8_t*)xe_key_obfuscation_key);
  XeCryptAesEcb_entry(&aes, obscured_key, key, 0);

  XeCryptHmacSha_entry(key, 0x10, inp_1, inp_1_size, inp_2, inp_2_size, inp_3,
                       inp_3_size, out, out_size);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysHmacShaUsingKey, kNone, kImplemented);

dword_result_t XeKeysGetConsoleType_entry(lpdword_t type_out) {
  *type_out = Retail;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysGetConsoleType, kNone, kImplemented);

dword_result_t XeKeysGetConsoleID_entry(pointer_t<XE_CONSOLE_ID> raw_bytes,
                                        lpstring_t hex_string) {
  // We dont care about KV or using official keys
  if (raw_bytes) {
    raw_bytes.Zero();
    raw_bytes->RefurbBits = 0b0011;
    raw_bytes->ManufactureMonth = 0b1001;
    raw_bytes->ManufactureYear = 0b0001;
    raw_bytes->MacIndex3 = 0b01000000;
    raw_bytes->MacIndex4 = 0b01100110;
    raw_bytes->MacIndex5 = 0b01111110;
    raw_bytes->Crc = 0b0000;
  }
  if (hex_string) {
    std::string key = "245149100000";
    std::memcpy(hex_string, key.c_str(), 0xC);
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysGetConsoleID, kNone, kImplemented);

dword_result_t XeKeysConsolePrivateKeySign_entry(
    lpvoid_t hash, pointer_t<XE_CONSOLE_CERTIFICATE> output) {
  if (!hash) {
    return false;
  }

  if (!output) {
    return false;
  }

  output.Zero();
  output->console_id.RefurbBits = 0b0011;
  output->console_id.ManufactureMonth = 0b1001;
  output->console_id.ManufactureYear = 0b0001;
  output->console_id.MacIndex3 = 0b01000000;
  output->console_id.MacIndex4 = 0b01100110;
  output->console_id.MacIndex5 = 0b01111110;
  output->console_id.Crc = 0b0000;
  output->console_type = Retail;
  output->manufacture_date[0] = 2;
  output->manufacture_date[1] = 0;
  output->manufacture_date[2] = 0;
  output->manufacture_date[3] = 5;
  output->manufacture_date[4] = 1;
  output->manufacture_date[5] = 1;
  output->manufacture_date[6] = 2;
  output->manufacture_date[7] = 2;
  return true;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysConsolePrivateKeySign, kNone, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Crypt);
