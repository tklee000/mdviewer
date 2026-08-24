#include "deflate_encoder.h"

#include <array>
#include <queue>

namespace fz {
namespace {

constexpr size_t kWindowSize = 32768;
constexpr size_t kWindowMask = kWindowSize - 1;
constexpr size_t kHashBits = 18;
constexpr size_t kHashSize = size_t{1} << kHashBits;
constexpr size_t kMinMatch = 3;
constexpr size_t kMaxMatch = 258;
constexpr size_t kCancelCheckInterval = 512;

constexpr std::array<uint16_t, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258
};
constexpr std::array<uint8_t, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0
};
constexpr std::array<uint16_t, 30> kDistBase = {
    1, 2, 3, 4, 5, 7, 9, 13,
    17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577
};
constexpr std::array<uint8_t, 30> kDistExtra = {
    0, 0, 0, 0, 1, 1, 2, 2,
    3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

constexpr std::array<uint8_t, kMaxMatch + 1> make_length_indices() {
    std::array<uint8_t, kMaxMatch + 1> result{};
    size_t index = 0;
    for (size_t length = 0; length < result.size(); ++length) {
        while (index + 1 < kLengthBase.size() &&
               length >= kLengthBase[index + 1]) {
            ++index;
        }
        result[length] = static_cast<uint8_t>(index);
    }
    return result;
}

constexpr std::array<uint8_t, 257> make_distance_low_indices() {
    std::array<uint8_t, 257> result{};
    size_t index = 0;
    for (size_t distance = 0; distance < result.size(); ++distance) {
        while (index + 1 < kDistBase.size() &&
               distance >= kDistBase[index + 1]) {
            ++index;
        }
        result[distance] = static_cast<uint8_t>(index);
    }
    return result;
}

// Above 256, every DEFLATE distance-code boundary is aligned to 128 bytes.
// One lookup entry can therefore cover each complete 128-byte range.
constexpr std::array<uint8_t, 256> make_distance_high_indices() {
    std::array<uint8_t, 256> result{};
    size_t index = 0;
    for (size_t bucket = 0; bucket < result.size(); ++bucket) {
        const size_t distance = bucket * 128u + 1u;
        while (index + 1 < kDistBase.size() &&
               distance >= kDistBase[index + 1]) {
            ++index;
        }
        result[bucket] = static_cast<uint8_t>(index);
    }
    return result;
}

constexpr auto kLengthIndex = make_length_indices();
constexpr auto kDistanceLowIndex = make_distance_low_indices();
constexpr auto kDistanceHighIndex = make_distance_high_indices();
static_assert(kLengthIndex[3] == 0 && kLengthIndex[258] == 28,
              "Invalid DEFLATE length lookup table");
static_assert(kDistanceLowIndex[1] == 0 &&
              kDistanceHighIndex[(32768u - 1u) >> 7] == 29,
              "Invalid DEFLATE distance lookup table");

class BitWriter {
public:
    explicit BitWriter(size_t reserve_bytes = 0) { bytes_.reserve(reserve_bytes); }

    void write_bits(uint32_t value, unsigned count) {
        if (count == 0) return;
        const uint64_t mask = count == 32 ? 0xFFFFFFFFull : ((uint64_t{1} << count) - 1u);
        bit_buffer_ |= (static_cast<uint64_t>(value) & mask) << buffered_bits_;
        buffered_bits_ += count;
        total_bits_ += count;
        while (buffered_bits_ >= 8) {
            bytes_.push_back(static_cast<uint8_t>(bit_buffer_));
            bit_buffer_ >>= 8;
            buffered_bits_ -= 8;
        }
    }

    void append_stream(const std::vector<uint8_t>& src, uint64_t bit_count) {
        const size_t full_bytes = static_cast<size_t>(bit_count / 8);
        if (buffered_bits_ == 0) {
            bytes_.insert(bytes_.end(), src.begin(),
                          src.begin() + static_cast<std::ptrdiff_t>(full_bytes));
            total_bits_ += static_cast<uint64_t>(full_bytes) * 8u;
            const unsigned tail = static_cast<unsigned>(bit_count & 7u);
            if (tail) write_bits(src[full_bytes], tail);
            return;
        }
        for (size_t i = 0; i < full_bytes; ++i) write_bits(src[i], 8);
        const unsigned tail = static_cast<unsigned>(bit_count & 7u);
        if (tail) write_bits(src[full_bytes], tail);
    }

    void append_bytes(const uint8_t* src, size_t size) {
        if (size == 0) return;
        if (buffered_bits_ != 0)
            throw std::runtime_error("Internal unaligned DEFLATE byte append");
        bytes_.insert(bytes_.end(), src, src + size);
        total_bits_ += static_cast<uint64_t>(size) * 8u;
    }

    uint64_t bit_count() const { return total_bits_; }

    void align_to_byte() {
        const unsigned remainder = static_cast<unsigned>(total_bits_ & 7u);
        if (remainder != 0) write_bits(0, 8u - remainder);
    }

    DeflateResult finish() {
        if (buffered_bits_ != 0) bytes_.push_back(static_cast<uint8_t>(bit_buffer_));
        return {std::move(bytes_), total_bits_};
    }

private:
    std::vector<uint8_t> bytes_;
    uint64_t bit_buffer_ = 0;
    unsigned buffered_bits_ = 0;
    uint64_t total_bits_ = 0;
};

uint16_t reverse_bits(uint16_t value, unsigned bit_count) {
    uint16_t reversed = 0;
    for (unsigned i = 0; i < bit_count; ++i) {
        reversed = static_cast<uint16_t>((reversed << 1) | (value & 1u));
        value = static_cast<uint16_t>(value >> 1);
    }
    return reversed;
}

struct HuffmanTable {
    std::vector<uint16_t> codes;
    std::vector<uint8_t> lengths;
};

std::vector<uint8_t> build_code_lengths(const std::vector<uint32_t>& frequencies, unsigned max_bits) {
    struct Node {
        uint64_t frequency = 0;
        int parent = -1;
        int symbol = -1;
    };
    struct QueueItem {
        uint64_t frequency = 0;
        int index = -1;
        int tie = 0;
        bool operator>(const QueueItem& other) const {
            if (frequency != other.frequency) return frequency > other.frequency;
            return tie > other.tie;
        }
    };

    std::vector<uint64_t> adjusted(frequencies.begin(), frequencies.end());
    size_t active_count = 0;
    size_t only_symbol = 0;
    for (size_t i = 0; i < adjusted.size(); ++i) {
        if (adjusted[i] != 0) { ++active_count; only_symbol = i; }
    }

    std::vector<uint8_t> lengths(frequencies.size(), 0);
    if (active_count == 0) {
        lengths[0] = 1;
        return lengths;
    }
    if (active_count == 1) {
        lengths[only_symbol] = 1;
        return lengths;
    }

    // A normal Huffman tree can rarely exceed DEFLATE's bit limit on very
    // skewed data. Rather than emitting an invalid oversubscribed tree, gently
    // flatten non-zero frequencies and rebuild until the tree fits. This keeps
    // the implementation compact, deterministic and always prefix-valid.
    for (;;) {
        std::vector<Node> nodes;
        nodes.reserve(active_count * 2);
        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> heap;
        for (size_t symbol = 0; symbol < adjusted.size(); ++symbol) {
            if (adjusted[symbol] == 0) continue;
            const int index = static_cast<int>(nodes.size());
            nodes.push_back({adjusted[symbol], -1, static_cast<int>(symbol)});
            heap.push({adjusted[symbol], index, static_cast<int>(symbol)});
        }

        int internal_tie = static_cast<int>(frequencies.size());
        while (heap.size() > 1) {
            const QueueItem a = heap.top(); heap.pop();
            const QueueItem b = heap.top(); heap.pop();
            const int parent = static_cast<int>(nodes.size());
            nodes.push_back({a.frequency + b.frequency, -1, -1});
            nodes[static_cast<size_t>(a.index)].parent = parent;
            nodes[static_cast<size_t>(b.index)].parent = parent;
            heap.push({a.frequency + b.frequency, parent, internal_tie++});
        }

        std::fill(lengths.begin(), lengths.end(), uint8_t{0});
        unsigned deepest = 0;
        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            if (nodes[node_index].symbol < 0) continue;
            unsigned depth = 0;
            int current = static_cast<int>(node_index);
            while (nodes[static_cast<size_t>(current)].parent >= 0) {
                ++depth;
                current = nodes[static_cast<size_t>(current)].parent;
            }
            lengths[static_cast<size_t>(nodes[node_index].symbol)] = static_cast<uint8_t>(depth);
            deepest = std::max(deepest, depth);
        }
        if (deepest <= max_bits) return lengths;

        for (uint64_t& frequency : adjusted) {
            if (frequency != 0) frequency = (frequency + 1) / 2;
        }
    }
}

