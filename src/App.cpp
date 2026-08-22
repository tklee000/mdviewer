#include "App.h"

#include "FileAssociation.h"
#include "Json.h"
#include "Localization.h"
#include "UiResourceProvider.h"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"MdViewer.MainWindow";
constexpr UINT kUiTaskMessage = WM_APP + 41;
constexpr UINT_PTR kFileWatchTimer = 1;
constexpr DWORD kEncodingGroupControlId = 2000;
constexpr DWORD kEncodingComboControlId = 2001;
constexpr DWORD kMainWindowStyle =
    WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
    WS_SYSMENU | WS_CLIPCHILDREN;

std::wstring FileName(const std::wstring& path) {
    return path.empty() ? std::wstring{} : std::filesystem::path(path).filename().wstring();
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

std::string NormalizeLineEndings(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\r') {
            if (index + 1 < input.size() && input[index + 1] == '\n') ++index;
            output.push_back('\n');
        } else {
            output.push_back(input[index]);
        }
    }
    return output;
}

std::string ApplyLineEndings(const std::string& input, bool crlf) {
    if (!crlf) return input;
    std::string output;
    output.reserve(input.size() + input.size() / 20);
    for (char character : input) {
        if (character == '\n') output.push_back('\r');
        output.push_back(character);
    }
    return output;
}

std::wstring EncodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Ansi: return L"ANSI";
    case TextEncoding::Utf8Bom: return L"UTF-8 BOM";
    case TextEncoding::Utf16Le: return L"UTF-16 LE";
    case TextEncoding::Utf16Be: return L"UTF-16 BE";
    case TextEncoding::Utf8:
    default: return L"UTF-8";
    }
}

DWORD EncodingItemId(TextEncoding encoding) {
    return static_cast<DWORD>(encoding) + 1;
}

TextEncoding EncodingFromItemId(DWORD itemId, TextEncoding fallback) {
    if (itemId < EncodingItemId(TextEncoding::Ansi) ||
        itemId > EncodingItemId(TextEncoding::Utf16Be)) return fallback;
    return static_cast<TextEncoding>(itemId - 1);
}

bool EncodeText(const std::string& utf8, TextEncoding encoding,
                std::string* output) {
    if (!output) return false;
    if (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf8Bom) {
        *output = utf8;
        if (encoding == TextEncoding::Utf8Bom) output->insert(0, "\xEF\xBB\xBF", 3);
        return true;
    }

    const std::wstring wide = json::Utf8ToWide(utf8, false);
    if (!utf8.empty() && wide.empty()) return false;
    if (encoding == TextEncoding::Ansi) {
        if (wide.empty()) {
            output->clear();
            return true;
        }
        BOOL usedDefault = FALSE;
        const int size = WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, &usedDefault);
        if (size <= 0 || usedDefault) return false;
        output->assign(static_cast<size_t>(size), '\0');
        usedDefault = FALSE;
        const int converted = WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(),
            static_cast<int>(wide.size()), output->data(), size,
            nullptr, &usedDefault);
        return converted == size && !usedDefault;
    }

    output->clear();
    output->reserve(2 + wide.size() * 2);
    const bool littleEndian = encoding == TextEncoding::Utf16Le;
    output->push_back(static_cast<char>(littleEndian ? 0xFF : 0xFE));
    output->push_back(static_cast<char>(littleEndian ? 0xFE : 0xFF));
    for (wchar_t character : wide) {
        const auto unit = static_cast<std::uint16_t>(character);
        const char low = static_cast<char>(unit & 0xFF);
        const char high = static_cast<char>((unit >> 8) & 0xFF);
        output->push_back(littleEndian ? low : high);
        output->push_back(littleEndian ? high : low);
    }
    return true;
}

std::wstring LastErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length)
                                           : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

bool WriteAll(HANDLE file, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>((std::min<size_t>)(
            bytes.size() - offset, 4 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr) ||
            written != request) return false;
        offset += written;
    }
    return true;
}

