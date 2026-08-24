#include "platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>
#else
#include <cerrno>
#include <cpuid.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fz {

MappedFile::MappedFile(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto access_path = filesystem_access_path(path);
    HANDLE file = CreateFileW(access_path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open input file: " + path_to_utf8(path));
    }
    file_ = file;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        file_ = nullptr;
        throw std::runtime_error("Cannot query input file size: " + path_to_utf8(path));
    }
    size_ = static_cast<uint64_t>(size.QuadPart);
    if (size_ == 0) {
        return;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        file_ = nullptr;
        throw std::runtime_error("Cannot map input file: " + path_to_utf8(path));
    }
    mapping_ = mapping;
    data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data_) {
        CloseHandle(mapping);
        CloseHandle(file);
        mapping_ = nullptr;
        file_ = nullptr;
        throw std::runtime_error("Cannot map input view: " + path_to_utf8(path));
    }
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot open input file: " + path_to_utf8(path));
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0 || st.st_size < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot query input file size: " + path_to_utf8(path));
    }
    size_ = static_cast<uint64_t>(st.st_size);
    if (size_ == 0) {
        return;
    }
    void* mapped = ::mmap(nullptr, static_cast<size_t>(size_), PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot map input file: " + path_to_utf8(path));
    }
    data_ = static_cast<const uint8_t*>(mapped);
#endif
}

MappedFile::~MappedFile() {
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (data_) ::munmap(const_cast<uint8_t*>(data_), static_cast<size_t>(size_));
    if (fd_ >= 0) ::close(fd_);
#endif
}

WritableMappedFile::WritableMappedFile(const std::filesystem::path& path,
                                       uint64_t size) : size_(size) {
#ifdef _WIN32
    if (size > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()))
        throw std::runtime_error("Mapped output file is too large");
    const auto access_path = filesystem_access_path(path);
    HANDLE file = CreateFileW(access_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create mapped output file: " +
                                 path_to_utf8(path));
    file_ = file;
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
        CloseHandle(file);
        file_ = nullptr;
        DeleteFileW(access_path.c_str());
        throw std::runtime_error("Cannot size mapped output file: " +
                                 path_to_utf8(path));
    }
    if (size == 0) return;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE,
                                        0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        file_ = nullptr;
        DeleteFileW(access_path.c_str());
        throw std::runtime_error("Cannot map output file: " + path_to_utf8(path));
    }
    mapping_ = mapping;
    data_ = static_cast<uint8_t*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (!data_) {
        CloseHandle(mapping);
        CloseHandle(file);
        mapping_ = nullptr;
        file_ = nullptr;
        DeleteFileW(access_path.c_str());
        throw std::runtime_error("Cannot map output view: " + path_to_utf8(path));
    }
#else
    if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("Mapped output file is too large");
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd_ < 0)
        throw std::runtime_error("Cannot create mapped output file: " +
                                 path_to_utf8(path));
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        ::close(fd_);
        fd_ = -1;
        ::unlink(path.c_str());
        throw std::runtime_error("Cannot size mapped output file: " +
                                 path_to_utf8(path));
    }
    if (size == 0) return;
    void* mapped = ::mmap(nullptr, static_cast<size_t>(size),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        ::unlink(path.c_str());
        throw std::runtime_error("Cannot map output file: " + path_to_utf8(path));
    }
    data_ = static_cast<uint8_t*>(mapped);
#endif
}

WritableMappedFile::~WritableMappedFile() {
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (data_) ::munmap(data_, static_cast<size_t>(size_));
    if (fd_ >= 0) ::close(fd_);
#endif
}

