#include "BrowserHost.h"

#include "UiResourceProvider.h"

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_process_message.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_handler.h"
#include "include/cef_response.h"
#include "include/cef_scheme.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr char kUiOrigin[] = "https://app.mdviewer/";
constexpr char kPdfViewerOrigin[] =
    "chrome-extension://mhjfbmdgcfjbbpaeojofohoefgiehjai/";
constexpr char kNativeMessageName[] = "MdViewer.NativeMessage";
constexpr char kHostMessageName[] = "MdViewer.HostMessage";

bool IsApplicationUrl(const std::string& url) {
    return url.compare(0, sizeof(kUiOrigin) - 1, kUiOrigin) == 0;
}

bool IsPdfViewerUrl(const std::string& url) {
    return url.compare(0, sizeof(kPdfViewerOrigin) - 1, kPdfViewerOrigin) == 0;
}

class PdfPrintCallback final : public CefPdfPrintCallback {
public:
    using Completion = std::function<void(const std::wstring&, bool)>;

    explicit PdfPrintCallback(Completion completion)
        : completion_(std::move(completion)) {}

    void OnPdfPrintFinished(const CefString& path, bool success) override {
        Completion completion = std::move(completion_);
        if (completion) completion(path.ToWString(), success);
    }

private:
    Completion completion_;
    IMPLEMENT_REFCOUNTING(PdfPrintCallback);
};

class MemoryResourceHandler final : public CefResourceHandler {
public:
    explicit MemoryResourceHandler(UiResource resource)
        : resource_(std::move(resource)) {}

    bool Open(CefRefPtr<CefRequest> request,
              bool& handleRequest,
              CefRefPtr<CefCallback> callback) override {
        handleRequest = true;
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response,
                            int64_t& responseLength,
                            CefString& redirectUrl) override {
        response->SetStatus(resource_.statusCode);
        response->SetStatusText(resource_.statusText);
        response->SetMimeType(resource_.mimeType);
        response->SetCharset(resource_.charset);
        response->SetHeaderByName("Cache-Control", "no-store", true);
        response->SetHeaderByName("X-Content-Type-Options", "nosniff", true);
        response->SetHeaderByName(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' "
            "'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
            "font-src 'self'; object-src 'self'; "
            "frame-src 'self' chrome-extension:; "
            "base-uri 'none'; form-action 'none'",
            true);
        responseLength = static_cast<int64_t>(resource_.bytes.size());
    }

    bool Skip(int64_t bytesToSkip,
              int64_t& bytesSkipped,
              CefRefPtr<CefResourceSkipCallback> callback) override {
        const size_t remaining = resource_.bytes.size() - offset_;
        const size_t skipped = (std::min)(
            remaining, static_cast<size_t>((std::max)(int64_t{0}, bytesToSkip)));
        offset_ += skipped;
        bytesSkipped = static_cast<int64_t>(skipped);
        return skipped > 0;
    }

    bool Read(void* dataOut,
              int bytesToRead,
              int& bytesRead,
              CefRefPtr<CefResourceReadCallback> callback) override {
        bytesRead = 0;
        if (!dataOut || bytesToRead <= 0 || offset_ >= resource_.bytes.size()) {
            return false;
        }
        const size_t count = (std::min)(resource_.bytes.size() - offset_,
                                        static_cast<size_t>(bytesToRead));
        std::memcpy(dataOut, resource_.bytes.data() + offset_, count);
        offset_ += count;
        bytesRead = static_cast<int>(count);
        return true;
    }

    void Cancel() override {}

private:
    UiResource resource_;
    size_t offset_ = 0;
    IMPLEMENT_REFCOUNTING(MemoryResourceHandler);
};

class ApplicationSchemeFactory final : public CefSchemeHandlerFactory {
public:
    explicit ApplicationSchemeFactory(std::shared_ptr<UiResourceProvider> provider)
        : provider_(std::move(provider)) {}

    CefRefPtr<CefResourceHandler> Create(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& schemeName,
        CefRefPtr<CefRequest> request) override {
        if (!request) return nullptr;
        return new MemoryResourceHandler(provider_->Load(request->GetURL().ToString()));
    }

private:
    std::shared_ptr<UiResourceProvider> provider_;
    IMPLEMENT_REFCOUNTING(ApplicationSchemeFactory);
};

