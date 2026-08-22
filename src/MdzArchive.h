#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mdz {

using Bytes = std::vector<std::uint8_t>;
using Entries = std::map<std::string, Bytes>;

struct Package {
    Entries entries;
    std::string entryPoint;
};

constexpr const char* kMimeType = "application/vnd.mdzip";

bool IsMdzPath(const std::wstring& path);
bool IsSafeArchivePath(const std::string& path, bool allowDirectory = false);

Package CreateDocument(const std::string& markdown,
                       const std::string& title = {});

bool ReadBytes(const std::string& bytes, Package* package,
               std::wstring* errorMessage);
bool BuildBytes(const Package& package, std::string* bytes,
                std::wstring* errorMessage);

}  // namespace mdz