void WritableMappedFile::flush() {
#ifdef _WIN32
    if (data_ && !FlushViewOfFile(data_, 0)) {
        throw std::runtime_error("Cannot flush mapped output view (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
    if (file_ && !FlushFileBuffers(static_cast<HANDLE>(file_))) {
        throw std::runtime_error("Cannot flush mapped output file (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    if (data_ && size_ != 0) {
        int result;
        do {
            result = ::msync(data_, static_cast<size_t>(size_), MS_SYNC);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            throw std::runtime_error("Cannot flush mapped output view: " +
                                     std::strerror(errno));
        }
    }
    if (fd_ >= 0) {
        int result;
        do {
            result = ::fsync(fd_);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            throw std::runtime_error("Cannot flush mapped output file: " +
                                     std::strerror(errno));
        }
    }
#endif
}

bool cpu_has_aesni() {
#ifdef _WIN32
    int regs[4]{};
    __cpuid(regs, 0);
    if (regs[0] < 1) return false;
    __cpuidex(regs, 1, 0);
    return (regs[2] & (1 << 25)) != 0; // CPUID.01H:ECX.AES[bit 25]
#else
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx & bit_AES) != 0;
#endif
}

bool cpu_has_avx2() {
#ifdef _WIN32
    int regs[4]{};
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuidex(regs, 1, 0);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    const unsigned __int64 xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
#else
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid_max(0, nullptr)) return false;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    const bool osxsave = (ecx & bit_OSXSAVE) != 0;
    const bool avx = (ecx & bit_AVX) != 0;
    if (!osxsave || !avx) return false;
    uint32_t xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6) != 0x6) return false;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
    return (ebx & bit_AVX2) != 0;
#endif
}

void replace_file_atomically(const std::filesystem::path& replacement,
                             const std::filesystem::path& target,
                             bool overwrite) {
#ifdef _WIN32
    const auto replacement_path = filesystem_access_path(replacement);
    const auto target_path = filesystem_access_path(target);
    if (!overwrite) {
        if (!MoveFileExW(replacement_path.c_str(), target_path.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot install output file (Windows error " +
                                     std::to_string(GetLastError()) + ")");
        }
        return;
    }
    const DWORD attributes = GetFileAttributesW(target_path.c_str());
    BOOL replaced = FALSE;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ReplaceFileW(target_path.c_str(), replacement_path.c_str(), nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    } else if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
        replaced = MoveFileExW(replacement_path.c_str(), target_path.c_str(), MOVEFILE_WRITE_THROUGH);
    }
    if (!replaced) {
        throw std::runtime_error("Cannot replace output file (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    std::error_code ec;
    if (!overwrite) {
        // Creating the target hard link is atomic and fails if a path appeared
        // after the caller's validation. The temporary file is in the same
        // directory, so it is necessarily on the same file system.
        std::filesystem::create_hard_link(replacement, target, ec);
        if (ec)
            throw std::runtime_error("Cannot install output file: " + ec.message());
        std::filesystem::remove(replacement, ec);
        if (ec)
            throw std::runtime_error("Cannot remove temporary output file: " +
                                     ec.message());
        return;
    }
    std::filesystem::rename(replacement, target, ec);
    if (ec) throw std::runtime_error("Cannot replace output file: " + ec.message());
#endif
}

std::pair<uint16_t, uint16_t> file_dos_datetime(const std::filesystem::path& path) {
    std::time_t tt = std::time(nullptr);
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        const auto now_file = std::filesystem::file_time_type::clock::now();
        const auto now_sys = std::chrono::system_clock::now();
        const auto sys_time = now_sys + (ft - now_file);
        tt = std::chrono::system_clock::to_time_t(sys_time);
    }

    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    int year = std::clamp(local.tm_year + 1900, 1980, 2107);
    int month = std::clamp(local.tm_mon + 1, 1, 12);
    int day = std::clamp(local.tm_mday, 1, 31);
    int hour = std::clamp(local.tm_hour, 0, 23);
    int minute = std::clamp(local.tm_min, 0, 59);
    int second = std::clamp(local.tm_sec, 0, 59);

    const uint16_t dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) | day);
    const uint16_t dos_time = static_cast<uint16_t>((hour << 11) | (minute << 5) | (second / 2));
    return {dos_date, dos_time};
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    std::wstring w = path.native();
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        w = L"\\\\" + w.substr(8);
    } else if (w.rfind(L"\\\\?\\", 0) == 0) {
        w.erase(0, 4);
    }
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    return path.u8string();
#endif
}

std::filesystem::path utf8_to_path(const std::string& text) {
#ifdef _WIN32
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), out.data(), n) != n) return {};
    return std::filesystem::path(out);
#else
    return std::filesystem::u8path(text);
#endif
}

std::filesystem::path filesystem_access_path(const std::filesystem::path& path) {
#ifdef _WIN32
    if (path.empty()) return path;
    std::wstring native = path.native();
    if (native.rfind(L"\\\\?\\", 0) == 0 || native.rfind(L"\\\\.\\", 0) == 0) return path;

    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) return path;
    native = absolute.native();
    if (native.rfind(L"\\\\", 0) == 0)
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
    return std::filesystem::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

} // namespace fz
