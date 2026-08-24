#include "PortableCef.h"

#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_process_message.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_handler.h"
#include "include/cef_response.h"
#include "include/cef_scheme.h"
#include "include/cef_v8.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace mdviewer {
namespace {

constexpr char kUiOrigin[] = "https://app.mdviewer/";
constexpr char kNativeMessageName[] = "MdViewer.NativeMessage";
constexpr char kHostMessageName[] = "MdViewer.HostMessage";

bool IsApplicationUrl(const std::string& url) {
    return url.compare(0, sizeof(kUiOrigin) - 1U, kUiOrigin) == 0;
}

bool IsApplicationFrame(CefRefPtr<CefFrame> frame) {
    return frame && frame->IsMain() && IsApplicationUrl(frame->GetURL().ToString());
}

class NativePostHandler final : public CefV8Handler {
public:
    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> object,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& returnValue,
                 CefString& exception) override {
        if (name != "postMessage") return false;
        if (arguments.size() != 1U || !arguments[0]->IsString()) {
            exception = "postMessage expects one JSON string.";
            return true;
        }
        const auto context = CefV8Context::GetCurrentContext();
        const auto frame = context ? context->GetFrame() : nullptr;
        if (!IsApplicationFrame(frame)) {
            exception = "Native messaging is available only to the MdViewer UI.";
            return true;
        }
        const auto message = CefProcessMessage::Create(kNativeMessageName);
        message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
        frame->SendProcessMessage(PID_BROWSER, message);
        returnValue = CefV8Value::CreateBool(true);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(NativePostHandler);
};

class PortableApplication final : public CefApp,
                                  public CefBrowserProcessHandler,
                                  public CefRenderProcessHandler {
public:
    explicit PortableApplication(PortableCefDelegate* delegate) : delegate_(delegate) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& processType,
                                       CefRefPtr<CefCommandLine> commandLine) override {
        commandLine->AppendSwitch("disable-dev-tools");
        commandLine->AppendSwitch("disable-extensions");
        commandLine->AppendSwitch("disable-component-update");
        commandLine->AppendSwitch("disable-pinch");
        commandLine->AppendSwitch("no-first-run");
        commandLine->AppendSwitchWithValue(
            "disable-features", "MediaRouter,Translate,OptimizationHints");
#if defined(__linux__)
        // Ubuntu 22.04 supports both X11 and Wayland. GTK3 embeds the CEF child
        // window through X11/XWayland for the first portable release.
        commandLine->AppendSwitchWithValue("ozone-platform", "x11");
        if (std::getenv("WSL_INTEROP") || std::getenv("WSL_DISTRO_NAME")) {
            // WSLg exposes an accelerated graphics stack, but some Mesa/D3D12
            // combinations crash CEF's GPU subprocess. Software compositing is
            // stable and still renders through WSLg's XWayland server.
            commandLine->AppendSwitch("disable-gpu");
            commandLine->AppendSwitch("disable-gpu-compositing");
        }
#endif
    }

    void OnContextInitialized() override {
        if (delegate_) delegate_->OnCefContextInitialized();
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (!IsApplicationFrame(frame) || !context) return;
        auto bridge = CefV8Value::CreateObject(nullptr, nullptr);
        bridge->SetValue(
            "postMessage",
            CefV8Value::CreateFunction("postMessage", new NativePostHandler()),
            V8_PROPERTY_ATTRIBUTE_READONLY);
        const auto attributes = static_cast<CefV8Value::PropertyAttribute>(
            V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
        context->GetGlobal()->SetValue("mdViewerNative", bridge, attributes);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId sourceProcess,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (!message || message->GetName() != kHostMessageName || !IsApplicationFrame(frame)) {
            return false;
        }
        const std::string json = message->GetArgumentList()->GetString(0).ToString();
        const std::string script =
            "window.dispatchEvent(new CustomEvent('mdviewerhostmessage',{detail:" + json + "}));";
        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        return true;
    }

private:
    PortableCefDelegate* delegate_ = nullptr;
    IMPLEMENT_REFCOUNTING(PortableApplication);
};

class MemoryResourceHandler final : public CefResourceHandler {
public:
    explicit MemoryResourceHandler(UiResource resource) : resource_(std::move(resource)) {}

    bool Open(CefRefPtr<CefRequest> request,
              bool& handleRequest,
              CefRefPtr<CefCallback> callback) override {
        handleRequest = true;
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response,
                            int64_t& responseLength,
                            CefString& redirectUrl) override {
        response->SetStatus(200);
        const auto separator = resource_.mimeType.find(';');
        response->SetMimeType(resource_.mimeType.substr(0, separator));
        if (separator != std::string::npos) response->SetCharset("utf-8");
        response->SetHeaderByName("Cache-Control", "no-store", true);
        response->SetHeaderByName("X-Content-Type-Options", "nosniff", true);
        response->SetHeaderByName(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; connect-src 'self'; font-src 'self'; "
            "object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'",
            true);
        responseLength = static_cast<int64_t>(resource_.bytes.size());
    }

