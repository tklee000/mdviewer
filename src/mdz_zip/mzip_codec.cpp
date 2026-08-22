#include "mzip_codec.h"

#include "deflate_decoder.h"
#include "deflate_encoder.h"
#include "platform.h"
#include "thread_pool.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr int kDefaultLevel = 5;
constexpr size_t kDefaultBlockSize = 1u * 1024u * 1024u;
constexpr size_t kMaxThreads = 256;

struct NormalizedOptions {
    int level = kDefaultLevel;
    size_t threads = 1;
    size_t block_size = kDefaultBlockSize;
    bool use_avx2 = true;
};

bool checked_add(size_t left, size_t right, size_t& result) {
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    result = left + right;
    return true;
}

size_t saturating_bound(size_t input_size, size_t overhead) {
    size_t result = 0;
    return checked_add(input_size, overhead, result)
               ? result : std::numeric_limits<size_t>::max();
}

NormalizedOptions normalize_options(const MzipCodecOptions* options) {
    NormalizedOptions normalized;
    if (options != nullptr) {
        if (options->level != 0) normalized.level = options->level;
        if (normalized.level < 1 || normalized.level > 9)
            throw std::invalid_argument("Mzip codec level must be 1..9 or 0");

        const size_t requested_threads = options->threads == 0
            ? std::max<unsigned>(1, std::thread::hardware_concurrency())
            : static_cast<size_t>(options->threads);
        normalized.threads = std::clamp<size_t>(requested_threads, 1, kMaxThreads);

        const uint32_t block_mib = options->block_mib == 0 ? 1u : options->block_mib;
        if (block_mib > 1024u)
            throw std::invalid_argument("Mzip codec block_mib must be 0..1024");
        normalized.block_size = static_cast<size_t>(block_mib) * 1024u * 1024u;
        normalized.use_avx2 = options->use_avx2 != 0;
    } else {
        normalized.threads = std::clamp<size_t>(
            std::max<unsigned>(1, std::thread::hardware_concurrency()), 1, kMaxThreads);
    }
    normalized.use_avx2 = normalized.use_avx2 && fz::cpu_has_avx2();
    return normalized;
}

void validate_input(const uint8_t* input, size_t input_size) {
    if (input == nullptr && input_size != 0)
        throw std::invalid_argument("Mzip codec input is null");
}

void validate_destination(uint8_t* destination, size_t destination_capacity) {
    if (destination == nullptr && destination_capacity != 0)
        throw std::invalid_argument("Mzip codec destination is null");
}

MzipCodecStatus status_from_exception(const std::exception& error) {
    if (dynamic_cast<const fz::OperationCancelled*>(&error) != nullptr)
        return MZIP_CODEC_CANCELED;
    if (dynamic_cast<const std::bad_alloc*>(&error) != nullptr)
        return MZIP_CODEC_MEMORY_ERROR;
    if (std::strstr(error.what(), "output limit") != nullptr)
        return MZIP_CODEC_OUTPUT_TOO_SMALL;
    return MZIP_CODEC_DATA_ERROR;
}

MzipCodecStatus copy_to_destination(const std::vector<uint8_t>& bytes,
                                    uint8_t* destination,
                                    size_t destination_capacity,
                                    size_t* destination_size) {
    if (bytes.size() > destination_capacity)
        return MZIP_CODEC_OUTPUT_TOO_SMALL;
    if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
    *destination_size = bytes.size();
    return MZIP_CODEC_OK;
}

MzipCodecStatus copy_to_buffer(std::vector<uint8_t>&& bytes,
                               MzipCodecBuffer* buffer) {
    if (!bytes.empty()) {
        auto* data = static_cast<uint8_t*>(std::malloc(bytes.size()));
        if (data == nullptr) return MZIP_CODEC_MEMORY_ERROR;
        std::memcpy(data, bytes.data(), bytes.size());
        buffer->data = data;
    }
    buffer->size = bytes.size();
    return MZIP_CODEC_OK;
}

std::vector<uint8_t> encode_deflate(const uint8_t* input, size_t input_size,
                                    const NormalizedOptions& options) {
    // deflate_parallel intentionally leaves an empty input without a block;
    // expose the canonical final empty stored block for the public API.
    if (input_size == 0) return {0x01, 0x00, 0x00, 0xFF, 0xFF};
    fz::DeflateOptions deflate;
    deflate.level = options.level;
    deflate.chunk_size = options.block_size;
    deflate.use_avx2 = options.use_avx2;
    fz::ThreadPool pool(options.threads);
    return fz::deflate_parallel(input, input_size, deflate, pool).bytes;
}

MzipCodecStatus decode_deflate_to_buffer(const uint8_t* input, size_t input_size,
                                         size_t expected_output_size,
                                         MzipCodecBuffer* buffer) {
    uint8_t* data = nullptr;
    if (expected_output_size != 0) {
        data = static_cast<uint8_t*>(std::malloc(expected_output_size));
        if (data == nullptr) return MZIP_CODEC_MEMORY_ERROR;
    }
    try {
        const fz::InflateResult result = fz::inflate_raw_to_buffer(
            input, input_size, expected_output_size, data);
        buffer->data = data;
        buffer->size = static_cast<size_t>(result.size);
        return MZIP_CODEC_OK;
    } catch (...) {
        std::free(data);
        throw;
    }
}

