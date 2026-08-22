#pragma once
#include "common.h"
#include "match.h"
#include "thread_pool.h"

namespace fz {

struct DeflateOptions {
    int level = 5;                    // 1=fastest ... 9=maximum search
    size_t chunk_size = 1u * 1024u * 1024u;
    bool use_avx2 = true;
    std::function<void(size_t)> progress;
    CancellationCallback cancel;
};

struct DeflateResult {
    std::vector<uint8_t> bytes;
    uint64_t bit_count = 0;
};

DeflateResult deflate_parallel(const uint8_t* data,
                               size_t size,
                               const DeflateOptions& options,
                               ThreadPool& pool);

} // namespace fz
