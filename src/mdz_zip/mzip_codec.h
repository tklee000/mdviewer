// mzip codec library
// Author: tklee000
// Version: 0.7
#pragma once

#define MZIP_LIBRARY_AUTHOR "tklee000"
#define MZIP_LIBRARY_VERSION_MAJOR 0
#define MZIP_LIBRARY_VERSION_MINOR 7
#define MZIP_LIBRARY_VERSION_PATCH 0
#define MZIP_LIBRARY_VERSION_TEXT "0.7"

// Public, container-free raw RFC 1951 DEFLATE codec used by ZIP entries.
// MdViewer's copy intentionally omits the original 7-Zip LZMA2 API.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(MZIP_CODEC_EXPORTS)
#define MZIP_CODEC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MZIP_CODEC_SHARED)
#define MZIP_CODEC_API __declspec(dllimport)
#else
#define MZIP_CODEC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MzipCodecStatus {
    MZIP_CODEC_OK = 0,
    MZIP_CODEC_INVALID_ARGUMENT = 1,
    MZIP_CODEC_OUTPUT_TOO_SMALL = 2,
    MZIP_CODEC_DATA_ERROR = 3,
    MZIP_CODEC_MEMORY_ERROR = 4,
    MZIP_CODEC_CANCELED = 5,
    MZIP_CODEC_INTERNAL_ERROR = 6
} MzipCodecStatus;

typedef struct MzipCodecOptions {
    // 0 selects the library default (level 5); 1..9 select a concrete level.
    int level;
    // 0 selects the logical processor count. Values above 256 are clamped.
    uint32_t threads;
    // Deflate parallel chunk size in MiB. 0 selects 1 MiB.
    uint32_t block_mib;
    // Non-zero requests AVX2 matching. The library automatically falls back
    // to the scalar matcher when the current CPU does not support AVX2.
    uint8_t use_avx2;
    uint8_t reserved[3];
} MzipCodecOptions;

typedef struct MzipCodecBuffer {
    uint8_t* data;
    size_t size;
} MzipCodecBuffer;

MZIP_CODEC_API void mzip_codec_options_init(MzipCodecOptions* options);
// Initialize every buffer before passing it to an allocating call.
MZIP_CODEC_API void mzip_codec_buffer_init(MzipCodecBuffer* buffer);
MZIP_CODEC_API void mzip_codec_buffer_free(MzipCodecBuffer* buffer);
MZIP_CODEC_API const char* mzip_codec_status_string(MzipCodecStatus status);

// Conservative upper bounds for one-shot compression buffers.
MZIP_CODEC_API size_t mzip_deflate_bound(size_t input_size);

// Raw Deflate, zlib-style fixed-output call. destination_size receives the
// number of bytes written on success.
MZIP_CODEC_API MzipCodecStatus mzip_deflate(
    const uint8_t* input, size_t input_size,
    const MzipCodecOptions* options,
    uint8_t* destination, size_t destination_capacity,
    size_t* destination_size);

// Allocating convenience form. Release buffer->data with
// mzip_codec_buffer_free (never with delete or a different CRT allocator).
MZIP_CODEC_API MzipCodecStatus mzip_deflate_alloc(
    const uint8_t* input, size_t input_size,
    const MzipCodecOptions* options,
    MzipCodecBuffer* buffer);

// Raw Deflate decode. Since raw Deflate does not carry an uncompressed size,
// expected_output_size is required and is checked exactly. The destination
// capacity may be larger than the expected size.
MZIP_CODEC_API MzipCodecStatus mzip_inflate(
    const uint8_t* input, size_t input_size,
    size_t expected_output_size,
    uint8_t* destination, size_t destination_capacity,
    size_t* destination_size);

// Allocating raw Deflate decode. The returned size is exactly
// expected_output_size on success.
MZIP_CODEC_API MzipCodecStatus mzip_inflate_alloc(
    const uint8_t* input, size_t input_size,
    size_t expected_output_size,
    MzipCodecBuffer* buffer);

#ifdef __cplusplus
} // extern "C"
#endif
