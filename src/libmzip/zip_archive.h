// mzip archive library
// Author: tklee000
// Version: 0.7
#pragma once
#include "common.h"

namespace fz {

inline constexpr const char* kLibraryAuthor = "tklee000";
inline constexpr unsigned kLibraryVersionMajor = 0;
inline constexpr unsigned kLibraryVersionMinor = 7;
inline constexpr unsigned kLibraryVersionPatch = 0;
inline constexpr const char* kLibraryVersion = "0.7";

enum class ProgressStage {
    Compressing,
    Extracting,
    Verifying
};

struct ProgressInfo {
    ProgressStage stage = ProgressStage::Compressing;
    uint64_t completed_bytes = 0;
    uint64_t total_bytes = 0;
    uint64_t completed_files = 0;
    uint64_t total_files = 0;
    std::string current_entry;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

enum class CompressionPolicy {
    // Use Store when compression is disabled or the Deflate payload is not smaller.
    Auto,
    AlwaysStore,
    AlwaysDeflate
};

enum class DirectoryEntryPolicy {
    Include,
    // Only file entries are written. Empty directories and their metadata are lost.
    Omit
};

struct ArchiveOptions {
    int level = 5;
    size_t chunk_size = 1u * 1024u * 1024u;
    bool allow_avx2 = true;
    bool allow_aesni = true;
    bool verify_after_create = true;
    bool verbose = true;
    std::string password; // Non-empty enables WinZip AES-256 (AE-2).
    std::vector<std::filesystem::path> excluded_paths;
    ProgressCallback progress;
    CancellationCallback cancel;
    // ZIP-only policies. Other archive formats ignore these fields.
    CompressionPolicy compression = CompressionPolicy::Auto;
    DirectoryEntryPolicy directories = DirectoryEntryPolicy::Include;
    // When non-empty, ZIP entry names are relative to this directory. Every input
    // must resolve inside it; the root directory itself is never emitted as an entry.
    std::filesystem::path entry_root;
};

struct ExtractOptions {
    std::filesystem::path output_directory = std::filesystem::path(".");
    // When every entry is below this root, omit it from extracted paths.
    // Explorer's "Extract to <name>" uses this; CLI and GUI extraction leave it empty.
    std::filesystem::path strip_root_directory;
    bool overwrite = false;
    bool allow_aesni = true;
    bool verbose = true;
    bool show_summary = true;
    std::string password;
    ProgressCallback progress;
    CancellationCallback cancel;
};

struct ZipEntryInfo {
    std::string name;
    std::string method;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    bool directory = false;
    bool encrypted = false;
};

class VerificationError : public std::runtime_error {
public:
    VerificationError(std::string message,
                      std::filesystem::path report_path,
                      std::filesystem::path temporary_path)
        : std::runtime_error(std::move(message)),
          report_path_(std::move(report_path)),
          temporary_path_(std::move(temporary_path)) {}

    const std::filesystem::path& report_path() const noexcept { return report_path_; }
    const std::filesystem::path& temporary_path() const noexcept { return temporary_path_; }

private:
    std::filesystem::path report_path_;
    std::filesystem::path temporary_path_;
};

void create_zip(const std::filesystem::path& output,
                const std::vector<std::filesystem::path>& inputs,
                const ArchiveOptions& options);

void list_zip(const std::filesystem::path& archive);

std::vector<ZipEntryInfo> inspect_zip(const std::filesystem::path& archive);

void extract_zip(const std::filesystem::path& archive,
                 const ExtractOptions& options);

} // namespace fz
