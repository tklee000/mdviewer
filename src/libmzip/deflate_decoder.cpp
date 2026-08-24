#include "deflate_decoder.h"
#include "crc32.h"

#include <array>

namespace fz {
namespace {

#if defined(_MSC_VER)
#define FZ_DEFLATE_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define FZ_DEFLATE_FORCEINLINE inline __attribute__((always_inline))
#else
#define FZ_DEFLATE_FORCEINLINE inline
#endif

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : current_(data), end_(data + size) {}

    FZ_DEFLATE_FORCEINLINE uint32_t read(unsigned count) {
        if (count > 16) throw std::runtime_error("Invalid DEFLATE bit count");
        refill(count);
        if (bits_ < count) throw std::runtime_error("Truncated DEFLATE stream");
        if (count == 0) return 0;
        const uint32_t value = static_cast<uint32_t>(
            buffer_ & ((uint64_t{1} << count) - 1u));
        buffer_ >>= count;
        bits_ -= count;
        return value;
    }

    FZ_DEFLATE_FORCEINLINE uint32_t peek(unsigned count) {
        if (count > 15) throw std::runtime_error("Invalid DEFLATE Huffman bit count");
        refill(count);
        if (count == 0) return 0;
        return static_cast<uint32_t>(
            buffer_ & ((uint64_t{1} << count) - 1u));
    }

    unsigned available_bits() const noexcept { return bits_; }

    FZ_DEFLATE_FORCEINLINE void drop(unsigned count) {
        if (bits_ < count) throw std::runtime_error("Truncated DEFLATE Huffman code");
        buffer_ >>= count;
        bits_ -= count;
    }

    void align_to_byte() {
        drop(bits_ & 7u);
    }

    void read_bytes(uint8_t* destination, size_t count) {
        if ((bits_ & 7u) != 0)
            throw std::runtime_error("Internal unaligned DEFLATE byte read");
        const size_t buffered_bytes = bits_ / 8u;
        const size_t remaining_bytes = static_cast<size_t>(end_ - current_);
        if (count > buffered_bytes && count - buffered_bytes > remaining_bytes)
            throw std::runtime_error("Truncated DEFLATE stored block");

        while (count != 0 && bits_ >= 8) {
            *destination++ = static_cast<uint8_t>(buffer_);
            buffer_ >>= 8;
            bits_ -= 8;
            --count;
        }
        if (count != 0) {
            std::memcpy(destination, current_, count);
            current_ += count;
        }
    }

private:
    FZ_DEFLATE_FORCEINLINE void refill(unsigned count) {
        while (bits_ < count && current_ != end_) {
            if (bits_ <= 32 && static_cast<size_t>(end_ - current_) >= 4) {
                buffer_ |= static_cast<uint64_t>(read_u32(current_)) << bits_;
                current_ += 4;
                bits_ += 32;
            } else {
                buffer_ |= static_cast<uint64_t>(*current_++) << bits_;
                bits_ += 8;
            }
        }
    }

    const uint8_t* current_;
    const uint8_t* end_;
    uint64_t buffer_ = 0;
    unsigned bits_ = 0;
};

uint16_t reverse_bits(uint16_t value, unsigned count) {
    uint16_t reversed = 0;
    for (unsigned i = 0; i < count; ++i) {
        reversed = static_cast<uint16_t>((reversed << 1) | (value & 1u));
        value = static_cast<uint16_t>(value >> 1);
    }
    return reversed;
}

class HuffmanTable {
public:
    void build(const uint8_t* lengths, size_t length_count, unsigned root_bits,
               bool allow_empty = false) {
        if (root_bits == 0 || root_bits > 15)
            throw std::runtime_error("Invalid DEFLATE Huffman root size");

        std::array<unsigned, 16> counts{};
        for (size_t i = 0; i < length_count; ++i) {
            const uint8_t length = lengths[i];
            if (length > 15) throw std::runtime_error("Invalid DEFLATE Huffman length");
            if (length != 0) ++counts[length];
        }

        unsigned symbols = 0;
        int left = 1;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            symbols += counts[bits];
            left = (left << 1) - static_cast<int>(counts[bits]);
            if (left < 0) throw std::runtime_error("Oversubscribed DEFLATE Huffman tree");
        }
        if (symbols == 0) {
            if (!allow_empty)
                throw std::runtime_error("Empty DEFLATE Huffman tree");

            // RFC 1951 permits a dynamic block with no distance codes when the
            // block contains literals only. Keep an invalid sentinel table so
            // any unexpected length/distance pair still fails at decode time.
            root_bits_ = static_cast<uint8_t>(root_bits);
            const size_t root_size = size_t{1} << root_bits;
            table_.assign(root_size, Entry{});
            secondary_bits_.assign(root_size, 0);
            secondary_offsets_.assign(root_size, 0);
            reversed_codes_.assign(length_count, 0);
            return;
        }

