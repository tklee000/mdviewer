#include "zip_archive.h"
#include "crc32.h"
#include "crypto.h"
#include "deflate_decoder.h"
#include "deflate_encoder.h"
#include "platform.h"

#include <cctype>
#include <cwchar>
#include <ctime>

namespace fz {
namespace {

constexpr uint32_t kLocalHeaderSig = 0x04034B50u;
constexpr uint32_t kCentralHeaderSig = 0x02014B50u;
constexpr uint32_t kEocdSig = 0x06054B50u;
constexpr uint32_t kZip64EocdSig = 0x06064B50u;
constexpr uint32_t kZip64LocatorSig = 0x07064B50u;
constexpr uint16_t kEncryptedFlag = 1u << 0;
constexpr uint16_t kUtf8Flag = 1u << 11;
constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kMethodDeflate = 8;
constexpr uint16_t kMethodWinZipAes = 99;
constexpr uint16_t kVersion20 = 20;
constexpr uint16_t kVersion45 = 45;
constexpr uint16_t kVersion51 = 51;

void report_progress(const ProgressCallback& callback,
                     ProgressStage stage,
                     uint64_t completed_bytes,
                     uint64_t total_bytes,
                     uint64_t completed_files,
                     uint64_t total_files,
                     const std::string& current_entry = {}) {
    if (!callback) return;
    ProgressInfo info;
    info.stage = stage;
    info.completed_bytes = completed_bytes;
    info.total_bytes = total_bytes;
    info.completed_files = completed_files;
    info.total_files = total_files;
    info.current_entry = current_entry;
    callback(info);
}

struct SourceEntry {
    std::filesystem::path source;
    std::string name;
    bool directory = false;
    uint64_t size = 0;
    uint16_t dos_date = 0;
    uint16_t dos_time = 0;
};

struct CentralEntry {
    std::string name;
    uint16_t method = 0;         // ZIP header method. 99 for WinZip AES.
    uint16_t actual_method = 0;  // Store or Deflate inside AES.
    uint16_t dos_date = 0;
    uint16_t dos_time = 0;
    uint32_t crc32 = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_offset = 0;
    bool directory = false;
    bool encrypted = false;
};

struct PreparedPayload {
    std::unique_ptr<MappedFile> mapped;
    DeflateResult compressed;
    std::vector<uint8_t> encrypted_payload;
    uint32_t crc32 = 0;
    uint16_t actual_method = kMethodStore;
    uint64_t compressed_size = 0;
    bool encrypted = false;
    double seconds = 0.0;
};

uint64_t stream_position(std::ostream& os) {
    const auto pos = os.tellp();
    if (pos == std::streampos(-1)) throw std::runtime_error("Cannot query output position");
    return static_cast<uint64_t>(pos);
}

void write_large(std::ostream& os, const uint8_t* data, uint64_t size,
                 const CancellationCallback& cancel = {}) {
    constexpr uint64_t kMaxWrite = 4ull * 1024ull * 1024ull;
    while (size != 0) {
        throw_if_cancelled(cancel);
        const uint64_t n = std::min(size, kMaxWrite);
        os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!os) throw std::runtime_error("Archive write failed");
        data += n;
        size -= n;
    }
}

std::string normalized_archive_name(const std::filesystem::path& path, bool directory) {
    std::string name = path_to_utf8(path);
    std::replace(name.begin(), name.end(), '\\', '/');
    while (name.rfind("./", 0) == 0) name.erase(0, 2);
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    if (name.empty()) throw std::runtime_error("Empty archive entry name");
    if (directory && name.back() != '/') name.push_back('/');
    if (name.size() > 0xFFFFu) throw std::runtime_error("ZIP entry name is too long: " + name);
    return name;
}

void add_source_entry(std::vector<SourceEntry>& entries,
                      std::unordered_set<std::string>& names,
                      const std::filesystem::path& source,
                      const std::filesystem::path& archive_path,
                      bool directory) {
    SourceEntry item;
    item.source = source;
    item.name = normalized_archive_name(archive_path, directory);
    item.directory = directory;
    if (!directory) {
        std::error_code ec;
        item.size = std::filesystem::file_size(source, ec);
        if (ec) throw std::runtime_error("Cannot read file size: " + path_to_utf8(source));
    }
    const auto dt = file_dos_datetime(source);
    item.dos_date = dt.first;
    item.dos_time = dt.second;
    if (!names.insert(item.name).second) throw std::runtime_error("Duplicate ZIP entry name: " + item.name);
    entries.push_back(std::move(item));
}

bool same_or_descendant_path(const std::filesystem::path& candidate,
                             const std::filesystem::path& root) {
    if (root.empty()) return false;
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(candidate_part->c_str(), root_part->c_str()) != 0) return false;
#else
        if (*candidate_part != *root_part) return false;
#endif
    }
    return true;
}

std::vector<SourceEntry> scan_inputs(const std::filesystem::path& output,
                                     const std::vector<std::filesystem::path>& inputs,
                                     const std::filesystem::path& excluded_root = {},
                                     const std::filesystem::path& excluded_file = {},
                                     const CancellationCallback& cancel = {},
                                     const std::vector<std::filesystem::path>& excluded_paths = {},
                                     const std::filesystem::path& entry_root = {},
                                     DirectoryEntryPolicy directory_policy = DirectoryEntryPolicy::Include) {
    std::vector<SourceEntry> entries;
    std::unordered_set<std::string> names;
    std::error_code ec;
    const auto output_abs = std::filesystem::absolute(output, ec).lexically_normal();
    const auto excluded_abs = excluded_root.empty() ? std::filesystem::path{} :
        std::filesystem::absolute(excluded_root, ec).lexically_normal();
    const auto excluded_file_abs = excluded_file.empty() ? std::filesystem::path{} :
        std::filesystem::absolute(excluded_file, ec).lexically_normal();
    std::vector<std::filesystem::path> excluded_paths_abs;
    excluded_paths_abs.reserve(excluded_paths.size());
    for (const auto& excluded : excluded_paths) {
        std::error_code excluded_error;
        const auto absolute = std::filesystem::absolute(excluded, excluded_error).lexically_normal();
        if (!excluded_error && !absolute.empty()) excluded_paths_abs.push_back(absolute);
    }
    const auto user_excluded = [&](const std::filesystem::path& path) {
        return std::any_of(excluded_paths_abs.begin(), excluded_paths_abs.end(),
            [&](const std::filesystem::path& excluded) {
                return same_or_descendant_path(path, excluded);
            });
    };

    std::filesystem::path entry_root_abs;
    std::filesystem::path entry_root_resolved;
    if (!entry_root.empty()) {
        entry_root_abs = std::filesystem::absolute(entry_root, ec).lexically_normal();
        if (ec) throw std::runtime_error("Cannot resolve ZIP entry root: " + path_to_utf8(entry_root));
        const auto root_status = std::filesystem::status(entry_root_abs, ec);
        if (ec || !std::filesystem::is_directory(root_status))
            throw std::runtime_error("ZIP entry root is not a directory: " + path_to_utf8(entry_root));
        entry_root_resolved = std::filesystem::canonical(entry_root_abs, ec).lexically_normal();
        if (ec) throw std::runtime_error("Cannot resolve ZIP entry root: " + path_to_utf8(entry_root));
    }

    const auto require_inside_entry_root = [&](const std::filesystem::path& path) {
        if (entry_root_abs.empty()) return;
        if (!same_or_descendant_path(path, entry_root_abs))
            throw std::runtime_error("ZIP input is outside the entry root: " + path_to_utf8(path));
        std::error_code resolved_error;
        const auto resolved = std::filesystem::canonical(path, resolved_error).lexically_normal();
        if (resolved_error)
            throw std::runtime_error("Cannot resolve ZIP input under the entry root: " + path_to_utf8(path));
        if (!same_or_descendant_path(resolved, entry_root_resolved))
            throw std::runtime_error("ZIP input resolves outside the entry root: " + path_to_utf8(path));
    };

    bool accepted_directory_input = false;

    for (const auto& raw : inputs) {
        throw_if_cancelled(cancel);
        const auto input = std::filesystem::absolute(raw, ec).lexically_normal();
        if (ec || !std::filesystem::exists(input))
            throw std::runtime_error("Input does not exist: " + path_to_utf8(raw));
        if (user_excluded(input)) continue;

        require_inside_entry_root(input);

        if (std::filesystem::is_regular_file(input)) {
            if (input != output_abs && input != excluded_file_abs) {
                const auto archive_path = entry_root_abs.empty() ? input.filename() :
                    input.lexically_relative(entry_root_abs);
                add_source_entry(entries, names, input, archive_path, false);
            }
            continue;
        }
        if (!std::filesystem::is_directory(input)) {
            std::cerr << "Skip unsupported input: " << path_to_utf8(input) << '\n';
            continue;
        }

        accepted_directory_input = true;

        const auto base = entry_root_abs.empty() ? input.parent_path() : entry_root_abs;
        const auto input_relative = input.lexically_relative(base);
        if (directory_policy == DirectoryEntryPolicy::Include && !input_relative.empty())
            add_source_entry(entries, names, input, input_relative, true);

        std::filesystem::recursive_directory_iterator it(
            input, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            throw_if_cancelled(cancel);
            if (ec) {
                std::cerr << "Skip unreadable path under " << path_to_utf8(input) << '\n';
                ec.clear();
                continue;
            }
            const auto p = it->path().lexically_normal();
            if (p == output_abs || p == excluded_file_abs) continue;
            if (user_excluded(p)) {
                it.disable_recursion_pending();
                continue;
            }
            if (!excluded_abs.empty() && same_or_descendant_path(p, excluded_abs)) {
                it.disable_recursion_pending();
                continue;
            }

            const auto status = it->symlink_status(ec);
            if (ec) { ec.clear(); continue; }
            if (std::filesystem::is_symlink(status)) {
                if (std::filesystem::is_directory(status)) it.disable_recursion_pending();
                std::cerr << "Skip symbolic link: " << path_to_utf8(p) << '\n';
                continue;
            }
            require_inside_entry_root(p);
            const auto rel = p.lexically_relative(base);
            if (std::filesystem::is_directory(status)) {
                if (directory_policy == DirectoryEntryPolicy::Include)
                    add_source_entry(entries, names, p, rel, true);
            }
            else if (std::filesystem::is_regular_file(status)) add_source_entry(entries, names, p, rel, false);
        }
    }

    const bool empty_archive_is_representable = accepted_directory_input &&
        (directory_policy == DirectoryEntryPolicy::Omit || !entry_root_abs.empty());
    if (entries.empty() && !empty_archive_is_representable)
        throw std::runtime_error("No input files to archive");
    return entries;
}

