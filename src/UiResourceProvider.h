#pragma once

#include "MdzArchive.h"

#include <memory>
#include <string>
#include <vector>

struct UiResource {
    int statusCode = 404;
    std::string statusText = "Not Found";
    std::string mimeType = "text/plain";
    std::string charset = "utf-8";
    std::vector<unsigned char> bytes;
};

class UiResourceProvider {
public:
    virtual ~UiResourceProvider() = default;
    virtual UiResource Load(const std::string& url) const = 0;
    virtual void SetDocumentDirectory(const std::wstring& directory) = 0;
    virtual void SetDocumentArchive(
        std::shared_ptr<const mdz::Entries> entries,
        const std::string& entryPoint) = 0;
    virtual void SetPdfPreview(
        std::shared_ptr<const std::vector<unsigned char>> bytes) = 0;
};

std::shared_ptr<UiResourceProvider> CreatePlatformUiResourceProvider(
    void* nativeApplicationHandle);