HuffmanTable make_huffman(const std::vector<uint32_t>& frequencies, unsigned max_bits) {
    HuffmanTable table;
    table.lengths = build_code_lengths(frequencies, max_bits);
    table.codes.assign(frequencies.size(), 0);

    std::vector<uint16_t> counts(max_bits + 1, 0), next_code(max_bits + 1, 0);
    for (uint8_t bits : table.lengths) if (bits != 0) ++counts[bits];
    uint16_t code = 0;
    for (unsigned bits = 1; bits <= max_bits; ++bits) {
        code = static_cast<uint16_t>((code + counts[bits - 1]) << 1);
        next_code[bits] = code;
    }
    for (size_t symbol = 0; symbol < table.lengths.size(); ++symbol) {
        const unsigned bits = table.lengths[symbol];
        if (bits != 0) table.codes[symbol] = reverse_bits(next_code[bits]++, bits);
    }
    return table;
}

const HuffmanTable& fixed_literal_table() {
    static const HuffmanTable table = [] {
        HuffmanTable t;
        t.lengths.assign(288, 0);
        for (int symbol = 0; symbol <= 143; ++symbol) t.lengths[symbol] = 8;
        for (int symbol = 144; symbol <= 255; ++symbol) t.lengths[symbol] = 9;
        for (int symbol = 256; symbol <= 279; ++symbol) t.lengths[symbol] = 7;
        for (int symbol = 280; symbol <= 287; ++symbol) t.lengths[symbol] = 8;
        t.codes.assign(288, 0);
        std::array<uint16_t, 16> counts{}, next{};
        for (uint8_t bits : t.lengths) ++counts[bits];
        uint16_t code = 0;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            code = static_cast<uint16_t>((code + counts[bits - 1]) << 1);
            next[bits] = code;
        }
        for (size_t s = 0; s < t.lengths.size(); ++s) t.codes[s] = reverse_bits(next[t.lengths[s]]++, t.lengths[s]);
        return t;
    }();
    return table;
}

inline uint32_t hash3(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) |
                 (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16);
    v ^= v >> 13;
    v *= 0x1E35A7BDu;
    return v >> (32 - kHashBits);
}