class BrowserClient final : public CefClient,
                            public CefLifeSpanHandler,
                            public CefLoadHandler,
                            public CefContextMenuHandler,
                            public CefDragHandler,
                            public CefKeyboardHandler,
                            public CefRequestHandler {
public:
    explicit BrowserClient(BrowserHostDelegate* delegate) : delegate_(delegate) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        bool closeRequested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            creationPending_ = false;
            browser_ = browser;
            browserWindow_ = browser->GetHost()->GetWindowHandle();
            closeRequested = closeRequested_;
        }
        if (closeRequested) {
            browser->GetHost()->CloseBrowser(true);
        } else if (auto* delegate = delegate_.load()) {
            delegate->OnBrowserCreated();
        }
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popupId,
                       const CefString& targetUrl,
                       const CefString& targetFrameName,
                       CefLifeSpanHandler::WindowOpenDisposition targetDisposition,
                       bool userGesture,
                       const CefPopupFeatures& popupFeatures,
                       CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extraInfo,
                       bool* noJavascriptAccess) override {
        return true;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            creationPending_ = false;
            browser_ = nullptr;
            browserWindow_ = nullptr;
            closed_ = true;
        }
        closedCondition_.notify_all();
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override {
        if (!frame || !frame->IsMain() || errorCode == ERR_ABORTED) return;
        if (auto* delegate = delegate_.load()) {
            delegate->OnBrowserLoadError(errorText.ToWString());
        }
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        if (model) model->Clear();
    }

    bool OnDragEnter(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefDragData> dragData,
                     DragOperationsMask mask) override {
        std::vector<std::wstring> paths;
        if (dragData && dragData->IsFile()) {
            std::vector<CefString> cefPaths;
            if (dragData->GetFilePaths(cefPaths)) {
                paths.reserve(cefPaths.size());
                for (const auto& path : cefPaths) {
                    if (!path.empty()) paths.push_back(path.ToWString());
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingDropPaths_ = std::move(paths);
            pendingDropTime_ = std::chrono::steady_clock::now();
        }
        return false;
    }

    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                       const CefKeyEvent& event,
                       CefEventHandle osEvent,
                       bool* isKeyboardShortcut) override {
        if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
            return false;
        }
        const bool control = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
        const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
        const int key = event.windows_key_code;
        return key == VK_F12 || (control && key == 'U') ||
            (control && shift && (key == 'I' || key == 'J' || key == 'C'));
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool userGesture,
                        bool isRedirect) override {
        if (!request) return true;
        const std::string url = request->GetURL().ToString();
        return url != "about:blank" && !IsApplicationUrl(url) &&
            !IsPdfViewerUrl(url);
    }

    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          const CefString& targetUrl,
                          CefRequestHandler::WindowOpenDisposition targetDisposition,
                          bool userGesture) override {
        return true;
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message) override {
        if (!message || message->GetName() != kNativeMessageName || !frame ||
            !frame->IsMain() || !IsApplicationUrl(frame->GetURL().ToString())) {
            return false;
        }
        const std::string messageJson =
            message->GetArgumentList()->GetString(0).ToString();
        if (messageJson.find("\"type\":\"files.dropped\"") != std::string::npos) {
            std::vector<std::wstring> paths;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (std::chrono::steady_clock::now() - pendingDropTime_ <=
                    std::chrono::seconds(10)) {
                    paths = std::move(pendingDropPaths_);
                }
                pendingDropPaths_.clear();
            }
            if (!paths.empty()) {
                if (auto* delegate = delegate_.load()) {
                    delegate->OnFilesDropped(paths);
                }
            }
            return true;
        }
        if (auto* delegate = delegate_.load()) {
            delegate->OnBrowserMessage(messageJson);
        }
        return true;
    }

    void Resize(int width, int height) {
        HWND window = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window = browserWindow_;
        }
        if (window) {
            SetWindowPos(window, nullptr, 0, 0, (std::max)(1, width),
                         (std::max)(1, height), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void SendJson(const std::string& json) {
        CefRefPtr<CefBrowser> browser;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            browser = browser_;
        }
        if (!browser) return;
        auto message = CefProcessMessage::Create(kHostMessageName);
        message->GetArgumentList()->SetString(0, json);
        browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
    }

    bool PrintToPdf(std::uint64_t requestId,
                    const std::wstring& path,
                    const PdfPrintSettings& settings) {
        CefRefPtr<CefBrowser> browser;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            browser = browser_;
        }
        if (!browser || path.empty()) return false;

        constexpr double millimetersPerInch = 25.4;
        CefPdfPrintSettings printSettings;
        printSettings.landscape = settings.landscape ? 1 : 0;
        printSettings.print_background = settings.printBackground ? 1 : 0;
        printSettings.scale = 1.0;
        printSettings.paper_width =
            settings.paperWidthMillimeters / millimetersPerInch;
        printSettings.paper_height =
            settings.paperHeightMillimeters / millimetersPerInch;
        printSettings.prefer_css_page_size = 0;
        // The print stylesheet paints cloned per-page padding. Keeping native
        // margins at zero avoids transparent page edges in Chromium's viewer.
        printSettings.margin_type = PDF_PRINT_MARGIN_CUSTOM;
        printSettings.margin_top = 0.0;
        printSettings.margin_right = 0.0;
        printSettings.margin_bottom = 0.0;
        printSettings.margin_left = 0.0;
        printSettings.display_header_footer = settings.pageNumbers ? 1 : 0;
        if (settings.pageNumbers) {
            CefString(&printSettings.header_template) =
                "<span style='font-size:1px'></span>";
            CefString(&printSettings.footer_template) =
                "<div style='box-sizing:border-box;width:100%;padding:0 12mm;"
                "color:#667085;font:9px Segoe UI,sans-serif;text-align:center'>"
                "<span class='pageNumber'></span> / "
                "<span class='totalPages'></span></div>";
        }
        printSettings.generate_tagged_pdf = 1;
        printSettings.generate_document_outline = 1;

        CefRefPtr<BrowserClient> keepAlive(this);
        browser->GetHost()->PrintToPDF(
            path, printSettings,
            new PdfPrintCallback(
                [keepAlive, requestId](const std::wstring& finishedPath,
                                       bool success) {
                    if (auto* delegate = keepAlive->delegate_.load()) {
                        delegate->OnPdfPrintFinished(
                            requestId, finishedPath, success);
                    }
                }));
        return true;
    }

    void Close() {
        CefRefPtr<CefBrowser> browser;
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closeRequested_ = true;
            browser = browser_;
            if (!browser && !creationPending_) {
                closed_ = true;
                notify = true;
            }
        }
        if (browser) browser->GetHost()->CloseBrowser(true);
        if (notify) closedCondition_.notify_all();
    }

    void BeginBrowserCreation() {
        std::lock_guard<std::mutex> lock(mutex_);
        creationPending_ = true;
        closed_ = false;
    }

    void BrowserCreationFailed() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            creationPending_ = false;
            closed_ = true;
        }
        closedCondition_.notify_all();
    }

    void DetachDelegate() { delegate_ = nullptr; }

    bool WaitForClose(int timeoutMilliseconds) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) return true;
        return closedCondition_.wait_for(
            lock, std::chrono::milliseconds(timeoutMilliseconds),
            [this] { return closed_; });
    }