std::wstring BuildFilter(const std::wstring& markdownLabel,
                         const std::wstring& allLabel) {
    std::wstring filter = markdownLabel + L" (*.md;*.markdown)";
    filter.push_back(L'\0');
    filter += L"*.md;*.markdown";
    filter.push_back(L'\0');
    filter += allLabel + L" (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

}  // namespace

const wchar_t* MdViewerWindowClassName() {
    return kWindowClassName;
}

DesktopApp::DesktopApp(HINSTANCE instance, std::wstring initialFile)
    : instance_(instance), initialFile_(std::move(initialFile)),
      config_(configStore_.Load()) {}

DesktopApp::~DesktopApp() {
    alive_ = false;
    browserHost_.reset();
}

int DesktopApp::Run(int showCommand) {
    if (!RegisterWindowClass() || !CreateMainWindow(showCommand)) return 1;

    const std::wstring executablePath = CurrentExecutablePath();
    if (!executablePath.empty() &&
        CheckAndRepairMarkdownAssociation(executablePath) ==
            FileAssociationState::NeedsUserConsent) {
        const int answer = MessageBoxW(
            window_,
            Localized(L"MdViewer is not currently associated with Markdown files. Do you want to use MdViewer to open .md and .markdown files?").c_str(),
            Localized(L"Markdown file association").c_str(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (answer == IDYES) {
            const FileAssociationResult result =
                AssociateMarkdownFiles(executablePath);
            if (!result.success) {
                ShowError(
                    Localized(L"The Markdown file association could not be updated."),
                    Localized(L"File association error"));
            } else if (result.needsSystemConfirmation) {
                OpenDefaultAppsSettings(window_);
            }
        }
    }

    resources_ = CreatePlatformUiResourceProvider(instance_);
    if (!initialFile_.empty()) {
        if (!OpenDocument(initialFile_, false)) NewDocument();
    } else {
        NewDocument();
    }
    InitializeBrowser();
    startupComplete_ = true;
    if (closeRequested_) {
        if (browserHost_ && browserCreated_) BeginBrowserClose();
        else if (!browserHost_) DestroyWindow(window_);
    }
    if (window_ && IsWindow(window_)) {
        SetTimer(window_, kFileWatchTimer, 2000, nullptr);
        if (!closeRequested_) {
            ShowWindow(window_, config_.maximized ? SW_MAXIMIZE : showCommand);
            UpdateWindow(window_);
        }
    }

    MSG message{};
    BOOL messageResult = 0;
    while ((messageResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return messageResult == -1 ? 1 : 0;
}

bool DesktopApp::ShutdownBrowser() {
    alive_ = false;
    if (!browserHost_) return true;
    browserHost_->Close();
    const bool closed = browserHost_->WaitForClose(5000);
    browserHost_.reset();
    return closed;
}

bool DesktopApp::RegisterWindowClass() {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    windowClass.lpszClassName = kWindowClassName;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool DesktopApp::CreateMainWindow(int showCommand) {
    RECT bounds{0, 0, config_.windowWidth, config_.windowHeight};
    AdjustWindowRectEx(&bounds, kMainWindowStyle, FALSE, 0);
    window_ = CreateWindowExW(
        0, kWindowClassName, L"MdViewer", kMainWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    ApplyNativeTheme();
    return true;
}

LRESULT CALLBACK DesktopApp::WindowProcedure(HWND window, UINT message,
                                              WPARAM wParam, LPARAM lParam) {
    DesktopApp* app = reinterpret_cast<DesktopApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<DesktopApp*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->HandleWindowMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT DesktopApp::HandleWindowMessage(UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        ResizeBrowser();
        SendWindowState();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 640;
        limits->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COPYDATA: {
        const auto* copyData = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!copyData || copyData->dwData != kMdViewerOpenFileMessage ||
            !copyData->lpData || copyData->cbData < sizeof(wchar_t) ||
            copyData->cbData > 64 * 1024 ||
            copyData->cbData % sizeof(wchar_t) != 0) return FALSE;
        const size_t count = copyData->cbData / sizeof(wchar_t);
        const auto* value = static_cast<const wchar_t*>(copyData->lpData);
        if (value[count - 1] != L'\0') return FALSE;
        const std::wstring path(value);
        const bool openedInNewWindow = !path.empty() &&
            OpenExternalDocuments({path}, true);
        return openedInNewWindow ? kMdViewerOpenedNewWindowResult : TRUE;
    }
    case WM_TIMER:
        if (wParam == kFileWatchTimer) CheckExternalFileChange();
        return 0;
    case kUiTaskMessage: {
        std::unique_ptr<std::function<void()>> callback(
            reinterpret_cast<std::function<void()>*>(lParam));
        if (callback && alive_) (*callback)();
        return 0;
    }
    case WM_QUERYENDSESSION:
        // Returning FALSE keeps Windows from ending the session while this
        // window owns unsaved Markdown changes. Windows displays the block
        // reason registered by UpdateShutdownProtection().
        return document_.dirty ? FALSE : TRUE;
    case WM_ENDSESSION:
        return 0;
    case WM_CLOSE:
        if (browserCanClose_) {
            DestroyWindow(window_);
            return 0;
        }
        if (closeRequested_ || !ConfirmSaveChanges()) return 0;
        closeRequested_ = true;
        if (browserHost_ && browserCreated_) {
            BeginBrowserClose();
        } else if (startupComplete_) {
            if (!browserHost_) DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY:
        alive_ = false;
        KillTimer(window_, kFileWatchTimer);
        ShutdownBlockReasonDestroy(window_);
        SaveWindowState();
        browserReady_ = false;
        if (browserHost_) browserHost_->Close();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void DesktopApp::InitializeBrowser() {
    browserHost_ = CreateCefBrowserHost(resources_, this);
    const std::string url = "https://app.mdviewer/index.html?lang=" +
        json::WideToUtf8(config_.language) + "&theme=" +
        json::WideToUtf8(config_.theme);
    if (!browserHost_ || !browserHost_->Create(window_, url)) {
        browserHost_.reset();
        ShowError(Localized(L"The embedded browser view could not be created."),
                  Localized(L"MdViewer startup error"));
    }
}

void DesktopApp::ResizeBrowser() {
    if (!browserHost_ || !window_) return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    browserHost_->Resize(bounds.right - bounds.left, bounds.bottom - bounds.top);
}

void DesktopApp::BeginBrowserClose() {
    if (browserCanClose_) return;
    browserCanClose_ = true;
    PostMessageW(window_, WM_CLOSE, 0, 0);
}

void DesktopApp::UpdateWindowTitle() {
    const std::wstring name = document_.path.empty()
        ? Localized(L"Untitled") : FileName(document_.path);
    const std::wstring title = name + (document_.dirty ? L" *" : L"") +
        L" — MdViewer";
    SetWindowTextW(window_, title.c_str());
    UpdateShutdownProtection();
}

void DesktopApp::UpdateShutdownProtection() {
    if (!window_) return;
    if (document_.dirty) {
        const std::wstring reason = Localized(L"Unsaved changes");
        ShutdownBlockReasonCreate(window_, reason.c_str());
    } else {
        ShutdownBlockReasonDestroy(window_);
    }
}

void DesktopApp::SaveWindowState() {
    if (!window_) return;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(window_, &placement)) {
        config_.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
        config_.windowWidth = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
        config_.windowHeight = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    }
    configStore_.Save(config_);
}

void DesktopApp::PostToUi(std::function<void()> callback) {
    if (!alive_ || !window_) return;
    auto* allocated = new std::function<void()>(std::move(callback));
    if (!PostMessageW(window_, kUiTaskMessage, 0,
                      reinterpret_cast<LPARAM>(allocated))) delete allocated;
}

void DesktopApp::OnBrowserCreated() {
    PostToUi([this] {
        browserCreated_ = true;
        ResizeBrowser();
        if (closeRequested_) BeginBrowserClose();
    });
}

void DesktopApp::OnBrowserMessage(const std::string& message) {
    PostToUi([this, message] { HandleBrowserMessage(message); });
}

void DesktopApp::OnFilesDropped(const std::vector<std::wstring>& paths) {
    PostToUi([this, paths] { OpenExternalDocuments(paths, false); });
}

void DesktopApp::OnBrowserLoadError(const std::wstring& message) {
    PostToUi([this, message] {
        ShowError(Localized(L"The embedded browser could not load the application UI.") +
                      L"\n" + message,
                  Localized(L"MdViewer startup error"));
    });
}

void DesktopApp::HandleBrowserMessage(const std::string& message) {
    const std::string type = json::GetString(message, "type").value_or("");
    if (type == "ready") {
        browserReady_ = true;
        SendDocumentState();
        SendWindowState();
    } else if (type == "document.changed") {
        const std::string text = json::GetString(message, "text").value_or(document_.text);
        bool changed = false;
        if (text != document_.text) {
            document_.text = text;
            changed = true;
        }
        if (const auto eol = json::GetString(message, "eol")) {
            const bool crlf = *eol == "CRLF";
            if ((*eol == "LF" || *eol == "CRLF") && crlf != document_.crlf) {
                document_.crlf = crlf;
                changed = true;
            }
        }
        const auto reportedDirty = json::GetBool(message, "dirty");
        if (reportedDirty || changed) {
            document_.dirty = reportedDirty.value_or(true);
            UpdateWindowTitle();
        }
        editorMode_ = json::GetString(message, "mode").value_or(editorMode_);
    } else if (type == "editor.modeChanged") {
        editorMode_ = json::GetString(message, "mode").value_or(editorMode_);
    } else if (type == "command") {
        const std::string name = json::GetString(message, "name").value_or("");
        if (name == "file.new") NewDocument();
        else if (name == "file.open") ChooseAndOpenDocument();
        else if (name == "file.save") SaveDocument();
        else if (name == "file.saveAs") SaveDocumentAs();
        else if (name == "app.exit" || name == "window.close") {
            SendMessageW(window_, WM_CLOSE, 0, 0);
        } else if (name == "app.about") {
            MessageBoxW(window_,
                Localized(L"MdViewer\n\nEdit Markdown source or the rendered preview in one desktop application.").c_str(),
                Localized(L"About MdViewer").c_str(),
                MB_OK | MB_ICONINFORMATION);
        } else if (name == "window.minimize") {
            ShowWindow(window_, SW_MINIMIZE);
        } else if (name == "window.maximizeToggle") {
            ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (name == "window.drag") {
            POINT cursor{};
            GetCursorPos(&cursor);
            ReleaseCapture();
            SendMessageW(window_, WM_NCLBUTTONDOWN, HTCAPTION,
                         MAKELPARAM(cursor.x, cursor.y));
        }
    } else if (type == "settings.languageChanged") {
        SetLanguage(json::Utf8ToWide(
            json::GetString(message, "locale").value_or("ko-KR")));
    } else if (type == "settings.themeChanged") {
        SetTheme(json::Utf8ToWide(
            json::GetString(message, "theme").value_or("dark")));
    } else if (type == "openExternal") {
        const std::string url = json::GetString(message, "url").value_or("");
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 ||
            url.rfind("mailto:", 0) == 0) {
            ShellExecuteW(window_, L"open", json::Utf8ToWide(url).c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

void DesktopApp::SendJson(const std::string& value) {
    if (browserHost_ && browserReady_) browserHost_->SendJson(value);
}

void DesktopApp::SendDocumentState(const char* type) {
    if (!browserReady_) return;
    const std::string path = json::WideToUtf8(document_.path);
    const std::string name = json::WideToUtf8(
        document_.path.empty() ? Localized(L"Untitled") : FileName(document_.path));
    SendJson("{\"type\":" + json::Quote(type) +
             ",\"language\":" + json::Quote(json::WideToUtf8(config_.language)) +
             ",\"theme\":" + json::Quote(json::WideToUtf8(config_.theme)) +
             ",\"mode\":" + json::Quote(editorMode_) +
             ",\"document\":{\"path\":" + json::Quote(path) +
             ",\"name\":" + json::Quote(name) +
             ",\"text\":" + json::Quote(document_.text) +
             ",\"dirty\":" + (document_.dirty ? "true" : "false") +
             ",\"encoding\":" + json::Quote(json::WideToUtf8(EncodingName(document_.encoding))) +
             ",\"eol\":" + json::Quote(document_.crlf ? "CRLF" : "LF") + "}}");
}

void DesktopApp::SendWindowState() {
    if (!browserReady_ || !window_) return;
    SendJson(std::string("{\"type\":\"window.stateChanged\",\"maximized\":") +
             (IsZoomed(window_) ? "true}" : "false}"));
}

void DesktopApp::SetLanguage(const std::wstring& language) {
    const std::wstring normalized = NormalizeAppLanguage(language);
    if (normalized == config_.language) return;
    config_.language = normalized;
    configStore_.Save(config_);
    UpdateWindowTitle();
    SendJson("{\"type\":\"language.changed\",\"locale\":" +
             json::Quote(json::WideToUtf8(config_.language)) + "}");
}

void DesktopApp::SetTheme(const std::wstring& theme) {
    const std::wstring normalized = theme == L"light" ? L"light" : L"dark";
    if (normalized == config_.theme) return;
    config_.theme = normalized;
    configStore_.Save(config_);
    ApplyNativeTheme();
    SendJson("{\"type\":\"theme.changed\",\"theme\":" +
             json::Quote(json::WideToUtf8(config_.theme)) + "}");
}

void DesktopApp::ApplyNativeTheme() {
    if (!window_) return;
    const BOOL dark = config_.theme == L"dark" ? TRUE : FALSE;
    constexpr DWORD useImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(window_, useImmersiveDarkMode,
                                     &dark, sizeof(dark)))) {
        constexpr DWORD legacyUseImmersiveDarkMode = 19;
        DwmSetWindowAttribute(window_, legacyUseImmersiveDarkMode,
                              &dark, sizeof(dark));
    }
    RedrawWindow(window_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

bool DesktopApp::ConfirmSaveChanges() {
    if (!document_.dirty) return true;
    const std::string name = json::WideToUtf8(
        document_.path.empty() ? Localized(L"Untitled") : FileName(document_.path));
    const std::wstring message = Localized(
        "Do you want to save the changes to {name}?", {{"name", name}});
    const int answer = MessageBoxW(window_, message.c_str(),
        Localized(L"Save changes").c_str(),
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) return SaveDocument();
    return true;
}

void DesktopApp::NewDocument() {
    if (window_ && !ConfirmSaveChanges()) return;
    document_ = Document{};
    document_.text.clear();
    document_.crlf = true;
    editorMode_ = "preview";
    externalChangeReported_ = false;
    if (resources_) resources_->SetDocumentDirectory(L"");
    UpdateWindowTitle();
    SendDocumentState("document.opened");
}

void DesktopApp::ChooseAndOpenDocument() {
    const std::wstring path = ChooseFileToOpen();
    if (!path.empty()) OpenDocument(path);
}

bool DesktopApp::OpenExternalDocuments(
    const std::vector<std::wstring>& paths, bool activateCurrentWindow) {
    std::vector<std::wstring> documents;
    documents.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.empty()) continue;
        std::error_code error;
        const std::filesystem::path normalized =
            std::filesystem::absolute(path, error).lexically_normal();
        if (error || !std::filesystem::is_regular_file(normalized, error) || error) {
            continue;
        }
        std::wstring extension = normalized.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t value) { return std::towlower(value); });
        if (extension != L".md" && extension != L".markdown") continue;
        const std::wstring value = normalized.wstring();
        if (std::find(documents.begin(), documents.end(), value) == documents.end()) {
            documents.push_back(value);
        }
    }
    if (documents.empty()) return false;

    size_t firstNewWindow = 0;
    if (!document_.dirty) {
        if (!OpenDocument(documents.front(), false)) return false;
        firstNewWindow = 1;
        if (activateCurrentWindow) {
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
        }
    }
    bool openedInNewWindow = false;
    for (size_t index = firstNewWindow; index < documents.size(); ++index) {
        openedInNewWindow = LaunchNewWindow(documents[index]) || openedInNewWindow;
    }
    return firstNewWindow == 0 && openedInNewWindow;
}

bool DesktopApp::LaunchNewWindow(const std::wstring& path) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return false;
    const std::wstring arguments = L"--new-window \"" + path + L"\"";
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        window_, L"open", executable.c_str(), arguments.c_str(),
        std::filesystem::path(path).parent_path().c_str(), SW_SHOWNORMAL));
    if (result > 32) return true;
    ShowError(Localized("Could not open {path}.",
                        {{"path", json::WideToUtf8(path)}}),
              Localized(L"File open error"));
    return false;
}

bool DesktopApp::OpenDocument(const std::wstring& path, bool confirmCurrent) {
    if (confirmCurrent && !ConfirmSaveChanges()) return false;
    Document loaded;
    std::wstring error;
    if (!ReadDocument(path, &loaded, &error)) {
        ShowError(Localized("Could not open {path}.",
                            {{"path", json::WideToUtf8(path)}}) + L"\n" + error,
                  Localized(L"File open error"));
        return false;
    }
    document_ = std::move(loaded);
    editorMode_ = "preview";
    externalChangeReported_ = false;
    if (resources_) {
        resources_->SetDocumentDirectory(
            std::filesystem::path(document_.path).parent_path().wstring());
    }
    UpdateWindowTitle();
    SendDocumentState("document.opened");
    return true;
}

bool DesktopApp::ReadDocument(const std::wstring& path, Document* result,
                              std::wstring* errorMessage) const {
    if (!result) return false;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        if (errorMessage) *errorMessage = LastErrorMessage(GetLastError());
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    if (bytes.size() > 32 * 1024 * 1024) {
        if (errorMessage) *errorMessage = Localized(L"The file is larger than the 32 MB safety limit.");
        return false;
    }

    Document loaded;
    loaded.path = std::filesystem::absolute(path).lexically_normal().wstring();
    const bool hasUtf8Bom = bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF;
    const bool hasUtf16LeBom = bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE;
    const bool hasUtf16BeBom = bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF;

    if (hasUtf8Bom) {
        bytes.erase(0, 3);
        loaded.encoding = TextEncoding::Utf8Bom;
    } else if (hasUtf16LeBom || hasUtf16BeBom) {
        const size_t count = (bytes.size() - 2) / 2;
        std::wstring wide(count, L'\0');
        for (size_t index = 0; index < count; ++index) {
            const auto first = static_cast<unsigned char>(bytes[2 + index * 2]);
            const auto second = static_cast<unsigned char>(bytes[3 + index * 2]);
            const std::uint16_t unit = hasUtf16LeBom
                ? static_cast<std::uint16_t>(first | (second << 8))
                : static_cast<std::uint16_t>((first << 8) | second);
            wide[index] = static_cast<wchar_t>(unit);
        }
        bytes = json::WideToUtf8(wide);
        loaded.encoding = hasUtf16LeBom
            ? TextEncoding::Utf16Le : TextEncoding::Utf16Be;
    } else if (!json::IsValidUtf8(bytes)) {
        bytes = json::WideToUtf8(json::Utf8ToWide(bytes, true));
        loaded.encoding = TextEncoding::Ansi;
    } else {
        loaded.encoding = TextEncoding::Utf8;
    }

    loaded.crlf = bytes.find("\r\n") != std::string::npos;
    loaded.text = NormalizeLineEndings(bytes);
    loaded.dirty = false;
    std::error_code timeError;
    loaded.diskWriteTime = std::filesystem::last_write_time(loaded.path, timeError);
    loaded.hasDiskWriteTime = !timeError;
    *result = std::move(loaded);
    return true;
}

bool DesktopApp::SaveDocument() {
    if (document_.path.empty()) return SaveDocumentAs();
    std::wstring error;
    if (!WriteDocument(document_.path, &error)) {
        ShowError(Localized("Could not save {path}.",
                            {{"path", json::WideToUtf8(document_.path)}}) + L"\n" + error,
                  Localized(L"File save error"));
        return false;
    }
    document_.dirty = false;
    std::error_code timeError;
    document_.diskWriteTime =
        std::filesystem::last_write_time(document_.path, timeError);
    document_.hasDiskWriteTime = !timeError;
    externalChangeReported_ = false;
    UpdateWindowTitle();
    SendDocumentState("document.saved");
    return true;
}

bool DesktopApp::SaveDocumentAs() {
    const auto selection = ChooseFileToSave();
    if (!selection) return false;
    const std::wstring previousPath = document_.path;
    const TextEncoding previousEncoding = document_.encoding;
    document_.path = selection->path;
    document_.encoding = selection->encoding;
    if (!SaveDocument()) {
        document_.path = previousPath;
        document_.encoding = previousEncoding;
        return false;
    }
    if (resources_) {
        resources_->SetDocumentDirectory(
            std::filesystem::path(document_.path).parent_path().wstring());
    }
    return true;
}

bool DesktopApp::WriteDocument(const std::wstring& path,
                               std::wstring* errorMessage) {
    const std::string text = ApplyLineEndings(document_.text, document_.crlf);
    std::string bytes;
    if (!EncodeText(text, document_.encoding, &bytes)) {
        if (errorMessage) {
            *errorMessage = Localized(
                L"The selected encoding cannot represent all characters in this document.");
        }
        return false;
    }
    const std::wstring temporary = path + L".mdviewer." +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage) *errorMessage = LastErrorMessage(GetLastError());
        return false;
    }
    const bool written = WriteAll(file, bytes) && FlushFileBuffers(file) != FALSE;
    const DWORD writeError = written ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        if (errorMessage) *errorMessage = LastErrorMessage(writeError);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporary.c_str());
        if (errorMessage) *errorMessage = LastErrorMessage(moveError);
        return false;
    }
    return true;
}