void append_zip64_extra(std::vector<uint8_t>& extra,
                        bool include_uncompressed, uint64_t uncompressed,
                        bool include_compressed, uint64_t compressed,
                        bool include_offset, uint64_t offset) {
    std::vector<uint8_t> payload;
    if (include_uncompressed) append_u64(payload, uncompressed);
    if (include_compressed) append_u64(payload, compressed);
    if (include_offset) append_u64(payload, offset);
    if (payload.empty()) return;
    append_u16(extra, 0x0001);
    append_u16(extra, static_cast<uint16_t>(payload.size()));
    extra.insert(extra.end(), payload.begin(), payload.end());
}

void append_aes_extra(std::vector<uint8_t>& extra, uint16_t actual_method) {
    append_u16(extra, 0x9901); // WinZip AES extra field
    append_u16(extra, 7);
    append_u16(extra, 2);      // AE-2
    extra.push_back('A');
    extra.push_back('E');
    extra.push_back(3);        // AES-256
    append_u16(extra, actual_method);
}

std::vector<uint8_t> make_local_extra(const CentralEntry& e, bool zip64) {
    std::vector<uint8_t> extra;
    if (zip64) append_zip64_extra(extra, true, e.uncompressed_size, true, e.compressed_size, false, 0);
    if (e.encrypted) append_aes_extra(extra, e.actual_method);
    return extra;
}

std::vector<uint8_t> make_central_extra(const CentralEntry& e,
                                        bool need_uncompressed,
                                        bool need_compressed,
                                        bool need_offset) {
    std::vector<uint8_t> extra;
    append_zip64_extra(extra,
                       need_uncompressed, e.uncompressed_size,
                       need_compressed, e.compressed_size,
                       need_offset, e.local_offset);
    if (e.encrypted) append_aes_extra(extra, e.actual_method);
    return extra;
}

uint16_t flags_for(const CentralEntry& e) {
    return static_cast<uint16_t>(kUtf8Flag | (e.encrypted ? kEncryptedFlag : 0));
}

void write_local_header(std::ostream& os, const CentralEntry& e) {
    const bool zip64 = e.compressed_size >= 0xFFFFFFFFull || e.uncompressed_size >= 0xFFFFFFFFull;
    const auto extra = make_local_extra(e, zip64);
    const uint16_t version = e.encrypted ? kVersion51 : (zip64 ? kVersion45 : kVersion20);

    write_u32(os, kLocalHeaderSig);
    write_u16(os, version);
    write_u16(os, flags_for(e));
    write_u16(os, e.method);
    write_u16(os, e.dos_time);
    write_u16(os, e.dos_date);
    write_u32(os, e.crc32);
    write_u32(os, zip64 ? 0xFFFFFFFFu : static_cast<uint32_t>(e.compressed_size));
    write_u32(os, zip64 ? 0xFFFFFFFFu : static_cast<uint32_t>(e.uncompressed_size));
    write_u16(os, static_cast<uint16_t>(e.name.size()));
    write_u16(os, static_cast<uint16_t>(extra.size()));
    os.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
    if (!extra.empty()) write_large(os, extra.data(), extra.size());
}

void write_central_header(std::ostream& os, const CentralEntry& e) {
    const bool need_uncompressed = e.uncompressed_size >= 0xFFFFFFFFull;
    const bool need_compressed = e.compressed_size >= 0xFFFFFFFFull;
    const bool need_offset = e.local_offset >= 0xFFFFFFFFull;
    const bool zip64 = need_uncompressed || need_compressed || need_offset;
    const auto extra = make_central_extra(e, need_uncompressed, need_compressed, need_offset);
    const uint16_t version = e.encrypted ? kVersion51 : (zip64 ? kVersion45 : kVersion20);

    write_u32(os, kCentralHeaderSig);
    write_u16(os, e.encrypted ? kVersion51 : kVersion45); // creator version, DOS/Windows host
    write_u16(os, version);
    write_u16(os, flags_for(e));
    write_u16(os, e.method);
    write_u16(os, e.dos_time);
    write_u16(os, e.dos_date);
    write_u32(os, e.crc32);
    write_u32(os, need_compressed ? 0xFFFFFFFFu : static_cast<uint32_t>(e.compressed_size));
    write_u32(os, need_uncompressed ? 0xFFFFFFFFu : static_cast<uint32_t>(e.uncompressed_size));
    write_u16(os, static_cast<uint16_t>(e.name.size()));
    write_u16(os, static_cast<uint16_t>(extra.size()));
    write_u16(os, 0); // comment
    write_u16(os, 0); // disk start
    write_u16(os, 0); // internal attributes
    write_u32(os, e.directory ? 0x10u : 0u); // DOS directory attribute
    write_u32(os, need_offset ? 0xFFFFFFFFu : static_cast<uint32_t>(e.local_offset));
    os.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
    if (!extra.empty()) write_large(os, extra.data(), extra.size());
}

void write_archive_end(std::ostream& os,
                       uint64_t central_offset,
                       uint64_t central_size,
                       uint64_t entry_count) {
    const bool zip64 = central_offset >= 0xFFFFFFFFull || central_size >= 0xFFFFFFFFull || entry_count >= 0xFFFFull;
    if (zip64) {
        const uint64_t zip64_eocd_offset = stream_position(os);
        write_u32(os, kZip64EocdSig);
        write_u64(os, 44);
        write_u16(os, kVersion45);
        write_u16(os, kVersion45);
        write_u32(os, 0);
        write_u32(os, 0);
        write_u64(os, entry_count);
        write_u64(os, entry_count);
        write_u64(os, central_size);
        write_u64(os, central_offset);

        write_u32(os, kZip64LocatorSig);
        write_u32(os, 0);
        write_u64(os, zip64_eocd_offset);
        write_u32(os, 1);
    }

    write_u32(os, kEocdSig);
    write_u16(os, 0);
    write_u16(os, 0);
    write_u16(os, zip64 ? 0xFFFFu : static_cast<uint16_t>(entry_count));
    write_u16(os, zip64 ? 0xFFFFu : static_cast<uint16_t>(entry_count));
    write_u32(os, zip64 ? 0xFFFFFFFFu : static_cast<uint32_t>(central_size));
    write_u32(os, zip64 ? 0xFFFFFFFFu : static_cast<uint32_t>(central_offset));
    write_u16(os, 0);
}

std::vector<uint8_t> read_file_tail(std::ifstream& is, uint64_t file_size, size_t max_size) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(file_size, max_size));
    std::vector<uint8_t> data(n);
    is.seekg(static_cast<std::streamoff>(file_size - n), std::ios::beg);
    is.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n));
    if (!is) throw std::runtime_error("Cannot read ZIP tail");
    return data;
}

bool parse_zip64_extra(const uint8_t* extra, size_t extra_size,
                       uint32_t comp32, uint32_t uncomp32, uint32_t offset32,
                       uint64_t& comp, uint64_t& uncomp, uint64_t& offset) {
    const bool required = comp32 == 0xFFFFFFFFu || uncomp32 == 0xFFFFFFFFu ||
                          offset32 == 0xFFFFFFFFu;
    size_t p = 0;
    while (p + 4 <= extra_size) {
        const uint16_t id = read_u16(extra + p);
        const uint16_t len = read_u16(extra + p + 2);
        p += 4;
        if (p + len > extra_size) break;
        if (id == 0x0001) {
            size_t q = p;
            if (uncomp32 == 0xFFFFFFFFu) {
                if (q + 8 > p + len) return false;
                uncomp = read_u64(extra + q); q += 8;
            }
            if (comp32 == 0xFFFFFFFFu) {
                if (q + 8 > p + len) return false;
                comp = read_u64(extra + q); q += 8;
            }
            if (offset32 == 0xFFFFFFFFu) {
                if (q + 8 > p + len) return false;
                offset = read_u64(extra + q); q += 8;
            }
            return true;
        }
        p += len;
    }
    return !required;
}

bool parse_aes_extra(const uint8_t* extra, size_t extra_size,
                     uint16_t& actual_method, uint8_t& strength, uint16_t& aes_version) {
    size_t p = 0;
    while (p + 4 <= extra_size) {
        const uint16_t id = read_u16(extra + p);
        const uint16_t len = read_u16(extra + p + 2);
        p += 4;
        if (p + len > extra_size) break;
        if (id == 0x9901 && len >= 7 && extra[p + 2] == 'A' && extra[p + 3] == 'E') {
            aes_version = read_u16(extra + p);
            strength = extra[p + 4];
            actual_method = read_u16(extra + p + 5);
            return true;
        }
        p += len;
    }
    return false;
}

