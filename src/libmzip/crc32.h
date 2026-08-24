#pragma once
#include "common.h"

namespace fz {

class Crc32 {
public:
    void reset() noexcept { state_ = 0xFFFFFFFFu; }
    void update(const uint8_t* data, size_t size);
    uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

    static uint32_t compute(const uint8_t* data, size_t size);

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

} // namespace fz
