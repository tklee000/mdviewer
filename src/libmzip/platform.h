#pragma once
#include "common.h"

namespace fz {

class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* data() const noexcept { return data_; }
    uint64_t size() const noexcept { return size_; }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

class WritableMappedFile {
public:
    WritableMappedFile(const std::filesystem::path& path, uint64_t size);
    ~WritableMappedFile();

    WritableMappedFile(const WritableMappedFile&) = delete;
    WritableMappedFile& operator=(const WritableMappedFile&) = delete;

    uint8_t* data() const noexcept { return data_; }
    uint64_t size() const noexcept { return size_; }
    // Persist mapped writes before the file is atomically installed. The
    // destructor only releases the view and handles and cannot report errors.
    void flush();

private:
    uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

bool cpu_has_avx2();
bool cpu_has_aesni();
void replace_file_atomically(const std::filesystem::path& replacement,
                             const std::filesystem::path& target,
                             bool overwrite = true);
std::pair<uint16_t, uint16_t> file_dos_datetime(const std::filesystem::path& path);
std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path utf8_to_path(const std::string& text);
std::filesystem::path filesystem_access_path(const std::filesystem::path& path);

} // namespace fz