private:
    std::atomic<BrowserHostDelegate*> delegate_;
    std::mutex mutex_;
    std::condition_variable closedCondition_;
    CefRefPtr<CefBrowser> browser_;
    HWND browserWindow_ = nullptr;
    std::vector<std::wstring> pendingDropPaths_;
    std::chrono::steady_clock::time_point pendingDropTime_{};
    bool creationPending_ = false;
    bool closeRequested_ = false;
    bool closed_ = false;
    IMPLEMENT_REFCOUNTING(BrowserClient);
};

class CefBrowserView final : public BrowserHost {
public:
    CefBrowserView(std::shared_ptr<UiResourceProvider> resources,
                   BrowserHostDelegate* delegate)
        : resources_(std::move(resources)), client_(new BrowserClient(delegate)) {}

    ~CefBrowserView() override {
        client_->DetachDelegate();
        Close();
    }

    bool Create(void* nativeParent, const std::string& initialUrl) override {
        if (!nativeParent || !resources_) return false;
        if (!CefRegisterSchemeHandlerFactory(
                "https", "app.mdviewer",
                new ApplicationSchemeFactory(resources_))) return false;

        RECT bounds{};
        GetClientRect(static_cast<HWND>(nativeParent), &bounds);
        CefWindowInfo windowInfo;
        windowInfo.SetAsChild(
            static_cast<HWND>(nativeParent),
            CefRect(0, 0, (std::max)(1L, bounds.right - bounds.left),
                    (std::max)(1L, bounds.bottom - bounds.top)));
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

        CefBrowserSettings settings;
        settings.background_color = CefColorSetARGB(255, 18, 18, 18);
        client_->BeginBrowserCreation();
        const bool created = CefBrowserHost::CreateBrowser(
            windowInfo, client_, initialUrl, settings, nullptr, nullptr);
        if (!created) client_->BrowserCreationFailed();
        return created;
    }

    void Resize(int width, int height) override { client_->Resize(width, height); }
    void SendJson(const std::string& json) override { client_->SendJson(json); }
    bool PrintToPdf(std::uint64_t requestId,
                    const std::wstring& path,
                    const PdfPrintSettings& settings) override {
        return client_->PrintToPdf(requestId, path, settings);
    }
    void Close() override {
        if (closeStarted_.exchange(true)) return;
        client_->Close();
    }
    bool WaitForClose(int timeoutMilliseconds) override {
        return client_->WaitForClose(timeoutMilliseconds);
    }

private:
    std::shared_ptr<UiResourceProvider> resources_;
    CefRefPtr<BrowserClient> client_;
    std::atomic<bool> closeStarted_{false};
};

}  // namespace

std::unique_ptr<BrowserHost> CreateCefBrowserHost(
    std::shared_ptr<UiResourceProvider> resources,
    BrowserHostDelegate* delegate) {
    return std::make_unique<CefBrowserView>(std::move(resources), delegate);
}
