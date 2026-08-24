#include "match.h"
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace fz {

#if defined(__GNUC__) && !defined(_MSC_VER)
__attribute__((target("avx2")))
#endif
size_t match_length_avx2(const uint8_t* a, const uint8_t* b, size_t max_len) {
    size_t i = 0;
    while (i + 32 <= max_len) {
        const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        const __m256i eq = _mm256_cmpeq_epi8(va, vb);
        const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
        if (mask != 0xFFFFFFFFu) {
            const uint32_t diff = ~mask;
#ifdef _MSC_VER
            unsigned long index = 0;
            _BitScanForward(&index, diff);
            return i + index;
#else
            return i + static_cast<size_t>(__builtin_ctz(diff));
#endif
        }
        i += 32;
    }
    return i + match_length_scalar(a + i, b + i, max_len - i);
}

namespace {
unsigned popcount32(uint32_t value) {
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    return ((value + (value >> 4) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
} // namespace

#if defined(__GNUC__) && !defined(_MSC_VER)
__attribute__((target("avx2")))
#endif
size_t count_equal_bytes_avx2(const uint8_t* a, const uint8_t* b, size_t size) {
    size_t equal = 0;
    size_t i = 0;
    while (i + 32 <= size) {
        const __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const __m256i right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        const uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(left, right)));
        equal += popcount32(mask);
        i += 32;
    }
    return equal + count_equal_bytes_scalar(a + i, b + i, size - i);
}

} // namespace fz
