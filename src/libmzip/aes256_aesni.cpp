#include "aes256_aesni.h"

#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#include <wmmintrin.h>
#define FZ_AESNI_X86 1
#elif defined(__x86_64__) || defined(__i386__)
#include <wmmintrin.h>
#define FZ_AESNI_X86 1
#else
#define FZ_AESNI_X86 0
#endif

namespace fz {

#if FZ_AESNI_X86
namespace {

inline __m128i encrypt_one(__m128i state, const __m128i keys[15]) {
    state = _mm_xor_si128(state, keys[0]);
    for (int round = 1; round < 14; ++round) {
        state = _mm_aesenc_si128(state, keys[round]);
    }
    return _mm_aesenclast_si128(state, keys[14]);
}

inline void encrypt_eight(__m128i state[8], const __m128i keys[15]) {
    for (int lane = 0; lane < 8; ++lane) {
        state[lane] = _mm_xor_si128(state[lane], keys[0]);
    }
    for (int round = 1; round < 14; ++round) {
        const __m128i key = keys[round];
        for (int lane = 0; lane < 8; ++lane) {
            state[lane] = _mm_aesenc_si128(state[lane], key);
        }
    }
    const __m128i last = keys[14];
    for (int lane = 0; lane < 8; ++lane) {
        state[lane] = _mm_aesenclast_si128(state[lane], last);
    }
}

inline __m128i make_counter(uint64_t value) {
    // WinZip AES uses a 128-bit little-endian counter. Archive sizes cannot
    // realistically overflow the low 64 bits, so the high lane remains zero.
    return _mm_set_epi64x(0, static_cast<long long>(value));
}

} // namespace

void aes256_ctr_xor_aesni(const uint8_t round_keys[240],
                          const uint8_t* input,
                          uint8_t* output,
                          size_t size) {
    __m128i keys[15];
    for (int round = 0; round < 15; ++round) {
        keys[round] = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(round_keys + round * 16));
    }

    uint64_t counter = 1; // WinZip AES counter starts at 1.

    // Eight independent counter blocks hide AES instruction latency and make
    // better use of the CPU's AES execution throughput than a one-block loop.
    while (size >= 8u * 16u) {
        __m128i stream[8] = {
            make_counter(counter + 0), make_counter(counter + 1),
            make_counter(counter + 2), make_counter(counter + 3),
            make_counter(counter + 4), make_counter(counter + 5),
            make_counter(counter + 6), make_counter(counter + 7)
        };
        encrypt_eight(stream, keys);

        for (int lane = 0; lane < 8; ++lane) {
            const __m128i plain = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(input + lane * 16));
            const __m128i cipher = _mm_xor_si128(plain, stream[lane]);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(output + lane * 16), cipher);
        }
        input += 8u * 16u;
        output += 8u * 16u;
        size -= 8u * 16u;
        counter += 8;
    }

    while (size >= 16u) {
        const __m128i stream = encrypt_one(make_counter(counter++), keys);
        const __m128i plain = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), _mm_xor_si128(plain, stream));
        input += 16;
        output += 16;
        size -= 16;
    }

    if (size != 0) {
        const __m128i stream128 = encrypt_one(make_counter(counter), keys);
        alignas(16) uint8_t stream[16];
        _mm_store_si128(reinterpret_cast<__m128i*>(stream), stream128);
        for (size_t i = 0; i < size; ++i) {
            output[i] = static_cast<uint8_t>(input[i] ^ stream[i]);
        }
    }
}

#else

void aes256_ctr_xor_aesni(const uint8_t[240],
                          const uint8_t*,
                          uint8_t*,
                          size_t) {
    throw std::runtime_error("AES-NI is unavailable on this CPU architecture");
}

#endif

} // namespace fz
