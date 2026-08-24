#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace docx {

using Bytes = std::vector<std::uint8_t>;

struct Image {
    std::string id;
    std::string mediaType;
    std::string extension;
    Bytes bytes;
};

struct Hyperlink {
    std::string id;
    std::string target;
};

struct ListDefinition {
    bool ordered = false;
    std::uint32_t start = 1;
};

struct Document {
    std::string title;
    std::string author;
    std::string documentXml;
    bool sansSerif = true;
    std::vector<Image> images;
    std::vector<Hyperlink> hyperlinks;
    std::vector<ListDefinition> lists;
};

constexpr const char* kMimeType =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document";

bool BuildBytes(const Document& document, std::string* bytes,
                std::wstring* errorMessage);

}  // namespace docx