bool likely_incompressible_block(const uint8_t* data,
                                 size_t size,
                                 const uint8_t* history,
                                 size_t history_size) {
    if (size < 4096) return false;

    constexpr size_t kByteSamples = 4096;
    const size_t byte_samples = std::min(kByteSamples, size);
    std::array<uint16_t, 256> histogram{};
    size_t adjacent_equal = 0;
    for (size_t sample = 0; sample < byte_samples; ++sample) {
        const size_t position = sample * size / byte_samples;
        ++histogram[data[position]];
        if (position + 1 < size && data[position] == data[position + 1])
            ++adjacent_equal;
    }

    size_t unique = 0;
    size_t most_common = 0;
    for (uint16_t count : histogram) {
        if (count != 0) ++unique;
        most_common = std::max(most_common, static_cast<size_t>(count));
    }
    if (unique < 250 || most_common * 32u >= byte_samples ||
        adjacent_equal * 64u >= byte_samples) {
        return false;
    }

    // A uniform byte histogram can still describe repeated binary data. Probe
    // exact 8-byte sequences in both the current block and the preceding
    // Deflate window before taking the Store fast path.
    constexpr size_t kSequenceSamples = 512;
    constexpr size_t kHashSlots = 2048;
    constexpr size_t kProbeLimit = 4;
    std::array<uint64_t, kHashSlots> hashes{};
    std::array<uint8_t, kHashSlots> occupied{};
    const auto hash_value = [=](uint64_t value) {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        return static_cast<size_t>(value) & (kHashSlots - 1u);
    };
    const auto insert_or_find = [&](uint64_t value) {
        const size_t first = hash_value(value);
        for (size_t probe = 0; probe < kProbeLimit; ++probe) {
            const size_t slot = (first + probe) & (kHashSlots - 1u);
            if (!occupied[slot]) {
                occupied[slot] = 1;
                hashes[slot] = value;
                return false;
            }
            if (hashes[slot] == value) return true;
        }
        return false;
    };

    if (history != nullptr && history_size >= sizeof(uint64_t)) {
        const size_t samples = std::min(kSequenceSamples,
                                        history_size / sizeof(uint64_t));
        for (size_t sample = 0; sample < samples; ++sample) {
            const size_t position = sample *
                (history_size - sizeof(uint64_t)) / std::max<size_t>(1, samples - 1);
            uint64_t value = 0;
            std::memcpy(&value, history + position, sizeof(value));
            (void)insert_or_find(value);
        }
    }

    const size_t samples = std::min(kSequenceSamples,
                                    size / sizeof(uint64_t));
    size_t repeated = 0;
    for (size_t sample = 0; sample < samples; ++sample) {
        const size_t position = sample *
            (size - sizeof(uint64_t)) / std::max<size_t>(1, samples - 1);
        uint64_t value = 0;
        std::memcpy(&value, data + position, sizeof(value));
        if (insert_or_find(value) && ++repeated >= 2) return false;
    }
    return true;
}

struct Match {
    size_t length = 0;
    size_t distance = 0;
};

struct SearchSettings {
    unsigned max_chain = 128;
    unsigned nice_length = 128;
    unsigned max_lazy = 32;
};

SearchSettings settings_for_level(int level) {
    static constexpr SearchSettings settings[10] = {
        {0, 0, 0},
        {8, 32, 0},
        {16, 48, 4},
        {32, 64, 8},
        {64, 96, 16},
        {128, 128, 32},
        {256, 160, 64},
        {512, 192, 96},
        {1024, 224, 160},
        {2048, 258, 258}
    };
    return settings[static_cast<size_t>(std::clamp(level, 1, 9))];
}

Match find_match(const uint8_t* data,
                 size_t size,
                 size_t pos,
                 int32_t candidate,
                 const std::vector<int32_t>& previous,
                 const SearchSettings& settings,
                 MatchLengthFn match_length) {
    Match best{};
    const size_t earliest = pos > kWindowSize ? pos - kWindowSize : 0;
    const size_t max_len = std::min(kMaxMatch, size - pos);
    unsigned remaining = settings.max_chain;

    while (candidate >= 0 && static_cast<size_t>(candidate) >= earliest && remaining-- != 0) {
        const size_t c = static_cast<size_t>(candidate);
        if (data[c] == data[pos] && data[c + 1] == data[pos + 1] && data[c + 2] == data[pos + 2] &&
            (best.length == 0 || data[c + best.length] == data[pos + best.length])) {
            const size_t length = match_length(data + c, data + pos, max_len);
            if (length > best.length && length >= kMinMatch) {
                best.length = length;
                best.distance = pos - c;
                if (length >= settings.nice_length || length == max_len) break;
            }
        }
        candidate = previous[c & kWindowMask];
    }
    return best;
}

struct Token {
    uint16_t literal_or_length = 0; // 0..255 literal; 256 means match
    uint16_t length = 0;
    uint16_t distance = 0;

    static Token literal(uint8_t value) { return {value, 0, 0}; }
    static Token match(size_t len, size_t dist) {
        return {256, static_cast<uint16_t>(len), static_cast<uint16_t>(dist)};
    }
    bool is_literal() const { return literal_or_length < 256; }
};

unsigned length_index(size_t length) {
    return kLengthIndex[std::min(length, kMaxMatch)];
}

unsigned distance_index(size_t distance) {
    distance = std::min(distance, kWindowSize);
    if (distance <= 256) return kDistanceLowIndex[distance];
    return kDistanceHighIndex[(distance - 1u) >> 7];
}

struct TokenizedBlock {
    std::vector<Token> tokens;
    std::vector<uint32_t> lit_freq = std::vector<uint32_t>(286, 0);
    std::vector<uint32_t> dist_freq = std::vector<uint32_t>(30, 0);
};

struct MatchOption {
    uint16_t length = 0;
    uint16_t distance = 0;
};

struct MatchRange {
    uint32_t offset = 0;
    uint16_t count = 0;
};

struct MatchBlock {
    std::vector<MatchRange> ranges;
    std::vector<MatchOption> options;
};

