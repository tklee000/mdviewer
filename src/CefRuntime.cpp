#include "CefRuntime.h"

#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace {

constexpr char kUiOrigin[] = "https://app.mdviewer/";
constexpr char kNativeMessageName[] = "MdViewer.NativeMessage";
constexpr char kHostMessageName[] = "MdViewer.HostMessage";

bool IsApplicationFrame(CefRefPtr<CefFrame> frame) {
    if (!frame || !frame->IsMain()) return false;
    const std::string url = frame->GetURL().ToString();
    return url.compare(0, sizeof(kUiOrigin) - 1, kUiOrigin) == 0;
}

class NativePostHandler final : public CefV8Handler {
public:
    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> object,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& returnValue,
                 CefString& exception) override {
        if (name != "postMessage") return false;
        if (arguments.size() != 1 || !arguments[0]->IsString()) {
            exception = "postMessage expects one JSON string.";
            return true;
        }
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        if (!IsApplicationFrame(frame)) {
            exception = "Native messaging is available only to the application UI.";
            return true;
        }
        CefRefPtr<CefProcessMessage> message =
            CefProcessMessage::Create(kNativeMessageName);
        message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
        frame->SendProcessMessage(PID_BROWSER, message);
        returnValue = CefV8Value::CreateBool(true);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(NativePostHandler);
};

class MdViewerCefApplication final : public CefApp,
                                     public CefBrowserProcessHandler,
                                     public CefRenderProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    void OnBeforeCommandLineProcessing(
        const CefString& processType,
        CefRefPtr<CefCommandLine> commandLine) override {
        commandLine->AppendSwitch("disable-dev-tools");
        commandLine->AppendSwitch("disable-extensions");
        commandLine->AppendSwitch("disable-component-update");
        commandLine->AppendSwitch("disable-pinch");
        commandLine->AppendSwitch("no-first-run");
        commandLine->AppendSwitchWithValue(
            "disable-features", "MediaRouter,Translate,OptimizationHints");
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (!IsApplicationFrame(frame) || !context) return;
        CefRefPtr<CefV8Value> bridge = CefV8Value::CreateObject(nullptr, nullptr);
        bridge->SetValue(
            "postMessage",
            CefV8Value::CreateFunction("postMessage", new NativePostHandler()),
            V8_PROPERTY_ATTRIBUTE_READONLY);
        const auto attributes = static_cast<CefV8Value::PropertyAttribute>(
            V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
        context->GetGlobal()->SetValue("mdViewerNative", bridge, attributes);
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message) override {
        if (!message || message->GetName() != kHostMessageName ||
            !IsApplicationFrame(frame)) return false;
        const std::string json =
            message->GetArgumentList()->GetString(0).ToString();
        const std::string script =
            "window.dispatchEvent(new CustomEvent('mdviewerhostmessage',"
            "{detail:" + json + "}));";
        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(MdViewerCefApplication);
};

std::atomic<bool> gInitialized{false};

}  // namespace

namespace cef_runtime {

int ExecuteSubprocess(void* nativeApplicationHandle) {
    CefMainArgs arguments(static_cast<HINSTANCE>(nativeApplicationHandle));
    return CefExecuteProcess(arguments, new MdViewerCefApplication(), nullptr);
}

bool Initialize(void* nativeApplicationHandle,
                const std::wstring& userDataPath,
                const std::wstring& locale,
                std::wstring* errorMessage) {
    CefMainArgs arguments(static_cast<HINSTANCE>(nativeApplicationHandle));
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = true;
    settings.log_severity = LOGSEVERITY_DISABLE;
    CefString(&settings.cache_path) = userDataPath;
    CefString(&settings.root_cache_path) = userDataPath;
    CefString(&settings.locale) = locale;
    CefString(&settings.accept_language_list) = locale + L",en-US,en";
    if (!CefInitialize(arguments, settings,
                       new MdViewerCefApplication(), nullptr)) {
        if (errorMessage) {
            *errorMessage = L"CEF initialization failed (exit code " +
                std::to_wstring(CefGetExitCode()) + L").";
        }
        return false;
    }
    gInitialized = true;
    return true;
}

void Shutdown(bool browserClosedCleanly) {
    if (!gInitialized.exchange(false)) return;
    if (!browserClosedCleanly) return;
    CefClearSchemeHandlerFactories();
    CefShutdown();
}

}  // namespace cef_runtime
