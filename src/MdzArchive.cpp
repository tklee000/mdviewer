#include "MdzArchive.h"

#include "Json.h"
#include "libmzip/crc32.h"
#include "libmzip/crypto.h"
#include "libmzip/deflate_decoder.h"
#include "libmzip/mzip_codec.h"
#include "libmzip/platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50u;
constexpr std::uint32_t kEndSignature = 0x06054B50u;
constexpr std::uint16_t kUtf8Flag = 1u << 11;
constexpr std::uint16_t kEncryptedFlag = 1u;
constexpr std::uint16_t kStoreMethod = 0;
constexpr std::uint16_t kDeflateMethod = 8;
constexpr std::uint16_t kWinZipAesMethod = 99;
constexpr std::uint16_t kWinZipAesExtraId = 0x9901;
constexpr std::uint16_t kWinZipAesVersion = 2;
constexpr std::uint8_t kWinZipAes256Strength = 3;
constexpr std::uint16_t kWinZipAesRequiredVersion = 51;
constexpr std::size_t kMaximumArchiveBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaximumEntryBytes = 128ull * 1024 * 1024;
constexpr std::size_t kMaximumTotalBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaximumEntries = 4096;
constexpr std::size_t kMaximumPathBytes = 1024;

class PasswordRequiredError final : public std::runtime_error {
public:
    PasswordRequiredError()
        : std::runtime_error("The MDZ file is password protected") {}
};

class IncorrectPasswordError final : public std::runtime_error {
public:
    IncorrectPasswordError()
        : std::runtime_error("The MDZ password is incorrect") {}
};

std::uint16_t Read16(std::string_view bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        throw std::runtime_error("Truncated ZIP structure");
    }
    const auto* value = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint16_t>(value[0] | (value[1] << 8));
}

std::uint32_t Read32(std::string_view bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("Truncated ZIP structure");
    }
    const auto* value = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8) |
           (static_cast<std::uint32_t>(value[2]) << 16) |
           (static_cast<std::uint32_t>(value[3]) << 24);
}

void Append16(std::string* bytes, std::uint16_t value) {
    bytes->push_back(static_cast<char>(value & 0xFF));
    bytes->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Append32(std::string* bytes, std::uint32_t value) {
    bytes->push_back(static_cast<char>(value & 0xFF));
    bytes->push_back(static_cast<char>((value >> 8) & 0xFF));
    bytes->push_back(static_cast<char>((value >> 16) & 0xFF));
    bytes->push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendWinZipAesExtra(std::string* bytes, std::uint16_t actualMethod) {
    Append16(bytes, kWinZipAesExtraId);
    Append16(bytes, 7);
    Append16(bytes, kWinZipAesVersion);
    bytes->append("AE", 2);
    bytes->push_back(static_cast<char>(kWinZipAes256Strength));
    Append16(bytes, actualMethod);
}

struct WinZipAesExtra {
    std::uint16_t version = 0;
    std::uint8_t strength = 0;
    std::uint16_t actualMethod = 0;
};

std::optional<WinZipAesExtra> ParseWinZipAesExtra(std::string_view extra) {
    std::size_t cursor = 0;
    while (cursor + 4 <= extra.size()) {
        const std::uint16_t id = Read16(extra, cursor);
        const std::uint16_t length = Read16(extra, cursor + 2);
        cursor += 4;
        if (length > extra.size() - cursor) {
            throw std::runtime_error("Invalid ZIP extra field");
        }
        if (id == kWinZipAesExtraId) {
            if (length < 7 || extra[cursor + 2] != 'A' ||
                extra[cursor + 3] != 'E') {
                throw std::runtime_error("Invalid WinZip AES extra field");
            }
            return WinZipAesExtra{
                Read16(extra, cursor),
                static_cast<std::uint8_t>(extra[cursor + 4]),
                Read16(extra, cursor + 5),
            };
        }
        cursor += length;
    }
    if (cursor != extra.size()) {
        throw std::runtime_error("Invalid ZIP extra field bounds");
    }
    return std::nullopt;
}

std::wstring WideError(const std::exception& error) {
    const std::string message = error.what();
    return std::wstring(message.begin(), message.end());
}

bool IsMarkdownPath(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash)) return false;
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".md" || extension == ".markdown";
}