void append_utf8_codepoint(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        output.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

bool valid_utf8(const std::string& text) {
    size_t position = 0;
    while (position < text.size()) {
        const uint8_t first = static_cast<uint8_t>(text[position++]);
        if (first <= 0x7Fu) continue;
        unsigned continuation_count = 0;
        uint32_t codepoint = 0;
        if (first >= 0xC2u && first <= 0xDFu) {
            continuation_count = 1;
            codepoint = first & 0x1Fu;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            continuation_count = 2;
            codepoint = first & 0x0Fu;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            continuation_count = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (continuation_count > text.size() - position) return false;
        for (unsigned i = 0; i < continuation_count; ++i) {
            const uint8_t next = static_cast<uint8_t>(text[position++]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3Fu);
        }
        if ((continuation_count == 2 && codepoint < 0x800u) ||
            (continuation_count == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return false;
    }
    return true;
}

std::string cp437_to_utf8(const std::string& text) {
    static constexpr uint16_t kCp437High[128] = {
        0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
        0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
        0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
        0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
        0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
        0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
        0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
        0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
        0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
        0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
        0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
        0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
        0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
        0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
        0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
    };
    std::string decoded;
    decoded.reserve(text.size());
    for (const unsigned char byte : text) {
        append_utf8_codepoint(decoded, byte < 0x80u ? byte : kCp437High[byte - 0x80u]);
    }
    return decoded;
}

bool parse_unicode_path_extra(const uint8_t* extra, size_t size,
                              const std::string& raw_name, std::string& decoded) {
    size_t position = 0;
    while (position + 4 <= size) {
        const uint16_t tag = read_u16(extra + position);
        const uint16_t length = read_u16(extra + position + 2);
        position += 4;
        if (length > size - position) return false;
        if (tag == 0x7075u && length >= 5 && extra[position] == 1 &&
            read_u32(extra + position + 1) ==
                Crc32::compute(reinterpret_cast<const uint8_t*>(raw_name.data()), raw_name.size())) {
            std::string candidate(reinterpret_cast<const char*>(extra + position + 5), length - 5);
            if (!candidate.empty() && valid_utf8(candidate)) {
                decoded = std::move(candidate);
                return true;
            }
        }
        position += length;
    }
    return false;
}

std::string decode_zip_name(const std::string& raw_name, uint16_t flags,
                            const uint8_t* extra, size_t extra_size) {
    if ((flags & kUtf8Flag) != 0) {
        if (!valid_utf8(raw_name)) throw std::runtime_error("ZIP entry name is not valid UTF-8");
        return raw_name;
    }
    std::string unicode_name;
    if (parse_unicode_path_extra(extra, extra_size, raw_name, unicode_name)) return unicode_name;
    return cp437_to_utf8(raw_name);
}

std::string method_label(uint16_t method, uint16_t actual_method, bool aes, uint8_t strength) {
    const char* base = actual_method == kMethodStore ? "Store" :
                       actual_method == kMethodDeflate ? "Deflate" : "Other";
    if (!aes) return base;
    std::ostringstream oss;
    oss << "AES" << (strength == 1 ? "128" : strength == 2 ? "192" : "256")
        << '-' << (actual_method == kMethodStore ? "S" : actual_method == kMethodDeflate ? "D" : "?");
    (void)method;
    return oss.str();
}

struct ExtractEntry {
    std::string name;
    std::string raw_name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint16_t actual_method = 0;
    uint16_t aes_version = 0;
    uint8_t aes_strength = 0;
    uint16_t dos_date = 0;
    uint16_t dos_time = 0;
    uint32_t crc32 = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_offset = 0;
    uint64_t data_offset = 0;
    bool directory = false;
    bool aes = false;
};

struct ParsedArchive {
    std::vector<ExtractEntry> entries;
    uint64_t central_offset = 0;
    uint64_t central_size = 0;
};

const uint8_t* checked_bytes(const uint8_t* archive_data,
                             uint64_t archive_size,
                             uint64_t offset,
                             uint64_t length,
                             const char* description) {
    if (offset > archive_size || length > archive_size - offset ||
        offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(std::string("Truncated or invalid ") + description);
    }
    return archive_data + static_cast<size_t>(offset);
}

ParsedArchive parse_archive_directory(const uint8_t* data, uint64_t file_size) {
    if (file_size < 22 || file_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("Not a valid ZIP file");
    const size_t size = static_cast<size_t>(file_size);
    const size_t search_start = size - std::min<size_t>(size, 22u + 0xFFFFu + 20u);
    size_t eocd_pos = std::string::npos;
    for (size_t pos = size - 22 + 1; pos-- > search_start;) {
        if (read_u32(data + pos) != kEocdSig) continue;
        const uint16_t comment_length = read_u16(data + pos + 20);
        if (comment_length <= size - pos - 22 && pos + 22u + comment_length == size) {
            eocd_pos = pos;
            break;
        }
    }
    if (eocd_pos == std::string::npos) throw std::runtime_error("ZIP end record not found");

    const uint16_t disk_number = read_u16(data + eocd_pos + 4);
    const uint16_t central_disk = read_u16(data + eocd_pos + 6);
    const uint16_t disk_count = read_u16(data + eocd_pos + 8);
    uint64_t count = read_u16(data + eocd_pos + 10);
    uint64_t central_size = read_u32(data + eocd_pos + 12);
    uint64_t central_offset = read_u32(data + eocd_pos + 16);
    if (disk_number != 0 || central_disk != 0 ||
        (disk_count != count && disk_count != 0xFFFFu)) {
        throw std::runtime_error("Multi-disk ZIP archives are not supported");
    }

    uint64_t central_limit = eocd_pos;
    if (count == 0xFFFFu || central_size == 0xFFFFFFFFu || central_offset == 0xFFFFFFFFu) {
        if (eocd_pos < 20) throw std::runtime_error("ZIP64 locator missing");
        const uint8_t* locator = data + eocd_pos - 20;
        if (read_u32(locator) != kZip64LocatorSig || read_u32(locator + 4) != 0 ||
            read_u32(locator + 16) != 1) {
            throw std::runtime_error("ZIP64 locator invalid or multi-disk ZIP64 archive");
        }
        const uint64_t zip64_offset = read_u64(locator + 8);
        const uint8_t* zip64 = checked_bytes(data, file_size, zip64_offset, 56, "ZIP64 end record");
        if (read_u32(zip64) != kZip64EocdSig || read_u64(zip64 + 4) < 44)
            throw std::runtime_error("ZIP64 end record invalid");
        const uint64_t zip64_body_size = read_u64(zip64 + 4);
        const uint64_t locator_offset = eocd_pos - 20;
        if (zip64_offset > locator_offset || locator_offset - zip64_offset < 12 ||
            zip64_body_size > locator_offset - zip64_offset - 12)
            throw std::runtime_error("Truncated ZIP64 end record");
        if (read_u32(zip64 + 16) != 0 || read_u32(zip64 + 20) != 0 ||
            read_u64(zip64 + 24) != read_u64(zip64 + 32)) {
            throw std::runtime_error("Multi-disk ZIP64 archives are not supported");
        }
        count = read_u64(zip64 + 32);
        central_size = read_u64(zip64 + 40);
        central_offset = read_u64(zip64 + 48);
        central_limit = zip64_offset;
    }

    if (central_offset > central_limit || central_size > central_limit - central_offset)
        throw std::runtime_error("Invalid ZIP central directory bounds");
    if (count != 0 && (central_size < 46 || count > central_size / 46))
        throw std::runtime_error("Invalid ZIP central directory entry count");

    ParsedArchive parsed;
    parsed.central_offset = central_offset;
    parsed.central_size = central_size;
    parsed.entries.reserve(static_cast<size_t>(count));
    uint64_t position = central_offset;
    const uint64_t central_end = central_offset + central_size;
    for (uint64_t index = 0; index < count; ++index) {
        if (position > central_end || 46 > central_end - position)
            throw std::runtime_error("Truncated ZIP central directory entry");
        const uint8_t* fixed = checked_bytes(data, file_size, position, 46, "central directory");
        if (read_u32(fixed) != kCentralHeaderSig)
            throw std::runtime_error("Invalid ZIP central directory signature");
        const uint16_t name_length = read_u16(fixed + 28);
        const uint16_t extra_length = read_u16(fixed + 30);
        const uint16_t comment_length = read_u16(fixed + 32);
        const uint64_t record_size = 46ull + name_length + extra_length + comment_length;
        if (position > central_end || record_size > central_end - position)
            throw std::runtime_error("Truncated ZIP central directory entry");

        const uint8_t* name_data = fixed + 46;
        const uint8_t* extra = name_data + name_length;
        ExtractEntry entry;
        entry.raw_name.assign(reinterpret_cast<const char*>(name_data), name_length);
        entry.flags = read_u16(fixed + 8);
        entry.method = read_u16(fixed + 10);
        entry.actual_method = entry.method;
        entry.dos_time = read_u16(fixed + 12);
        entry.dos_date = read_u16(fixed + 14);
        entry.crc32 = read_u32(fixed + 16);
        const uint32_t compressed32 = read_u32(fixed + 20);
        const uint32_t uncompressed32 = read_u32(fixed + 24);
        const uint32_t offset32 = read_u32(fixed + 42);
        entry.compressed_size = compressed32;
        entry.uncompressed_size = uncompressed32;
        entry.local_offset = offset32;
        if (!parse_zip64_extra(extra, extra_length, compressed32, uncompressed32, offset32,
                               entry.compressed_size, entry.uncompressed_size, entry.local_offset)) {
            throw std::runtime_error("Invalid or missing ZIP64 entry extra field");
        }
        if (read_u16(fixed + 34) != 0)
            throw std::runtime_error("Multi-disk ZIP entries are not supported");

        entry.name = decode_zip_name(entry.raw_name, entry.flags, extra, extra_length);

        const uint32_t external_attributes = read_u32(fixed + 38);
        const bool unix_directory = ((external_attributes >> 16) & 0xF000u) == 0x4000u;
        entry.directory = (!entry.name.empty() &&
                           (entry.name.back() == '/' || entry.name.back() == '\\')) ||
                          (external_attributes & 0x10u) != 0 || unix_directory;
        entry.aes = entry.method == kMethodWinZipAes &&
            parse_aes_extra(extra, extra_length, entry.actual_method,
                            entry.aes_strength, entry.aes_version);
        parsed.entries.push_back(std::move(entry));
        position += record_size;
    }
    if (position != central_end)
        throw std::runtime_error("Unsupported data at the end of the ZIP central directory");

    for (auto& entry : parsed.entries) {
        if (entry.name.empty() || entry.name.find('\0') != std::string::npos)
            throw std::runtime_error("ZIP entry has an invalid empty or NUL-containing name");
        const bool encrypted = (entry.flags & kEncryptedFlag) != 0;
        if (encrypted && !entry.aes)
            throw std::runtime_error("Traditional ZIP encryption is not supported: " + entry.name);
        if (entry.aes && !encrypted)
            throw std::runtime_error("Invalid WinZip AES flags: " + entry.name);
        if (entry.method == kMethodWinZipAes && !entry.aes)
            throw std::runtime_error("Invalid WinZip AES extra field: " + entry.name);
        if (entry.aes && (entry.aes_strength != 3 ||
                          (entry.aes_version != 1 && entry.aes_version != 2))) {
            throw std::runtime_error("Only WinZip AES-256 AE-1/AE-2 entries are supported: " + entry.name);
        }
        if (entry.actual_method != kMethodStore && entry.actual_method != kMethodDeflate)
            throw std::runtime_error("Unsupported ZIP compression method for: " + entry.name);
        if (entry.directory && (entry.compressed_size != 0 || entry.uncompressed_size != 0))
            throw std::runtime_error("ZIP directory entry unexpectedly contains data: " + entry.name);

        const uint8_t* local = checked_bytes(data, file_size, entry.local_offset, 30, "local ZIP header");
        if (read_u32(local) != kLocalHeaderSig)
            throw std::runtime_error("Invalid local ZIP header for: " + entry.name);
        if (read_u16(local + 8) != entry.method)
            throw std::runtime_error("ZIP local/central method mismatch for: " + entry.name);
        const uint16_t local_name_length = read_u16(local + 26);
        const uint16_t local_extra_length = read_u16(local + 28);
        const uint64_t local_header_size = 30ull + local_name_length + local_extra_length;
        checked_bytes(data, file_size, entry.local_offset, local_header_size, "local ZIP header fields");
        const uint64_t data_offset = entry.local_offset + local_header_size;
        const uint8_t* local_name = checked_bytes(data, file_size, entry.local_offset + 30,
                                                  local_name_length, "local ZIP name");
        if (local_name_length != entry.raw_name.size() ||
            std::memcmp(local_name, entry.raw_name.data(), local_name_length) != 0) {
            throw std::runtime_error("ZIP local/central name mismatch for: " + entry.name);
        }
        checked_bytes(data, file_size, data_offset, entry.compressed_size, "ZIP entry payload");
        if (data_offset > central_offset || entry.compressed_size > central_offset - data_offset)
            throw std::runtime_error("ZIP entry payload overlaps the central directory: " + entry.name);
        entry.data_offset = data_offset;
    }
    return parsed;
}

std::string lowercase_ascii(std::string text) {
    for (char& ch : text) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value >= 'A' && value <= 'Z') ch = static_cast<char>(value - 'A' + 'a');
    }
    return text;
}

bool valid_windows_component(const std::string& component) {
#ifdef _WIN32
    if (component.empty() || component.back() == '.' || component.back() == ' ') return false;
    for (const unsigned char ch : component) {
        if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '|' || ch == '?' || ch == '*') return false;
    }
    std::string base = component.substr(0, component.find('.'));
    base = lowercase_ascii(base);
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return false;
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
        base[3] >= '1' && base[3] <= '9') return false;
#else
    (void)component;
#endif
    return true;
}

std::filesystem::path safe_relative_path(const std::string& archive_name) {
    std::string normalized = archive_name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty() || normalized.front() == '/')
        throw std::runtime_error("Unsafe absolute ZIP entry path: " + archive_name);

    std::filesystem::path relative;
    size_t begin = 0;
    while (begin <= normalized.size()) {
        const size_t slash = normalized.find('/', begin);
        const size_t end = slash == std::string::npos ? normalized.size() : slash;
        const std::string component = normalized.substr(begin, end - begin);
        if (component == "..")
            throw std::runtime_error("Unsafe parent traversal in ZIP entry: " + archive_name);
        if (!component.empty() && component != ".") {
            if (!valid_windows_component(component))
                throw std::runtime_error("Unsafe Windows ZIP entry name: " + archive_name);
            const auto path_component = utf8_to_path(component);
            if (path_component.empty())
                throw std::runtime_error("ZIP entry name is not valid UTF-8: " + archive_name);
            relative /= path_component;
        }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory())
        throw std::runtime_error("Unsafe ZIP entry path: " + archive_name);
    auto normalized_path = relative.lexically_normal();
    // A directory entry commonly ends in '/'. MSVC's weakly_canonical can
    // report ERROR_FILE_NOT_FOUND for that spelling until the directory is
    // created, even though the same path without the trailing separator is a
    // valid safe destination. Keep the normalized relative path separator-free.
    if (normalized_path.filename().empty()) normalized_path = normalized_path.parent_path();
    if (normalized_path.empty())
        throw std::runtime_error("Unsafe ZIP entry path: " + archive_name);
    return normalized_path;
}