    bool Skip(int64_t bytesToSkip,
              int64_t& bytesSkipped,
              CefRefPtr<CefResourceSkipCallback> callback) override {
        const std::size_t remaining = resource_.bytes.size() - offset_;
        const std::size_t skipped = (std::min)(
            remaining, static_cast<std::size_t>((std::max)(int64_t{0}, bytesToSkip)));
        offset_ += skipped;
        bytesSkipped = static_cast<int64_t>(skipped);
        return skipped > 0U;
    }

    bool Read(void* dataOut,
              int bytesToRead,
              int& bytesRead,
              CefRefPtr<CefResourceReadCallback> callback) override {
        bytesRead = 0;
        if (!dataOut || bytesToRead <= 0 || offset_ >= resource_.bytes.size()) return false;
        const std::size_t count = (std::min)(resource_.bytes.size() - offset_,
                                             static_cast<std::size_t>(bytesToRead));
        std::memcpy(dataOut, resource_.bytes.data() + offset_, count);
        offset_ += count;
        bytesRead = static_cast<int>(count);
        return true;
    }

    void Cancel() override {}

private:
    UiResource resource_;
    std::size_t offset_ = 0U;
    IMPLEMENT_REFCOUNTING(MemoryResourceHandler);
};

class ApplicationSchemeFactory final : public CefSchemeHandlerFactory {
public:
    explicit ApplicationSchemeFactory(std::shared_ptr<FileResourceProvider> provider)
        : provider_(std::move(provider)) {}

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         const CefString& schemeName,
                                         CefRefPtr<CefRequest> request) override {
        if (!request) return nullptr;
        const auto resource = provider_->Load(request->GetURL().ToString());
        return resource ? new MemoryResourceHandler(*resource) : nullptr;
    }

private:
    std::shared_ptr<FileResourceProvider> provider_;
    IMPLEMENT_REFCOUNTING(ApplicationSchemeFactory);
};

class PortableBrowserClient final : public CefClient,
                                    public CefLifeSpanHandler,
                                    public CefLoadHandler,
                                    public CefContextMenuHandler,
                                    public CefKeyboardHandler,
                                    public CefRequestHandler {
public:
    explicit PortableBrowserClient(PortableCefDelegate* delegate) : delegate_(delegate) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (delegate_) delegate_->OnBrowserCreated(browser);
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
        if (delegate_) delegate_->OnBrowserClosed();
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override {
        if (frame && frame->IsMain() && errorCode != ERR_ABORTED && delegate_) {
            delegate_->OnBrowserLoadError(errorText.ToString());
        }
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        if (model) model->Clear();
    }

    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                       const CefKeyEvent& event,
                       CefEventHandle osEvent,
                       bool* isKeyboardShortcut) override {
        if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) return false;
        const bool control = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
        const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
        const int key = event.windows_key_code;
        return key == 0x7B || (control && key == 'U') ||
               (control && shift && (key == 'I' || key == 'J' || key == 'C'));
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool userGesture,
                        bool isRedirect) override {
        if (!request) return true;
        const std::string url = request->GetURL().ToString();
        return url != "about:blank" && !IsApplicationUrl(url);
    }

    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          const CefString& targetUrl,
                          CefRequestHandler::WindowOpenDisposition targetDisposition,
                          bool userGesture) override {
        return true;
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId sourceProcess,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (!message || message->GetName() != kNativeMessageName || !IsApplicationFrame(frame)) {
            return false;
        }
        if (delegate_) {
            delegate_->OnBrowserMessage(message->GetArgumentList()->GetString(0).ToString());
        }
        return true;
    }

private:
    PortableCefDelegate* delegate_ = nullptr;
    IMPLEMENT_REFCOUNTING(PortableBrowserClient);
};

}  // namespace

CefRefPtr<CefApp> CreatePortableCefApp(PortableCefDelegate* delegate) {
    return new PortableApplication(delegate);
}

CefRefPtr<CefClient> CreatePortableCefClient(PortableCefDelegate* delegate) {
    return new PortableBrowserClient(delegate);
}

bool RegisterPortableResourceScheme(std::shared_ptr<FileResourceProvider> provider) {
    return CefRegisterSchemeHandlerFactory(
        "https", "app.mdviewer", new ApplicationSchemeFactory(std::move(provider)));
}

void SendPortableJson(CefRefPtr<CefBrowser> browser, const std::string& json) {
    if (!browser) return;
    const auto message = CefProcessMessage::Create(kHostMessageName);
    message->GetArgumentList()->SetString(0, json);
    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
}

}  // namespace mdviewer