class StreamingMatchFinder {
public:
    StreamingMatchFinder(const uint8_t* data,
                         size_t size,
                         size_t history_size,
                         SearchSettings settings,
                         MatchLengthFn match_length,
                         const CancellationCallback& cancel)
        : data_(data), size_(size), settings_(settings), match_length_(match_length),
          head_(kHashSize, -1), children_((kWindowSize + 1) * 2, -1) {
        for (size_t pos = 0; pos < history_size; ++pos) {
            if ((pos % kCancelCheckInterval) == 0) throw_if_cancelled(cancel);
            insert_only(pos);
        }
    }

    MatchBlock collect(size_t block_start, size_t block_end,
                       const CancellationCallback& cancel) {
        MatchBlock block;
        const size_t block_size = block_end - block_start;
        block.ranges.resize(block_size);
        block.options.reserve(block_size * 2);

        for (size_t pos = block_start; pos < block_end; ++pos) {
            if (((pos - block_start) % kCancelCheckInterval) == 0)
                throw_if_cancelled(cancel);
            MatchRange& range = block.ranges[pos - block_start];
            range.offset = static_cast<uint32_t>(block.options.size());
            insert_position(pos, block_end, &block.options);

            const size_t count = block.options.size() - range.offset;
            range.count = static_cast<uint16_t>(std::min<size_t>(count, 0xFFFFu));
        }
        return block;
    }

    void reset_history(size_t position,
                       const CancellationCallback& cancel) {
        if (position > size_)
            throw std::runtime_error("Internal DEFLATE history position is out of range");
        std::fill(head_.begin(), head_.end(), int32_t{-1});
        std::fill(children_.begin(), children_.end(), int32_t{-1});
        const size_t history_begin = position > kWindowSize ?
            position - kWindowSize : 0;
        for (size_t pos = history_begin; pos < position; ++pos) {
            if (((pos - history_begin) % kCancelCheckInterval) == 0)
                throw_if_cancelled(cancel);
            insert_only(pos);
        }
    }

private:
    void insert_only(size_t pos) {
        insert_position(pos, pos, nullptr);
    }