template <typename Function>
MzipCodecStatus guarded(Function&& function) {
    try {
        return function();
    } catch (const std::invalid_argument&) {
        return MZIP_CODEC_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        return status_from_exception(error);
    } catch (...) {
        return MZIP_CODEC_INTERNAL_ERROR;
    }
}

} // namespace

extern "C" {

MZIP_CODEC_API void mzip_codec_options_init(MzipCodecOptions* options) {
    if (options == nullptr) return;
    options->level = kDefaultLevel;
    options->threads = 0;
    options->block_mib = 1;
    options->use_avx2 = 1;
    options->reserved[0] = options->reserved[1] = options->reserved[2] = 0;
}

MZIP_CODEC_API void mzip_codec_buffer_init(MzipCodecBuffer* buffer) {
    if (buffer == nullptr) return;
    buffer->data = nullptr;
    buffer->size = 0;
}

MZIP_CODEC_API void mzip_codec_buffer_free(MzipCodecBuffer* buffer) {
    if (buffer == nullptr) return;
    std::free(buffer->data);
    buffer->data = nullptr;
    buffer->size = 0;
}

MZIP_CODEC_API const char* mzip_codec_status_string(MzipCodecStatus status) {
    switch (status) {
    case MZIP_CODEC_OK: return "ok";
    case MZIP_CODEC_INVALID_ARGUMENT: return "invalid argument";
    case MZIP_CODEC_OUTPUT_TOO_SMALL: return "output buffer too small";
    case MZIP_CODEC_DATA_ERROR: return "corrupt or unsupported data";
    case MZIP_CODEC_MEMORY_ERROR: return "out of memory";
    case MZIP_CODEC_CANCELED: return "canceled";
    case MZIP_CODEC_INTERNAL_ERROR: return "internal error";
    default: return "unknown status";
    }
}

MZIP_CODEC_API size_t mzip_deflate_bound(size_t input_size) {
    // The encoder always chooses a stored representation when it is smaller;
    // six bytes per maximum stored block plus a small final-block allowance is
    // therefore a conservative bound for this raw stream.
    const size_t blocks = input_size / 65535u + 1u;
    size_t overhead = 64u;
    if (blocks > (std::numeric_limits<size_t>::max() - overhead) / 6u)
        return std::numeric_limits<size_t>::max();
    overhead += blocks * 6u;
    return saturating_bound(input_size, overhead);
}

MZIP_CODEC_API MzipCodecStatus mzip_deflate(
    const uint8_t* input, size_t input_size,
    const MzipCodecOptions* options,
    uint8_t* destination, size_t destination_capacity,
    size_t* destination_size) {
    if (destination_size == nullptr) return MZIP_CODEC_INVALID_ARGUMENT;
    *destination_size = 0;
    return guarded([&]() {
        validate_input(input, input_size);
        validate_destination(destination, destination_capacity);
        const auto normalized = normalize_options(options);
        const auto bytes = encode_deflate(input, input_size, normalized);
        return copy_to_destination(bytes, destination, destination_capacity,
                                   destination_size);
    });
}

MZIP_CODEC_API MzipCodecStatus mzip_deflate_alloc(
    const uint8_t* input, size_t input_size,
    const MzipCodecOptions* options,
    MzipCodecBuffer* buffer) {
    if (buffer == nullptr) return MZIP_CODEC_INVALID_ARGUMENT;
    mzip_codec_buffer_free(buffer);
    return guarded([&]() {
        validate_input(input, input_size);
        const auto normalized = normalize_options(options);
        return copy_to_buffer(encode_deflate(input, input_size, normalized), buffer);
    });
}

MZIP_CODEC_API MzipCodecStatus mzip_inflate(
    const uint8_t* input, size_t input_size,
    size_t expected_output_size,
    uint8_t* destination, size_t destination_capacity,
    size_t* destination_size) {
    if (destination_size == nullptr) return MZIP_CODEC_INVALID_ARGUMENT;
    *destination_size = 0;
    return guarded([&]() {
        validate_input(input, input_size);
        validate_destination(destination, destination_capacity);
        if (expected_output_size > destination_capacity)
            return MZIP_CODEC_OUTPUT_TOO_SMALL;
        const fz::InflateResult result = fz::inflate_raw_to_buffer(
            input, input_size, expected_output_size, destination);
        *destination_size = static_cast<size_t>(result.size);
        return MZIP_CODEC_OK;
    });
}

MZIP_CODEC_API MzipCodecStatus mzip_inflate_alloc(
    const uint8_t* input, size_t input_size,
    size_t expected_output_size,
    MzipCodecBuffer* buffer) {
    if (buffer == nullptr) return MZIP_CODEC_INVALID_ARGUMENT;
    mzip_codec_buffer_free(buffer);
    return guarded([&]() {
        validate_input(input, input_size);
        return decode_deflate_to_buffer(input, input_size,
                                        expected_output_size, buffer);
    });
}

} // extern "C"
