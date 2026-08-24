#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

class UiResourceProvider;

struct PdfPrintSettings {
    double paperWidthMillimeters = 210.0;
    double paperHeightMillimeters = 297.0;
    // Applied by the print stylesheet so the full PDF page remains opaque.
    double marginMillimeters = 20.0;
    bool landscape = false;
    bool printBackground = true;
    bool pageNumbers = false;
    // One-based ranges accepted by Chromium, for example "1-3,5".
    // Empty prints every page.
    std::string pageRanges;
};

class BrowserHostDelegate {
public:
    virtual ~BrowserHostDelegate() = default;
    virtual void OnBrowserCreated() = 0;
    virtual void OnBrowserMessage(const std::string& message) = 0;
    virtual void OnFilesDropped(const std::vector<std::wstring>& paths) = 0;
    virtual void OnBrowserLoadError(const std::wstring& message) = 0;
    virtual void OnPdfPrintFinished(std::uint64_t requestId,
                                    const std::wstring& path,
                                    bool success) = 0;
};

// Platform-neutral boundary. Future Linux and macOS shells can keep the
// application/document layer independent of CEF types.
class BrowserHost {
public:
    virtual ~BrowserHost() = default;
    virtual bool Create(void* nativeParent, const std::string& initialUrl) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void SendJson(const std::string& json) = 0;
    virtual bool PrintToPdf(std::uint64_t requestId,
                            const std::wstring& path,
                            const PdfPrintSettings& settings) = 0;
    virtual void Close() = 0;
    virtual bool WaitForClose(int timeoutMilliseconds) = 0;
};

std::unique_ptr<BrowserHost> CreateCefBrowserHost(
    std::shared_ptr<UiResourceProvider> resources,
    BrowserHostDelegate* delegate);