std::string path_key(const std::filesystem::path& path) {
    std::string key = path_to_utf8(path.lexically_normal());
    std::replace(key.begin(), key.end(), '\\', '/');
#ifdef _WIN32
    key = lowercase_ascii(std::move(key));
#endif
    return key;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end()) return false;
        std::string left = path_to_utf8(*root_part);
        std::string right = path_to_utf8(*candidate_part);
#ifdef _WIN32
        left = lowercase_ascii(std::move(left));
        right = lowercase_ascii(std::move(right));
#endif
        if (left != right) return false;
    }
    return true;
}

std::filesystem::path safe_output_path(const std::filesystem::path& root,
                                       const std::filesystem::path& relative,
                                       const std::string& entry_name) {
    // Canonicalize the ordinary path before adding the Windows extended-length
    // prefix. Some Windows filesystem implementations return a prefixed and
    // an unprefixed spelling for a not-yet-created leaf; comparing those two
    // spellings would incorrectly reject valid directory entries.
    const auto lexical = (root / relative).lexically_normal();
    if (!path_is_within(root, lexical))
        throw std::runtime_error("ZIP entry escapes the output directory: " + entry_name);

    // There is no leaf to canonicalize before extraction creates it. On
    // Windows, weakly_canonical may report ACCESS_DENIED for such a path in
    // the system temporary directory even though its existing parents are
    // safe. Existing targets are canonicalized; missing leaves are checked by
    // the parent/target revalidation immediately before installation.
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(lexical, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("Cannot inspect ZIP output path " +
                                 path_to_utf8(lexical) + ": " + status_error.message());
    if (!status_error && status.type() != std::filesystem::file_type::not_found) {
        std::error_code canonical_error;
        const auto candidate = std::filesystem::weakly_canonical(lexical, canonical_error);
        if (canonical_error)
            throw std::runtime_error("Cannot canonicalize ZIP output path " +
                                     path_to_utf8(lexical) + ": " + canonical_error.message());
        if (!path_is_within(root, candidate))
            throw std::runtime_error("ZIP entry escapes the output directory: " +
                                     entry_name + " (root=" + path_to_utf8(root) +
                                     ", candidate=" + path_to_utf8(candidate) + ")");
    }
    return filesystem_access_path(lexical);
}

std::filesystem::file_status extraction_path_status(const std::filesystem::path& path,
                                                     std::error_code& error) {
    error.clear();
    auto status = std::filesystem::symlink_status(path, error);
    if (!error) return status;
    if (status.type() == std::filesystem::file_type::not_found ||
        error == std::errc::no_such_file_or_directory) {
        error.clear();
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    std::error_code probe_error;
    const bool exists = std::filesystem::exists(path, probe_error);
    if (!exists && !probe_error) {
        error.clear();
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    return status;
}

void set_dos_timestamp(const std::filesystem::path& path, uint16_t date, uint16_t time) noexcept {
    std::tm local{};
    local.tm_year = static_cast<int>((date >> 9) & 0x7Fu) + 80;
    local.tm_mon = static_cast<int>((date >> 5) & 0x0Fu) - 1;
    local.tm_mday = static_cast<int>(date & 0x1Fu);
    local.tm_hour = static_cast<int>((time >> 11) & 0x1Fu);
    local.tm_min = static_cast<int>((time >> 5) & 0x3Fu);
    local.tm_sec = static_cast<int>(time & 0x1Fu) * 2;
    local.tm_isdst = -1;
    if (local.tm_mon < 0 || local.tm_mday == 0) return;
    const std::time_t value = std::mktime(&local);
    if (value == static_cast<std::time_t>(-1)) return;
    const auto system_time = std::chrono::system_clock::from_time_t(value);
    const auto file_time = std::filesystem::file_time_type::clock::now() +
        (system_time - std::chrono::system_clock::now());
    std::error_code ec;
    std::filesystem::last_write_time(path, file_time, ec);
}

struct PreparedEntry {
    const ExtractEntry* entry = nullptr;
    std::filesystem::path relative;
    std::filesystem::path target;
};

std::filesystem::path make_temporary_path(const std::filesystem::path& target) {
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        std::filesystem::path temporary = target;
        temporary += utf8_to_path(".mzip.tmp." + std::to_string(nonce) + "." + std::to_string(attempt));
        std::error_code ec;
        if (!std::filesystem::exists(temporary, ec) && !ec) return temporary;
    }
    throw std::runtime_error("Cannot allocate a temporary extraction path for: " + path_to_utf8(target));
}

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::filesystem::path path, bool active = true)
        : path_(std::move(path)), active_(active) {}
    ~TemporaryFileGuard() {
        if (!active_) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    void activate() noexcept { active_ = true; }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

InflateResult write_stored_data(const uint8_t* data, uint64_t size, std::ostream& output,
                                const std::function<void(uint64_t)>& progress = {}) {
    Crc32 crc;
    uint64_t remaining = size;
    uint64_t completed = 0;
    while (remaining != 0) {
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(remaining, 64u * 1024u));
        crc.update(data, amount);
        output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(amount));
        if (!output) throw std::runtime_error("Cannot write extracted file");
        data += amount;
        remaining -= amount;
        completed += amount;
        if (progress) progress(completed);
    }
    return {size, crc.value()};
}

struct VerificationIssue {
    std::string entry;
    std::string problem;
    std::filesystem::path source;
    std::filesystem::path extracted;
    uint64_t source_size = 0;
    uint64_t extracted_size = 0;
    uint64_t first_difference = 0;
    uint32_t source_crc32 = 0;
    uint32_t extracted_crc32 = 0;
    int source_byte = -1;
    int extracted_byte = -1;
    bool has_sizes = false;
    bool has_difference = false;
    bool has_crc32 = false;
};

struct VerificationResult {
    uint64_t matching_files = 0;
    uint64_t matching_bytes = 0;
    size_t issue_count = 0;
    size_t omitted_issues = 0;
    std::vector<VerificationIssue> issues;
};

void add_verification_issue(VerificationResult& result, VerificationIssue issue) {
    constexpr size_t kMaxReportedIssues = 1000;
    ++result.issue_count;
    if (result.issues.size() < kMaxReportedIssues) result.issues.push_back(std::move(issue));
    else ++result.omitted_issues;
}

std::filesystem::path create_verification_directory() {
    std::error_code ec;
    const auto temp_root = std::filesystem::temp_directory_path(ec);
    if (ec || !std::filesystem::is_directory(temp_root))
        throw std::runtime_error("Cannot access the system temporary directory");

    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const auto candidate = temp_root / utf8_to_path(
            "mzip_verify_" + std::to_string(nonce) + "_" + std::to_string(attempt));
        ec.clear();
        if (std::filesystem::create_directory(candidate, ec)) return candidate;
        if (ec && ec != std::errc::file_exists)
            throw std::runtime_error("Cannot create verification directory: " + path_to_utf8(candidate));
    }
    throw std::runtime_error("Cannot allocate a unique verification directory");
}

std::string verification_timestamp(bool filename_safe) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream text;
    text << std::put_time(&local, filename_safe ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S");
    return text.str();
}

std::filesystem::path choose_verification_report_path(const std::filesystem::path& archive) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(archive, ec);
    if (ec) absolute = archive;
    const std::string stamp = verification_timestamp(true);
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        std::filesystem::path report = absolute;
        report += utf8_to_path(".verify-error-" + stamp +
                               (attempt == 0 ? std::string{} : "-" + std::to_string(attempt)) + ".txt");
        ec.clear();
        if (!std::filesystem::exists(report, ec) && !ec) return report;
    }
    std::filesystem::path fallback = absolute.parent_path() /
        utf8_to_path("mzip-verify-error-" + stamp + ".txt");
    return fallback;
}

