#include "crc32.h"

namespace fz {
namespace {

struct Tables {
    uint32_t t[8][256]{};

    Tables() {
        constexpr uint32_t poly = 0xEDB88320u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c >> 1) ^ ((c & 1u) ? poly : 0u);
            }
            t[0][i] = c;
        }
        for (int slice = 1; slice < 8; ++slice) {
            for (uint32_t i = 0; i < 256; ++i) {
                const uint32_t c = t[slice - 1][i];
                t[slice][i] = (c >> 8) ^ t[0][c & 0xFFu];
            }
        }
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

} // namespace

void Crc32::update(const uint8_t* data, size_t size) {
    const auto& tab = tables().t;
    uint32_t crc = state_;

    while (size && (reinterpret_cast<uintptr_t>(data) & 7u)) {
        crc = tab[0][(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
        --size;
    }

    while (size >= 8) {
        uint64_t block;
        std::memcpy(&block, data, sizeof(block));
        block ^= crc;
        crc = tab[7][(block      ) & 0xFFu] ^
              tab[6][(block >>  8) & 0xFFu] ^
              tab[5][(block >> 16) & 0xFFu] ^
              tab[4][(block >> 24) & 0xFFu] ^
              tab[3][(block >> 32) & 0xFFu] ^
              tab[2][(block >> 40) & 0xFFu] ^
              tab[1][(block >> 48) & 0xFFu] ^
              tab[0][(block >> 56) & 0xFFu];
        data += 8;
        size -= 8;
    }

    while (size--) {
        crc = tab[0][(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
    }
    state_ = crc;
}

uint32_t Crc32::compute(const uint8_t* data, size_t size) {
    Crc32 crc;
    crc.update(data, size);
    return crc.value();
}

} // namespace fz
