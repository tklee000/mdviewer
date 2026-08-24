#include "match.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace fz {

size_t match_length_scalar(const uint8_t* a, const uint8_t* b, size_t max_len) {
    size_t i = 0;
    while (i + sizeof(uint64_t) <= max_len) {
        uint64_t x, y;
        std::memcpy(&x, a + i, sizeof(x));
        std::memcpy(&y, b + i, sizeof(y));
        const uint64_t diff = x ^ y;
        if (diff != 0) {
#ifdef _MSC_VER
            unsigned long bit = 0;
#if defined(_M_X64)
            _BitScanForward64(&bit, diff);
#else
            const uint32_t lo = static_cast<uint32_t>(diff);
            if (lo) _BitScanForward(&bit, lo);
            else { _BitScanForward(&bit, static_cast<uint32_t>(diff >> 32)); bit += 32; }
#endif
            return i + static_cast<size_t>(bit >> 3);
#else
            return i + static_cast<size_t>(__builtin_ctzll(diff) >> 3);
#endif
        }
        i += sizeof(uint64_t);
    }
    while (i < max_len && a[i] == b[i]) ++i;
    return i;
}

size_t count_equal_bytes_scalar(const uint8_t* a, const uint8_t* b, size_t size) {
    size_t equal = 0;
    for (size_t i = 0; i < size; ++i) equal += a[i] == b[i] ? 1u : 0u;
    return equal;
}

} // namespace fz