void populate_verification_crc(VerificationIssue& issue,
                               const MappedFile& source,
                               const MappedFile& extracted) {
    if (source.size() > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        extracted.size() > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return;
    issue.source_crc32 = Crc32::compute(source.data(), static_cast<size_t>(source.size()));
    issue.extracted_crc32 = Crc32::compute(extracted.data(), static_cast<size_t>(extracted.size()));
    issue.has_crc32 = true;
}

VerificationResult compare_verified_tree(const std::filesystem::path& verification_root,
                                          const std::vector<SourceEntry>& source_entries,
                                          const CancellationCallback& cancel) {
    VerificationResult result;
    const auto verification_access_root = filesystem_access_path(verification_root);
    std::unordered_set<std::string> expected_names;
    expected_names.reserve(source_entries.size());

    for (const auto& source_entry : source_entries) {
        throw_if_cancelled(cancel);
        expected_names.insert(source_entry.name);
        const auto relative = safe_relative_path(source_entry.name);
        if (!source_entry.directory) {
            for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
                expected_names.insert(normalized_archive_name(parent, true));
        }
        const auto extracted_path = verification_access_root / relative;
        std::error_code ec;
        const auto extracted_status = extraction_path_status(extracted_path, ec);
        if (ec || extracted_status.type() == std::filesystem::file_type::not_found) {
            VerificationIssue issue;
            issue.entry = source_entry.name;
            issue.problem = ec ? "Cannot inspect extracted path: " + ec.message() : "Extracted path is missing";
            issue.source = source_entry.source;
            issue.extracted = extracted_path;
            add_verification_issue(result, std::move(issue));
            continue;
        }

        if (source_entry.directory) {
            if (!std::filesystem::is_directory(extracted_status)) {
                VerificationIssue issue;
                issue.entry = source_entry.name;
                issue.problem = "Expected a directory but extraction produced another file type";
                issue.source = source_entry.source;
                issue.extracted = extracted_path;
                add_verification_issue(result, std::move(issue));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(extracted_status)) {
            VerificationIssue issue;
            issue.entry = source_entry.name;
            issue.problem = "Expected a regular file but extraction produced another file type";
            issue.source = source_entry.source;
            issue.extracted = extracted_path;
            add_verification_issue(result, std::move(issue));
            continue;
        }

        try {
            MappedFile source_file(source_entry.source);
            MappedFile extracted_file(extracted_path);
            if (source_file.size() != extracted_file.size()) {
                VerificationIssue issue;
                issue.entry = source_entry.name;
                issue.problem = "File size mismatch";
                issue.source = source_entry.source;
                issue.extracted = extracted_path;
                issue.source_size = source_file.size();
                issue.extracted_size = extracted_file.size();
                issue.has_sizes = true;
                populate_verification_crc(issue, source_file, extracted_file);
                add_verification_issue(result, std::move(issue));
                continue;
            }

            uint64_t offset = 0;
            bool mismatch = false;
            constexpr size_t kCompareChunk = 4u * 1024u * 1024u;
            while (offset < source_file.size()) {
                throw_if_cancelled(cancel);
                const size_t amount = static_cast<size_t>(
                    std::min<uint64_t>(source_file.size() - offset, kCompareChunk));
                const uint8_t* source_data = source_file.data() + static_cast<size_t>(offset);
                const uint8_t* extracted_data = extracted_file.data() + static_cast<size_t>(offset);
                if (std::memcmp(source_data, extracted_data, amount) != 0) {
                    size_t local_offset = 0;
                    while (local_offset < amount && source_data[local_offset] == extracted_data[local_offset])
                        ++local_offset;
                    VerificationIssue issue;
                    issue.entry = source_entry.name;
                    issue.problem = "File content mismatch";
                    issue.source = source_entry.source;
                    issue.extracted = extracted_path;
                    issue.source_size = source_file.size();
                    issue.extracted_size = extracted_file.size();
                    issue.first_difference = offset + local_offset;
                    issue.source_byte = source_data[local_offset];
                    issue.extracted_byte = extracted_data[local_offset];
                    issue.has_sizes = true;
                    issue.has_difference = true;
                    populate_verification_crc(issue, source_file, extracted_file);
                    add_verification_issue(result, std::move(issue));
                    mismatch = true;
                    break;
                }
                offset += amount;
            }
            if (!mismatch) {
                ++result.matching_files;
                result.matching_bytes += source_file.size();
            }
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            VerificationIssue issue;
            issue.entry = source_entry.name;
            issue.problem = std::string("Cannot compare source and extracted file: ") + error.what();
            issue.source = source_entry.source;
            issue.extracted = extracted_path;
            add_verification_issue(result, std::move(issue));
        }
    }

    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(verification_access_root,
        std::filesystem::directory_options::none, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) throw std::runtime_error("Cannot enumerate verification directory: " + ec.message());
    for (; iterator != end; iterator.increment(ec)) {
        throw_if_cancelled(cancel);
        if (ec) throw std::runtime_error("Cannot enumerate verification directory: " + ec.message());
        const auto status = iterator->status(ec);
        if (ec) throw std::runtime_error("Cannot inspect verification output: " + ec.message());
        const bool directory = std::filesystem::is_directory(status);
        std::error_code relative_error;
        const auto relative = std::filesystem::relative(
            iterator->path(), verification_access_root, relative_error);
        if (relative_error || relative.empty()) {
            throw std::runtime_error("Cannot resolve a path inside the verification directory: " +
                                     path_to_utf8(iterator->path()));
        }
        const std::string name = normalized_archive_name(relative, directory);
        if (expected_names.find(name) == expected_names.end()) {
            VerificationIssue issue;
            issue.entry = name;
            issue.problem = "Unexpected file or directory exists in extracted output";
            issue.extracted = iterator->path();
            add_verification_issue(result, std::move(issue));
        }
    }
    return result;
}

void write_verification_report(const std::filesystem::path& report_path,
                               const std::filesystem::path& archive,
                               const std::filesystem::path& verification_root,
                               const std::vector<std::filesystem::path>& inputs,
                               const ArchiveOptions& options,
                               const std::string& stage,
                               const std::string& failure,
                               const VerificationResult& result) {
    std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
    if (!report) throw std::runtime_error("Cannot create verification error report");
    std::error_code archive_path_error;
    const auto archive_absolute = std::filesystem::absolute(archive, archive_path_error);
    report << "mzip post-create verification error report\n"
           << "=============================================\n"
           << "Generated: " << verification_timestamp(false) << "\n"
           << "Result: FAILED\n"
           << "Stage: " << stage << "\n"
           << "Failure: " << failure << "\n"
           << "Archive: " << path_to_utf8(archive_path_error ? archive : archive_absolute) << "\n"
           << "Temporary extraction: "
           << (verification_root.empty() ? "<not created>" : path_to_utf8(verification_root)) << "\n"
           << "Compression level: " << options.level << "\n"
           << "Compression policy: "
           << (options.compression == CompressionPolicy::Auto ? "Auto" :
               options.compression == CompressionPolicy::AlwaysStore ? "AlwaysStore" : "AlwaysDeflate") << "\n"
           << "Directory entries: "
           << (options.directories == DirectoryEntryPolicy::Include ? "Include" : "Omit") << "\n"
           << "ZIP entry root: "
           << (options.entry_root.empty() ? "<default>" : path_to_utf8(options.entry_root)) << "\n"
           << "Block size: " << options.chunk_size << " bytes\n"
           << "Encryption: " << (options.password.empty() ? "OFF" : "WinZip AES-256") << "\n"
           << "AVX2 allowed: " << (options.allow_avx2 ? "yes" : "no") << "\n"
           << "AES-NI allowed: " << (options.allow_aesni ? "yes" : "no") << "\n"
           << "Matching files before failure: " << result.matching_files << "\n"
           << "Matching bytes before failure: " << result.matching_bytes << "\n"
           << "Detected issues: " << result.issue_count << "\n\n"
           << "Inputs:\n";
    for (const auto& input : inputs) report << "  - " << path_to_utf8(input) << "\n";

    if (!result.issues.empty()) report << "\nMismatch details:\n";
    size_t number = 0;
    for (const auto& issue : result.issues) {
        report << "\n[" << ++number << "] " << issue.problem << "\n"
               << "Entry: " << issue.entry << "\n";
        if (!issue.source.empty()) report << "Source: " << path_to_utf8(issue.source) << "\n";
        if (!issue.extracted.empty()) report << "Extracted: " << path_to_utf8(issue.extracted) << "\n";
        if (issue.has_sizes) {
            report << "Source size: " << issue.source_size << " bytes\n"
                   << "Extracted size: " << issue.extracted_size << " bytes\n";
        }
        if (issue.has_difference) {
            report << "First differing offset: " << issue.first_difference
                   << " (0x" << std::hex << std::uppercase << issue.first_difference << std::dec << ")\n"
                   << "Source byte: 0x" << std::hex << std::uppercase << std::setw(2)
                   << std::setfill('0') << issue.source_byte
                   << ", extracted byte: 0x" << std::setw(2) << issue.extracted_byte
                   << std::dec << std::setfill(' ') << "\n";
        }
        if (issue.has_crc32) {
            report << "Source CRC-32: 0x" << std::hex << std::uppercase << std::setw(8)
                   << std::setfill('0') << issue.source_crc32
                   << ", extracted CRC-32: 0x" << std::setw(8) << issue.extracted_crc32
                   << std::dec << std::setfill(' ') << "\n";
        }
    }
    if (result.omitted_issues != 0)
        report << "\nAdditional omitted issues: " << result.omitted_issues << "\n";
    report << "\nThe temporary archive and extraction cleanup were attempted.\n"
           << "The password is intentionally not included in this report.\n";
    report.flush();
    if (!report) throw std::runtime_error("Cannot finish verification error report");
}

void verify_created_archive(const std::filesystem::path& archive,
                            const std::filesystem::path& final_output,
                            const std::vector<std::filesystem::path>& inputs,
                            const ArchiveOptions& options) {
    const auto begin = std::chrono::steady_clock::now();
    std::filesystem::path verification_root;
    std::string stage = "creating temporary verification directory";
    VerificationResult comparison;

    try {
        throw_if_cancelled(options.cancel);
        verification_root = create_verification_directory();
        stage = "extracting the completed archive";
        std::cout << "[VERIFY] Extracting the completed archive to temporary storage...\n";
        ExtractOptions extract_options;
        extract_options.output_directory = verification_root;
        extract_options.allow_aesni = options.allow_aesni;
        extract_options.verbose = false;
        extract_options.show_summary = false;
        extract_options.password = options.password;
        if (options.progress) {
            extract_options.progress = [callback = options.progress](const ProgressInfo& info) {
                ProgressInfo verification = info;
                verification.stage = ProgressStage::Verifying;
                callback(verification);
            };
        }
        extract_zip(archive, extract_options);

        stage = "rescanning source inputs";
        const auto current_sources = scan_inputs(archive, inputs, verification_root,
                                                 final_output, options.cancel,
                                                 options.excluded_paths,
                                                 options.entry_root,
                                                 options.directories);
        stage = "byte-for-byte source comparison";
        std::cout << "[VERIFY] Comparing directory structure, file sizes, and every byte...\n";
        comparison = compare_verified_tree(verification_root, current_sources, options.cancel);
        if (comparison.issue_count != 0) {
            throw std::runtime_error(std::to_string(comparison.issue_count) +
                                     " source/extracted mismatch(es) detected");
        }

        stage = "removing temporary verification data";
        std::error_code ec;
        const auto verification_access_root = filesystem_access_path(verification_root);
        std::filesystem::remove_all(verification_access_root, ec);
        if (ec || std::filesystem::exists(verification_access_root))
            throw std::runtime_error("Verification passed, but temporary data could not be removed");

        const double elapsed = std::max(1e-9, seconds_since(begin));
        std::cout << "[VERIFY] PASS: " << comparison.matching_files << " files, "
                  << format_bytes(comparison.matching_bytes)
                  << " matched 100% byte-for-byte; temporary data removed ("
                  << std::fixed << std::setprecision(3) << elapsed << " s)\n";
    } catch (const OperationCancelled&) {
        std::error_code cleanup_error;
        if (!verification_root.empty()) {
            const auto verification_access_root = filesystem_access_path(verification_root);
            std::filesystem::remove_all(verification_access_root, cleanup_error);
            std::error_code exists_error;
            if (cleanup_error || std::filesystem::exists(verification_access_root, exists_error) || exists_error) {
                throw std::runtime_error("Operation cancelled, but verification temporary data could not be removed: " +
                                         path_to_utf8(verification_root));
            }
        }
        throw;
    } catch (const std::exception& error) {
        const std::string failure = error.what();
        std::error_code cleanup_error;
        bool temporary_removed = verification_root.empty();
        if (!verification_root.empty()) {
            const auto verification_access_root = filesystem_access_path(verification_root);
            std::filesystem::remove_all(verification_access_root, cleanup_error);
            std::error_code exists_error;
            temporary_removed = !cleanup_error &&
                !std::filesystem::exists(verification_access_root, exists_error) && !exists_error;
        }
        std::filesystem::path report_path;
        std::string report_failure;
        try {
            report_path = choose_verification_report_path(final_output);
            write_verification_report(report_path, final_output, verification_root, inputs,
                                      options, stage, failure, comparison);
        } catch (const std::exception& report_error) {
            report_failure = report_error.what();
        }

        std::ostringstream message;
        message << "Post-create verification failed during " << stage << ": " << failure << ". ";
        if (!report_path.empty() && report_failure.empty())
            message << "Error report: " << path_to_utf8(report_path) << ". ";
        else
            message << "The error report could not be written"
                    << (report_failure.empty() ? std::string{} : ": " + report_failure) << ". ";
        if (!verification_root.empty()) {
            if (temporary_removed) {
                message << "Temporary extraction was removed.";
            } else {
                message << "Temporary extraction cleanup failed at: "
                        << path_to_utf8(verification_root);
                if (cleanup_error) message << " (" << cleanup_error.message() << ")";
                message << ".";
            }
        }
        throw VerificationError(message.str(),
                                report_failure.empty() ? report_path : std::filesystem::path{},
                                temporary_removed ? std::filesystem::path{} : verification_root);
    }
}

} // namespace

std::vector<ZipEntryInfo> inspect_zip(const std::filesystem::path& archive) {
    MappedFile mapped(archive);
    const ParsedArchive parsed = parse_archive_directory(mapped.data(), mapped.size());

    std::vector<ZipEntryInfo> result;
    result.reserve(parsed.entries.size());
    for (const auto& entry : parsed.entries) {
        ZipEntryInfo info;
        info.name = entry.name;
        info.method = method_label(entry.method, entry.actual_method,
                                   entry.aes, entry.aes_strength);
        if ((entry.flags & kEncryptedFlag) != 0 && !entry.aes)
            info.method += '*';
        info.compressed_size = entry.compressed_size;
        info.uncompressed_size = entry.uncompressed_size;
        info.directory = entry.directory;
        info.encrypted = (entry.flags & kEncryptedFlag) != 0;
        result.push_back(std::move(info));
    }
    return result;
}

void create_zip(const std::filesystem::path& output,
                const std::vector<std::filesystem::path>& inputs,
                const ArchiveOptions& options) {
    const auto all_begin = std::chrono::steady_clock::now();
    switch (options.compression) {
    case CompressionPolicy::Auto:
    case CompressionPolicy::AlwaysStore:
    case CompressionPolicy::AlwaysDeflate:
        break;
    default:
        throw std::runtime_error("Invalid ZIP compression policy");
    }
    switch (options.directories) {
    case DirectoryEntryPolicy::Include:
    case DirectoryEntryPolicy::Omit:
        break;
    default:
        throw std::runtime_error("Invalid ZIP directory entry policy");
    }
    const auto entries = scan_inputs(output, inputs, {}, {}, options.cancel,
                                     options.excluded_paths, options.entry_root,
                                     options.directories);
    uint64_t progress_total_bytes = 0;
    uint64_t progress_total_files = 0;
    for (const auto& entry : entries) {
        if (!entry.directory) {
            progress_total_bytes += entry.size;
            ++progress_total_files;
        }
    }
    uint64_t progress_completed_bytes = 0;
    uint64_t progress_completed_files = 0;
    report_progress(options.progress, ProgressStage::Compressing,
                    progress_completed_bytes, progress_total_bytes,
                    progress_completed_files, progress_total_files);
    const bool avx2 = options.allow_avx2 && cpu_has_avx2();
    const bool aesni = options.allow_aesni && cpu_has_aesni();
    const bool always_store = options.compression == CompressionPolicy::AlwaysStore;
    const bool always_deflate = options.compression == CompressionPolicy::AlwaysDeflate;
    const std::filesystem::path temporary_output = make_temporary_path(output);
    TemporaryFileGuard temporary_output_guard(temporary_output);
    throw_if_cancelled(options.cancel);
    std::ofstream os(temporary_output, std::ios::binary | std::ios::trunc);
    if (!os) throw std::runtime_error("Cannot create archive: " + path_to_utf8(output));

    std::vector<CentralEntry> central;
    central.reserve(entries.size());
    uint64_t total_in = 0;
    uint64_t total_out = 0;

    if (options.verbose) {
        std::cout << "mzip: AVX2=" << (avx2 ? "ON" : "OFF")
                  << ", level=" << options.level
                  << ", block=" << format_bytes(options.chunk_size)
                  << ", encryption=" << (options.password.empty() ? "OFF" : "AES-256")
                  << ", AES-NI=" << (options.password.empty() ? "N/A" : (aesni ? "ON" : "OFF")) << "\n";
    }

    auto prepare_file = [&](size_t index) {
        throw_if_cancelled(options.cancel);
        const SourceEntry& src = entries[index];
        const auto begin = std::chrono::steady_clock::now();
        PreparedPayload payload;
        payload.mapped = std::make_unique<MappedFile>(src.source);
        payload.crc32 = Crc32::compute(payload.mapped->data(), static_cast<size_t>(payload.mapped->size()));
        throw_if_cancelled(options.cancel);

        const bool try_deflate = !always_store &&
            (always_deflate || (options.level > 0 && payload.mapped->size() > 0));
        if (try_deflate &&
            payload.mapped->size() <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            DeflateOptions deflate_options;
            deflate_options.level = options.level;
            deflate_options.chunk_size = options.chunk_size;
            deflate_options.use_avx2 = avx2;
            deflate_options.cancel = options.cancel;
            if (options.progress) {
                deflate_options.progress = [&](size_t completed_in_file) {
                    report_progress(options.progress, ProgressStage::Compressing,
                                    progress_completed_bytes + completed_in_file,
                                    progress_total_bytes, progress_completed_files,
                                    progress_total_files, src.name);
                };
            }
            payload.compressed = deflate(payload.mapped->data(),
                                         static_cast<size_t>(payload.mapped->size()),
                                         deflate_options);
            if (always_deflate || payload.compressed.bytes.size() < payload.mapped->size()) {
                payload.actual_method = kMethodDeflate;
                payload.compressed_size = payload.compressed.bytes.size();
            } else {
                payload.actual_method = kMethodStore;
                payload.compressed_size = payload.mapped->size();
                payload.compressed.bytes.clear();
                payload.compressed.bytes.shrink_to_fit();
            }
        } else if (try_deflate) {
            throw std::runtime_error("ZIP entry is too large to Deflate in this process: " + src.name);
        } else {
            payload.actual_method = kMethodStore;
            payload.compressed_size = payload.mapped->size();
        }

        if (!options.password.empty()) {
            throw_if_cancelled(options.cancel);
            const uint8_t* plain = payload.actual_method == kMethodDeflate ?
                payload.compressed.bytes.data() : payload.mapped->data();
            payload.encrypted_payload = winzip_aes256_encrypt(
                plain, static_cast<size_t>(payload.compressed_size), options.password, aesni);
            payload.encrypted = true;
            payload.crc32 = 0; // AE-2 authenticates ciphertext with HMAC instead of CRC-32.
            payload.compressed_size = payload.encrypted_payload.size();
        }
        throw_if_cancelled(options.cancel);
        payload.seconds = std::max(1e-9, seconds_since(begin));
        return payload;
    };

    for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
        throw_if_cancelled(options.cancel);
        const SourceEntry& src = entries[entry_index];
        if (!src.directory) {
            report_progress(options.progress, ProgressStage::Compressing,
                            progress_completed_bytes, progress_total_bytes,
                            progress_completed_files, progress_total_files, src.name);
        }
        CentralEntry e;
        e.name = src.name;
        e.dos_date = src.dos_date;
        e.dos_time = src.dos_time;
        e.directory = src.directory;
        e.uncompressed_size = src.size;
        e.local_offset = stream_position(os);

        PreparedPayload payload;

        if (!src.directory) {
            payload = prepare_file(entry_index);
            e.crc32 = payload.crc32;
            e.actual_method = payload.actual_method;
            e.encrypted = payload.encrypted;
            e.method = payload.encrypted ? kMethodWinZipAes : payload.actual_method;
            e.compressed_size = payload.compressed_size;
        }

        write_local_header(os, e);
        if (!src.directory) {
            if (e.encrypted) {
                write_large(os, payload.encrypted_payload.data(), payload.encrypted_payload.size(), options.cancel);
            } else if (e.method == kMethodDeflate) {
                write_large(os, payload.compressed.bytes.data(), payload.compressed.bytes.size(), options.cancel);
            } else if (payload.mapped->size() != 0) {
                write_large(os, payload.mapped->data(), payload.mapped->size(), options.cancel);
            }
        }
        if (!os) throw std::runtime_error("Archive write failed for: " + e.name);

        total_in += e.uncompressed_size;
        total_out += e.compressed_size;
        central.push_back(e);
        if (!src.directory) {
            progress_completed_bytes += e.uncompressed_size;
            ++progress_completed_files;
            report_progress(options.progress, ProgressStage::Compressing,
                            progress_completed_bytes, progress_total_bytes,
                            progress_completed_files, progress_total_files, src.name);
        }

        if (options.verbose) {
            const double sec = src.directory ? 1e-9 : payload.seconds;
            const double mbps = (static_cast<double>(e.uncompressed_size) / (1024.0 * 1024.0)) / sec;
            const double ratio = e.uncompressed_size ?
                100.0 * static_cast<double>(e.compressed_size) / static_cast<double>(e.uncompressed_size) : 0.0;
            const char* tag = src.directory ? "[DIR ] " : e.encrypted ? "[AES ] " :
                              e.method == kMethodDeflate ? "[DFLT] " : "[STOR] ";
            std::cout << tag << e.name;
            if (!src.directory) {
                std::cout << "  " << format_bytes(e.uncompressed_size)
                          << " -> " << format_bytes(e.compressed_size)
                          << "  " << std::fixed << std::setprecision(1) << ratio << "%"
                          << "  " << std::setprecision(1) << mbps << " MiB/s";
            }
            std::cout << '\n';
        }
    }

    const uint64_t central_offset = stream_position(os);
    for (const auto& e : central) write_central_header(os, e);
    const uint64_t central_size = stream_position(os) - central_offset;
    write_archive_end(os, central_offset, central_size, central.size());
    os.flush();
    if (!os) throw std::runtime_error("Failed to finalize archive");
    os.close();
    if (!os) throw std::runtime_error("Failed to close finalized archive");
    throw_if_cancelled(options.cancel);

    const double sec = std::max(1e-9, seconds_since(all_begin));
    const double mbps = (static_cast<double>(total_in) / (1024.0 * 1024.0)) / sec;
    const double ratio = total_in ? 100.0 * static_cast<double>(total_out) / static_cast<double>(total_in) : 0.0;
    std::cout << "Created: " << path_to_utf8(output) << '\n'
              << "Files/entries: " << central.size() << ", input " << format_bytes(total_in)
              << ", payload " << format_bytes(total_out)
              << ", ratio " << std::fixed << std::setprecision(1) << ratio << "%"
              << ", elapsed " << std::setprecision(3) << sec << " s"
              << ", throughput " << std::setprecision(1) << mbps << " MiB/s\n";
    if (options.verify_after_create) {
        report_progress(options.progress, ProgressStage::Verifying,
                        0, progress_total_bytes, 0, progress_total_files);
        throw_if_cancelled(options.cancel);
        ArchiveOptions verification_options = options;
        verification_options.cancel = {};
        verify_created_archive(temporary_output, output, inputs, verification_options);
    }
    replace_file_atomically(temporary_output, output);
    temporary_output_guard.release();
}

