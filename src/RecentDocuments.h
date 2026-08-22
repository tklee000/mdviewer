#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class RecentDocumentKind {
    Local,
    GoogleDrive,
};

struct RecentDocument {
    RecentDocumentKind kind = RecentDocumentKind::Local;
    std::wstring location;
    std::wstring name;
    std::int64_t lastOpened = 0;
};

class RecentDocuments {
public:
    explicit RecentDocuments(const std::filesystem::path& applicationDirectory);

    void Load();
    bool AddLocal(const std::wstring& path);
    bool AddGoogleDrive(const std::wstring& fileId, const std::wstring& name);
    bool Remove(RecentDocumentKind kind, const std::wstring& location);
    const std::vector<RecentDocument>& Items() const { return items_; }
    std::string ToJson() const;
    const std::filesystem::path& Path() const { return path_; }

private:
    void LoadFromDisk();
    bool Add(RecentDocument entry);
    bool Save() const;

    std::filesystem::path path_;
    std::vector<RecentDocument> items_;
};
