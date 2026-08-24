#pragma once
#include "common.h"

namespace fz {

// WinZip AES-256 (AE-2) payload:
// 16-byte salt + 2-byte password verifier + ciphertext + 10-byte authentication code.
std::vector<uint8_t> winzip_aes256_encrypt(const uint8_t* data,
                                           size_t size,
                                           const std::string& password,
                                           bool use_aesni = true);

// Verifies and decrypts a complete WinZip AES-256 payload. The returned bytes
// are still compressed when the ZIP entry's actual method is DEFLATE.
std::vector<uint8_t> winzip_aes256_decrypt(const uint8_t* payload,
                                           size_t payload_size,
                                           const std::string& password,
                                           bool use_aesni = true);

} // namespace fz
