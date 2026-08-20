#pragma once

#include <memory>
#include <string>

class UiResourceProvider;

class BrowserHostDelegate {
public:
    virtual ~BrowserHostDelegate() = default;
    virtual void OnBrowserCreated() = 0;
    virtual void OnBrowserMessage(const std::string& message) = 0;
    virtual void OnBrowserLoadError(const std::wstring& message) = 0;
};

// Platform-neutral boundary. Future Linux and macOS shells can keep the
// application/document layer independent of CEF types.
class BrowserHost {
public:
    virtual ~BrowserHost() = default;
    virtual bool Create(void* nativeParent, const std::string& initialUrl) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void SendJson(const std::string& json) = 0;
    virtual void Close() = 0;
    virtual bool WaitForClose(int timeoutMilliseconds) = 0;
};

std::unique_ptr<BrowserHost> CreateCefBrowserHost(
    std::shared_ptr<UiResourceProvider> resources,
    BrowserHostDelegate* delegate);
