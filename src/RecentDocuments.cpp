#include "RecentDocuments.h"

#include "Json.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>

namespace {

constexpr size_t kMaximumRecentDocuments = 10;
constexpr wchar_t kRecentDocumentsMutex[] =
    L"Local\\MdViewer.RecentDocuments.Store";

class StoreLock {
public:
    StoreLock() : mutex_(CreateMutexW(nullptr, FALSE, kRecentDocumentsMutex)) {
        if (mutex_) {
            const DWORD result = WaitForSingleObject(mutex_, 5000);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~StoreLock() {
        if (locked_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    explicit operator bool() const { return locked_; }
private:
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return value;
}

std::wstring NormalizedLocalPath(const std::wstring& value) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(value, error);
    return error ? std::filesystem::path(value).lexically_normal().wstring()
                 : absolute.lexically_normal().wstring();
}

std::int64_t NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> JsonObjects(const std::string& text) {
    std::vector<std::string> result;
    size_t start = std::string::npos;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '{') {
            if (depth++ == 0) start = index;
        } else if (character == '}' && depth > 0) {
            if (--depth == 0 && start != std::string::npos) {
                result.push_back(text.substr(start, index - start + 1));
                start = std::string::npos;
            }
        }
    }
    return result;
}

bool SameEntry(const RecentDocument& left, const RecentDocument& right) {
    if (left.kind != right.kind) return false;
    if (left.kind == RecentDocumentKind::GoogleDrive) {
        return left.location == right.location;
    }
    return Lower(left.location) == Lower(right.location);
}

}  // namespace

RecentDocuments::RecentDocuments(
    const std::filesystem::path& applicationDirectory)
    : path_(applicationDirectory / L"recent-documents.json") {}

void RecentDocuments::Load() {
    StoreLock lock;
    if (!lock) return;
    LoadFromDisk();
}

void RecentDocuments::LoadFromDisk() {
    items_.clear();
    std::ifstream input(path_, std::ios::binary);
    if (!input) return;
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() > 1024 * 1024) return;
    for (const auto& object : JsonObjects(bytes)) {
        const std::string kind = json::GetString(object, "kind").value_or("");
        const std::string location =
            json::GetString(object, "location").value_or("");
        const std::string name = json::GetString(object, "name").value_or("");
        if ((kind != "local" && kind != "googleDrive") || location.empty()) {
            continue;
        }
        RecentDocument item;
        item.kind = kind == "googleDrive" ? RecentDocumentKind::GoogleDrive
                                          : RecentDocumentKind::Local;
        item.location = json::Utf8ToWide(location, false);
        item.name = json::Utf8ToWide(name, false);
        item.lastOpened = json::GetInteger(object, "lastOpened").value_or(0);
        if (item.location.empty()) continue;
        if (item.kind == RecentDocumentKind::Local) {
            item.location = NormalizedLocalPath(item.location);
            if (item.name.empty()) {
                item.name = std::filesystem::path(item.location).filename().wstring();
            }
        } else if (item.name.empty()) {
            item.name = item.location;
        }
        const auto duplicate = std::find_if(
            items_.begin(), items_.end(),
            [&](const RecentDocument& existing) { return SameEntry(existing, item); });
        if (duplicate == items_.end()) items_.push_back(std::move(item));
    }
    std::stable_sort(items_.begin(), items_.end(),
                     [](const auto& left, const auto& right) {
                         return left.lastOpened > right.lastOpened;
                     });
    if (items_.size() > kMaximumRecentDocuments) {
        items_.resize(kMaximumRecentDocuments);
    }
}

bool RecentDocuments::AddLocal(const std::wstring& path) {
    if (path.empty()) return false;
    const std::wstring normalized = NormalizedLocalPath(path);
    return Add({RecentDocumentKind::Local, normalized,
                std::filesystem::path(normalized).filename().wstring(),
                NowMilliseconds()});
}

bool RecentDocuments::AddGoogleDrive(const std::wstring& fileId,
                                     const std::wstring& name) {
    if (fileId.empty()) return false;
    return Add({RecentDocumentKind::GoogleDrive, fileId,
                name.empty() ? fileId : name, NowMilliseconds()});
}

bool RecentDocuments::Add(RecentDocument entry) {
    StoreLock lock;
    if (!lock) return false;
    LoadFromDisk();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [&](const RecentDocument& existing) { return SameEntry(existing, entry); }),
        items_.end());
    items_.insert(items_.begin(), std::move(entry));
    if (items_.size() > kMaximumRecentDocuments) {
        items_.resize(kMaximumRecentDocuments);
    }
    return Save();
}

bool RecentDocuments::Remove(RecentDocumentKind kind,
                             const std::wstring& location) {
    StoreLock lock;
    if (!lock) return false;
    LoadFromDisk();
    RecentDocument target{kind, location, {}, 0};
    const size_t previousSize = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [&](const RecentDocument& existing) { return SameEntry(existing, target); }),
        items_.end());
    return previousSize == items_.size() || Save();
}

std::string RecentDocuments::ToJson() const {
    std::string output = "[";
    for (size_t index = 0; index < items_.size(); ++index) {
        if (index) output += ',';
        const auto& item = items_[index];
        output += "{\"kind\":" + json::Quote(
            item.kind == RecentDocumentKind::GoogleDrive ? "googleDrive" : "local");
        output += ",\"location\":" + json::Quote(json::WideToUtf8(item.location));
        output += ",\"name\":" + json::Quote(json::WideToUtf8(item.name));
        output += ",\"lastOpened\":" + std::to_string(item.lastOpened) + '}';
    }
    output += ']';
    return output;
}

bool RecentDocuments::Save() const {
    std::error_code directoryError;
    std::filesystem::create_directories(path_.parent_path(), directoryError);
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const std::string bytes = ToJson();
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return false;
    return MoveFileExW(temporary.c_str(), path_.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