std::string BytesToString(const mdz::Bytes& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

mdz::Bytes StringToBytes(const std::string& value) {
    return mdz::Bytes(value.begin(), value.end());
}

std::size_t FindEndRecord(std::string_view bytes) {
    if (bytes.size() < 22) throw std::runtime_error("Not a ZIP archive");
    const std::size_t earliest = bytes.size() > 22 + 0xFFFF
        ? bytes.size() - (22 + 0xFFFF) : 0;
    std::size_t position = bytes.size() - 22;
    for (;;) {
        if (Read32(bytes, position) == kEndSignature) {
            const std::size_t commentLength = Read16(bytes, position + 20);
            if (position + 22 + commentLength == bytes.size()) return position;
        }
        if (position == earliest) break;
        --position;
    }
    throw std::runtime_error("ZIP end record is missing");
}

std::pair<std::uint16_t, std::uint16_t> CurrentDosTime() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const int year = (std::max)(1980, local.tm_year + 1900);
    const std::uint16_t date = static_cast<std::uint16_t>(
        ((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    const std::uint16_t time = static_cast<std::uint16_t>(
        (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    return {time, date};
}

std::string ResolveEntryPoint(const mdz::Entries& entries) {
    const auto manifest = entries.find("manifest.json");
    if (manifest != entries.end()) {
        const std::string manifestText = BytesToString(manifest->second);
        if (!json::IsValidUtf8(manifestText)) {
            throw std::runtime_error("manifest.json is not valid UTF-8");
        }
        if (const auto declared = json::GetString(manifestText, "entryPoint")) {
            if (!mdz::IsSafeArchivePath(*declared) || !IsMarkdownPath(*declared)) {
                throw std::runtime_error("manifest.json contains an invalid entryPoint");
            }
            if (entries.find(*declared) == entries.end()) {
                throw std::runtime_error("manifest.json entryPoint does not exist");
            }
            return *declared;
        }
    }
    if (entries.find("index.md") != entries.end()) return "index.md";

    std::string rootMarkdown;
    for (const auto& [path, ignored] : entries) {
        (void)ignored;
        if (path.find('/') != std::string::npos || !IsMarkdownPath(path)) continue;
        if (!rootMarkdown.empty()) {
            throw std::runtime_error("MDZ entry point is ambiguous");
        }
        rootMarkdown = path;
    }
    if (rootMarkdown.empty()) {
        throw std::runtime_error("MDZ archive has no resolvable Markdown entry point");
    }
    return rootMarkdown;
}

struct CentralRecord {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t expandedSize = 0;
    std::uint32_t offset = 0;
    bool encrypted = false;
};

struct CodecBuffer {
    CodecBuffer() { mzip_codec_buffer_init(&value); }
    ~CodecBuffer() { mzip_codec_buffer_free(&value); }
    CodecBuffer(const CodecBuffer&) = delete;
    CodecBuffer& operator=(const CodecBuffer&) = delete;
    MzipCodecBuffer value{};
};

mdz::Bytes Deflate(const mdz::Bytes& data) {
    MzipCodecOptions options{};
    mzip_codec_options_init(&options);
    options.level = 6;
    options.threads = data.size() >= 4ull * 1024 * 1024 ? 0u : 1u;
    CodecBuffer compressed;
    const MzipCodecStatus status = mzip_deflate_alloc(
        data.data(), data.size(), &options, &compressed.value);
    if (status != MZIP_CODEC_OK) {
        throw std::runtime_error(std::string("DEFLATE compression failed: ") +
                                 mzip_codec_status_string(status));
    }
    return mdz::Bytes(compressed.value.data,
                      compressed.value.data + compressed.value.size);
}

}  // namespace

namespace mdz {

bool IsMdzPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    return extension == L".mdz";
}

bool IsSafeArchivePath(const std::string& path, bool allowDirectory) {
    if (path.empty() || path.size() > kMaximumPathBytes ||
        path.front() == '/' || path.find('\\') != std::string::npos ||
        !json::IsValidUtf8(path)) return false;
    if (!allowDirectory && path.back() == '/') return false;

    std::size_t segmentStart = 0;
    while (segmentStart < path.size()) {
        const std::size_t slash = path.find('/', segmentStart);
        const std::size_t segmentEnd = slash == std::string::npos
            ? path.size() : slash;
        if (segmentEnd == segmentStart) return false;
        const std::string_view segment(path.data() + segmentStart,
                                       segmentEnd - segmentStart);
        if (segment == "." || segment == ".." || segment.back() == '.' ||
            segment.back() == ' ') return false;
        for (const unsigned char value : segment) {
            if (value < 0x20 || value == 0x7F || value == '<' || value == '>' ||
                value == ':' || value == '"' || value == '|' || value == '?' ||
                value == '*') return false;
        }
        if (slash == std::string::npos) break;
        segmentStart = slash + 1;
        if (segmentStart == path.size()) return allowDirectory;
    }
    return true;
}

Package CreateDocument(const std::string& markdown, const std::string& title) {
    Package package;
    package.entryPoint = "index.md";
    package.entries.emplace(package.entryPoint, StringToBytes(markdown));
    const std::string manifest =
        "{\n  \"spec\": {\n    \"name\": \"mdzip-spec\",\n"
        "    \"version\": \"1.1.0\"\n  },\n  \"title\": " +
        json::Quote(title.empty() ? "Untitled" : title) +
        ",\n  \"mode\": \"document\",\n"
        "  \"entryPoint\": \"index.md\"\n}\n";
    package.entries.emplace("manifest.json", StringToBytes(manifest));
    return package;
}

bool ReadBytes(const std::string& bytes, Package* package,
               std::wstring* errorMessage, const std::string& password,
               ReadStatus* status) {
    if (!package) return false;
    if (status) *status = ReadStatus::Error;
    try {
        if (bytes.size() > kMaximumArchiveBytes) {
            throw std::runtime_error("MDZ archive exceeds the 512 MB safety limit");
        }
        const std::string_view view(bytes);
        const std::size_t endOffset = FindEndRecord(view);
        const std::uint16_t disk = Read16(view, endOffset + 4);
        const std::uint16_t centralDisk = Read16(view, endOffset + 6);
        const std::uint16_t diskCount = Read16(view, endOffset + 8);
        const std::uint16_t entryCount = Read16(view, endOffset + 10);
        const std::uint32_t centralSize = Read32(view, endOffset + 12);
        const std::uint32_t centralOffset = Read32(view, endOffset + 16);
        if (disk != 0 || centralDisk != 0 || diskCount != entryCount) {
            throw std::runtime_error("Multi-disk ZIP archives are not supported");
        }
        if (entryCount == 0xFFFF || centralSize == 0xFFFFFFFFu ||
            centralOffset == 0xFFFFFFFFu) {
            throw std::runtime_error("ZIP64 MDZ archives are not supported");
        }
        if (entryCount > kMaximumEntries ||
            centralOffset > endOffset || centralSize > endOffset - centralOffset) {
            throw std::runtime_error("Invalid ZIP central directory");
        }

        Package loaded;
        std::size_t cursor = centralOffset;
        std::size_t totalExpanded = 0;
        for (std::size_t index = 0; index < entryCount; ++index) {
            if (cursor > endOffset || endOffset - cursor < 46 ||
                Read32(view, cursor) != kCentralHeaderSignature) {
                throw std::runtime_error("Invalid ZIP central entry");
            }
            const std::uint16_t flags = Read16(view, cursor + 8);
            const std::uint16_t method = Read16(view, cursor + 10);
            const std::uint32_t crc = Read32(view, cursor + 16);
            const std::uint32_t compressedSize = Read32(view, cursor + 20);
            const std::uint32_t expandedSize = Read32(view, cursor + 24);
            const std::uint16_t nameLength = Read16(view, cursor + 28);
            const std::uint16_t extraLength = Read16(view, cursor + 30);
            const std::uint16_t commentLength = Read16(view, cursor + 32);
            const std::uint16_t startDisk = Read16(view, cursor + 34);
            const std::uint32_t localOffset = Read32(view, cursor + 42);
            const std::size_t recordLength = 46ull + nameLength +
                                             extraLength + commentLength;
            if (recordLength > endOffset - cursor || startDisk != 0) {
                throw std::runtime_error("Invalid ZIP central entry bounds");
            }
            const std::string name(view.substr(cursor + 46, nameLength));
            const std::string_view centralExtra = view.substr(
                cursor + 46 + nameLength, extraLength);
            const auto aes = ParseWinZipAesExtra(centralExtra);
            cursor += recordLength;

            const bool directory = !name.empty() && name.back() == '/';
            if (!IsSafeArchivePath(name, directory)) {
                throw std::runtime_error("MDZ contains an unsafe archive path");
            }
            const bool encrypted = (flags & kEncryptedFlag) != 0;
            if (encrypted && (!aes || method != kWinZipAesMethod)) {
                throw std::runtime_error(
                    "Traditional ZIP encryption is not supported in MDZ files");
            }
            if (!encrypted && (aes || method == kWinZipAesMethod)) {
                throw std::runtime_error("Invalid WinZip AES entry flags");
            }
            if (aes && (aes->strength != kWinZipAes256Strength ||
                        (aes->version != 1 &&
                         aes->version != kWinZipAesVersion))) {
                throw std::runtime_error(
                    "Only WinZip AES-256 AE-1/AE-2 MDZ entries are supported");
            }
            const std::uint16_t actualMethod = aes ? aes->actualMethod : method;
            if (actualMethod != kStoreMethod && actualMethod != kDeflateMethod) {
                throw std::runtime_error("MDZ contains an unsupported ZIP compression method");
            }
            if (encrypted && password.empty()) throw PasswordRequiredError();
            if (expandedSize > kMaximumEntryBytes ||
                totalExpanded > kMaximumTotalBytes - expandedSize) {
                throw std::runtime_error("MDZ expanded data exceeds the safety limit");
            }
            totalExpanded += expandedSize;
            if (compressedSize == 0xFFFFFFFFu || expandedSize == 0xFFFFFFFFu ||
                localOffset == 0xFFFFFFFFu || localOffset > centralOffset ||
                centralOffset - localOffset < 30 ||
                Read32(view, localOffset) != kLocalHeaderSignature) {
                throw std::runtime_error("Invalid ZIP local entry");
            }
            const std::uint16_t localNameLength = Read16(view, localOffset + 26);
            const std::uint16_t localExtraLength = Read16(view, localOffset + 28);
            const std::size_t dataOffset = static_cast<std::size_t>(localOffset) +
                                           30 + localNameLength + localExtraLength;
            if (dataOffset > centralOffset || compressedSize > centralOffset - dataOffset) {
                throw std::runtime_error("Invalid ZIP entry data bounds");
            }
            const std::string localName(view.substr(localOffset + 30,
                                                    localNameLength));
            if (localName != name) {
                throw std::runtime_error("ZIP local and central names differ");
            }
            if (Read16(view, localOffset + 6) != flags ||
                Read16(view, localOffset + 8) != method) {
                throw std::runtime_error("ZIP local and central entry fields differ");
            }
            const auto localAes = ParseWinZipAesExtra(view.substr(
                localOffset + 30 + localNameLength, localExtraLength));
            if (static_cast<bool>(localAes) != static_cast<bool>(aes) ||
                (aes && (localAes->version != aes->version ||
                         localAes->strength != aes->strength ||
                         localAes->actualMethod != aes->actualMethod))) {
                throw std::runtime_error("ZIP local and central AES fields differ");
            }
            if (directory) continue;
            if (!loaded.entries.emplace(name, Bytes{}).second) {
                throw std::runtime_error("MDZ contains duplicate entry paths");
            }
            Bytes& expanded = loaded.entries.at(name);
            expanded.resize(expandedSize);
            const auto* payload = reinterpret_cast<const std::uint8_t*>(
                bytes.data() + dataOffset);
            Bytes decrypted;
            const std::uint8_t* compressed = payload;
            std::size_t actualCompressedSize = compressedSize;
            if (encrypted) {
                if (compressedSize < 28) {
                    throw std::runtime_error("Invalid WinZip AES payload size");
                }
                try {
                    decrypted = fz::winzip_aes256_decrypt(
                        payload, compressedSize, password, fz::cpu_has_aesni());
                } catch (const std::exception&) {
                    throw IncorrectPasswordError();
                }
                compressed = decrypted.data();
                actualCompressedSize = decrypted.size();
            }
            if (actualMethod == kStoreMethod) {
                if (actualCompressedSize != expandedSize) {
                    throw std::runtime_error("Invalid stored ZIP entry size");
                }
                if (expandedSize) std::memcpy(expanded.data(), compressed, expandedSize);
            } else {
                const fz::InflateResult result = fz::inflate_raw_to_buffer(
                    compressed, actualCompressedSize, expandedSize, expanded.data());
                if (result.size != expandedSize) {
                    throw std::runtime_error("DEFLATE output size mismatch");
                }
            }
            if ((!aes || aes->version == 1) &&
                fz::Crc32::compute(expanded.data(), expanded.size()) != crc) {
                throw std::runtime_error("ZIP entry CRC32 mismatch");
            }
            if (IsMarkdownPath(name) &&
                !json::IsValidUtf8(BytesToString(expanded))) {
                throw std::runtime_error("MDZ Markdown entry is not valid UTF-8");
            }
        }
        if (cursor != static_cast<std::size_t>(centralOffset) + centralSize) {
            throw std::runtime_error("ZIP central directory size mismatch");
        }
        loaded.entryPoint = ResolveEntryPoint(loaded.entries);
        *package = std::move(loaded);
        if (status) *status = ReadStatus::Success;
        return true;
    } catch (const PasswordRequiredError& error) {
        if (status) *status = ReadStatus::PasswordRequired;
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    } catch (const IncorrectPasswordError& error) {
        if (status) *status = ReadStatus::IncorrectPassword;
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    } catch (const std::exception& error) {
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    }
}

bool BuildBytes(const Package& package, std::string* bytes,
                std::wstring* errorMessage, const std::string& password) {
    if (!bytes) return false;
    try {
        if (package.entries.empty() || package.entries.size() > kMaximumEntries ||
            package.entries.size() > 0xFFFF) {
            throw std::runtime_error("MDZ archive has an invalid entry count");
        }
        if (!IsSafeArchivePath(package.entryPoint) ||
            !IsMarkdownPath(package.entryPoint) ||
            package.entries.find(package.entryPoint) == package.entries.end()) {
            throw std::runtime_error("MDZ entry point is invalid or missing");
        }

        std::size_t total = 0;
        for (const auto& [name, data] : package.entries) {
            if (!IsSafeArchivePath(name)) {
                throw std::runtime_error("MDZ contains an unsafe archive path");
            }
            if (data.size() > kMaximumEntryBytes ||
                total > kMaximumTotalBytes - data.size() ||
                name.size() > 0xFFFF || data.size() > 0xFFFFFFFFu) {
                throw std::runtime_error("MDZ entry exceeds the safety limit");
            }
            total += data.size();
        }

        std::string result;
        result.reserve(total + package.entries.size() * 128 + 22);
        std::vector<CentralRecord> central;
        central.reserve(package.entries.size());
        const auto [dosTime, dosDate] = CurrentDosTime();
        for (const auto& [name, data] : package.entries) {
            if (result.size() > 0xFFFFFFFFu) {
                throw std::runtime_error("MDZ archive requires ZIP64");
            }
            CentralRecord record;
            record.name = name;
            record.encrypted = !password.empty();
            record.crc = record.encrypted
                ? 0u : fz::Crc32::compute(data.data(), data.size());
            const mdz::Bytes compressed = Deflate(data);
            const mdz::Bytes payload = record.encrypted
                ? fz::winzip_aes256_encrypt(
                    compressed.data(), compressed.size(), password,
                    fz::cpu_has_aesni())
                : compressed;
            if (payload.size() > 0xFFFFFFFFu) {
                throw std::runtime_error("Compressed MDZ entry requires ZIP64");
            }
            record.compressedSize = static_cast<std::uint32_t>(payload.size());
            record.expandedSize = static_cast<std::uint32_t>(data.size());
            record.offset = static_cast<std::uint32_t>(result.size());
            central.push_back(record);

            Append32(&result, kLocalHeaderSignature);
            Append16(&result, record.encrypted ? kWinZipAesRequiredVersion : 20);
            Append16(&result, static_cast<std::uint16_t>(
                kUtf8Flag | (record.encrypted ? kEncryptedFlag : 0)));
            Append16(&result, record.encrypted
                ? kWinZipAesMethod : kDeflateMethod);
            Append16(&result, dosTime);
            Append16(&result, dosDate);
            Append32(&result, record.crc);
            Append32(&result, record.compressedSize);
            Append32(&result, record.expandedSize);
            Append16(&result, static_cast<std::uint16_t>(name.size()));
            Append16(&result, record.encrypted ? 11 : 0);
            result.append(name);
            if (record.encrypted) AppendWinZipAesExtra(&result, kDeflateMethod);
            if (!payload.empty()) {
                result.append(reinterpret_cast<const char*>(payload.data()),
                              payload.size());
            }
        }

        if (result.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("MDZ archive requires ZIP64");
        }
        const std::uint32_t centralOffset = static_cast<std::uint32_t>(result.size());
        for (const CentralRecord& record : central) {
            Append32(&result, kCentralHeaderSignature);
            Append16(&result, record.encrypted ? kWinZipAesRequiredVersion : 20);
            Append16(&result, record.encrypted ? kWinZipAesRequiredVersion : 20);
            Append16(&result, static_cast<std::uint16_t>(
                kUtf8Flag | (record.encrypted ? kEncryptedFlag : 0)));
            Append16(&result, record.encrypted
                ? kWinZipAesMethod : kDeflateMethod);
            Append16(&result, dosTime);
            Append16(&result, dosDate);
            Append32(&result, record.crc);
            Append32(&result, record.compressedSize);
            Append32(&result, record.expandedSize);
            Append16(&result, static_cast<std::uint16_t>(record.name.size()));
            Append16(&result, record.encrypted ? 11 : 0);
            Append16(&result, 0);
            Append16(&result, 0);
            Append16(&result, 0);
            Append32(&result, 0);
            Append32(&result, record.offset);
            result.append(record.name);
            if (record.encrypted) AppendWinZipAesExtra(&result, kDeflateMethod);
        }
        if (result.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("MDZ archive requires ZIP64");
        }
        const std::uint32_t centralSize =
            static_cast<std::uint32_t>(result.size() - centralOffset);
        Append32(&result, kEndSignature);
        Append16(&result, 0);
        Append16(&result, 0);
        Append16(&result, static_cast<std::uint16_t>(central.size()));
        Append16(&result, static_cast<std::uint16_t>(central.size()));
        Append32(&result, centralSize);
        Append32(&result, centralOffset);
        Append16(&result, 0);
        if (result.size() > kMaximumArchiveBytes) {
            throw std::runtime_error("MDZ archive exceeds the 512 MB safety limit");
        }
        *bytes = std::move(result);
        return true;
    } catch (const std::exception& error) {
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    }
}

}  // namespace mdz