std::wstring DesktopApp::ChooseFileToOpen() const {
    std::vector<wchar_t> path(32768, L'\0');
    const std::wstring filter = BuildFilter(
        Localized(L"Markdown files"), Localized(L"All files"));
    const std::wstring title = Localized(L"Open Markdown file");
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::wstring(path.data()) : std::wstring{};
}

std::optional<DesktopApp::SaveSelection> DesktopApp::ChooseFileToSave() const {
    IFileSaveDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        std::vector<wchar_t> path(32768, L'\0');
        const std::wstring suggested = document_.path.empty()
            ? Localized(L"Untitled") + L".md" : document_.path;
        std::copy_n(suggested.begin(),
                    (std::min)(suggested.size(), path.size() - 1), path.begin());
        const std::wstring filter = BuildFilter(
            Localized(L"Markdown files"), Localized(L"All files"));
        const std::wstring title = Localized(L"Save Markdown file");
        OPENFILENAMEW fallback{sizeof(fallback)};
        fallback.hwndOwner = window_;
        fallback.lpstrFilter = filter.c_str();
        fallback.lpstrFile = path.data();
        fallback.nMaxFile = static_cast<DWORD>(path.size());
        fallback.lpstrDefExt = L"md";
        fallback.lpstrTitle = title.c_str();
        fallback.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                         OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&fallback)) return std::nullopt;
        return SaveSelection{std::wstring(path.data()), document_.encoding};
    }

    const std::wstring title = Localized(L"Save Markdown file");
    const std::wstring markdownType =
        Localized(L"Markdown files") + L" (*.md;*.markdown)";
    const std::wstring allType = Localized(L"All files") + L" (*.*)";
    const COMDLG_FILTERSPEC filters[] = {
        {markdownType.c_str(), L"*.md;*.markdown"},
        {allType.c_str(), L"*.*"},
    };
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"md");
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    }

    if (document_.path.empty()) {
        const std::wstring suggested = Localized(L"Untitled") + L".md";
        dialog->SetFileName(suggested.c_str());
    } else {
        const std::filesystem::path current(document_.path);
        dialog->SetFileName(current.filename().c_str());
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    IFileDialogCustomize* customize = nullptr;
    if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&customize))) && customize) {
        const std::wstring encodingLabel = Localized(L"Encoding");
        customize->StartVisualGroup(kEncodingGroupControlId, encodingLabel.c_str());
        customize->AddComboBox(kEncodingComboControlId);
        const TextEncoding encodings[] = {
            TextEncoding::Ansi,
            TextEncoding::Utf8,
            TextEncoding::Utf8Bom,
            TextEncoding::Utf16Le,
            TextEncoding::Utf16Be,
        };
        for (TextEncoding encoding : encodings) {
            const std::wstring label = EncodingName(encoding);
            customize->AddControlItem(
                kEncodingComboControlId, EncodingItemId(encoding), label.c_str());
        }
        customize->SetSelectedControlItem(
            kEncodingComboControlId, EncodingItemId(document_.encoding));
        customize->EndVisualGroup();
    }

    const HRESULT shown = dialog->Show(window_);
    if (FAILED(shown)) {
        if (customize) customize->Release();
        dialog->Release();
        return std::nullopt;
    }

    TextEncoding selectedEncoding = document_.encoding;
    if (customize) {
        DWORD selectedItem = EncodingItemId(selectedEncoding);
        if (SUCCEEDED(customize->GetSelectedControlItem(
                kEncodingComboControlId, &selectedItem))) {
            selectedEncoding = EncodingFromItemId(selectedItem, selectedEncoding);
        }
        customize->Release();
    }

    IShellItem* result = nullptr;
    PWSTR selectedPath = nullptr;
    const HRESULT resultStatus = dialog->GetResult(&result);
    if (SUCCEEDED(resultStatus) && result) {
        result->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
        result->Release();
    }
    dialog->Release();
    if (!selectedPath) return std::nullopt;
    SaveSelection selection{selectedPath, selectedEncoding};
    CoTaskMemFree(selectedPath);
    return selection;
}

