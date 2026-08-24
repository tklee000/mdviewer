#pragma once
#include "common.h"

namespace fz {

using MatchLengthFn = size_t (*)(const uint8_t* a, const uint8_t* b, size_t max_len);

size_t match_length_scalar(const uint8_t* a, const uint8_t* b, size_t max_len);
size_t match_length_avx2(const uint8_t* a, const uint8_t* b, size_t max_len);
size_t count_equal_bytes_scalar(const uint8_t* a, const uint8_t* b, size_t size);
size_t count_equal_bytes_avx2(const uint8_t* a, const uint8_t* b, size_t size);

} // namespace fz