    void insert_position(size_t pos, size_t emit_end, std::vector<MatchOption>* options) {
        const size_t slot = (pos % (kWindowSize + 1)) * 2;
        int32_t* less_link = &children_[slot];
        int32_t* greater_link = &children_[slot + 1];
        *less_link = -1;
        *greater_link = -1;
        if (pos + kMinMatch > size_) return;

        const uint32_t h = hash3(data_ + pos);
        int32_t candidate = head_[h];
        head_[h] = static_cast<int32_t>(pos);
        const size_t earliest = pos > kWindowSize ? pos - kWindowSize : 0;
        const size_t compare_limit = std::min<size_t>(settings_.nice_length, size_ - pos);
        const size_t emit_limit = emit_end > pos ? std::min(kMaxMatch, emit_end - pos) : 0;
        size_t best_length = kMinMatch - 1;
        unsigned remaining = settings_.max_chain;

        while (candidate >= 0 && static_cast<size_t>(candidate) >= earliest && remaining-- != 0) {
            const size_t c = static_cast<size_t>(candidate);
            int32_t* candidate_children = &children_[(c % (kWindowSize + 1)) * 2];
            const size_t compared = match_length_(data_ + c, data_ + pos, compare_limit);

            if (options != nullptr && emit_limit >= kMinMatch && compared >= kMinMatch) {
                size_t emitted_length = std::min(compared, emit_limit);
                if (compared == compare_limit && compare_limit < kMaxMatch) {
                    const size_t full_limit = std::min(kMaxMatch, emit_limit);
                    emitted_length = match_length_(data_ + c, data_ + pos, full_limit);
                }
                if (emitted_length > best_length) {
                    options->push_back({static_cast<uint16_t>(emitted_length),
                                        static_cast<uint16_t>(pos - c)});
                    best_length = emitted_length;
                }
            }

            if (compared == compare_limit) {
                *less_link = candidate_children[0];
                *greater_link = candidate_children[1];
                return;
            }

            if (data_[c + compared] < data_[pos + compared]) {
                *less_link = candidate;
                less_link = &candidate_children[1];
                candidate = *less_link;
            } else {
                *greater_link = candidate;
                greater_link = &candidate_children[0];
                candidate = *greater_link;
            }
        }
        *less_link = -1;
        *greater_link = -1;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    SearchSettings settings_{};
    MatchLengthFn match_length_ = nullptr;
    std::vector<int32_t> head_;
    std::vector<int32_t> children_;
};

struct ParsePrices {
    std::array<uint8_t, 256> literals{};
    std::array<uint8_t, kMaxMatch + 1> lengths{};
    std::array<uint8_t, 30> distances{};
};

ParsePrices dynamic_parse_prices(const TokenizedBlock& block) {
    ParsePrices prices;
    const HuffmanTable literal_table = make_huffman(block.lit_freq, 15);
    const HuffmanTable distance_table = make_huffman(block.dist_freq, 15);
    for (size_t symbol = 0; symbol < prices.literals.size(); ++symbol) {
        const uint8_t bits = literal_table.lengths[symbol];
        prices.literals[symbol] = bits != 0 ? bits : uint8_t{11};
    }
    for (size_t length = kMinMatch; length <= kMaxMatch; ++length) {
        const unsigned index = length_index(length);
        const uint8_t bits = literal_table.lengths[257u + index];
        prices.lengths[length] = static_cast<uint8_t>((bits != 0 ? bits : uint8_t{11}) + kLengthExtra[index]);
    }
    for (size_t index = 0; index < prices.distances.size(); ++index) {
        const uint8_t bits = distance_table.lengths[index];
        prices.distances[index] = static_cast<uint8_t>((bits != 0 ? bits : uint8_t{6}) + kDistExtra[index]);
    }
    return prices;
}

struct ParseNode {
    uint32_t cost = std::numeric_limits<uint32_t>::max();
    uint32_t previous = 0;
    uint16_t length = 0;
    uint16_t distance = 0;
};

TokenizedBlock parse_block_optimal(const uint8_t* data,
                                   size_t block_start,
                                   size_t block_end,
                                   const MatchBlock& matches,
                                   const ParsePrices& prices,
                                   const CancellationCallback& cancel) {
    const size_t block_size = block_end - block_start;
    std::vector<ParseNode> nodes(block_size + 1);
    nodes[0].cost = 0;

    for (size_t offset = 0; offset < block_size; ++offset) {
        if ((offset % kCancelCheckInterval) == 0) throw_if_cancelled(cancel);
        const uint32_t base_cost = nodes[offset].cost;
        if (base_cost == std::numeric_limits<uint32_t>::max()) continue;

        ParseNode& literal_node = nodes[offset + 1];
        const uint32_t literal_cost = base_cost + prices.literals[data[block_start + offset]];
        if (literal_cost < literal_node.cost) {
            literal_node.cost = literal_cost;
            literal_node.previous = static_cast<uint32_t>(offset);
            literal_node.length = 1;
            literal_node.distance = 0;
        }

        const MatchRange& range = matches.ranges[offset];
        size_t covered_length = kMinMatch - 1;
        for (size_t option_index = 0; option_index < range.count; ++option_index) {
            const MatchOption& option = matches.options[range.offset + option_index];
            const size_t max_length = std::min<size_t>(option.length, block_size - offset);
            if (max_length <= covered_length) continue;
            const unsigned distance_slot = distance_index(option.distance);
            const uint32_t distance_cost = prices.distances[distance_slot];
            for (size_t length = covered_length + 1; length <= max_length; ++length) {
                ParseNode& target = nodes[offset + length];
                const uint32_t cost = base_cost + prices.lengths[length] + distance_cost;
                if (cost < target.cost) {
                    target.cost = cost;
                    target.previous = static_cast<uint32_t>(offset);
                    target.length = static_cast<uint16_t>(length);
                    target.distance = option.distance;
                }
            }
            covered_length = max_length;
        }
    }

    TokenizedBlock result;
    result.tokens.reserve(block_size / 2 + 64);
    size_t cursor = block_size;
    while (cursor != 0) {
        const ParseNode& node = nodes[cursor];
        if (node.length == 1) {
            result.tokens.push_back(Token::literal(data[block_start + node.previous]));
        } else if (node.length >= kMinMatch) {
            result.tokens.push_back(Token::match(node.length, node.distance));
        } else {
            throw std::runtime_error("Internal Deflate optimal parser failure");
        }
        cursor = node.previous;
    }
    std::reverse(result.tokens.begin(), result.tokens.end());

    for (const Token& token : result.tokens) {
        if (token.is_literal()) {
            ++result.lit_freq[token.literal_or_length];
        } else {
            ++result.lit_freq[257u + length_index(token.length)];
            ++result.dist_freq[distance_index(token.distance)];
        }
    }
    ++result.lit_freq[256];
    if (std::none_of(result.dist_freq.begin(), result.dist_freq.end(), [](uint32_t value) { return value != 0; }))
        result.dist_freq[0] = 1;
    return result;
}

TokenizedBlock seed_block_prices(const uint8_t* data,
                                 size_t block_start,
                                 size_t block_end,
                                 const MatchBlock& matches,
                                 const CancellationCallback& cancel) {
    const size_t block_size = block_end - block_start;
    TokenizedBlock result;
    result.tokens.reserve(block_size / 2 + 64);

    size_t next_cancel_check = 0;
    for (size_t offset = 0; offset < block_size;) {
        if (offset >= next_cancel_check) {
            throw_if_cancelled(cancel);
            next_cancel_check = offset + kCancelCheckInterval;
        }
        const MatchRange& range = matches.ranges[offset];
        MatchOption best{};
        if (range.count != 0) best = matches.options[range.offset + range.count - 1];

        bool emit_literal = best.length < kMinMatch;
        if (!emit_literal && best.length <= 32 && offset + 1 < block_size) {
            const MatchRange& next_range = matches.ranges[offset + 1];
            if (next_range.count != 0) {
                const MatchOption& next = matches.options[next_range.offset + next_range.count - 1];
                emit_literal = next.length > best.length + 1;
            }
        }

        if (emit_literal) {
            const uint8_t value = data[block_start + offset];
            result.tokens.push_back(Token::literal(value));
            ++result.lit_freq[value];
            ++offset;
        } else {
            const size_t length = std::min<size_t>(best.length, block_size - offset);
            result.tokens.push_back(Token::match(length, best.distance));
            ++result.lit_freq[257u + length_index(length)];
            ++result.dist_freq[distance_index(best.distance)];
            offset += length;
        }
    }

    ++result.lit_freq[256];
    if (std::none_of(result.dist_freq.begin(), result.dist_freq.end(), [](uint32_t value) { return value != 0; }))
        result.dist_freq[0] = 1;
    return result;
}

TokenizedBlock tokenize_block(const uint8_t* data,
                              size_t size,
                              size_t emit_start,
                              int level,
                              MatchLengthFn match_length,
                              const CancellationCallback& cancel) {
    TokenizedBlock out;
    out.tokens.reserve((size - emit_start) / 2 + 64);
    const SearchSettings settings = settings_for_level(level);
    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> previous(kWindowSize, -1);

    auto insert_position = [&](size_t pos) {
        if (pos + 3 > size) return;
        const uint32_t h = hash3(data + pos);
        previous[pos & kWindowMask] = head[h];
        head[h] = static_cast<int32_t>(pos);
    };

    // Prime the hash chains with the preceding 32 KiB so separately encoded
    // chunks can still reference data across the chunk boundary.
    for (size_t pos = 0; pos < emit_start; ++pos) {
        if ((pos % kCancelCheckInterval) == 0) throw_if_cancelled(cancel);
        insert_position(pos);
    }

    size_t pos = emit_start;
    size_t next_cancel_check = pos;
    while (pos < size) {
        if (pos >= next_cancel_check) {
            throw_if_cancelled(cancel);
            next_cancel_check = pos + kCancelCheckInterval;
        }
        Match best{};
        if (pos + 3 <= size) {
            const uint32_t h = hash3(data + pos);
            const int32_t candidate = head[h];
            if (candidate >= 0 && pos + kMinMatch <= size)
                best = find_match(data, size, pos, candidate, previous, settings, match_length);
            previous[pos & kWindowMask] = candidate;
            head[h] = static_cast<int32_t>(pos);
        }

        if (best.length >= kMinMatch && settings.max_lazy != 0 && best.length <= settings.max_lazy && pos + 1 + 3 <= size) {
            const uint32_t next_hash = hash3(data + pos + 1);
            const int32_t next_candidate = head[next_hash];
            if (next_candidate >= 0) {
                const Match next = find_match(data, size, pos + 1, next_candidate, previous, settings, match_length);
                if (next.length > best.length + 1) {
                    out.tokens.push_back(Token::literal(data[pos]));
                    ++out.lit_freq[data[pos]];
                    ++pos;
                    continue;
                }
            }
        }

        if (best.length >= kMinMatch) {
            out.tokens.push_back(Token::match(best.length, best.distance));
            const unsigned li = length_index(best.length);
            const unsigned di = distance_index(best.distance);
            ++out.lit_freq[257u + li];
            ++out.dist_freq[di];

            const size_t end = pos + best.length;
            ++pos; // current position already inserted
            while (pos < end) {
                insert_position(pos);
                ++pos;
            }
        } else {
            out.tokens.push_back(Token::literal(data[pos]));
            ++out.lit_freq[data[pos]];
            ++pos;
        }
    }
    ++out.lit_freq[256]; // end-of-block
    if (std::none_of(out.dist_freq.begin(), out.dist_freq.end(), [](uint32_t v) { return v != 0; })) out.dist_freq[0] = 1;
    return out;
}

void emit_code(BitWriter& writer, const HuffmanTable& table, unsigned symbol) {
    writer.write_bits(table.codes[symbol], table.lengths[symbol]);
}

void emit_match(BitWriter& writer,
                const HuffmanTable& lit_table,
                const HuffmanTable& dist_table,
                size_t length,
                size_t distance) {
    const unsigned li = length_index(length);
    emit_code(writer, lit_table, 257u + li);
    if (kLengthExtra[li] != 0)
        writer.write_bits(static_cast<uint32_t>(length - kLengthBase[li]), kLengthExtra[li]);

    const unsigned di = distance_index(distance);
    emit_code(writer, dist_table, di);
    if (kDistExtra[di] != 0)
        writer.write_bits(static_cast<uint32_t>(distance - kDistBase[di]), kDistExtra[di]);
}

DeflateResult encode_fixed(const TokenizedBlock& block, bool final_block, size_t reserve_bytes,
                           const CancellationCallback& cancel) {
    BitWriter writer(reserve_bytes);
    writer.write_bits(final_block ? 1u : 0u, 1);
    writer.write_bits(1u, 2); // BTYPE=01
    const auto& lit = fixed_literal_table();
    static const HuffmanTable dist = [] {
        HuffmanTable t;
        t.lengths.assign(32, 5);
        t.codes.resize(32);
        for (unsigned i = 0; i < 32; ++i) t.codes[i] = reverse_bits(static_cast<uint16_t>(i), 5);
        return t;
    }();

    for (size_t index = 0; index < block.tokens.size(); ++index) {
        if ((index % kCancelCheckInterval) == 0) throw_if_cancelled(cancel);
        const Token& token = block.tokens[index];
        if (token.is_literal()) emit_code(writer, lit, token.literal_or_length);
        else emit_match(writer, lit, dist, token.length, token.distance);
    }
    emit_code(writer, lit, 256);
    return writer.finish();
}

struct CodeLengthItem {
    uint8_t symbol = 0;
    uint8_t extra_bits = 0;
    uint16_t extra_value = 0;
};

std::vector<CodeLengthItem> encode_code_lengths(const std::vector<uint8_t>& lengths) {
    std::vector<CodeLengthItem> result;
    size_t pos = 0;
    while (pos < lengths.size()) {
        const uint8_t value = lengths[pos];
        size_t run = 1;
        while (pos + run < lengths.size() && lengths[pos + run] == value) ++run;
        const size_t original_run = run;

        if (value == 0) {
            while (run >= 11) {
                const size_t count = std::min<size_t>(run, 138);
                result.push_back({18, 7, static_cast<uint16_t>(count - 11)});
                run -= count;
            }
            if (run >= 3) {
                const size_t count = std::min<size_t>(run, 10);
                result.push_back({17, 3, static_cast<uint16_t>(count - 3)});
                run -= count;
            }
            while (run-- != 0) result.push_back({0, 0, 0});
        } else {
            result.push_back({value, 0, 0});
            --run;
            while (run >= 3) {
                const size_t count = std::min<size_t>(run, 6);
                result.push_back({16, 2, static_cast<uint16_t>(count - 3)});
                run -= count;
            }
            while (run-- != 0) result.push_back({value, 0, 0});
        }
        pos += original_run;
    }
    return result;
}

DeflateResult encode_dynamic(const TokenizedBlock& block, bool final_block, size_t reserve_bytes,
                             const CancellationCallback& cancel) {
    HuffmanTable lit = make_huffman(block.lit_freq, 15);
    HuffmanTable dist = make_huffman(block.dist_freq, 15);

    size_t lit_count = 286;
    while (lit_count > 257 && lit.lengths[lit_count - 1] == 0) --lit_count;
    size_t dist_count = 30;
    while (dist_count > 1 && dist.lengths[dist_count - 1] == 0) --dist_count;

    std::vector<uint8_t> combined_lengths;
    combined_lengths.reserve(lit_count + dist_count);
    combined_lengths.insert(combined_lengths.end(), lit.lengths.begin(), lit.lengths.begin() + static_cast<std::ptrdiff_t>(lit_count));
    combined_lengths.insert(combined_lengths.end(), dist.lengths.begin(), dist.lengths.begin() + static_cast<std::ptrdiff_t>(dist_count));
    const auto encoded_lengths = encode_code_lengths(combined_lengths);

    std::vector<uint32_t> code_length_freq(19, 0);
    for (const auto& item : encoded_lengths) ++code_length_freq[item.symbol];
    HuffmanTable code_length_table = make_huffman(code_length_freq, 7);

    static constexpr std::array<uint8_t, 19> order = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    size_t code_length_count = 19;
    while (code_length_count > 4 && code_length_table.lengths[order[code_length_count - 1]] == 0) --code_length_count;

    BitWriter writer(reserve_bytes);
    writer.write_bits(final_block ? 1u : 0u, 1);
    writer.write_bits(2u, 2); // BTYPE=10
    writer.write_bits(static_cast<uint32_t>(lit_count - 257), 5);
    writer.write_bits(static_cast<uint32_t>(dist_count - 1), 5);
    writer.write_bits(static_cast<uint32_t>(code_length_count - 4), 4);
    for (size_t i = 0; i < code_length_count; ++i)
        writer.write_bits(code_length_table.lengths[order[i]], 3);
    for (const auto& item : encoded_lengths) {
        emit_code(writer, code_length_table, item.symbol);
        if (item.extra_bits != 0) writer.write_bits(item.extra_value, item.extra_bits);
    }

    for (size_t index = 0; index < block.tokens.size(); ++index) {
        if ((index % kCancelCheckInterval) == 0) throw_if_cancelled(cancel);
        const Token& token = block.tokens[index];
        if (token.is_literal()) emit_code(writer, lit, token.literal_or_length);
        else emit_match(writer, lit, dist, token.length, token.distance);
    }
    emit_code(writer, lit, 256);
    return writer.finish();
}

DeflateResult compress_block(const uint8_t* data,
                             size_t size,
                             size_t emit_start,
                             bool final_block,
                             int level,
                             MatchLengthFn match_length,
                             const CancellationCallback& cancel) {
    const TokenizedBlock tokens = tokenize_block(data, size, emit_start, level, match_length, cancel);
    throw_if_cancelled(cancel);
    const size_t emitted_size = size - emit_start;
    DeflateResult fixed = encode_fixed(tokens, final_block,
                                       emitted_size + emitted_size / 32 + 128, cancel);
    throw_if_cancelled(cancel);
    if (level <= 1) return fixed;
    DeflateResult dynamic = encode_dynamic(tokens, final_block,
                                           emitted_size + emitted_size / 64 + 256, cancel);
    return dynamic.bit_count < fixed.bit_count ? std::move(dynamic) : std::move(fixed);
}

// Keep entropy decisions aligned with the largest legal stored DEFLATE block.
// Besides avoiding an extra split, 65,535-byte blocks stay very close to
// common 64 KiB data boundaries, so an incompressible/compressible transition
// is not needlessly encoded with one shared Huffman tree.
constexpr size_t kOptimalBlockSize = 0xFFFFu;
static_assert(kOptimalBlockSize <= 0xFFFFu,
              "Stored DEFLATE blocks are limited to 65,535 bytes");

uint64_t stored_block_bit_count(uint64_t current_bits, size_t size) {
    const unsigned after_header = static_cast<unsigned>((current_bits + 3u) & 7u);
    const unsigned padding = after_header == 0 ? 0 : 8u - after_header;
    return 3u + padding + 32u + static_cast<uint64_t>(size) * 8u;
}

void write_stored_block(BitWriter& writer,
                        const uint8_t* data,
                        size_t size,
                        bool final_block,
                        const CancellationCallback& cancel = {}) {
    if (size > 0xFFFFu) throw std::runtime_error("Internal Deflate stored block is too large");
    writer.write_bits(final_block ? 1u : 0u, 1);
    writer.write_bits(0u, 2); // BTYPE=00
    writer.align_to_byte();
    writer.write_bits(static_cast<uint32_t>(size), 16);
    writer.write_bits(static_cast<uint32_t>(~size) & 0xFFFFu, 16);
    throw_if_cancelled(cancel);
    writer.append_bytes(data, size);
}

void write_alignment_block(BitWriter& writer) {
    write_stored_block(writer, nullptr, 0, false);
}

DeflateResult compress_fast_chunk(const uint8_t* data,
                                  size_t size,
                                  size_t emit_start,
                                  bool final_chunk,
                                  int level,
                                  MatchLengthFn match_length,
                                  const CancellationCallback& cancel) {
    BitWriter writer(size - emit_start + (size - emit_start) / 32 + 512);
    for (size_t block_start = emit_start; block_start < size;) {
        throw_if_cancelled(cancel);
        const size_t block_end = std::min(size, block_start + kOptimalBlockSize);
        const size_t block_size = block_end - block_start;
        const bool final_block = final_chunk && block_end == size;
        const size_t history_size = std::min(block_start, kWindowSize);
        const uint8_t* history = history_size == 0 ? nullptr :
            data + block_start - history_size;

        if (likely_incompressible_block(data + block_start, block_size,
                                        history, history_size)) {
            write_stored_block(writer, data + block_start, block_size,
                               final_block, cancel);
            block_start = block_end;
            continue;
        }

        const uint8_t* block_data = history_size == 0 ? data + block_start : history;
        DeflateResult compressed = compress_block(
            block_data, history_size + block_size, history_size,
            final_block, level, match_length, cancel);
        const uint64_t stored_bits = stored_block_bit_count(
            writer.bit_count(), block_size);
        if (stored_bits <= compressed.bit_count) {
            write_stored_block(writer, data + block_start, block_size,
                               final_block, cancel);
        } else {
            writer.append_stream(compressed.bytes, compressed.bit_count);
        }
        block_start = block_end;
    }

    if (!final_chunk) write_alignment_block(writer);
    return writer.finish();
}

DeflateResult compress_optimal_chunk(const uint8_t* data,
                                     size_t size,
                                     size_t emit_start,
                                     bool final_chunk,
                                     int level,
                                     MatchLengthFn match_length,
                                     const CancellationCallback& cancel) {
    SearchSettings settings = settings_for_level(level);
    settings.nice_length = static_cast<unsigned>(kMaxMatch);
    if (level >= 9) settings.max_chain = 256;
    StreamingMatchFinder finder(data, size, emit_start, settings, match_length, cancel);
    BitWriter writer(size - emit_start + (size - emit_start) / 32 + 512);
    bool matcher_history_dirty = false;

    for (size_t block_start = emit_start; block_start < size;) {
        throw_if_cancelled(cancel);
        const size_t block_end = std::min(size, block_start + kOptimalBlockSize);
        const bool final_block = final_chunk && block_end == size;
        const size_t block_size = block_end - block_start;
        const size_t history_size = std::min(block_start, kWindowSize);
        if (likely_incompressible_block(
                data + block_start, block_size,
                history_size == 0 ? nullptr : data + block_start - history_size,
                history_size)) {
            write_stored_block(writer, data + block_start, block_size,
                               final_block, cancel);
            matcher_history_dirty = true;
            block_start = block_end;
            continue;
        }
        if (matcher_history_dirty) {
            finder.reset_history(block_start, cancel);
            matcher_history_dirty = false;
        }
        const MatchBlock matches = finder.collect(block_start, block_end, cancel);

        const TokenizedBlock seed = seed_block_prices(data, block_start, block_end, matches, cancel);
        TokenizedBlock tokens = parse_block_optimal(data, block_start, block_end,
                                                    matches, dynamic_parse_prices(seed), cancel);
        const unsigned refinement_passes = level >= 9 ? 0u : level >= 7 ? 1u : 0u;
        for (unsigned pass = 0; pass < refinement_passes; ++pass) {
            const ParsePrices prices = dynamic_parse_prices(tokens);
            tokens = parse_block_optimal(data, block_start, block_end, matches, prices, cancel);
        }

        DeflateResult fixed = encode_fixed(tokens, final_block,
                                           block_size + block_size / 32 + 128, cancel);
        throw_if_cancelled(cancel);
        DeflateResult dynamic = encode_dynamic(tokens, final_block,
                                               block_size + block_size / 64 + 256, cancel);
        throw_if_cancelled(cancel);
        DeflateResult* best = dynamic.bit_count < fixed.bit_count ? &dynamic : &fixed;
        const uint64_t stored_bits = stored_block_bit_count(writer.bit_count(), block_size);
        if (stored_bits <= best->bit_count) {
            write_stored_block(writer, data + block_start, block_size, final_block, cancel);
        } else {
            writer.append_stream(best->bytes, best->bit_count);
        }
        block_start = block_end;
    }

    // Separately encoded chunks must start on the same bit boundary. A tiny
    // empty stored block aligns every non-final chunk without resetting the
    // 32 KiB Deflate history used by the next chunk.
    if (!final_chunk) write_alignment_block(writer);
    return writer.finish();
}

} // namespace

