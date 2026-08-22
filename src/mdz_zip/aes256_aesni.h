#pragma once
#include "common.h"

namespace fz {

// AES-256 CTR transform for the WinZip AES little-endian counter format.
// round_keys must contain 15 consecutive 16-byte AES-256 round keys.
// This function must only be called after cpu_has_aesni() returned true.
void aes256_ctr_xor_aesni(const uint8_t round_keys[240],
                          const uint8_t* input,
                          uint8_t* output,
                          size_t size);

} // namespace fz