        root_bits_ = static_cast<uint8_t>(root_bits);
        const size_t root_size = size_t{1} << root_bits;
        table_.assign(root_size, Entry{});

        // Long codes sharing the same root prefix use one compact secondary
        // table sized only for the longest suffix under that prefix.
        secondary_bits_.assign(root_size, 0);
        secondary_offsets_.assign(root_size, 0);

        std::array<unsigned, 16> next_code{};
        unsigned code = 0;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            code = (code + counts[bits - 1]) << 1;
            next_code[bits] = code;
        }

        reversed_codes_.assign(length_count, 0);
        for (size_t symbol = 0; symbol < length_count; ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0) continue;
            const uint16_t reversed = reverse_bits(
                static_cast<uint16_t>(next_code[length]++), length);
            reversed_codes_[symbol] = reversed;
            if (length > root_bits) {
                const size_t prefix = reversed & (root_size - 1u);
                secondary_bits_[prefix] = std::max<uint8_t>(
                    secondary_bits_[prefix],
                    static_cast<uint8_t>(length - root_bits));
            }
        }

        for (size_t prefix = 0; prefix < root_size; ++prefix) {
            const unsigned suffix_bits = secondary_bits_[prefix];
            if (suffix_bits == 0) continue;
            const size_t offset = table_.size();
            const size_t secondary_size = size_t{1} << suffix_bits;
            if (offset > std::numeric_limits<uint32_t>::max() ||
                secondary_size > std::numeric_limits<uint32_t>::max() - offset) {
                throw std::runtime_error("DEFLATE Huffman table is too large");
            }
            secondary_offsets_[prefix] = offset;
            table_.resize(offset + secondary_size);
            table_[prefix].value = static_cast<uint32_t>(offset);
            table_[prefix].bits = static_cast<uint8_t>(root_bits);
            table_[prefix].secondary_bits = static_cast<uint8_t>(suffix_bits);
        }

        for (size_t symbol = 0; symbol < length_count; ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0) continue;
            const size_t reversed = reversed_codes_[symbol];
            Entry terminal;
            terminal.value = static_cast<uint32_t>(symbol);

            if (length <= root_bits) {
                terminal.bits = static_cast<uint8_t>(length);
                const size_t step = size_t{1} << length;
                for (size_t index = reversed; index < root_size; index += step)
                    table_[index] = terminal;
                continue;
            }

            const size_t prefix = reversed & (root_size - 1u);
            const unsigned suffix_length = length - root_bits;
            terminal.bits = static_cast<uint8_t>(suffix_length);
            const size_t suffix = reversed >> root_bits;
            const size_t secondary_size = size_t{1} << secondary_bits_[prefix];
            const size_t step = size_t{1} << suffix_length;
            const size_t offset = secondary_offsets_[prefix];
            for (size_t index = suffix; index < secondary_size; index += step)
                table_[offset + index] = terminal;
        }
    }

    FZ_DEFLATE_FORCEINLINE uint16_t decode(BitReader& bits) const {
        Entry entry = table_[bits.peek(root_bits_)];
        if (entry.secondary_bits != 0) {
            if (bits.available_bits() < entry.bits)
                throw std::runtime_error("Invalid or truncated DEFLATE Huffman code");
            bits.drop(entry.bits);
            const size_t index = static_cast<size_t>(entry.value) +
                bits.peek(entry.secondary_bits);
            entry = table_[index];
        }
        if (entry.bits == 0 || bits.available_bits() < entry.bits)
            throw std::runtime_error("Invalid or truncated DEFLATE Huffman code");
        bits.drop(entry.bits);
        return static_cast<uint16_t>(entry.value);
    }