void DesktopApp::CheckExternalFileChange() {
    if (document_.path.empty() || !document_.hasDiskWriteTime ||
        externalChangeReported_) return;
    std::error_code error;
    const auto current = std::filesystem::last_write_time(document_.path, error);
    if (error || current == document_.diskWriteTime) return;
    externalChangeReported_ = true;
    document_.diskWriteTime = current;
    if (document_.dirty) {
        MessageBoxW(window_,
            Localized(L"The file changed on disk while this document has unsaved changes. Save As to preserve both versions.").c_str(),
            Localized(L"External file change").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    const int answer = MessageBoxW(window_,
        Localized(L"The file changed on disk. Reload it now?").c_str(),
        Localized(L"External file change").c_str(),
        MB_YESNO | MB_ICONQUESTION);
    if (answer == IDYES) OpenDocument(document_.path, false);
}

std::wstring DesktopApp::Localized(const std::wstring& english) const {
    return localization::Text(instance_, config_.language, english);
}

std::wstring DesktopApp::Localized(
    const std::string& english,
    std::initializer_list<std::pair<std::string, std::string>> parameters) const {
    return json::Utf8ToWide(
        localization::Text(instance_, config_.language, english, parameters));
}

void DesktopApp::ShowError(const std::wstring& message,
                           const std::wstring& title) const {
    MessageBoxW(window_, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}