void extract_zip(const std::filesystem::path& archive,
                 const ExtractOptions& options) {
    const auto all_begin = std::chrono::steady_clock::now();
    throw_if_cancelled(options.cancel);
    MappedFile mapped(archive);
    const ParsedArchive parsed = parse_archive_directory(mapped.data(), mapped.size());
    const bool aesni = options.allow_aesni && cpu_has_aesni();

    for (const auto& entry : parsed.entries) {
        throw_if_cancelled(options.cancel);
        if (entry.aes && options.password.empty())
            throw std::runtime_error("A password is required to extract AES entry: " + entry.name);
    }

    std::vector<PreparedEntry> prepared;
    prepared.reserve(parsed.entries.size());
    for (const auto& entry : parsed.entries) {
        throw_if_cancelled(options.cancel);
        PreparedEntry item;
        item.entry = &entry;
        item.relative = safe_relative_path(entry.name);
        prepared.push_back(std::move(item));
    }

    const ExtractEntry* stripped_root_entry = nullptr;
    const auto requested_root = options.strip_root_directory.filename();
    bool strip_root = !requested_root.empty() && !prepared.empty();
    const std::string requested_root_key = path_key(requested_root);
    if (strip_root) {
        for (const auto& item : prepared) {
            auto component = item.relative.begin();
            if (component == item.relative.end() ||
                path_key(*component) != requested_root_key) {
                strip_root = false;
                break;
            }
            ++component;
            if (component == item.relative.end() && !item.entry->directory) {
                strip_root = false;
                break;
            }
        }
    }
    if (strip_root) {
        auto output = prepared.begin();
        for (auto item = prepared.begin(); item != prepared.end(); ++item) {
            auto component = item->relative.begin();
            ++component;
            if (component == item->relative.end()) {
                stripped_root_entry = item->entry;
                continue;
            }
            std::filesystem::path relative;
            for (; component != item->relative.end(); ++component)
                relative /= *component;
            item->relative = std::move(relative);
            if (output != item) *output = std::move(*item);
            ++output;
        }
        prepared.erase(output, prepared.end());
    }

    std::unordered_set<std::string> file_paths;
    std::unordered_set<std::string> directory_paths;
    for (const auto& item : prepared) {
        const auto& entry = *item.entry;
        const std::string item_key = path_key(item.relative);

        for (auto parent = item.relative.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            const std::string parent_key = path_key(parent);
            if (file_paths.find(parent_key) != file_paths.end())
                throw std::runtime_error("ZIP contains a file/directory path conflict: " + entry.name);
            directory_paths.insert(parent_key);
        }
        if (entry.directory) {
            if (file_paths.find(item_key) != file_paths.end())
                throw std::runtime_error("ZIP contains a file/directory path conflict: " + entry.name);
            directory_paths.insert(item_key);
        } else {
            if (directory_paths.find(item_key) != directory_paths.end() ||
                !file_paths.insert(item_key).second) {
                throw std::runtime_error("ZIP contains a duplicate or conflicting output path: " + entry.name);
            }
        }
    }

    uint64_t progress_total_bytes = 0;
    uint64_t progress_total_files = 0;
    for (const auto& item : prepared) {
        throw_if_cancelled(options.cancel);
        if (!item.entry->directory) {
            if (item.entry->uncompressed_size >
                std::numeric_limits<uint64_t>::max() - progress_total_bytes) {
                throw std::runtime_error("ZIP total uncompressed size overflows 64 bits");
            }
            progress_total_bytes += item.entry->uncompressed_size;
            ++progress_total_files;
        }
    }
    report_progress(options.progress, ProgressStage::Extracting,
                     0, progress_total_bytes, 0, progress_total_files);
    throw_if_cancelled(options.cancel);

    std::error_code ec;
    auto output_root = std::filesystem::absolute(options.output_directory, ec);
    if (ec) throw std::runtime_error("Cannot resolve output directory: " + path_to_utf8(options.output_directory));
    std::filesystem::create_directories(output_root, ec);
    if (ec) throw std::runtime_error("Cannot create output directory: " + path_to_utf8(output_root));
    output_root = std::filesystem::weakly_canonical(output_root, ec);
    if (ec || !std::filesystem::is_directory(output_root))
        throw std::runtime_error("Extraction output is not a directory: " + path_to_utf8(output_root));
    const auto canonical_output_root = output_root;
    output_root = filesystem_access_path(output_root);

    for (auto& item : prepared) {
        throw_if_cancelled(options.cancel);
        item.target = safe_output_path(canonical_output_root, item.relative,
                                       item.entry->name);
        for (auto parent = item.target.parent_path(); path_is_within(output_root, parent); parent = parent.parent_path()) {
            const auto status = extraction_path_status(parent, ec);
            if (ec) {
                throw std::runtime_error("Cannot inspect extraction path: " + path_to_utf8(parent) +
                                         " (" + ec.message() + ")");
            } else if (status.type() != std::filesystem::file_type::not_found &&
                       !std::filesystem::is_directory(status)) {
                throw std::runtime_error("Extraction parent is not a directory: " + path_to_utf8(parent));
            }
            if (parent == output_root) break;
        }

        const auto status = extraction_path_status(item.target, ec);
        bool exists = false;
        if (ec) {
            throw std::runtime_error("Cannot inspect extraction target: " + path_to_utf8(item.target) +
                                     " (" + ec.message() + ")");
        } else {
            exists = status.type() != std::filesystem::file_type::not_found;
        }
        if (exists && std::filesystem::is_symlink(status))
            throw std::runtime_error("Refusing to extract through a symbolic link: " + item.entry->name);
        if (item.entry->directory) {
            if (exists && !std::filesystem::is_directory(status))
                throw std::runtime_error("A file blocks ZIP directory: " + item.entry->name);
        } else if (exists) {
            if (!std::filesystem::is_regular_file(status))
                throw std::runtime_error("Extraction target is not a regular file: " + item.entry->name);
            if (!options.overwrite)
                throw std::runtime_error("Output file already exists (use --overwrite): " + item.entry->name);
        }
    }

    // Each file is decoded into an exclusive temporary mapping and is installed
    // only after every entry has passed its CRC check. This prevents a corrupt
    // late entry from exposing partially verified files.
    for (size_t index = 0; index < prepared.size(); ++index) {
        throw_if_cancelled(options.cancel);
        const auto& item = prepared[index];
        if (item.entry->directory) {
            std::filesystem::create_directories(item.target, ec);
            if (ec)
                throw std::runtime_error("Cannot create directory: " +
                                         path_to_utf8(item.target));
            if (options.verbose) std::cout << "[DIR ] " << item.entry->name << '\n';
        } else {
            std::filesystem::create_directories(item.target.parent_path(), ec);
            if (ec)
                throw std::runtime_error("Cannot create parent directory: " +
                                         path_to_utf8(item.target.parent_path()));
        }
    }

    struct PendingExtraction {
        const PreparedEntry* item = nullptr;
        std::filesystem::path temporary;
        std::unique_ptr<TemporaryFileGuard> guard;
        InflateResult result;
    };

    uint64_t extracted_bytes = 0;
    uint64_t extracted_files = 0;
    std::vector<std::unique_ptr<PendingExtraction>> pending(prepared.size());
    for (size_t index = 0; index < prepared.size(); ++index) {
        const auto& item = prepared[index];
        const auto& entry = *item.entry;
        if (entry.directory) continue;

        try {
            throw_if_cancelled(options.cancel);
            if (entry.compressed_size >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
                entry.uncompressed_size >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::runtime_error("ZIP entry is too large for this process");
            }

            const uint8_t* compressed = checked_bytes(
                mapped.data(), mapped.size(), entry.data_offset,
                entry.compressed_size, "ZIP entry payload");
            size_t compressed_size = static_cast<size_t>(entry.compressed_size);
            std::vector<uint8_t> decrypted;
            if (entry.aes) {
                decrypted = winzip_aes256_decrypt(
                    compressed, compressed_size, options.password, aesni);
                throw_if_cancelled(options.cancel);
                compressed = decrypted.data();
                compressed_size = decrypted.size();
            }

            std::error_code extraction_ec;
            for (auto parent = item.target.parent_path();
                 path_is_within(output_root, parent);
                 parent = parent.parent_path()) {
                const auto parent_status =
                    extraction_path_status(parent, extraction_ec);
                if (extraction_ec ||
                    !std::filesystem::is_directory(parent_status) ||
                    std::filesystem::is_symlink(parent_status)) {
                    throw std::runtime_error(
                        "Extraction parent changed or is not a directory: " +
                        path_to_utf8(parent));
                }
                if (parent == output_root) break;
            }

            auto extracted = std::make_unique<PendingExtraction>();
            extracted->item = &item;
            extracted->temporary = make_temporary_path(item.target);
            extracted->guard = std::make_unique<TemporaryFileGuard>(
                extracted->temporary, false);

            uint64_t reported_in_file = 0;
            const std::function<void(uint64_t)> file_progress =
                [&](uint64_t completed_in_file) {
                throw_if_cancelled(options.cancel);
                if (completed_in_file < reported_in_file ||
                    completed_in_file > entry.uncompressed_size) {
                    throw std::runtime_error("Invalid extraction progress");
                }
                extracted_bytes += completed_in_file - reported_in_file;
                reported_in_file = completed_in_file;
                report_progress(options.progress, ProgressStage::Extracting,
                                extracted_bytes, progress_total_bytes,
                                extracted_files, progress_total_files, entry.name);
            };

            {
                throw_if_cancelled(options.cancel);
                WritableMappedFile output(extracted->temporary,
                                          entry.uncompressed_size);
                extracted->guard->activate();
                if (entry.actual_method == kMethodStore) {
                    if (compressed_size != entry.uncompressed_size)
                        throw std::runtime_error(
                            "Stored entry size does not match the ZIP directory");
                    Crc32 crc;
                    constexpr size_t kStoredCopyChunk = 4u * 1024u * 1024u;
                    size_t copied = 0;
                    while (copied != compressed_size) {
                        throw_if_cancelled(options.cancel);
                        const size_t amount = std::min(
                            kStoredCopyChunk, compressed_size - copied);
                        std::memcpy(output.data() + copied,
                                    compressed + copied, amount);
                        crc.update(compressed + copied, amount);
                        copied += amount;
                        file_progress(copied);
                    }
                    extracted->result = {entry.uncompressed_size, crc.value()};
                } else {
                    extracted->result = inflate_raw_to_buffer(
                        compressed, compressed_size, entry.uncompressed_size,
                        output.data(), file_progress);
                }
                throw_if_cancelled(options.cancel);
                if ((!entry.aes || entry.aes_version == 1) &&
                    extracted->result.crc32 != entry.crc32) {
                    throw std::runtime_error("CRC-32 verification failed");
                }
                output.flush();
            }

            if (reported_in_file != extracted->result.size)
                file_progress(extracted->result.size);
            pending[index] = std::move(extracted);
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Failed to extract " + entry.name + ": " + error.what());
        }
    }

    if (extracted_bytes != progress_total_bytes) {
        throw std::runtime_error("ZIP extraction progress totals do not match the directory");
    }

    // Revalidate the path immediately before the atomic install. In
    // particular, reject a symlink or non-regular target that appeared while
    // later entries were being decoded.
    for (size_t index = 0; index < prepared.size(); ++index) {
        const auto& item = prepared[index];
        const auto& entry = *item.entry;
        if (entry.directory) continue;
        throw_if_cancelled(options.cancel);

        for (auto parent = item.target.parent_path();
             path_is_within(output_root, parent); parent = parent.parent_path()) {
            const auto parent_status = extraction_path_status(parent, ec);
            if (ec || !std::filesystem::is_directory(parent_status) ||
                std::filesystem::is_symlink(parent_status)) {
                throw std::runtime_error(
                    "Extraction parent changed or is not a directory: " +
                    path_to_utf8(parent));
            }
            if (parent == output_root) break;
        }

        const auto current_status = extraction_path_status(item.target, ec);
        if (ec)
            throw std::runtime_error("Cannot inspect output target before replacement");
        const bool target_exists =
            current_status.type() != std::filesystem::file_type::not_found;
        if (target_exists &&
            (!options.overwrite || std::filesystem::is_symlink(current_status) ||
             !std::filesystem::is_regular_file(current_status))) {
            throw std::runtime_error("Output target changed or cannot be overwritten");
        }

        replace_file_atomically(pending[index]->temporary, item.target,
                                options.overwrite);
        pending[index]->guard->release();
        set_dos_timestamp(item.target, entry.dos_date, entry.dos_time);
        ++extracted_files;
        report_progress(options.progress, ProgressStage::Extracting,
                        extracted_bytes,
                        progress_total_bytes, extracted_files,
                        progress_total_files, entry.name);
        if (options.verbose) {
            const char* tag = entry.aes ? "[AES ] " :
                              entry.actual_method == kMethodDeflate ?
                              "[DFLT] " : "[STOR] ";
            std::cout << tag << entry.name << "  "
                      << format_bytes(pending[index]->result.size) << '\n';
        }
    }

    const uint64_t final_extracted_bytes = extracted_bytes;
    const uint64_t final_extracted_files = extracted_files;
    report_progress(options.progress, ProgressStage::Extracting,
                    final_extracted_bytes, progress_total_bytes,
                    final_extracted_files, progress_total_files);

    for (auto item = prepared.rbegin(); item != prepared.rend(); ++item) {
        if (item->entry->directory) {
            const auto status = extraction_path_status(item->target, ec);
            if (ec || !std::filesystem::is_directory(status) ||
                std::filesystem::is_symlink(status)) {
                throw std::runtime_error(
                    "ZIP directory changed before timestamp update: " +
                    item->entry->name);
            }
            set_dos_timestamp(item->target, item->entry->dos_date, item->entry->dos_time);
        }
    }
    if (stripped_root_entry)
        set_dos_timestamp(output_root, stripped_root_entry->dos_date, stripped_root_entry->dos_time);
    if (options.show_summary) {
        const double seconds = std::max(1e-9, seconds_since(all_begin));
        const double throughput = (static_cast<double>(final_extracted_bytes) / (1024.0 * 1024.0)) / seconds;
        std::cout << "Extracted: " << path_to_utf8(archive) << " -> " << path_to_utf8(output_root) << '\n'
                  << "Files: " << final_extracted_files << ", output " << format_bytes(final_extracted_bytes)
                  << ", elapsed " << std::fixed << std::setprecision(3) << seconds << " s"
                  << ", throughput " << std::setprecision(1) << throughput << " MiB/s\n";
    }
}

