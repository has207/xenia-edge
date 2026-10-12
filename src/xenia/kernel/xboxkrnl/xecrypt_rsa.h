#ifndef XECRYPT_RSA_H_
#define XECRYPT_RSA_H_

#include <cstdint>
#include <cstring>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/bignum.h"

// Performs RSA public-key encryption/decryption matching
// XeCryptBnQwNeRsaPubCrypt. All buffers use the Xbox 360 layout: big-endian
// uint64 limbs in little-endian limb order.
//
// qw_a:    input data (num_qwords * 8 bytes)
// qw_b:    output buffer (num_qwords * 8 bytes)
// modulus: RSA modulus following the XECRYPT_RSA header (num_qwords * 8 bytes)
// num_qwords: number of 64-bit limbs
// exponent:   public exponent
//
// Returns 1 on success, 0 on failure.
inline uint32_t XeCryptBnQwNeRsaPubCrypt(const uint8_t* qw_a, uint8_t* qw_b,
                                         const uint8_t* modulus,
                                         uint32_t num_qwords,
                                         uint32_t exponent) {
  // Reject keys below 512 bits (8 qwords) to match original BCrypt behavior.
  if (num_qwords < 8) {
    return 0;
  }

  uint32_t modulus_size = num_qwords * 8;

  auto input_be = std::vector<uint8_t>(modulus_size);
  auto mod_be = std::vector<uint8_t>(modulus_size);

  // Reverse qword order to produce big-endian byte arrays
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&input_be[i * 8], &qw_a[(num_qwords - 1 - i) * 8], 8);
    std::memcpy(&mod_be[i * 8], &modulus[(num_qwords - 1 - i) * 8], 8);
  }

  mbedtls_mpi base, exp, mod, result;
  mbedtls_mpi_init(&base);
  mbedtls_mpi_init(&exp);
  mbedtls_mpi_init(&mod);
  mbedtls_mpi_init(&result);
  auto result_be = std::vector<uint8_t>(modulus_size);
  const bool valid =
      !mbedtls_mpi_read_binary(&base, input_be.data(), modulus_size) &&
      !mbedtls_mpi_read_binary(&mod, mod_be.data(), modulus_size) &&
      !mbedtls_mpi_lset(&exp, exponent) &&
      !mbedtls_mpi_exp_mod(&result, &base, &exp, &mod, nullptr) &&
      !mbedtls_mpi_write_binary(&result, result_be.data(), modulus_size);
  mbedtls_mpi_free(&base);
  mbedtls_mpi_free(&exp);
  mbedtls_mpi_free(&mod);
  mbedtls_mpi_free(&result);
  if (!valid) {
    return 0;
  }

  // Convert back to Xbox format: reverse qword order
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&qw_b[i * 8], &result_be[(num_qwords - 1 - i) * 8], 8);
  }

  return 1;
}

#endif  // XECRYPT_RSA_H_