DeflateResult deflate(const uint8_t* data,
                      size_t size,
                      const DeflateOptions& options) {
    const size_t chunk_size = std::clamp<size_t>(options.chunk_size, 256u * 1024u, 1024u * 1024u * 1024u);
    const MatchLengthFn match_fn = options.use_avx2 ? match_length_avx2 : match_length_scalar;
    throw_if_cancelled(options.cancel);
    if (size == 0) {
        return compress_block(data, 0, 0, true, std::clamp(options.level, 1, 9),
                              match_fn, options.cancel);
    }
    const size_t chunk_count = (size + chunk_size - 1) / chunk_size;

    BitWriter combined(size + size / 64 + 256);
    for (size_t index = 0; index < chunk_count; ++index) {
        const size_t offset = index * chunk_size;
        const size_t payload_size = offset < size ? std::min(chunk_size, size - offset) : 0;
        const size_t history_size = std::min(offset, kWindowSize);
        const uint8_t* block_data = payload_size ? data + offset - history_size : nullptr;
        const size_t block_size = history_size + payload_size;
        const bool final_block = index + 1 == chunk_count;
        throw_if_cancelled(options.cancel);
        const int level = std::clamp(options.level, 1, 9);
        DeflateResult block;
        if (level == 5) {
            block = compress_fast_chunk(block_data, block_size, history_size,
                                        final_block, level, match_fn,
                                        options.cancel);
        } else if (level >= 6) {
            block = compress_optimal_chunk(block_data, block_size, history_size,
                                           final_block, level, match_fn,
                                           options.cancel);
        } else {
            block = compress_block(block_data, block_size, history_size,
                                   final_block, level, match_fn,
                                   options.cancel);
        }
        combined.append_stream(block.bytes, block.bit_count);
        if (options.progress)
            options.progress(std::min(size, (index + 1) * chunk_size));
    }
    return combined.finish();
}

} // namespace fz
