#pragma once
#include "common.h"

namespace fz {

struct InflateResult {
    uint64_t size = 0;
    uint32_t crc32 = 0;
};

// Expands a raw RFC 1951 DEFLATE stream (the format stored inside ZIP entries).
InflateResult inflate_raw_to_stream(const uint8_t* data,
                                    size_t size,
                                    uint64_t expected_size,
                                    std::ostream& output,
                                    const std::function<void(uint64_t)>& progress = {});

// Expands directly into an already allocated contiguous output buffer. The
// buffer must contain expected_size writable bytes (and may be null only when
// expected_size is zero). This is the preferred path for mapped ZIP outputs.
InflateResult inflate_raw_to_buffer(const uint8_t* data,
                                    size_t size,
                                    uint64_t expected_size,
                                    uint8_t* output,
                                    const std::function<void(uint64_t)>& progress = {});

} // namespace fz
