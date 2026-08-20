#pragma once

#include "PortableCore.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"

#include <memory>
#include <string>

namespace mdviewer {

class PortableCefDelegate {
public:
    virtual ~PortableCefDelegate() = default;

    virtual void OnCefContextInitialized() = 0;
    virtual void OnBrowserCreated(CefRefPtr<CefBrowser> browser) = 0;
    virtual void OnBrowserClosed() = 0;
    virtual void OnBrowserMessage(const std::string& json) = 0;
    virtual void OnBrowserLoadError(const std::string& error) = 0;
};

CefRefPtr<CefApp> CreatePortableCefApp(PortableCefDelegate* delegate);
CefRefPtr<CefClient> CreatePortableCefClient(PortableCefDelegate* delegate);
bool RegisterPortableResourceScheme(std::shared_ptr<FileResourceProvider> provider);
void SendPortableJson(CefRefPtr<CefBrowser> browser, const std::string& json);

}  // namespace mdviewer