private:
    struct Entry {
        uint32_t value = 0;
        uint8_t bits = 0;
        uint8_t secondary_bits = 0;
    };

    std::vector<Entry> table_;
    std::vector<uint8_t> secondary_bits_;
    std::vector<size_t> secondary_offsets_;
    std::vector<uint16_t> reversed_codes_;
    uint8_t root_bits_ = 0;
};

class StreamOutput {
public:
    StreamOutput(std::ostream& output, uint64_t expected_size,
                 const std::function<void(uint64_t)>& progress)
        : output_(output), expected_size_(expected_size), progress_(progress) {
        pending_.reserve(kPendingSize);
    }

    FZ_DEFLATE_FORCEINLINE void put(uint8_t value) {
        if (size_ >= expected_size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        window_[static_cast<size_t>(size_ & 0x7FFFu)] = value;
        ++size_;
        pending_.push_back(value);
        if (pending_.size() == kPendingSize) flush();
    }

    void copy(unsigned distance, unsigned length) {
        if (distance == 0 || distance > 32768 || distance > size_)
            throw std::runtime_error("Invalid DEFLATE back-reference distance");
        if (length > expected_size_ - size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        for (unsigned i = 0; i < length; ++i) {
            const uint8_t value = window_[static_cast<size_t>((size_ - distance) & 0x7FFFu)];
            put(value);
        }
    }

    void copy_stored(BitReader& bits, unsigned length) {
        std::array<uint8_t, 16u * 1024u> buffer;
        unsigned remaining = length;
        while (remaining != 0) {
            const size_t amount = std::min<size_t>(remaining, buffer.size());
            bits.read_bytes(buffer.data(), amount);
            put_bytes(buffer.data(), amount);
            remaining -= static_cast<unsigned>(amount);
        }
    }

    InflateResult finish() {
        if (size_ != expected_size_)
            throw std::runtime_error("DEFLATE output size does not match the ZIP directory");
        flush();
        return {size_, crc_.value()};
    }

private:
    static constexpr size_t kPendingSize = 64u * 1024u;

    void put_bytes(const uint8_t* data, size_t amount) {
        if (amount > expected_size_ - size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        while (amount != 0) {
            const size_t space = kPendingSize - pending_.size();
            const size_t current = std::min(amount, space);

            size_t copied = 0;
            while (copied != current) {
                const size_t window_position =
                    static_cast<size_t>((size_ + copied) & 0x7FFFu);
                const size_t window_amount = std::min(
                    current - copied, window_.size() - window_position);
                std::memcpy(window_.data() + window_position,
                            data + copied, window_amount);
                copied += window_amount;
            }

            pending_.insert(pending_.end(), data, data + current);
            size_ += current;
            data += current;
            amount -= current;
            if (pending_.size() == kPendingSize) flush();
        }
    }

    void flush() {
        if (pending_.empty()) return;
        crc_.update(pending_.data(), pending_.size());
        output_.write(reinterpret_cast<const char*>(pending_.data()),
                      static_cast<std::streamsize>(pending_.size()));
        if (!output_) throw std::runtime_error("Cannot write extracted file");
        pending_.clear();
        if (progress_) progress_(size_);
    }

    std::ostream& output_;
    uint64_t expected_size_;
    uint64_t size_ = 0;
    std::array<uint8_t, 32768> window_{};
    std::vector<uint8_t> pending_;
    Crc32 crc_;
    const std::function<void(uint64_t)>& progress_;
};

FZ_DEFLATE_FORCEINLINE void copy_exact(uint8_t* destination,
                                       const uint8_t* source,
                                       size_t size) noexcept {
    std::memcpy(destination, source, size);
}

FZ_DEFLATE_FORCEINLINE void copy_match(uint8_t* destination,
                                       unsigned distance,
                                       unsigned length) noexcept {
    if (distance == 1) {
        std::memset(destination, destination[-1], length);
        return;
    }

    // Copy the available history once, then repeatedly double the initialized
    // prefix. Every memcpy below is non-overlapping, including short-distance
    // matches such as distance 2 or 3.
    const size_t first = std::min<size_t>(distance, length);
    copy_exact(destination, destination - distance, first);
    size_t copied = first;
    while (copied < length) {
        const size_t amount = std::min<size_t>(copied, length - copied);
        copy_exact(destination + copied, destination, amount);
        copied += amount;
    }
}

class BufferOutput {
public:
    BufferOutput(uint8_t* output, size_t expected_size,
                 const std::function<void(uint64_t)>& progress)
        : output_(output), expected_size_(expected_size), progress_(progress) {}

    FZ_DEFLATE_FORCEINLINE void put(uint8_t value) {
        if (size_ == expected_size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        output_[size_++] = value;
        checkpoint(false);
    }

    FZ_DEFLATE_FORCEINLINE void copy(unsigned distance, unsigned length) {
        if (distance == 0 || distance > 32768 || distance > size_)
            throw std::runtime_error("Invalid DEFLATE back-reference distance");
        if (length > expected_size_ - size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        copy_match(output_ + size_, distance, length);
        size_ += length;
        checkpoint(false);
    }

    void copy_stored(BitReader& bits, unsigned length) {
        if (length > expected_size_ - size_)
            throw std::runtime_error("DEFLATE output exceeds the ZIP entry size");
        uint8_t* destination = length == 0 ? nullptr : output_ + size_;
        bits.read_bytes(destination, length);
        size_ += length;
        checkpoint(false);
    }

    InflateResult finish() {
        if (size_ != expected_size_)
            throw std::runtime_error("DEFLATE output size does not match the ZIP directory");
        checkpoint(true);
        return {static_cast<uint64_t>(size_), crc_.value()};
    }

private:
    static constexpr size_t kCheckpointSize = 256u * 1024u;

    FZ_DEFLATE_FORCEINLINE void checkpoint(bool force) {
        const size_t amount = size_ - crc_position_;
        if (amount == 0 || (!force && amount < kCheckpointSize)) return;
        crc_.update(output_ + crc_position_, amount);
        crc_position_ = size_;
        if (progress_) progress_(static_cast<uint64_t>(size_));
    }

    uint8_t* output_;
    size_t expected_size_;
    size_t size_ = 0;
    size_t crc_position_ = 0;
    Crc32 crc_;
    const std::function<void(uint64_t)>& progress_;
};

struct FixedTables {
    FixedTables() {
        std::array<uint8_t, 288> literal_lengths{};
        std::fill(literal_lengths.begin(), literal_lengths.begin() + 144, uint8_t{8});
        std::fill(literal_lengths.begin() + 144, literal_lengths.begin() + 256, uint8_t{9});
        std::fill(literal_lengths.begin() + 256, literal_lengths.begin() + 280, uint8_t{7});
        std::fill(literal_lengths.begin() + 280, literal_lengths.end(), uint8_t{8});
        literal_length.build(literal_lengths.data(), literal_lengths.size(), 10);

        std::array<uint8_t, 32> distance_lengths{};
        distance_lengths.fill(uint8_t{5});
        distance.build(distance_lengths.data(), distance_lengths.size(), 8);
    }

    HuffmanTable literal_length;
    HuffmanTable distance;
};

const FixedTables& fixed_tables() {
    static const FixedTables tables;
    return tables;
}

void build_dynamic_tables(BitReader& bits,
                          HuffmanTable& literal_length,
                          HuffmanTable& distance,
                          HuffmanTable& code_length_table) {
    const unsigned literal_count = bits.read(5) + 257;
    const unsigned distance_count = bits.read(5) + 1;
    const unsigned code_length_count = bits.read(4) + 4;
    if (literal_count > 286)
        throw std::runtime_error("Reserved DEFLATE literal/length table size");
    static constexpr unsigned order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    std::array<uint8_t, 19> code_lengths{};
    for (unsigned i = 0; i < code_length_count; ++i)
        code_lengths[order[i]] = static_cast<uint8_t>(bits.read(3));
    code_length_table.build(code_lengths.data(), code_lengths.size(), 7);

    std::array<uint8_t, 286 + 32> lengths{};
    const size_t total_lengths = literal_count + distance_count;
    size_t position = 0;
    while (position < total_lengths) {
        const uint16_t symbol = code_length_table.decode(bits);
        if (symbol <= 15) {
            lengths[position++] = static_cast<uint8_t>(symbol);
            continue;
        }

        unsigned repeat = 0;
        uint8_t value = 0;
        if (symbol == 16) {
            if (position == 0) throw std::runtime_error("Invalid DEFLATE code-length repeat");
            repeat = bits.read(2) + 3;
            value = lengths[position - 1];
        } else if (symbol == 17) {
            repeat = bits.read(3) + 3;
        } else if (symbol == 18) {
            repeat = bits.read(7) + 11;
        } else {
            throw std::runtime_error("Invalid DEFLATE code-length symbol");
        }
        if (repeat > total_lengths - position)
            throw std::runtime_error("DEFLATE code-length repeat exceeds the table");
        std::fill_n(lengths.data() + position, repeat, value);
        position += repeat;
    }

    if (lengths[256] == 0)
        throw std::runtime_error("DEFLATE literal tree has no end-of-block code");
    literal_length.build(lengths.data(), literal_count, 10);
    distance.build(lengths.data() + literal_count, distance_count, 8, true);
}

template <typename Output>
void decode_compressed_block(BitReader& bits,
                             const HuffmanTable& literal_length,
                             const HuffmanTable& distance,
                             Output& output) {
    static constexpr unsigned length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258
    };
    static constexpr uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static constexpr unsigned distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static constexpr uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };

    for (;;) {
        const uint16_t symbol = literal_length.decode(bits);
        if (symbol < 256) {
            output.put(static_cast<uint8_t>(symbol));
        } else if (symbol == 256) {
            return;
        } else {
            if (symbol < 257 || symbol > 285)
                throw std::runtime_error("Invalid DEFLATE length symbol");
            const unsigned length_index = symbol - 257;
            const unsigned length = length_base[length_index] +
                bits.read(length_extra[length_index]);
            const uint16_t distance_symbol = distance.decode(bits);
            if (distance_symbol >= 30)
                throw std::runtime_error("Invalid DEFLATE distance symbol");
            const unsigned copy_distance = distance_base[distance_symbol] +
                bits.read(distance_extra[distance_symbol]);
            output.copy(copy_distance, length);
        }
    }
}

template <typename Output>
InflateResult inflate_raw(const uint8_t* data,
                          size_t size,
                          Output& expanded) {
    if (size == 0) throw std::runtime_error("Empty DEFLATE stream");
    BitReader bits(data, size);
    HuffmanTable dynamic_literal_length;
    HuffmanTable dynamic_distance;
    HuffmanTable code_length_table;

    bool final_block = false;
    do {
        final_block = bits.read(1) != 0;
        const unsigned type = bits.read(2);
        if (type == 0) {
            bits.align_to_byte();
            const unsigned length = bits.read(16);
            const unsigned inverse_length = bits.read(16);
            if ((length ^ 0xFFFFu) != inverse_length)
                throw std::runtime_error("Invalid stored DEFLATE block length");
            expanded.copy_stored(bits, length);
        } else if (type == 1) {
            const FixedTables& fixed = fixed_tables();
            decode_compressed_block(bits, fixed.literal_length,
                                    fixed.distance, expanded);
        } else if (type == 2) {
            build_dynamic_tables(bits, dynamic_literal_length,
                                 dynamic_distance, code_length_table);
            decode_compressed_block(bits, dynamic_literal_length,
                                    dynamic_distance, expanded);
        } else {
            throw std::runtime_error("Reserved DEFLATE block type");
        }
    } while (!final_block);
    return expanded.finish();
}

} // namespace

InflateResult inflate_raw_to_stream(const uint8_t* data,
                                    size_t size,
                                    uint64_t expected_size,
                                    std::ostream& output,
                                    const std::function<void(uint64_t)>& progress) {
    if (data == nullptr && size != 0)
        throw std::runtime_error("Null DEFLATE input buffer");
    StreamOutput expanded(output, expected_size, progress);
    return inflate_raw(data, size, expanded);
}

InflateResult inflate_raw_to_buffer(const uint8_t* data,
                                    size_t size,
                                    uint64_t expected_size,
                                    uint8_t* output,
                                    const std::function<void(uint64_t)>& progress) {
    if (data == nullptr && size != 0)
        throw std::runtime_error("Null DEFLATE input buffer");
    if (expected_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("DEFLATE output is too large for this process");
    if (expected_size != 0 && output == nullptr)
        throw std::runtime_error("Null DEFLATE output buffer");
    BufferOutput expanded(output, static_cast<size_t>(expected_size), progress);
    return inflate_raw(data, size, expanded);
}

} // namespace fz
