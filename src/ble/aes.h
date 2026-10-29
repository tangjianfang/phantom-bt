#pragma once
// AES-128 单块加密（FIPS-197）与 AES-CMAC（RFC 4493）
#include <cstddef>
#include <cstdint>

namespace pb::crypto {

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes_cmac(const uint8_t key[16], const uint8_t* msg, size_t n, uint8_t out[16]);

}  // namespace pb::crypto
