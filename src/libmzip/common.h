#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fz {

class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Operation cancelled") {}
};

using CancellationCallback = std::function<bool()>;

inline void throw_if_cancelled(const CancellationCallback& cancel) {
    if (cancel && cancel()) throw OperationCancelled();
}

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64(const uint8_t* p) {
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

inline void append_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void append_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

inline void append_u64(std::vector<uint8_t>& out, uint64_t v) {
    append_u32(out, static_cast<uint32_t>(v));
    append_u32(out, static_cast<uint32_t>(v >> 32));
}

inline void write_u16(std::ostream& os, uint16_t v) {
    const uint8_t b[2] = {
        static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)
    };
    os.write(reinterpret_cast<const char*>(b), 2);
}

inline void write_u32(std::ostream& os, uint32_t v) {
    const uint8_t b[4] = {
        static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)
    };
    os.write(reinterpret_cast<const char*>(b), 4);
}

inline void write_u64(std::ostream& os, uint64_t v) {
    write_u32(os, static_cast<uint32_t>(v));
    write_u32(os, static_cast<uint32_t>(v >> 32));
}

inline std::string format_bytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return oss.str();
}

inline double seconds_since(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

} // namespace fz
