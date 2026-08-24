#include "crypto.h"
#include "aes256_aesni.h"

#include <array>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fz {
namespace {

inline uint32_t rotl32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

class Sha1 {
public:
    Sha1() { reset(); }

    void update(const uint8_t* data, size_t size) {
        total_bytes_ += size;
        while (size != 0) {
            const size_t take = std::min(size, block_.size() - block_used_);
            std::memcpy(block_.data() + block_used_, data, take);
            block_used_ += take;
            data += take;
            size -= take;
            if (block_used_ == block_.size()) {
                transform(block_.data());
                block_used_ = 0;
            }
        }
    }

    std::array<uint8_t, 20> finish() {
        const uint64_t bit_length = static_cast<uint64_t>(total_bytes_) * 8ull;
        block_[block_used_++] = 0x80;
        if (block_used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.end(), uint8_t{0});
            transform(block_.data());
            block_used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.begin() + 56, uint8_t{0});
        for (unsigned i = 0; i < 8; ++i) {
            block_[63 - i] = static_cast<uint8_t>(bit_length >> (i * 8));
        }
        transform(block_.data());

        std::array<uint8_t, 20> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        reset();
        return digest;
    }

private:
    void reset() {
        state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        block_.fill(0);
        block_used_ = 0;
        total_bytes_ = 0;
    }

    void transform(const uint8_t* block) {
        uint32_t w[80]{};
        for (unsigned i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (unsigned i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d); k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d; k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d; k = 0xCA62C1D6u;
            }
            const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e;
    }

    std::array<uint32_t, 5> state_{};
    std::array<uint8_t, 64> block_{};
    size_t block_used_ = 0;
    size_t total_bytes_ = 0;
};

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t key_size,
                                  const uint8_t* data, size_t data_size) {
    std::array<uint8_t, 64> key_block{};
    if (key_size > key_block.size()) {
        Sha1 sha;
        sha.update(key, key_size);
        const auto digest = sha.finish();
        std::copy(digest.begin(), digest.end(), key_block.begin());
    } else if (key_size != 0) {
        std::memcpy(key_block.data(), key, key_size);
    }

    std::array<uint8_t, 64> inner_pad{}, outer_pad{};
    for (size_t i = 0; i < key_block.size(); ++i) {
        inner_pad[i] = static_cast<uint8_t>(key_block[i] ^ 0x36u);
        outer_pad[i] = static_cast<uint8_t>(key_block[i] ^ 0x5Cu);
    }

    Sha1 inner;
    inner.update(inner_pad.data(), inner_pad.size());
    inner.update(data, data_size);
    const auto inner_digest = inner.finish();

    Sha1 outer;
    outer.update(outer_pad.data(), outer_pad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

std::vector<uint8_t> pbkdf2_hmac_sha1(const uint8_t* password, size_t password_size,
                                      const uint8_t* salt, size_t salt_size,
                                      unsigned iterations, size_t output_size) {
    if (iterations == 0) throw std::runtime_error("PBKDF2 iteration count must be positive");
    std::vector<uint8_t> output(output_size);
    std::vector<uint8_t> salt_block(salt, salt + salt_size);
    salt_block.resize(salt_size + 4);

    size_t written = 0;
    for (uint32_t block_index = 1; written < output_size; ++block_index) {
        salt_block[salt_size + 0] = static_cast<uint8_t>(block_index >> 24);
        salt_block[salt_size + 1] = static_cast<uint8_t>(block_index >> 16);
        salt_block[salt_size + 2] = static_cast<uint8_t>(block_index >> 8);
        salt_block[salt_size + 3] = static_cast<uint8_t>(block_index);

        auto u = hmac_sha1(password, password_size, salt_block.data(), salt_block.size());
        auto t = u;
        for (unsigned iteration = 1; iteration < iterations; ++iteration) {
            u = hmac_sha1(password, password_size, u.data(), u.size());
            for (size_t i = 0; i < t.size(); ++i) t[i] ^= u[i];
        }
        const size_t take = std::min(t.size(), output_size - written);
        std::copy_n(t.begin(), take, output.begin() + static_cast<std::ptrdiff_t>(written));
        written += take;
    }
    return output;
}

constexpr uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u));
}

class Aes256 {
public:
    explicit Aes256(const uint8_t key[32]) { expand_key(key); }

    const uint8_t* round_keys() const noexcept { return round_keys_.data(); }

