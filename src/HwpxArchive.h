#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hwpx {

using Bytes = std::vector<std::uint8_t>;

struct Image {
    std::string id;
    std::string mediaType;
    std::string extension;
    Bytes bytes;
};

struct Document {
    std::string title;
    std::string author;
    std::string sectionXml;
    std::string previewText;
    bool sansSerif = false;
    std::vector<Image> images;
};

constexpr const char* kMimeType = "application/hwp+zip";

bool BuildBytes(const Document& document, std::string* bytes,
                std::wstring* errorMessage);

}  // namespace hwpx