void list_zip(const std::filesystem::path& archive) {
    std::ifstream is(filesystem_access_path(archive), std::ios::binary);
    if (!is) throw std::runtime_error("Cannot open archive: " + path_to_utf8(archive));
    is.seekg(0, std::ios::end);
    const uint64_t file_size = static_cast<uint64_t>(is.tellg());
    if (file_size < 22) throw std::runtime_error("Not a valid ZIP file");

    const auto tail = read_file_tail(is, file_size, 22 + 0xFFFF + 20);
    size_t eocd_pos = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (read_u32(tail.data() + i) == kEocdSig) { eocd_pos = i; break; }
    }
    if (eocd_pos == std::string::npos) throw std::runtime_error("ZIP end record not found");

    uint64_t count = read_u16(tail.data() + eocd_pos + 10);
    uint64_t central_size = read_u32(tail.data() + eocd_pos + 12);
    uint64_t central_offset = read_u32(tail.data() + eocd_pos + 16);

    if (count == 0xFFFFu || central_size == 0xFFFFFFFFu || central_offset == 0xFFFFFFFFu) {
        const uint64_t eocd_absolute = file_size - tail.size() + eocd_pos;
        if (eocd_absolute < 20) throw std::runtime_error("ZIP64 locator missing");
        uint8_t locator[20];
        is.seekg(static_cast<std::streamoff>(eocd_absolute - 20), std::ios::beg);
        is.read(reinterpret_cast<char*>(locator), sizeof(locator));
        if (!is || read_u32(locator) != kZip64LocatorSig) throw std::runtime_error("ZIP64 locator invalid");
        const uint64_t z64_offset = read_u64(locator + 8);
        uint8_t z64[56];
        is.seekg(static_cast<std::streamoff>(z64_offset), std::ios::beg);
        is.read(reinterpret_cast<char*>(z64), sizeof(z64));
        if (!is || read_u32(z64) != kZip64EocdSig) throw std::runtime_error("ZIP64 end record invalid");
        count = read_u64(z64 + 32);
        central_size = read_u64(z64 + 40);
        central_offset = read_u64(z64 + 48);
    }

    is.seekg(static_cast<std::streamoff>(central_offset), std::ios::beg);
    std::cout << "Method          Original       Packed   Ratio  Name\n";
    std::cout << "----------  ------------  ------------  ------  ----\n";
    uint64_t total_original = 0, total_packed = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t fixed[46];
        is.read(reinterpret_cast<char*>(fixed), sizeof(fixed));
        if (!is || read_u32(fixed) != kCentralHeaderSig) throw std::runtime_error("Invalid central directory");

        const uint16_t flags = read_u16(fixed + 8);
        const uint16_t method = read_u16(fixed + 10);
        const uint32_t comp32 = read_u32(fixed + 20);
        const uint32_t uncomp32 = read_u32(fixed + 24);
        const uint16_t name_len = read_u16(fixed + 28);
        const uint16_t extra_len = read_u16(fixed + 30);
        const uint16_t comment_len = read_u16(fixed + 32);
        const uint32_t offset32 = read_u32(fixed + 42);
        std::vector<uint8_t> name(name_len), extra(extra_len);
        if (name_len) is.read(reinterpret_cast<char*>(name.data()), name_len);
        if (extra_len) is.read(reinterpret_cast<char*>(extra.data()), extra_len);
        if (comment_len) is.seekg(comment_len, std::ios::cur);
        if (!is) throw std::runtime_error("Truncated central directory");

        uint64_t comp = comp32, uncomp = uncomp32, offset = offset32;
        if (!parse_zip64_extra(extra.data(), extra.size(), comp32, uncomp32, offset32,
                               comp, uncomp, offset)) {
            throw std::runtime_error("Invalid or missing ZIP64 entry extra field");
        }
        (void)offset;

        uint16_t actual_method = method;
        uint8_t strength = 0;
        uint16_t aes_version = 0;
        const bool aes = method == kMethodWinZipAes &&
            parse_aes_extra(extra.data(), extra.size(), actual_method, strength, aes_version);
        (void)aes_version;
        const bool encrypted = (flags & kEncryptedFlag) != 0;
        const double ratio = uncomp ? 100.0 * static_cast<double>(comp) / static_cast<double>(uncomp) : 0.0;
        const std::string raw_filename(reinterpret_cast<const char*>(name.data()), name.size());
        const std::string filename = decode_zip_name(raw_filename, flags, extra.data(), extra.size());
        std::string label = method_label(method, actual_method, aes, strength);
        if (encrypted && !aes) label += "*";

        std::cout << std::left << std::setw(10) << label
                  << std::right << std::setw(14) << uncomp
                  << std::setw(14) << comp
                  << std::setw(7) << std::fixed << std::setprecision(1) << ratio << "%  "
                  << std::left << filename << '\n';
        total_original += uncomp;
        total_packed += comp;
    }
    std::cout << "Total: " << count << " entries, " << format_bytes(total_original)
              << " -> " << format_bytes(total_packed) << '\n';
}

} // namespace fz