    void encrypt_block(const uint8_t input[16], uint8_t output[16]) const {
        uint8_t state[16];
        std::memcpy(state, input, 16);
        add_round_key(state, round_keys_.data());
        for (unsigned round = 1; round < 14; ++round) {
            sub_bytes(state);
            shift_rows(state);
            mix_columns(state);
            add_round_key(state, round_keys_.data() + round * 16);
        }
        sub_bytes(state);
        shift_rows(state);
        add_round_key(state, round_keys_.data() + 14 * 16);
        std::memcpy(output, state, 16);
    }

private:
    static void sub_bytes(uint8_t state[16]) {
        for (unsigned i = 0; i < 16; ++i) state[i] = kSbox[state[i]];
    }

    static void shift_rows(uint8_t s[16]) {
        uint8_t t[16];
        t[0]=s[0];  t[1]=s[5];  t[2]=s[10]; t[3]=s[15];
        t[4]=s[4];  t[5]=s[9];  t[6]=s[14]; t[7]=s[3];
        t[8]=s[8];  t[9]=s[13]; t[10]=s[2]; t[11]=s[7];
        t[12]=s[12];t[13]=s[1]; t[14]=s[6]; t[15]=s[11];
        std::memcpy(s, t, 16);
    }

    static void mix_columns(uint8_t s[16]) {
        for (unsigned c = 0; c < 4; ++c) {
            uint8_t* p = s + c * 4;
            const uint8_t a0=p[0], a1=p[1], a2=p[2], a3=p[3];
            const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
            p[0] ^= static_cast<uint8_t>(all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
            p[1] ^= static_cast<uint8_t>(all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
            p[2] ^= static_cast<uint8_t>(all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
            p[3] ^= static_cast<uint8_t>(all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
        }
    }

    static void add_round_key(uint8_t state[16], const uint8_t* key) {
        for (unsigned i = 0; i < 16; ++i) state[i] ^= key[i];
    }

    static uint8_t rcon(unsigned i) {
        uint8_t x = 1;
        while (i > 1) { x = xtime(x); --i; }
        return x;
    }

    void expand_key(const uint8_t key[32]) {
        std::copy(key, key + 32, round_keys_.begin());
        unsigned generated = 32;
        unsigned rcon_index = 1;
        uint8_t temp[4]{};
        while (generated < round_keys_.size()) {
            for (unsigned i = 0; i < 4; ++i) temp[i] = round_keys_[generated - 4 + i];
            if (generated % 32 == 0) {
                const uint8_t first = temp[0];
                temp[0] = kSbox[temp[1]];
                temp[1] = kSbox[temp[2]];
                temp[2] = kSbox[temp[3]];
                temp[3] = kSbox[first];
                temp[0] ^= rcon(rcon_index++);
            } else if (generated % 32 == 16) {
                for (uint8_t& b : temp) b = kSbox[b];
            }
            for (unsigned i = 0; i < 4 && generated < round_keys_.size(); ++i) {
                round_keys_[generated] = static_cast<uint8_t>(round_keys_[generated - 32] ^ temp[i]);
                ++generated;
            }
        }
    }

    std::array<uint8_t, 240> round_keys_{};
};

void fill_secure_random(uint8_t* out, size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#else
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t done = 0;
        while (done < size) {
            const ssize_t n = ::read(fd, out + done, size - done);
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        ::close(fd);
        if (done == size) return;
    }
#endif
    std::random_device rd;
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(rd());
}

void increment_le_counter(std::array<uint8_t, 16>& counter) {
    for (uint8_t& b : counter) {
        ++b;
        if (b != 0) break;
    }
}

} // namespace

std::vector<uint8_t> winzip_aes256_encrypt(const uint8_t* data,
                                           size_t size,
                                           const std::string& password,
                                           bool use_aesni) {
    if (password.empty()) throw std::runtime_error("Encryption password cannot be empty");

    constexpr size_t kSaltSize = 16;
    constexpr size_t kKeySize = 32;
    constexpr size_t kVerifierSize = 2;
    constexpr size_t kAuthSize = 10;

    std::array<uint8_t, kSaltSize> salt{};
    fill_secure_random(salt.data(), salt.size());

    const auto derived = pbkdf2_hmac_sha1(
        reinterpret_cast<const uint8_t*>(password.data()), password.size(),
        salt.data(), salt.size(), 1000, kKeySize * 2 + kVerifierSize);

    const uint8_t* encryption_key = derived.data();
    const uint8_t* authentication_key = derived.data() + kKeySize;
    const uint8_t* verifier = derived.data() + kKeySize * 2;

    std::vector<uint8_t> result;
    result.resize(kSaltSize + kVerifierSize + size + kAuthSize);
    std::copy(salt.begin(), salt.end(), result.begin());
    result[kSaltSize] = verifier[0];
    result[kSaltSize + 1] = verifier[1];

    Aes256 aes(encryption_key);
    uint8_t* ciphertext = result.data() + kSaltSize + kVerifierSize;
    if (use_aesni && size != 0) {
        aes256_ctr_xor_aesni(aes.round_keys(), data, ciphertext, size);
    } else {
        std::array<uint8_t, 16> counter{};
        std::array<uint8_t, 16> stream{};
        size_t offset = 0;
        while (offset < size) {
            increment_le_counter(counter); // WinZip AES counter starts at 1.
            aes.encrypt_block(counter.data(), stream.data());
            const size_t n = std::min<size_t>(16, size - offset);
            for (size_t i = 0; i < n; ++i) {
                ciphertext[offset + i] = static_cast<uint8_t>(data[offset + i] ^ stream[i]);
            }
            offset += n;
        }
    }

    const auto auth = hmac_sha1(authentication_key, kKeySize, ciphertext, size);
    std::copy_n(auth.begin(), kAuthSize, result.begin() + static_cast<std::ptrdiff_t>(kSaltSize + kVerifierSize + size));
    return result;
}

std::vector<uint8_t> winzip_aes256_decrypt(const uint8_t* payload,
                                           size_t payload_size,
                                           const std::string& password,
                                           bool use_aesni) {
    if (password.empty()) throw std::runtime_error("A password is required for this AES entry");

    constexpr size_t kSaltSize = 16;
    constexpr size_t kKeySize = 32;
    constexpr size_t kVerifierSize = 2;
    constexpr size_t kAuthSize = 10;
    constexpr size_t kOverhead = kSaltSize + kVerifierSize + kAuthSize;
    if (payload_size < kOverhead) throw std::runtime_error("Truncated WinZip AES payload");

    const auto derived = pbkdf2_hmac_sha1(
        reinterpret_cast<const uint8_t*>(password.data()), password.size(),
        payload, kSaltSize, 1000, kKeySize * 2 + kVerifierSize);
    const uint8_t* encryption_key = derived.data();
    const uint8_t* authentication_key = derived.data() + kKeySize;
    const uint8_t* verifier = derived.data() + kKeySize * 2;

    unsigned verifier_difference = 0;
    verifier_difference |= static_cast<unsigned>(verifier[0] ^ payload[kSaltSize]);
    verifier_difference |= static_cast<unsigned>(verifier[1] ^ payload[kSaltSize + 1]);
    if (verifier_difference != 0) throw std::runtime_error("Incorrect password for WinZip AES entry");

    const size_t ciphertext_size = payload_size - kOverhead;
    const uint8_t* ciphertext = payload + kSaltSize + kVerifierSize;
    const uint8_t* stored_auth = ciphertext + ciphertext_size;
    const auto auth = hmac_sha1(authentication_key, kKeySize, ciphertext, ciphertext_size);
    unsigned auth_difference = 0;
    for (size_t i = 0; i < kAuthSize; ++i)
        auth_difference |= static_cast<unsigned>(auth[i] ^ stored_auth[i]);
    if (auth_difference != 0) throw std::runtime_error("WinZip AES authentication failed");

    std::vector<uint8_t> plaintext(ciphertext_size);
    Aes256 aes(encryption_key);
    if (use_aesni && ciphertext_size != 0) {
        aes256_ctr_xor_aesni(aes.round_keys(), ciphertext, plaintext.data(), ciphertext_size);
    } else {
        std::array<uint8_t, 16> counter{};
        std::array<uint8_t, 16> stream{};
        size_t offset = 0;
        while (offset < ciphertext_size) {
            increment_le_counter(counter);
            aes.encrypt_block(counter.data(), stream.data());
            const size_t n = std::min<size_t>(16, ciphertext_size - offset);
            for (size_t i = 0; i < n; ++i)
                plaintext[offset + i] = static_cast<uint8_t>(ciphertext[offset + i] ^ stream[i]);
            offset += n;
        }
    }
    return plaintext;
}

} // namespace fz
