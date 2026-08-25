#include "App.h"

#include "DocxArchive.h"
#include "FileAssociation.h"
#include "HwpxArchive.h"
#include "Json.h"
#include "Localization.h"
#include "PrinterService.h"
#include "UiResourceProvider.h"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincrypt.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"MdViewer.MainWindow";
constexpr UINT kUiTaskMessage = WM_APP + 41;
constexpr UINT_PTR kFileWatchTimer = 1;
constexpr DWORD kEncodingGroupControlId = 2000;
constexpr DWORD kEncodingComboControlId = 2001;
constexpr size_t kMaximumPdfPreviewSize = 512 * 1024 * 1024;
constexpr DWORD kMainWindowStyle =
    WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
    WS_SYSMENU | WS_CLIPCHILDREN;

std::optional<std::string> NormalizePdfPageRanges(std::string_view value) {
    if (value.empty()) return std::string{};
    if (value.size() > 128) return std::nullopt;
    std::string normalized;
    std::size_t position = 0;
    while (position < value.size()) {
        const std::size_t comma = value.find(',', position);
        std::string_view part = value.substr(
            position, comma == std::string_view::npos
                ? value.size() - position : comma - position);
        while (!part.empty() && std::isspace(
            static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
        while (!part.empty() && std::isspace(
            static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
        if (part.empty()) return std::nullopt;

        const std::size_t hyphen = part.find('-');
        if (hyphen != std::string_view::npos &&
            part.find('-', hyphen + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        auto parsePage = [](std::string_view text) -> std::optional<unsigned int> {
            while (!text.empty() && std::isspace(
                static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(
                static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            if (text.empty()) return std::nullopt;
            unsigned int page = 0;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), page);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                page == 0 || page > 1000000) return std::nullopt;
            return page;
        };
        const auto start = parsePage(part.substr(0, hyphen));
        if (!start) return std::nullopt;
        std::optional<unsigned int> end;
        if (hyphen != std::string_view::npos) {
            end = parsePage(part.substr(hyphen + 1));
            if (!end || *start > *end) return std::nullopt;
        }
        if (!normalized.empty()) normalized.push_back(',');
        normalized += std::to_string(*start);
        if (end) normalized += "-" + std::to_string(*end);
        if (comma == std::string_view::npos) break;
        position = comma + 1;
        if (position == value.size()) return std::nullopt;
    }
    return normalized;
}

std::wstring FileName(const std::wstring& path) {
    return path.empty() ? std::wstring{} : std::filesystem::path(path).filename().wstring();
}

std::optional<std::string> NormalizeGoogleDriveMarkdownName(
    const std::string& value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    std::string normalized = value.substr(first, last - first);
    if (normalized.empty() || normalized.size() > 1024 ||
        normalized.find('\0') != std::string::npos ||
        normalized.find('\r') != std::string::npos ||
        normalized.find('\n') != std::string::npos ||
        normalized.find('/') != std::string::npos ||
        normalized.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    std::wstring wide = json::Utf8ToWide(normalized, false);
    if (wide.empty() || wide.size() > 255) return std::nullopt;
    std::wstring extension = std::filesystem::path(wide).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    if (extension.empty()) {
        normalized += ".md";
    } else if (extension != L".md" && extension != L".markdown" &&
               extension != L".mdz") {
        return std::nullopt;
    }
    return normalized;
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

bool WriteAllBytes(HANDLE file, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t offset = 0;
    while (offset < size) {
        const DWORD request = static_cast<DWORD>((std::min<size_t>)(
            size - offset, 4 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, request, &written, nullptr) ||
            written != request) return false;
        offset += written;
    }
    return true;
}

bool WriteAll(HANDLE file, const std::string& bytes) {
    return WriteAllBytes(file, bytes.data(), bytes.size());
}

std::shared_ptr<const std::vector<unsigned char>> ReadPdfBytes(
    const std::wstring& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || !size || size > kMaximumPdfPreviewSize) return nullptr;
    std::ifstream input(path, std::ios::binary);
    if (!input) return nullptr;
    auto bytes = std::make_shared<std::vector<unsigned char>>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    static constexpr unsigned char signature[] = {'%', 'P', 'D', 'F', '-'};
    if (bytes->size() < std::size(signature) ||
        !std::equal(std::begin(signature), std::end(signature), bytes->begin())) {
        return nullptr;
    }
    return bytes;
}

std::wstring BuildFilter(const std::wstring& markdownLabel,
                         const std::wstring& mdzLabel,
                         const std::wstring& allLabel) {
    std::wstring filter = markdownLabel + L" (*.md;*.markdown)";
    filter.push_back(L'\0');
    filter += L"*.md;*.markdown";
    filter.push_back(L'\0');
    filter += mdzLabel + L" (*.mdz)";
    filter.push_back(L'\0');
    filter += L"*.mdz";
    filter.push_back(L'\0');
    filter += allLabel + L" (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

struct DecodedImageData {
    std::string mimeType;
    std::wstring extension;
    mdz::Bytes bytes;
};

std::optional<DecodedImageData> DecodeImageDataUrl(
    const std::string& dataUrl) {
    constexpr std::string_view prefix = "data:";
    if (dataUrl.rfind(prefix, 0) != 0) return std::nullopt;
    const size_t separator = dataUrl.find(',');
    if (separator == std::string::npos || separator > 128 ||
        dataUrl.substr(0, separator).find(";base64") == std::string::npos) {
        return std::nullopt;
    }
    const std::string mimeType = dataUrl.substr(prefix.size(),
        dataUrl.find(';', prefix.size()) - prefix.size());
    std::wstring extension;
    if (mimeType == "image/png") extension = L".png";
    else if (mimeType == "image/jpeg") extension = L".jpg";
    else if (mimeType == "image/gif") extension = L".gif";
    else if (mimeType == "image/webp") extension = L".webp";
    else if (mimeType == "image/bmp") extension = L".bmp";
    else return std::nullopt;

    const std::string encoded = dataUrl.substr(separator + 1);
    if (encoded.size() > 180ull * 1024 * 1024) return std::nullopt;
    DWORD required = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, nullptr, &required,
                              nullptr, nullptr) ||
        required == 0 || required > 128ull * 1024 * 1024) {
        return std::nullopt;
    }
    DecodedImageData result;
    result.mimeType = mimeType;
    result.extension = extension;
    result.bytes.resize(required);
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, result.bytes.data(),
                              &required, nullptr, nullptr)) {
        return std::nullopt;
    }
    result.bytes.resize(required);
    return result;
}

std::wstring SanitizeImageFileName(const std::string& fileName,
                                   const std::wstring& requiredExtension) {
    std::wstring name = std::filesystem::path(
        json::Utf8ToWide(fileName, false)).filename().wstring();
    if (name.empty()) name = L"image" + requiredExtension;
    for (wchar_t& value : name) {
        if (value < 0x20 || value == L'<' || value == L'>' || value == L':' ||
            value == L'"' || value == L'/' || value == L'\\' || value == L'|' ||
            value == L'?' || value == L'*') value = L'_';
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
    if (name.empty()) name = L"image";
    std::wstring extension = std::filesystem::path(name).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    if (extension != requiredExtension &&
        !(requiredExtension == L".jpg" && extension == L".jpeg")) {
        name = std::filesystem::path(name).stem().wstring() + requiredExtension;
    }
    if (name.size() > 160) {
        name = name.substr(0, 160 - requiredExtension.size()) + requiredExtension;
    }
    return name;
}

std::string RelativeArchiveReference(const std::string& entryPoint,
                                     const std::string& archivePath) {
    std::string result;
    const size_t depth = static_cast<size_t>(std::count(
        entryPoint.begin(), entryPoint.end(), '/'));
    for (size_t index = 0; index < depth; ++index) result += "../";
    return result + archivePath;
}

std::string EncodeMarkdownReference(const std::string& reference) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(reference.size());
    for (const unsigned char value : reference) {
        const bool safe = std::isalnum(value) || value == '-' || value == '_' ||
            value == '.' || value == '!' || value == '~' || value == '*' ||
            value == '\'' || value == '(' || value == ')' || value == '/';
        if (safe) {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back('%');
            result.push_back(hex[value >> 4]);
            result.push_back(hex[value & 0x0F]);
        }
    }
    return result;
}

}  // namespace

const wchar_t* MdViewerWindowClassName() {
    return kWindowClassName;
}

DesktopApp::DesktopApp(HINSTANCE instance, std::wstring initialFile)
    : instance_(instance), initialFile_(std::move(initialFile)),
      config_(configStore_.Load()),
      recentDocuments_(std::filesystem::path(configStore_.Path()).parent_path()),
      googleDrive_(std::filesystem::path(configStore_.Path()).parent_path()) {
    recentDocuments_.Load();
}

DesktopApp::~DesktopApp() {
    alive_ = false;
    if (googleDriveWorker_.joinable()) {
        googleDriveWorker_.request_stop();
        googleDriveWorker_.join();
    }
    if (printerWorker_.joinable()) {
        printerWorker_.request_stop();
        printerWorker_.join();
    }
    browserHost_.reset();
    if (resources_) resources_->SetPdfPreview(nullptr);
    if (!pdfPreviewTemporaryPath_.empty()) {
        std::error_code error;
        std::filesystem::remove(pdfPreviewTemporaryPath_, error);
    }
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
        if (closeRequested_ || !ConfirmSaveChanges(
                [this] { SendMessageW(window_, WM_CLOSE, 0, 0); })) return 0;
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
    const std::wstring name = DocumentDisplayName();
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

void DesktopApp::OnPdfPrintFinished(std::uint64_t requestId,
                                    const std::wstring& path,
                                    bool success) {
    PostToUi([this, requestId, path, success] {
        FinishPdfPreview(requestId, path, success);
    });
}

void DesktopApp::HandleBrowserMessage(const std::string& message) {
    const std::string type = json::GetString(message, "type").value_or("");
    if (type == "ready") {
        browserReady_ = true;
        SendDocumentState();
        SendRecentDocuments();
        SendWindowState();
        SendMdzPasswordRequest();
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
        ReconcileMdzManagedAssets();
        const auto reportedDirty = json::GetBool(message, "dirty");
        if (reportedDirty || changed) {
            document_.dirty = reportedDirty.value_or(true);
            UpdateWindowTitle();
        }
        editorMode_ = json::GetString(message, "mode").value_or(editorMode_);
    } else if (type == "editor.modeChanged") {
        editorMode_ = json::GetString(message, "mode").value_or(editorMode_);
    } else if (type == "image.embed") {
        EmbedImageInMdz(
            json::GetString(message, "dataUrl").value_or(""),
            json::GetString(message, "fileName").value_or(""),
            json::GetString(message, "alt").value_or(""));
    } else if (type == "mdz.passwordChanged") {
        const std::string password =
            json::GetString(message, "password").value_or("");
        if (password.size() <= 1024) ChangeMdzPassword(password);
    } else if (type == "mdz.passwordResponse") {
        const std::string password =
            json::GetString(message, "password").value_or("");
        if (!pendingMdzPasswordRequest_ || password.empty() ||
            password.size() > 1024) return;
        auto submit = std::move(pendingMdzPasswordRequest_->submit);
        pendingMdzPasswordRequest_.reset();
        if (submit) submit(password);
    } else if (type == "mdz.passwordCanceled") {
        pendingMdzPasswordRequest_.reset();
    } else if (type == "command") {
        const std::string name = json::GetString(message, "name").value_or("");
        if (name == "file.new") NewDocument();
        else if (name == "file.open") ChooseAndOpenDocument();
        else if (name == "file.openGoogleDrive") {
            ChooseAndOpenGoogleDriveDocument();
        }
        else if (name == "file.save") SaveDocument();
        else if (name == "file.saveAs") SaveDocumentAs();
        else if (name == "file.saveGoogleDriveAs") {
            SaveDocumentToGoogleDriveAs(
                json::GetString(message, "fileName").value_or(""),
                json::GetBool(message, "chooseFolder").value_or(true));
        }
        else if (name == "app.exit" || name == "window.close") {
            SendMessageW(window_, WM_CLOSE, 0, 0);
        } else if (name == "app.about") {
            ShowNotification(
                Localized(L"MdViewer\n\nEdit Markdown source or the rendered preview in one desktop application."),
                Localized(L"About MdViewer"), "info");
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
    } else if (type == "printer.list") {
        SendPrinters();
    } else if (type == "printer.properties") {
        const std::wstring printerName = json::Utf8ToWide(
            json::GetString(message, "printerName").value_or(""), false);
        if (!printerName.empty() && printerName.size() <= 1024) {
            ShowPrinterProperties(printerName);
        } else {
            SendJson("{\"type\":\"printer.propertiesFailed\"}");
        }
    } else if (type == "pdf.preview") {
        const auto requestId = json::GetInteger(message, "requestId");
        const std::string paper =
            json::GetString(message, "paper").value_or("");
        const std::string orientation =
            json::GetString(message, "orientation").value_or("");
        const auto margin = json::GetInteger(message, "marginMm");
        const auto pageRanges = NormalizePdfPageRanges(
            json::GetString(message, "pageRanges").value_or(""));
        if (!requestId || *requestId <= 0 ||
            (paper != "a4" && paper != "letter") ||
            (orientation != "portrait" && orientation != "landscape") ||
            !margin || (*margin != 0 && *margin != 10 && *margin != 20)) {
            SendJson("{\"type\":\"pdf.previewFailed\",\"requestId\":" +
                     std::to_string(requestId.value_or(0)) + "}");
            return;
        }
        if (!pageRanges) {
            SendJson("{\"type\":\"pdf.previewFailed\",\"requestId\":" +
                     std::to_string(*requestId) + "}");
            return;
        }
        PdfPreviewRequest request;
        request.requestId = static_cast<std::uint64_t>(*requestId);
        if (paper == "letter") {
            request.settings.paperWidthMillimeters = 215.9;
            request.settings.paperHeightMillimeters = 279.4;
        }
        request.settings.landscape = orientation == "landscape";
        request.settings.marginMillimeters = static_cast<double>(*margin);
        request.settings.printBackground =
            json::GetBool(message, "printBackground").value_or(true);
        request.settings.pageNumbers =
            json::GetBool(message, "pageNumbers").value_or(false);
        request.settings.pageRanges = *pageRanges;
        QueuePdfPreview(request);
    } else if (type == "pdf.previewClose") {
        ClosePdfPreview();
    } else if (type == "pdf.save") {
        const auto requestId = json::GetInteger(message, "requestId");
        if (requestId && *requestId > 0) {
            SavePdfPreview(static_cast<std::uint64_t>(*requestId));
        }
    } else if (type == "pdf.print") {
        const auto requestId = json::GetInteger(message, "requestId");
        const auto copies = json::GetInteger(message, "copies");
        const std::wstring printerName = json::Utf8ToWide(
            json::GetString(message, "printerName").value_or(""), false);
        if (requestId && *requestId > 0 && copies && *copies >= 1 &&
            *copies <= 999 && !printerName.empty() &&
            printerName.size() <= 1024) {
            PrintPdfPreview(static_cast<std::uint64_t>(*requestId),
                            printerName,
                            static_cast<std::uint32_t>(*copies));
        } else {
            SendJson("{\"type\":\"pdf.printFailed\"}");
        }
    } else if (type == "docx.export") {
        ExportDocx(message);
    } else if (type == "hwpx.export") {
        ExportHwpx(message);
    } else if (type == "recent.open") {
        const std::string kind = json::GetString(message, "kind").value_or("");
        const std::wstring location = json::Utf8ToWide(
            json::GetString(message, "location").value_or(""), false);
        if ((kind == "local" || kind == "googleDrive") &&
            !location.empty() && location.size() <= 32768) {
            OpenRecentDocument(kind, location);
        }
    } else if (type == "recent.refresh") {
        recentDocuments_.Load();
        SendRecentDocuments();
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

void DesktopApp::RequestMdzPassword(
    std::wstring displayName, bool incorrect,
    std::function<void(std::string)> submit) {
    pendingMdzPasswordRequest_ = PendingMdzPasswordRequest{
        std::move(displayName), incorrect, std::move(submit)};
    SendMdzPasswordRequest();
}

void DesktopApp::SendMdzPasswordRequest() {
    if (!browserReady_ || !pendingMdzPasswordRequest_) return;
    SendJson("{\"type\":\"mdz.passwordRequired\",\"name\":" +
             json::Quote(json::WideToUtf8(
                 pendingMdzPasswordRequest_->displayName)) +
             ",\"incorrect\":" +
             (pendingMdzPasswordRequest_->incorrect ? "true}" : "false}"));
}

void DesktopApp::ChangeMdzPassword(const std::string& password) {
    if (document_.format != DocumentFormat::Mdz || password.size() > 1024) return;
    const bool changed = password != document_.mdzPassword;
    if (changed) {
        document_.mdzPassword = password;
        document_.mdzPasswordDirty = true;
        document_.dirty = true;
        UpdateWindowTitle();
    }
    SendJson("{\"type\":\"mdz.passwordChanged\",\"encrypted\":" +
             std::string(document_.mdzPassword.empty() ? "false" : "true") +
             ",\"changed\":" + (changed ? "true" : "false") +
             ",\"dirty\":" + (document_.dirty ? "true}" : "false}"));
}

void DesktopApp::SendDocumentState(const char* type) {
    if (!browserReady_) return;
    const std::string path = json::WideToUtf8(document_.path);
    const std::string name = json::WideToUtf8(DocumentDisplayName());
    SendJson("{\"type\":" + json::Quote(type) +
             ",\"language\":" + json::Quote(json::WideToUtf8(config_.language)) +
             ",\"theme\":" + json::Quote(json::WideToUtf8(config_.theme)) +
             ",\"mode\":" + json::Quote(editorMode_) +
             ",\"document\":{\"path\":" + json::Quote(path) +
             ",\"name\":" + json::Quote(name) +
             ",\"format\":" + json::Quote(
                 document_.format == DocumentFormat::Mdz ? "mdz" : "markdown") +
             ",\"origin\":" + json::Quote(
                 document_.origin == DocumentOrigin::GoogleDrive
                     ? "googleDrive" : "local") +
             ",\"driveFileId\":" + json::Quote(document_.driveFileId) +
              ",\"text\":" + json::Quote(document_.text) +
              ",\"dirty\":" + (document_.dirty ? "true" : "false") +
              ",\"mdzEncrypted\":" +
                  (!document_.mdzPassword.empty() ? "true" : "false") +
              ",\"mdzPasswordDirty\":" +
                  (document_.mdzPasswordDirty ? "true" : "false") +
              ",\"encoding\":" + json::Quote(json::WideToUtf8(EncodingName(document_.encoding))) +
             ",\"eol\":" + json::Quote(document_.crlf ? "CRLF" : "LF") + "}}");
}

void DesktopApp::UpdateDocumentResources() {
    if (!resources_) return;
    if (document_.format == DocumentFormat::Mdz && document_.mdzEntries) {
        resources_->SetDocumentArchive(document_.mdzEntries,
                                       document_.mdzEntryPoint);
        return;
    }
    resources_->SetDocumentDirectory(
        document_.origin == DocumentOrigin::Local && !document_.path.empty()
            ? std::filesystem::path(document_.path).parent_path().wstring()
            : std::wstring{});
}

void DesktopApp::SynchronizeMdzEntry() {
    if (document_.format != DocumentFormat::Mdz || !document_.mdzEntries ||
        document_.mdzEntryPoint.empty()) return;
    const std::string text = ApplyLineEndings(document_.text, document_.crlf);
    auto entries = std::make_shared<mdz::Entries>(*document_.mdzEntries);
    (*entries)[document_.mdzEntryPoint] = mdz::Bytes(text.begin(), text.end());
    document_.mdzEntries = std::move(entries);
    UpdateDocumentResources();
}

void DesktopApp::ReconcileMdzManagedAssets() {
    if (document_.format != DocumentFormat::Mdz || !document_.mdzEntries ||
        document_.mdzManagedAssets.empty()) return;
    std::shared_ptr<mdz::Entries> updated;
    for (const auto& [archivePath, data] : document_.mdzManagedAssets) {
        const std::string rawReference = RelativeArchiveReference(
            document_.mdzEntryPoint, archivePath);
        const std::string encodedReference = EncodeMarkdownReference(rawReference);
        const bool referenced = document_.text.find(rawReference) != std::string::npos ||
                                document_.text.find(encodedReference) != std::string::npos;
        const bool present = document_.mdzEntries->find(archivePath) !=
                             document_.mdzEntries->end();
        if (referenced == present) continue;
        if (!updated) updated = std::make_shared<mdz::Entries>(*document_.mdzEntries);
        if (referenced) (*updated)[archivePath] = data;
        else updated->erase(archivePath);
    }
    if (!updated) return;
    document_.mdzEntries = std::move(updated);
    UpdateDocumentResources();
}

void DesktopApp::SendRecentDocuments() {
    SendJson("{\"type\":\"recent.changed\",\"documents\":" +
             recentDocuments_.ToJson() + "}");
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

bool DesktopApp::ConfirmSaveChanges(std::function<void()> afterSave) {
    if (googleDriveBusy_ && googleDriveSaveInProgress_) {
        pendingGoogleDriveSaveContinuation_ = std::move(afterSave);
        return false;
    }
    if (!document_.dirty) return true;
    const std::string name = json::WideToUtf8(DocumentDisplayName());
    const std::wstring message = Localized(
        "Do you want to save the changes to {name}?", {{"name", name}});
    const int answer = MessageBoxW(window_, message.c_str(),
        Localized(L"Save changes").c_str(),
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) {
        if (document_.origin == DocumentOrigin::GoogleDrive) {
            BeginGoogleDriveSave(std::move(afterSave));
            return false;
        }
        return SaveDocument();
    }
    return true;
}

void DesktopApp::NewDocument() {
    if (window_) {
        auto continuation = [this] { NewDocument(); };
        if (!ConfirmSaveChanges(continuation)) return;
    }
    document_ = Document{};
    ++documentGeneration_;
    document_.text.clear();
    document_.crlf = true;
    editorMode_ = "preview";
    externalChangeReported_ = false;
    UpdateDocumentResources();
    UpdateWindowTitle();
    SendDocumentState("document.opened");
}

void DesktopApp::ChooseAndOpenDocument() {
    const std::wstring path = ChooseFileToOpen();
    if (!path.empty()) OpenDocument(path);
}

void DesktopApp::ChooseAndOpenGoogleDriveDocument() {
    auto continuation = [this] { BeginGoogleDrivePicker(); };
    if (ConfirmSaveChanges(continuation)) continuation();
}

void DesktopApp::SaveDocumentToGoogleDriveAs(std::string fileName,
                                             bool chooseFolder) {
    const auto normalized = NormalizeGoogleDriveMarkdownName(fileName);
    if (!normalized) {
        ShowError(Localized(L"The Google Drive file name is invalid."),
                  Localized(L"Google Drive error"));
        return;
    }
    googleDriveSaveInProgress_ = true;
    if (chooseFolder) {
        BeginGoogleDriveFolderPicker(*normalized);
    } else {
        BeginGoogleDriveCreate(*normalized, {});
    }
}

void DesktopApp::OpenRecentDocument(const std::string& kind,
                                    const std::wstring& location) {
    if (kind == "googleDrive") {
        const std::string fileId = json::WideToUtf8(location);
        auto continuation = [this, fileId] { BeginGoogleDriveDownload(fileId); };
        if (ConfirmSaveChanges(continuation)) continuation();
        return;
    }
    OpenDocument(location);
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
        if (extension != L".md" && extension != L".markdown" &&
            extension != L".mdz") continue;
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
    if (confirmCurrent) {
        auto continuation = [this, path] { OpenDocument(path, false); };
        if (!ConfirmSaveChanges(continuation)) return false;
    }
    return OpenDocumentWithPassword(path, {});
}

bool DesktopApp::OpenDocumentWithPassword(const std::wstring& path,
                                          const std::string& password) {
    Document loaded;
    std::wstring error;
    mdz::ReadStatus readStatus = mdz::ReadStatus::Error;
    if (!ReadDocument(path, &loaded, &error, password, &readStatus)) {
        if (readStatus == mdz::ReadStatus::PasswordRequired ||
            readStatus == mdz::ReadStatus::IncorrectPassword) {
            RequestMdzPassword(
                std::filesystem::path(path).filename().wstring(),
                readStatus == mdz::ReadStatus::IncorrectPassword,
                [this, path](std::string suppliedPassword) {
                    OpenDocumentWithPassword(path, suppliedPassword);
                });
            return true;
        }
        ShowError(Localized("Could not open {path}.",
                            {{"path", json::WideToUtf8(path)}}) + L"\n" + error,
                  Localized(L"File open error"));
        return false;
    }
    document_ = std::move(loaded);
    ++documentGeneration_;
    editorMode_ = "preview";
    externalChangeReported_ = false;
    UpdateDocumentResources();
    UpdateWindowTitle();
    RememberCurrentDocument();
    SendDocumentState("document.opened");
    return true;
}

bool DesktopApp::ReadDocument(const std::wstring& path, Document* result,
                              std::wstring* errorMessage,
                              const std::string& password,
                              mdz::ReadStatus* readStatus) const {
    if (!result) return false;
    std::error_code sizeError;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    const std::uintmax_t maximumSize = mdz::IsMdzPath(path)
        ? 512ull * 1024 * 1024 : 32ull * 1024 * 1024;
    if (sizeError || fileSize > maximumSize) {
        if (errorMessage) {
            *errorMessage = sizeError
                ? LastErrorMessage(static_cast<DWORD>(sizeError.value()))
                : mdz::IsMdzPath(path)
                    ? L"The MDZ file is larger than the 512 MB safety limit."
                    : Localized(L"The file is larger than the 32 MB safety limit.");
        }
        return false;
    }
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        if (errorMessage) *errorMessage = LastErrorMessage(GetLastError());
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    Document loaded;
    const bool decoded = mdz::IsMdzPath(path)
        ? DecodeMdzBytes(std::move(bytes), &loaded, errorMessage,
                         password, readStatus)
        : DecodeDocumentBytes(std::move(bytes), &loaded, errorMessage);
    if (!decoded) return false;
    loaded.origin = DocumentOrigin::Local;
    loaded.path = std::filesystem::absolute(path).lexically_normal().wstring();
    std::error_code timeError;
    loaded.diskWriteTime = std::filesystem::last_write_time(loaded.path, timeError);
    loaded.hasDiskWriteTime = !timeError;
    *result = std::move(loaded);
    return true;
}

bool DesktopApp::DecodeDocumentBytes(std::string bytes, Document* result,
                                     std::wstring* errorMessage) const {
    if (!result) return false;
    if (bytes.size() > 32 * 1024 * 1024) {
        if (errorMessage) {
            *errorMessage = Localized(L"The file is larger than the 32 MB safety limit.");
        }
        return false;
    }
    Document loaded;
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
    *result = std::move(loaded);
    return true;
}

bool DesktopApp::DecodeMdzBytes(std::string bytes, Document* result,
                                std::wstring* errorMessage,
                                const std::string& password,
                                mdz::ReadStatus* readStatus) const {
    if (!result) return false;
    mdz::Package package;
    if (!mdz::ReadBytes(bytes, &package, errorMessage, password, readStatus)) {
        return false;
    }
    const auto entry = package.entries.find(package.entryPoint);
    if (entry == package.entries.end()) {
        if (errorMessage) *errorMessage = L"The MDZ Markdown entry is missing.";
        return false;
    }
    Document loaded;
    if (!DecodeDocumentBytes(
            std::string(reinterpret_cast<const char*>(entry->second.data()),
                        entry->second.size()),
            &loaded, errorMessage)) {
        return false;
    }
    loaded.format = DocumentFormat::Mdz;
    loaded.encoding = TextEncoding::Utf8;
    loaded.mdzEntryPoint = package.entryPoint;
    loaded.mdzEntries = std::make_shared<mdz::Entries>(
        std::move(package.entries));
    loaded.mdzPassword = password;
    *result = std::move(loaded);
    return true;
}

bool DesktopApp::BuildDocumentBytes(const Document& document,
                                    std::string* bytes,
                                    std::wstring* errorMessage) const {
    if (!bytes) return false;
    const std::string text = ApplyLineEndings(document.text, document.crlf);
    if (document.format == DocumentFormat::Markdown) {
        if (!EncodeText(text, document.encoding, bytes)) {
            if (errorMessage) {
                *errorMessage = Localized(
                    L"The selected encoding cannot represent all characters in this document.");
            }
            return false;
        }
        return true;
    }

    mdz::Package package;
    if (document.mdzEntries && !document.mdzEntryPoint.empty()) {
        package.entries = *document.mdzEntries;
        package.entryPoint = document.mdzEntryPoint;
    } else {
        package = mdz::CreateDocument(text,
            json::WideToUtf8(DocumentDisplayName()));
    }
    package.entries[package.entryPoint] = mdz::Bytes(text.begin(), text.end());
    return mdz::BuildBytes(package, bytes, errorMessage,
                           document.mdzPassword);
}

void DesktopApp::EmbedImageInMdz(const std::string& dataUrl,
                                 const std::string& fileName,
                                 const std::string& alt) {
    if (document_.format != DocumentFormat::Mdz ||
        !document_.mdzEntries || document_.mdzEntryPoint.empty()) {
        ShowError(L"Images can be packaged only after the document is saved as an MDZ file.",
                  L"MDZ image error");
        return;
    }
    const auto decoded = DecodeImageDataUrl(dataUrl);
    if (!decoded) {
        ShowError(L"The selected image data is invalid or unsupported.",
                  L"MDZ image error");
        return;
    }

    const std::wstring safeName = SanitizeImageFileName(
        fileName, decoded->extension);
    const std::filesystem::path namePath(safeName);
    const std::string stem = json::WideToUtf8(namePath.stem().wstring());
    const std::string extension = json::WideToUtf8(namePath.extension().wstring());
    std::string archivePath = "images/" + json::WideToUtf8(safeName);
    unsigned suffix = 2;
    for (;;) {
        const auto existing = document_.mdzEntries->find(archivePath);
        if (existing == document_.mdzEntries->end() ||
            existing->second == decoded->bytes) break;
        archivePath = "images/" + stem + "-" + std::to_string(suffix++) + extension;
    }
    if (!mdz::IsSafeArchivePath(archivePath)) {
        ShowError(L"The image file name cannot be represented safely in an MDZ file.",
                  L"MDZ image error");
        return;
    }

    const bool newlyAdded = document_.mdzEntries->find(archivePath) ==
                            document_.mdzEntries->end();
    auto entries = std::make_shared<mdz::Entries>(*document_.mdzEntries);
    (*entries)[archivePath] = decoded->bytes;
    document_.mdzEntries = std::move(entries);
    if (newlyAdded) document_.mdzManagedAssets[archivePath] = decoded->bytes;
    document_.dirty = true;
    UpdateWindowTitle();
    UpdateDocumentResources();

    const std::string relativePath = RelativeArchiveReference(
        document_.mdzEntryPoint, archivePath);
    SendJson("{\"type\":\"image.embedded\",\"path\":" +
             json::Quote(relativePath) + ",\"alt\":" + json::Quote(alt) +
             ",\"fileName\":" + json::Quote(fileName) + "}");
}

bool DesktopApp::SaveDocument() {
    if (document_.origin == DocumentOrigin::GoogleDrive) {
        BeginGoogleDriveSave();
        return false;
    }
    if (document_.path.empty()) return SaveDocumentAs();
    std::wstring error;
    if (!WriteDocument(document_.path, &error)) {
        ShowError(Localized("Could not save {path}.",
                            {{"path", json::WideToUtf8(document_.path)}}) + L"\n" + error,
                  Localized(L"File save error"));
        return false;
    }
    SynchronizeMdzEntry();
    document_.dirty = false;
    document_.mdzPasswordDirty = false;
    std::error_code timeError;
    document_.diskWriteTime =
        std::filesystem::last_write_time(document_.path, timeError);
    document_.hasDiskWriteTime = !timeError;
    externalChangeReported_ = false;
    UpdateWindowTitle();
    RememberCurrentDocument();
    SendDocumentState("document.saved");
    return true;
}

bool DesktopApp::SaveDocumentAs() {
    const auto selection = ChooseFileToSave();
    if (!selection) return false;
    const std::wstring previousPath = document_.path;
    const TextEncoding previousEncoding = document_.encoding;
    const DocumentOrigin previousOrigin = document_.origin;
    const DocumentFormat previousFormat = document_.format;
    const auto previousMdzEntries = document_.mdzEntries;
    const std::string previousMdzEntryPoint = document_.mdzEntryPoint;
    const auto previousMdzManagedAssets = document_.mdzManagedAssets;
    const std::string previousMdzPassword = document_.mdzPassword;
    const bool previousMdzPasswordDirty = document_.mdzPasswordDirty;
    const std::string previousDriveFileId = document_.driveFileId;
    const std::string previousDriveName = document_.driveName;
    const std::string previousDriveMimeType = document_.driveMimeType;
    const std::string previousDriveModifiedTime = document_.driveModifiedTime;
    document_.origin = DocumentOrigin::Local;
    document_.path = selection->path;
    document_.driveFileId.clear();
    document_.driveName.clear();
    document_.driveMimeType.clear();
    document_.driveModifiedTime.clear();
    document_.format = selection->format;
    document_.encoding = selection->format == DocumentFormat::Mdz
        ? TextEncoding::Utf8 : selection->encoding;
    if (document_.format == DocumentFormat::Mdz &&
        previousFormat != DocumentFormat::Mdz) {
        mdz::Package package = mdz::CreateDocument(
            ApplyLineEndings(document_.text, document_.crlf),
            json::WideToUtf8(std::filesystem::path(document_.path).stem().wstring()));
        document_.mdzEntryPoint = package.entryPoint;
        document_.mdzEntries = std::make_shared<mdz::Entries>(
            std::move(package.entries));
        document_.mdzManagedAssets.clear();
        document_.mdzPassword.clear();
        document_.mdzPasswordDirty = false;
    } else if (document_.format == DocumentFormat::Markdown) {
        document_.mdzEntries.reset();
        document_.mdzEntryPoint.clear();
        document_.mdzManagedAssets.clear();
        document_.mdzPassword.clear();
        document_.mdzPasswordDirty = false;
    }
    if (!SaveDocument()) {
        document_.origin = previousOrigin;
        document_.path = previousPath;
        document_.driveFileId = previousDriveFileId;
        document_.driveName = previousDriveName;
        document_.driveMimeType = previousDriveMimeType;
        document_.driveModifiedTime = previousDriveModifiedTime;
        document_.encoding = previousEncoding;
        document_.format = previousFormat;
        document_.mdzEntries = previousMdzEntries;
        document_.mdzEntryPoint = previousMdzEntryPoint;
        document_.mdzManagedAssets = previousMdzManagedAssets;
        document_.mdzPassword = previousMdzPassword;
        document_.mdzPasswordDirty = previousMdzPasswordDirty;
        return false;
    }
    UpdateDocumentResources();
    ++documentGeneration_;
    return true;
}

bool DesktopApp::WriteDocument(const std::wstring& path,
                               std::wstring* errorMessage) {
    std::string bytes;
    if (!BuildDocumentBytes(document_, &bytes, errorMessage)) return false;
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

void DesktopApp::QueuePdfPreview(PdfPreviewRequest request) {
    pdfPreviewOpen_ = true;
    if (pdfPrintInProgress_) {
        pendingPdfPreviewRequest_ = request;
        return;
    }
    StartPdfPreview(request);
}

void DesktopApp::StartPdfPreview(PdfPreviewRequest request) {
    if (!pdfPreviewOpen_ || !browserHost_) return;
    std::error_code error;
    std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path(error);
    if (error) {
        SendJson("{\"type\":\"pdf.previewFailed\",\"requestId\":" +
                 std::to_string(request.requestId) + "}");
        return;
    }
    temporaryDirectory /= L"MdViewer";
    std::filesystem::create_directories(temporaryDirectory, error);
    if (error) {
        SendJson("{\"type\":\"pdf.previewFailed\",\"requestId\":" +
                 std::to_string(request.requestId) + "}");
        return;
    }
    pdfPreviewTemporaryPath_ =
        (temporaryDirectory /
         (L"PdfPreview-" + std::to_wstring(GetCurrentProcessId()) + L".pdf"))
            .wstring();
    std::filesystem::remove(pdfPreviewTemporaryPath_, error);
    error.clear();
    pdfPrintInProgress_ = true;
    activePdfPreviewSettings_ = request.settings;
    if (!browserHost_->PrintToPdf(request.requestId,
                                  pdfPreviewTemporaryPath_,
                                  request.settings)) {
        FinishPdfPreview(request.requestId, pdfPreviewTemporaryPath_, false);
    }
}

void DesktopApp::FinishPdfPreview(std::uint64_t requestId,
                                  const std::wstring& path,
                                  bool success) {
    const auto bytes = success ? ReadPdfBytes(path) : nullptr;
    std::error_code cleanupError;
    if (!path.empty()) std::filesystem::remove(path, cleanupError);
    pdfPrintInProgress_ = false;

    const bool superseded = pendingPdfPreviewRequest_ &&
        pendingPdfPreviewRequest_->requestId > requestId;
    if (pdfPreviewOpen_ && !superseded) {
        if (bytes && resources_) {
            pdfPreviewBytes_ = bytes;
            pdfPreviewRequestId_ = requestId;
            pdfPreviewSettings_ = activePdfPreviewSettings_;
            resources_->SetPdfPreview(bytes);
            SendJson("{\"type\":\"pdf.previewReady\",\"requestId\":" +
                     std::to_string(requestId) +
                     ",\"url\":\"https://app.mdviewer/__pdf-preview?request=" +
                     std::to_string(requestId) + "\"}");
        } else {
            SendJson("{\"type\":\"pdf.previewFailed\",\"requestId\":" +
                     std::to_string(requestId) + "}");
        }
    }

    if (pdfPreviewOpen_ && pendingPdfPreviewRequest_) {
        const PdfPreviewRequest next = *pendingPdfPreviewRequest_;
        pendingPdfPreviewRequest_.reset();
        StartPdfPreview(next);
    } else {
        pendingPdfPreviewRequest_.reset();
    }
}

void DesktopApp::ClosePdfPreview() {
    pdfPreviewOpen_ = false;
    pendingPdfPreviewRequest_.reset();
    pdfPreviewRequestId_ = 0;
    pdfPreviewBytes_.reset();
    printerPropertiesApplied_ = false;
    printerPropertiesName_.clear();
    printerDeviceMode_.clear();
    if (resources_) resources_->SetPdfPreview(nullptr);
}

void DesktopApp::SavePdfPreview(std::uint64_t requestId) {
    if (!pdfPreviewOpen_ || !pdfPreviewBytes_ ||
        requestId != pdfPreviewRequestId_) {
        SendJson("{\"type\":\"pdf.saveFailed\"}");
        return;
    }
    const std::wstring path = ChoosePdfFileToSave();
    if (path.empty()) {
        SendJson("{\"type\":\"pdf.saveCanceled\"}");
        return;
    }
    std::wstring error;
    if (!WritePdfFile(path, *pdfPreviewBytes_, &error)) {
        ShowError(Localized("Could not export PDF to {path}.",
                            {{"path", json::WideToUtf8(path)}}) +
                      L"\n" + error,
                  Localized(L"PDF export error"));
        SendJson("{\"type\":\"pdf.saveFailed\"}");
        return;
    }
    SendJson("{\"type\":\"pdf.saved\",\"path\":" +
             json::Quote(json::WideToUtf8(path)) + "}");
}

void DesktopApp::SendPrinters() {
    printerPropertiesApplied_ = false;
    printerPropertiesName_.clear();
    printerDeviceMode_.clear();
    std::vector<printer::PrinterInfo> printers;
    const DWORD testLength = GetEnvironmentVariableW(
        L"MDVIEWER_PRINT_TEST_PRINTERS", nullptr, 0);
    if (testLength > 1 && testLength < 32768) {
        std::wstring value(testLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_PRINT_TEST_PRINTERS", value.data(), testLength);
        if (copied > 0 && copied < testLength) {
            value.resize(copied);
            std::size_t position = 0;
            while (position <= value.size()) {
                const std::size_t separator = value.find(L'|', position);
                std::wstring name = value.substr(
                    position, separator == std::wstring::npos
                        ? value.size() - position : separator - position);
                if (!name.empty() && name.size() <= 1024) {
                    printers.push_back(
                        {std::move(name), printers.empty()});
                }
                if (separator == std::wstring::npos) break;
                position = separator + 1;
            }
        }
    } else {
        printers = printer::ListPrinters();
    }

    std::string message = "{\"type\":\"printer.listed\",\"printers\":[";
    for (std::size_t index = 0; index < printers.size(); ++index) {
        if (index) message += ',';
        message += "{\"name\":" +
            json::Quote(json::WideToUtf8(printers[index].name)) +
            ",\"isDefault\":" +
            std::string(printers[index].isDefault ? "true" : "false") + "}";
    }
    message += "]}";
    SendJson(message);
}

bool DesktopApp::IsKnownPrinter(const std::wstring& printerName) const {
    if (printerName.empty() || printerName.size() > 1024) return false;
    const DWORD testLength = GetEnvironmentVariableW(
        L"MDVIEWER_PRINT_TEST_PRINTERS", nullptr, 0);
    if (testLength > 1 && testLength < 32768) {
        std::wstring value(testLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_PRINT_TEST_PRINTERS", value.data(), testLength);
        if (!copied || copied >= testLength) return false;
        value.resize(copied);
        std::size_t position = 0;
        while (position <= value.size()) {
            const std::size_t separator = value.find(L'|', position);
            const std::wstring candidate = value.substr(
                position, separator == std::wstring::npos
                    ? value.size() - position : separator - position);
            if (!_wcsicmp(candidate.c_str(), printerName.c_str())) return true;
            if (separator == std::wstring::npos) break;
            position = separator + 1;
        }
        return false;
    }
    const std::vector<printer::PrinterInfo> printers = printer::ListPrinters();
    return std::any_of(
        printers.begin(), printers.end(), [&](const printer::PrinterInfo& value) {
            return !_wcsicmp(value.name.c_str(), printerName.c_str());
        });
}

void DesktopApp::ShowPrinterProperties(std::wstring printerName) {
    if (printerPropertiesInProgress_ || directPrintInProgress_ ||
        !IsKnownPrinter(printerName)) {
        SendJson("{\"type\":\"printer.propertiesFailed\"}");
        return;
    }

    const DWORD testLength = GetEnvironmentVariableW(
        L"MDVIEWER_PRINT_TEST_PROPERTIES", nullptr, 0);
    if (testLength > 1 && testLength < 64) {
        std::wstring value(testLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_PRINT_TEST_PROPERTIES", value.data(), testLength);
        if (copied > 0 && copied < testLength) value.resize(copied);
        else value.clear();
        if (!_wcsicmp(value.c_str(), L"apply")) {
            printerPropertiesApplied_ = true;
            printerPropertiesName_ = printerName;
            printerDeviceMode_.clear();
            SendJson("{\"type\":\"printer.propertiesApplied\",\"printerName\":" +
                     json::Quote(json::WideToUtf8(printerName)) + "}");
        } else if (!_wcsicmp(value.c_str(), L"cancel")) {
            SendJson("{\"type\":\"printer.propertiesCanceled\",\"printerName\":" +
                     json::Quote(json::WideToUtf8(printerName)) + "}");
        } else {
            SendJson("{\"type\":\"printer.propertiesFailed\"}");
        }
        return;
    }

    printerPropertiesInProgress_ = true;
    SendJson("{\"type\":\"printer.propertiesStarted\"}");
    const std::vector<unsigned char> initial =
        printerPropertiesApplied_ &&
        !_wcsicmp(printerPropertiesName_.c_str(), printerName.c_str())
            ? printerDeviceMode_ : std::vector<unsigned char>{};
    printer::PrinterPropertiesResult result = printer::ShowPrinterProperties(
        window_, printerName, initial);
    printerPropertiesInProgress_ = false;
    if (result.status == printer::PrinterPropertiesStatus::Applied) {
        printerPropertiesApplied_ = true;
        printerPropertiesName_ = printerName;
        printerDeviceMode_ = std::move(result.deviceMode);
        SendJson("{\"type\":\"printer.propertiesApplied\",\"printerName\":" +
                 json::Quote(json::WideToUtf8(printerName)) + "}");
    } else if (result.status == printer::PrinterPropertiesStatus::Canceled) {
        SendJson("{\"type\":\"printer.propertiesCanceled\",\"printerName\":" +
                 json::Quote(json::WideToUtf8(printerName)) + "}");
    } else {
        SendJson("{\"type\":\"printer.propertiesFailed\",\"message\":" +
                 json::Quote(json::WideToUtf8(result.error)) + "}");
    }
}

void DesktopApp::PrintPdfPreview(std::uint64_t requestId,
                                 std::wstring printerName,
                                 std::uint32_t copies) {
    if (!pdfPreviewOpen_ || !pdfPreviewBytes_ ||
        requestId != pdfPreviewRequestId_ || directPrintInProgress_) {
        SendJson("{\"type\":\"pdf.printFailed\"}");
        return;
    }

    if (!IsKnownPrinter(printerName)) {
        SendJson("{\"type\":\"pdf.printFailed\"}");
        return;
    }

    const DWORD testPathLength = GetEnvironmentVariableW(
        L"MDVIEWER_PRINT_TEST_OUTPUT", nullptr, 0);
    std::wstring testPath;
    if (testPathLength > 1 && testPathLength < 32768) {
        testPath.assign(testPathLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_PRINT_TEST_OUTPUT", testPath.data(), testPathLength);
        if (copied > 0 && copied < testPathLength) {
            testPath.resize(copied);
        } else {
            testPath.clear();
        }
    }

    if (printerWorker_.joinable()) printerWorker_.join();
    directPrintInProgress_ = true;
    const auto bytes = pdfPreviewBytes_;
    const PdfPrintSettings settings = pdfPreviewSettings_;
    const std::wstring documentName = DocumentDisplayName();
    const bool advancedSettingsApplied = printerPropertiesApplied_ &&
        !_wcsicmp(printerPropertiesName_.c_str(), printerName.c_str());
    const std::vector<unsigned char> deviceMode = advancedSettingsApplied
        ? printerDeviceMode_ : std::vector<unsigned char>{};
    printerWorker_ = std::jthread(
        [this, bytes, settings, printerName = std::move(printerName), copies,
         documentName, testPath = std::move(testPath), advancedSettingsApplied,
         deviceMode](std::stop_token stopToken) {
            printer::PrintResult result;
            if (!testPath.empty()) {
                result = printer::ValidatePdf(*bytes, stopToken);
                if (result.success) {
                    std::wstring writeError;
                    if (!WritePdfFile(testPath, *bytes, &writeError)) {
                        result.success = false;
                        result.error = std::move(writeError);
                    }
                }
            } else {
                printer::PrintOptions options;
                options.printerName = printerName;
                options.documentName = documentName;
                options.copies = copies;
                options.paperWidthMillimeters = settings.paperWidthMillimeters;
                options.paperHeightMillimeters = settings.paperHeightMillimeters;
                options.landscape = settings.landscape;
                options.deviceMode = deviceMode;
                result = printer::PrintPdf(*bytes, options, stopToken);
            }
            PostToUi([this, result = std::move(result), printerName, copies,
                      advancedSettingsApplied] {
                directPrintInProgress_ = false;
                if (result.success) {
                    SendJson("{\"type\":\"pdf.printed\",\"printerName\":" +
                             json::Quote(json::WideToUtf8(printerName)) +
                             ",\"copies\":" + std::to_string(copies) +
                             ",\"pageCount\":" +
                             std::to_string(result.pageCount) +
                             ",\"advancedSettings\":" +
                             std::string(advancedSettingsApplied
                                 ? "true" : "false") + "}");
                } else {
                    SendJson("{\"type\":\"pdf.printFailed\",\"message\":" +
                             json::Quote(json::WideToUtf8(result.error)) + "}");
                }
            });
        });
    SendJson("{\"type\":\"pdf.printStarted\"}");
}

std::wstring DesktopApp::ChoosePdfFileToSave() const {
    std::filesystem::path documentName(DocumentDisplayName());
    std::wstring suggested = documentName.stem().wstring();
    if (suggested.empty()) suggested = Localized(L"Untitled");
    suggested += L".pdf";

    IFileSaveDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        std::vector<wchar_t> path(32768, L'\0');
        std::copy_n(suggested.begin(),
                    (std::min)(suggested.size(), path.size() - 1), path.begin());
        std::wstring filter = Localized(L"PDF files") + L" (*.pdf)";
        filter.push_back(L'\0');
        filter += L"*.pdf";
        filter.push_back(L'\0');
        filter.push_back(L'\0');
        const std::wstring title = Localized(L"Export PDF");
        OPENFILENAMEW fallback{sizeof(fallback)};
        fallback.hwndOwner = window_;
        fallback.lpstrFilter = filter.c_str();
        fallback.lpstrFile = path.data();
        fallback.nMaxFile = static_cast<DWORD>(path.size());
        fallback.lpstrDefExt = L"pdf";
        fallback.lpstrTitle = title.c_str();
        fallback.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                         OFN_EXPLORER | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&fallback) ? std::wstring(path.data())
                                           : std::wstring{};
    }

    const std::wstring title = Localized(L"Export PDF");
    const std::wstring pdfType = Localized(L"PDF files") + L" (*.pdf)";
    const COMDLG_FILTERSPEC filters[] = {{pdfType.c_str(), L"*.pdf"}};
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"pdf");
    dialog->SetFileName(suggested.c_str());
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                           FOS_STRICTFILETYPES);
    }
    if (document_.origin == DocumentOrigin::Local && !document_.path.empty()) {
        const std::filesystem::path current(document_.path);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    if (FAILED(dialog->Show(window_))) {
        dialog->Release();
        return {};
    }
    IShellItem* result = nullptr;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&result)) && result) {
        result->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
        result->Release();
    }
    dialog->Release();
    if (!selectedPath) return {};
    std::wstring path(selectedPath);
    CoTaskMemFree(selectedPath);
    return path;
}

bool DesktopApp::WritePdfFile(const std::wstring& path,
                              const std::vector<unsigned char>& bytes,
                              std::wstring* errorMessage) const {
    const std::wstring temporary = path + L".mdviewer." +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage) *errorMessage = LastErrorMessage(GetLastError());
        return false;
    }
    const bool written = WriteAllBytes(file, bytes.data(), bytes.size()) &&
        FlushFileBuffers(file) != FALSE;
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

void DesktopApp::ExportDocx(const std::string& message) {
    const std::string documentXml =
        json::GetString(message, "documentXml").value_or("");
    const std::string title = json::GetString(message, "title").value_or("");
    const std::string author = json::GetString(message, "author").value_or("");
    const std::string font = json::GetString(message, "font").value_or("sans");
    const std::string imageRecords =
        json::GetString(message, "images").value_or("");
    const std::string hyperlinkRecords =
        json::GetString(message, "hyperlinks").value_or("");
    const std::string listRecords =
        json::GetString(message, "lists").value_or("");
    if (documentXml.empty() || documentXml.size() > 32ull * 1024 * 1024 ||
        title.size() > 4096 || author.size() > 2048 ||
        (font != "serif" && font != "sans") ||
        imageRecords.size() > 180ull * 1024 * 1024 ||
        hyperlinkRecords.size() > 4ull * 1024 * 1024 ||
        listRecords.size() > 64ull * 1024) {
        SendJson("{\"type\":\"docx.saveFailed\"}");
        return;
    }

    docx::Document exportDocument;
    exportDocument.title = title;
    exportDocument.author = author;
    exportDocument.documentXml = documentXml;
    exportDocument.sansSerif = font == "sans";

    std::istringstream images(imageRecords);
    std::string line;
    while (std::getline(images, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        const auto decoded = DecodeImageDataUrl(line.substr(separator + 1));
        if (!decoded) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        docx::Image image;
        image.id = line.substr(0, separator);
        image.mediaType = decoded->mimeType;
        image.extension = json::WideToUtf8(decoded->extension);
        image.bytes = decoded->bytes;
        exportDocument.images.push_back(std::move(image));
        if (exportDocument.images.size() > 128) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
    }

    std::istringstream hyperlinks(hyperlinkRecords);
    while (std::getline(hyperlinks, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        exportDocument.hyperlinks.push_back(
            {line.substr(0, separator), line.substr(separator + 1)});
        if (exportDocument.hyperlinks.size() > 512) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
    }

    std::istringstream lists(listRecords);
    while (std::getline(lists, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        const std::string kind = line.substr(0, separator);
        const std::string startText = line.substr(separator + 1);
        if ((kind != "ordered" && kind != "bullet") || startText.empty() ||
            startText.size() > 7 || !std::all_of(
                startText.begin(), startText.end(), [](unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        try {
            const unsigned long start = std::stoul(startText);
            if (start == 0 || start > 1000000) throw std::out_of_range("list start");
            exportDocument.lists.push_back(
                {kind == "ordered", static_cast<std::uint32_t>(start)});
        } catch (const std::exception&) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
        if (exportDocument.lists.size() > 256) {
            SendJson("{\"type\":\"docx.saveFailed\"}");
            return;
        }
    }

    const std::wstring path = ChooseDocxFileToSave();
    if (path.empty()) {
        SendJson("{\"type\":\"docx.saveCanceled\"}");
        return;
    }

    std::string bytes;
    std::wstring error;
    if (!docx::BuildBytes(exportDocument, &bytes, &error) ||
        !WriteDocxFile(path, bytes, &error)) {
        ShowError(Localized("Could not export DOCX to {path}.",
                            {{"path", json::WideToUtf8(path)}}) +
                      L"\n" + error,
                  Localized(L"DOCX export error"));
        SendJson("{\"type\":\"docx.saveFailed\"}");
        return;
    }
    SendJson("{\"type\":\"docx.saved\",\"path\":" +
             json::Quote(json::WideToUtf8(path)) + "}");
}

std::wstring DesktopApp::ChooseDocxFileToSave() const {
    const DWORD testPathLength = GetEnvironmentVariableW(
        L"MDVIEWER_DOCX_TEST_OUTPUT", nullptr, 0);
    if (testPathLength > 1 && testPathLength < 32768) {
        std::wstring testPath(testPathLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_DOCX_TEST_OUTPUT", testPath.data(), testPathLength);
        if (copied > 0 && copied < testPathLength) {
            testPath.resize(copied);
            return testPath;
        }
    }

    std::filesystem::path documentName(DocumentDisplayName());
    std::wstring suggested = documentName.stem().wstring();
    if (suggested.empty()) suggested = Localized(L"Untitled");
    suggested += L".docx";

    IFileSaveDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        std::vector<wchar_t> path(32768, L'\0');
        std::copy_n(suggested.begin(),
                    (std::min)(suggested.size(), path.size() - 1), path.begin());
        std::wstring filter = Localized(L"Word documents") + L" (*.docx)";
        filter.push_back(L'\0');
        filter += L"*.docx";
        filter.push_back(L'\0');
        filter.push_back(L'\0');
        const std::wstring title = Localized(L"Export DOCX");
        OPENFILENAMEW fallback{sizeof(fallback)};
        fallback.hwndOwner = window_;
        fallback.lpstrFilter = filter.c_str();
        fallback.lpstrFile = path.data();
        fallback.nMaxFile = static_cast<DWORD>(path.size());
        fallback.lpstrDefExt = L"docx";
        fallback.lpstrTitle = title.c_str();
        fallback.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                         OFN_EXPLORER | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&fallback) ? std::wstring(path.data())
                                           : std::wstring{};
    }

    const std::wstring title = Localized(L"Export DOCX");
    const std::wstring type = Localized(L"Word documents") + L" (*.docx)";
    const COMDLG_FILTERSPEC filters[] = {{type.c_str(), L"*.docx"}};
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"docx");
    dialog->SetFileName(suggested.c_str());
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                           FOS_STRICTFILETYPES);
    }
    if (document_.origin == DocumentOrigin::Local && !document_.path.empty()) {
        const std::filesystem::path current(document_.path);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (FAILED(dialog->Show(window_))) {
        dialog->Release();
        return {};
    }
    IShellItem* result = nullptr;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&result)) && result) {
        result->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
        result->Release();
    }
    dialog->Release();
    if (!selectedPath) return {};
    std::wstring path(selectedPath);
    CoTaskMemFree(selectedPath);
    return path;
}

bool DesktopApp::WriteDocxFile(const std::wstring& path,
                               const std::string& bytes,
                               std::wstring* errorMessage) const {
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

void DesktopApp::ExportHwpx(const std::string& message) {
    const std::string sectionXml =
        json::GetString(message, "sectionXml").value_or("");
    const std::string previewText =
        json::GetString(message, "previewText").value_or("");
    const std::string title = json::GetString(message, "title").value_or("");
    const std::string author = json::GetString(message, "author").value_or("");
    const std::string font = json::GetString(message, "font").value_or("serif");
    const std::string imageRecords =
        json::GetString(message, "images").value_or("");
    if (sectionXml.empty() || sectionXml.size() > 32ull * 1024 * 1024 ||
        previewText.size() > 8ull * 1024 * 1024 || title.size() > 4096 ||
        author.size() > 2048 || (font != "serif" && font != "sans") ||
        imageRecords.size() > 180ull * 1024 * 1024) {
        SendJson("{\"type\":\"hwpx.saveFailed\"}");
        return;
    }

    hwpx::Document exportDocument;
    exportDocument.title = title;
    exportDocument.author = author;
    exportDocument.sectionXml = sectionXml;
    exportDocument.previewText = previewText;
    exportDocument.sansSerif = font == "sans";

    std::istringstream records(imageRecords);
    std::string line;
    while (std::getline(records, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos) {
            SendJson("{\"type\":\"hwpx.saveFailed\"}");
            return;
        }
        const std::string id = line.substr(0, separator);
        const auto decoded = DecodeImageDataUrl(line.substr(separator + 1));
        if (!decoded) {
            SendJson("{\"type\":\"hwpx.saveFailed\"}");
            return;
        }
        hwpx::Image image;
        image.id = id;
        image.mediaType = decoded->mimeType;
        image.extension = json::WideToUtf8(decoded->extension);
        image.bytes = decoded->bytes;
        exportDocument.images.push_back(std::move(image));
        if (exportDocument.images.size() > 128) {
            SendJson("{\"type\":\"hwpx.saveFailed\"}");
            return;
        }
    }

    const std::wstring path = ChooseHwpxFileToSave();
    if (path.empty()) {
        SendJson("{\"type\":\"hwpx.saveCanceled\"}");
        return;
    }

    std::string bytes;
    std::wstring error;
    if (!hwpx::BuildBytes(exportDocument, &bytes, &error) ||
        !WriteHwpxFile(path, bytes, &error)) {
        ShowError(Localized("Could not export HWPX to {path}.",
                            {{"path", json::WideToUtf8(path)}}) +
                      L"\n" + error,
                  Localized(L"HWPX export error"));
        SendJson("{\"type\":\"hwpx.saveFailed\"}");
        return;
    }
    SendJson("{\"type\":\"hwpx.saved\",\"path\":" +
             json::Quote(json::WideToUtf8(path)) + "}");
}

std::wstring DesktopApp::ChooseHwpxFileToSave() const {
    const DWORD testPathLength = GetEnvironmentVariableW(
        L"MDVIEWER_HWPX_TEST_OUTPUT", nullptr, 0);
    if (testPathLength > 1 && testPathLength < 32768) {
        std::wstring testPath(testPathLength, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"MDVIEWER_HWPX_TEST_OUTPUT", testPath.data(), testPathLength);
        if (copied > 0 && copied < testPathLength) {
            testPath.resize(copied);
            return testPath;
        }
    }

    std::filesystem::path documentName(DocumentDisplayName());
    std::wstring suggested = documentName.stem().wstring();
    if (suggested.empty()) suggested = Localized(L"Untitled");
    suggested += L".hwpx";

    IFileSaveDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        std::vector<wchar_t> path(32768, L'\0');
        std::copy_n(suggested.begin(),
                    (std::min)(suggested.size(), path.size() - 1), path.begin());
        std::wstring filter = Localized(L"HWPX files") + L" (*.hwpx)";
        filter.push_back(L'\0');
        filter += L"*.hwpx";
        filter.push_back(L'\0');
        filter.push_back(L'\0');
        const std::wstring title = Localized(L"Export HWPX");
        OPENFILENAMEW fallback{sizeof(fallback)};
        fallback.hwndOwner = window_;
        fallback.lpstrFilter = filter.c_str();
        fallback.lpstrFile = path.data();
        fallback.nMaxFile = static_cast<DWORD>(path.size());
        fallback.lpstrDefExt = L"hwpx";
        fallback.lpstrTitle = title.c_str();
        fallback.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                         OFN_EXPLORER | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&fallback) ? std::wstring(path.data())
                                           : std::wstring{};
    }

    const std::wstring title = Localized(L"Export HWPX");
    const std::wstring type = Localized(L"HWPX files") + L" (*.hwpx)";
    const COMDLG_FILTERSPEC filters[] = {{type.c_str(), L"*.hwpx"}};
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"hwpx");
    dialog->SetFileName(suggested.c_str());
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                           FOS_STRICTFILETYPES);
    }
    if (document_.origin == DocumentOrigin::Local && !document_.path.empty()) {
        const std::filesystem::path current(document_.path);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (FAILED(dialog->Show(window_))) {
        dialog->Release();
        return {};
    }
    IShellItem* result = nullptr;
    PWSTR selectedPath = nullptr;
    if (SUCCEEDED(dialog->GetResult(&result)) && result) {
        result->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
        result->Release();
    }
    dialog->Release();
    if (!selectedPath) return {};
    std::wstring path(selectedPath);
    CoTaskMemFree(selectedPath);
    return path;
}

bool DesktopApp::WriteHwpxFile(const std::wstring& path,
                               const std::string& bytes,
                               std::wstring* errorMessage) const {
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

bool DesktopApp::StartGoogleDriveOperation(
    std::function<void(std::stop_token)> operation) {
    if (googleDriveBusy_.exchange(true)) {
        ShowNotification(
            Localized(L"A Google Drive operation is already in progress."),
            Localized(L"Google Drive"), "info");
        return false;
    }
    SendJson("{\"type\":\"googleDrive.busy\",\"busy\":true}");
    if (googleDriveWorker_.joinable()) googleDriveWorker_.join();
    googleDriveWorker_ = std::jthread(
        [operation = std::move(operation)](std::stop_token stopToken) mutable {
            operation(stopToken);
        });
    return true;
}

void DesktopApp::FinishGoogleDriveOperation() {
    googleDriveBusy_ = false;
    SendJson("{\"type\":\"googleDrive.busy\",\"busy\":false}");
}

void DesktopApp::BeginGoogleDrivePicker() {
    StartGoogleDriveOperation([this](std::stop_token stopToken) {
        auto result = googleDrive_.PickFile(stopToken);
        PostToUi([this, result = std::move(result)]() mutable {
            FinishGoogleDriveOperation();
            if (result.canceled) return;
            if (!result) {
                ShowError(result.error, Localized(L"Google Drive error"));
                return;
            }
            BeginGoogleDriveDownload(result.value->fileId);
        });
    });
}

void DesktopApp::BeginGoogleDriveFolderPicker(std::string fileName) {
    if (!StartGoogleDriveOperation(
        [this, fileName = std::move(fileName)](
            std::stop_token stopToken) mutable {
            auto result = googleDrive_.PickFolder(stopToken);
            PostToUi([this, fileName = std::move(fileName),
                      result = std::move(result)]() mutable {
                FinishGoogleDriveOperation();
                if (result.canceled) {
                    googleDriveSaveInProgress_ = false;
                    pendingGoogleDriveSaveContinuation_ = {};
                    return;
                }
                if (!result) {
                    googleDriveSaveInProgress_ = false;
                    pendingGoogleDriveSaveContinuation_ = {};
                    ShowError(result.error, Localized(L"Google Drive error"));
                    return;
                }
                BeginGoogleDriveCreate(
                    std::move(fileName), std::move(result.value->fileId));
            });
        })) {
        googleDriveSaveInProgress_ = false;
    }
}

void DesktopApp::BeginGoogleDriveDownload(const std::string& fileId) {
    StartGoogleDriveOperation([this, fileId](std::stop_token stopToken) {
        auto result = googleDrive_.DownloadFile(fileId, stopToken);
        PostToUi([this, result = std::move(result)]() mutable {
            FinishGoogleDriveOperation();
            if (result.canceled) return;
            if (!result) {
                ShowError(result.error, Localized(L"Google Drive error"));
                return;
            }
            std::wstring extension = std::filesystem::path(
                json::Utf8ToWide(result.value->name)).extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](wchar_t character) {
                               return static_cast<wchar_t>(std::towlower(character));
                           });
            if (extension != L".md" && extension != L".markdown" &&
                extension != L".mdz") {
                ShowError(
                    Localized(L"Only .md and .markdown files can be opened from Google Drive."),
                    Localized(L"Google Drive error"));
                return;
            }
            auto file = std::make_shared<GoogleDriveFile>(
                std::move(*result.value));
            auto openDownloaded = [this, file] { OpenGoogleDriveFile(file); };
            if (ConfirmSaveChanges(openDownloaded)) openDownloaded();
        });
    });
}

void DesktopApp::OpenGoogleDriveFile(std::shared_ptr<GoogleDriveFile> file,
                                     const std::string& password) {
    if (!file) return;
    Document loaded;
    std::wstring decodeError;
    mdz::ReadStatus readStatus = mdz::ReadStatus::Error;
    const bool decoded = mdz::IsMdzPath(
        json::Utf8ToWide(file->name, false))
        ? DecodeMdzBytes(file->bytes, &loaded, &decodeError,
                         password, &readStatus)
        : DecodeDocumentBytes(file->bytes, &loaded, &decodeError);
    if (!decoded) {
        if (readStatus == mdz::ReadStatus::PasswordRequired ||
            readStatus == mdz::ReadStatus::IncorrectPassword) {
            RequestMdzPassword(
                json::Utf8ToWide(file->name, false),
                readStatus == mdz::ReadStatus::IncorrectPassword,
                [this, file](std::string suppliedPassword) {
                    OpenGoogleDriveFile(file, suppliedPassword);
                });
            return;
        }
        ShowError(decodeError, Localized(L"Google Drive error"));
        return;
    }
    loaded.origin = DocumentOrigin::GoogleDrive;
    loaded.path.clear();
    loaded.driveFileId = file->id;
    loaded.driveName = file->name;
    loaded.driveMimeType = file->mimeType;
    loaded.driveModifiedTime = file->modifiedTime;
    document_ = std::move(loaded);
    ++documentGeneration_;
    editorMode_ = "preview";
    externalChangeReported_ = false;
    UpdateDocumentResources();
    UpdateWindowTitle();
    RememberCurrentDocument();
    SendDocumentState("document.opened");
}

void DesktopApp::BeginGoogleDriveCreate(std::string fileName,
                                        std::string parentFolderId) {
    const std::string snapshotText = document_.text;
    const bool snapshotCrlf = document_.crlf;
    const TextEncoding sourceEncoding = document_.encoding;
    const DocumentFormat sourceFormat = document_.format;
    const auto sourceMdzEntries = document_.mdzEntries;
    const std::string sourceMdzPassword = document_.mdzPassword;
    const std::uint64_t generation = documentGeneration_;
    Document snapshot = document_;
    snapshot.format = mdz::IsMdzPath(json::Utf8ToWide(fileName, false))
        ? DocumentFormat::Mdz : DocumentFormat::Markdown;
    snapshot.encoding = snapshot.format == DocumentFormat::Mdz
        ? TextEncoding::Utf8 : snapshot.encoding;
    if (snapshot.format == DocumentFormat::Mdz &&
        sourceFormat != DocumentFormat::Mdz) {
        mdz::Package package = mdz::CreateDocument(
            ApplyLineEndings(snapshot.text, snapshot.crlf),
            json::WideToUtf8(std::filesystem::path(
                json::Utf8ToWide(fileName, false)).stem().wstring()));
        snapshot.mdzEntryPoint = package.entryPoint;
        snapshot.mdzEntries = std::make_shared<mdz::Entries>(
            std::move(package.entries));
        snapshot.mdzManagedAssets.clear();
        snapshot.mdzPassword.clear();
    } else if (snapshot.format == DocumentFormat::Markdown) {
        snapshot.mdzEntries.reset();
        snapshot.mdzEntryPoint.clear();
        snapshot.mdzManagedAssets.clear();
        snapshot.mdzPassword.clear();
    }
    snapshot.mdzPasswordDirty = false;
    const DocumentFormat snapshotFormat = snapshot.format;
    const TextEncoding snapshotEncoding = snapshot.encoding;
    const auto snapshotMdzEntries = snapshot.mdzEntries;
    const std::string snapshotMdzEntryPoint = snapshot.mdzEntryPoint;
    const auto snapshotMdzManagedAssets = snapshot.mdzManagedAssets;
    const std::string snapshotMdzPassword = snapshot.mdzPassword;
    const std::string uploadMimeType = snapshotFormat == DocumentFormat::Mdz
        ? mdz::kMimeType : "text/markdown";
    std::string bytes;
    std::wstring buildError;
    if (!BuildDocumentBytes(snapshot, &bytes, &buildError)) {
        googleDriveSaveInProgress_ = false;
        pendingGoogleDriveSaveContinuation_ = {};
        ShowError(buildError, Localized(L"Google Drive error"));
        return;
    }
    if (!StartGoogleDriveOperation(
        [this, fileName = std::move(fileName),
         parentFolderId = std::move(parentFolderId), bytes = std::move(bytes),
          snapshotText, snapshotCrlf, sourceEncoding, sourceFormat,
          sourceMdzEntries, sourceMdzPassword, snapshotFormat, snapshotEncoding,
          snapshotMdzEntries, snapshotMdzEntryPoint, snapshotMdzManagedAssets,
          snapshotMdzPassword, uploadMimeType, generation](
            std::stop_token stopToken) mutable {
            auto result = googleDrive_.CreateMarkdownFile(
                fileName, parentFolderId, bytes, uploadMimeType, stopToken);
            PostToUi(
                [this, generation, snapshotText, snapshotCrlf,
                  sourceEncoding, sourceFormat, sourceMdzEntries,
                  sourceMdzPassword,
                  snapshotFormat, snapshotEncoding, snapshotMdzEntries,
                  snapshotMdzEntryPoint, snapshotMdzManagedAssets,
                  snapshotMdzPassword, uploadMimeType,
                 result = std::move(result)]() mutable {
                    FinishGoogleDriveOperation();
                    googleDriveSaveInProgress_ = false;
                    if (result.canceled) {
                        pendingGoogleDriveSaveContinuation_ = {};
                        return;
                    }
                    if (!result) {
                        pendingGoogleDriveSaveContinuation_ = {};
                        ShowError(result.error, Localized(L"Google Drive error"));
                        return;
                    }
                    if (documentGeneration_ != generation) {
                        recentDocuments_.AddGoogleDrive(
                            json::Utf8ToWide(result.value->id, false),
                            json::Utf8ToWide(result.value->name, false));
                        SendRecentDocuments();
                        pendingGoogleDriveSaveContinuation_ = {};
                        return;
                    }
                    const bool snapshotStillCurrent =
                        document_.text == snapshotText &&
                        document_.crlf == snapshotCrlf &&
                        document_.encoding == sourceEncoding &&
                        document_.format == sourceFormat &&
                        document_.mdzEntries == sourceMdzEntries &&
                        document_.mdzPassword == sourceMdzPassword;
                    document_.origin = DocumentOrigin::GoogleDrive;
                    if (snapshotStillCurrent || snapshotFormat != sourceFormat) {
                        document_.format = snapshotFormat;
                        document_.encoding = snapshotEncoding;
                        document_.mdzEntries = snapshotMdzEntries;
                        document_.mdzEntryPoint = snapshotMdzEntryPoint;
                        document_.mdzManagedAssets = snapshotMdzManagedAssets;
                        document_.mdzPassword = snapshotMdzPassword;
                        document_.mdzPasswordDirty = false;
                    }
                    document_.path.clear();
                    document_.driveFileId = result.value->id;
                    document_.driveName = result.value->name;
                    document_.driveMimeType = result.value->mimeType.empty()
                        ? uploadMimeType : result.value->mimeType;
                    document_.driveModifiedTime = result.value->modifiedTime;
                    document_.hasDiskWriteTime = false;
                    document_.dirty = !snapshotStillCurrent;
                    ++documentGeneration_;
                    externalChangeReported_ = false;
                    SynchronizeMdzEntry();
                    UpdateDocumentResources();
                    UpdateWindowTitle();
                    RememberCurrentDocument();
                    if (snapshotStillCurrent) {
                        SendDocumentState("document.saved");
                        auto continuation = pendingGoogleDriveSaveContinuation_
                            ? std::move(pendingGoogleDriveSaveContinuation_)
                            : std::function<void()>{};
                        pendingGoogleDriveSaveContinuation_ = {};
                        if (continuation) continuation();
                    } else {
                        pendingGoogleDriveSaveContinuation_ = {};
                        SendGoogleDriveSavedSnapshot(snapshotText, snapshotCrlf);
                        SendJson("{\"type\":\"googleDrive.savedOlderRevision\"}");
                    }
                });
        })) {
        googleDriveSaveInProgress_ = false;
    }
}

void DesktopApp::BeginGoogleDriveSave(std::function<void()> afterSave,
                                      bool overwriteConflict) {
    if (document_.origin != DocumentOrigin::GoogleDrive ||
        document_.driveFileId.empty()) return;
    const std::string snapshotText = document_.text;
    const bool snapshotCrlf = document_.crlf;
    const TextEncoding snapshotEncoding = document_.encoding;
    const DocumentFormat snapshotFormat = document_.format;
    const auto snapshotMdzEntries = document_.mdzEntries;
    const std::string snapshotMdzPassword = document_.mdzPassword;
    const std::string fileId = document_.driveFileId;
    const std::string expectedModifiedTime = document_.driveModifiedTime;
    const std::string mimeType = document_.driveMimeType.empty()
        ? (snapshotFormat == DocumentFormat::Mdz
            ? std::string(mdz::kMimeType) : std::string("text/markdown"))
        : document_.driveMimeType;
    std::string bytes;
    std::wstring buildError;
    if (!BuildDocumentBytes(document_, &bytes, &buildError)) {
        ShowError(buildError, Localized(L"Google Drive error"));
        return;
    }
    googleDriveSaveInProgress_ = true;
    if (!StartGoogleDriveOperation(
        [this, fileId, mimeType, expectedModifiedTime, bytes = std::move(bytes),
          snapshotText, snapshotCrlf, snapshotEncoding, overwriteConflict,
          snapshotFormat, snapshotMdzEntries, snapshotMdzPassword,
         afterSave = std::move(afterSave)](std::stop_token stopToken) mutable {
            auto result = googleDrive_.UploadFile(
                fileId, bytes, mimeType, expectedModifiedTime,
                overwriteConflict, stopToken);
            PostToUi(
                [this, fileId, snapshotText, snapshotCrlf, snapshotEncoding,
                  snapshotFormat, snapshotMdzEntries,
                  snapshotMdzPassword,
                 result = std::move(result),
                 afterSave = std::move(afterSave)]() mutable {
                    FinishGoogleDriveOperation();
                    googleDriveSaveInProgress_ = false;
                    if (result.canceled) {
                        pendingGoogleDriveSaveContinuation_ = {};
                        return;
                    }
                    if (result.conflict) {
                        const int answer = MessageBoxW(
                            window_,
                            Localized(L"This Google Drive file was changed elsewhere. Overwrite the newer Drive version with the current document?").c_str(),
                            Localized(L"Google Drive conflict").c_str(),
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                        if (answer == IDYES) {
                            BeginGoogleDriveSave(std::move(afterSave), true);
                        } else {
                            pendingGoogleDriveSaveContinuation_ = {};
                        }
                        return;
                    }
                    if (!result) {
                        pendingGoogleDriveSaveContinuation_ = {};
                        ShowError(result.error, Localized(L"Google Drive error"));
                        return;
                    }
                    if (document_.origin != DocumentOrigin::GoogleDrive ||
                        document_.driveFileId != fileId) {
                        pendingGoogleDriveSaveContinuation_ = {};
                        return;
                    }
                    if (!result.value->name.empty()) {
                        document_.driveName = result.value->name;
                    }
                    document_.driveModifiedTime = result.value->modifiedTime;
                    const bool snapshotStillCurrent =
                        document_.text == snapshotText &&
                        document_.crlf == snapshotCrlf &&
                        document_.encoding == snapshotEncoding &&
                        document_.format == snapshotFormat &&
                        document_.mdzEntries == snapshotMdzEntries &&
                        document_.mdzPassword == snapshotMdzPassword;
                    if (!snapshotStillCurrent) {
                        document_.dirty = true;
                        pendingGoogleDriveSaveContinuation_ = {};
                        UpdateWindowTitle();
                        SendGoogleDriveSavedSnapshot(snapshotText, snapshotCrlf);
                        SendJson("{\"type\":\"googleDrive.savedOlderRevision\"}");
                        return;
                    }
                    document_.dirty = false;
                    document_.mdzPasswordDirty = false;
                    SynchronizeMdzEntry();
                    UpdateWindowTitle();
                    RememberCurrentDocument();
                    SendDocumentState("document.saved");
                    auto continuation = pendingGoogleDriveSaveContinuation_
                        ? std::move(pendingGoogleDriveSaveContinuation_)
                        : std::move(afterSave);
                    pendingGoogleDriveSaveContinuation_ = {};
                    if (continuation) continuation();
                });
        })) {
        googleDriveSaveInProgress_ = false;
    }
}

void DesktopApp::SendGoogleDriveSavedSnapshot(
    const std::string& savedText, bool savedCrlf) {
    const std::string path = json::WideToUtf8(document_.path);
    const std::string name = json::WideToUtf8(DocumentDisplayName());
    SendJson("{\"type\":\"document.savedSnapshot\",\"document\":{"
             "\"path\":" + json::Quote(path) +
             ",\"name\":" + json::Quote(name) +
             ",\"format\":" + json::Quote(
                 document_.format == DocumentFormat::Mdz ? "mdz" : "markdown") +
             ",\"origin\":" + json::Quote(
                 document_.origin == DocumentOrigin::GoogleDrive
                     ? "googleDrive" : "local") +
             ",\"encoding\":" +
                 json::Quote(json::WideToUtf8(EncodingName(document_.encoding))) +
              ",\"eol\":" + json::Quote(document_.crlf ? "CRLF" : "LF") +
              ",\"dirty\":" + (document_.dirty ? "true" : "false") +
              ",\"mdzEncrypted\":" +
                  (!document_.mdzPassword.empty() ? "true" : "false") +
              ",\"mdzPasswordDirty\":" +
                  (document_.mdzPasswordDirty ? "true" : "false") +
              "},\"savedText\":" + json::Quote(savedText) +
             ",\"savedEol\":" + json::Quote(savedCrlf ? "CRLF" : "LF") +
             "}");
}

std::wstring DesktopApp::ChooseFileToOpen() const {
    std::vector<wchar_t> path(32768, L'\0');
    const std::wstring filter = BuildFilter(
        Localized(L"Markdown files"), L"MDZip files", Localized(L"All files"));
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
            ? Localized(L"Untitled") +
                (document_.format == DocumentFormat::Mdz ? L".mdz" : L".md")
            : document_.path;
        std::copy_n(suggested.begin(),
                    (std::min)(suggested.size(), path.size() - 1), path.begin());
        const std::wstring filter = BuildFilter(
            Localized(L"Markdown files"), L"MDZip files", Localized(L"All files"));
        const std::wstring title = Localized(L"Save Markdown file");
        OPENFILENAMEW fallback{sizeof(fallback)};
        fallback.hwndOwner = window_;
        fallback.lpstrFilter = filter.c_str();
        fallback.lpstrFile = path.data();
        fallback.nMaxFile = static_cast<DWORD>(path.size());
        fallback.lpstrDefExt = document_.format == DocumentFormat::Mdz
            ? L"mdz" : L"md";
        fallback.lpstrTitle = title.c_str();
        fallback.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                         OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&fallback)) return std::nullopt;
        const std::wstring selectedPath(path.data());
        return SaveSelection{selectedPath, document_.encoding,
            mdz::IsMdzPath(selectedPath) ? DocumentFormat::Mdz
                                         : DocumentFormat::Markdown};
    }

    const std::wstring title = Localized(L"Save Markdown file");
    const std::wstring markdownType =
        Localized(L"Markdown files") + L" (*.md;*.markdown)";
    const std::wstring mdzType = L"MDZip files (*.mdz)";
    const std::wstring allType = Localized(L"All files") + L" (*.*)";
    const COMDLG_FILTERSPEC filters[] = {
        {markdownType.c_str(), L"*.md;*.markdown"},
        {mdzType.c_str(), L"*.mdz"},
        {allType.c_str(), L"*.*"},
    };
    dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(document_.format == DocumentFormat::Mdz ? 2 : 1);
    dialog->SetDefaultExtension(document_.format == DocumentFormat::Mdz
                                    ? L"mdz" : L"md");
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    }

    if (document_.path.empty()) {
        const std::wstring suggested = Localized(L"Untitled") +
            (document_.format == DocumentFormat::Mdz ? L".mdz" : L".md");
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
    SaveSelection selection{
        selectedPath,
        selectedEncoding,
        mdz::IsMdzPath(selectedPath) ? DocumentFormat::Mdz
                                     : DocumentFormat::Markdown};
    CoTaskMemFree(selectedPath);
    return selection;
}

void DesktopApp::CheckExternalFileChange() {
    if (document_.origin != DocumentOrigin::Local || document_.path.empty() ||
        !document_.hasDiskWriteTime ||
        externalChangeReported_) return;
    std::error_code error;
    const auto current = std::filesystem::last_write_time(document_.path, error);
    if (error || current == document_.diskWriteTime) return;
    externalChangeReported_ = true;
    document_.diskWriteTime = current;
    if (document_.dirty) {
        ShowNotification(
            Localized(L"The file changed on disk while this document has unsaved changes. Save As to preserve both versions."),
            Localized(L"External file change"), "warning");
        return;
    }
    const int answer = MessageBoxW(window_,
        Localized(L"The file changed on disk. Reload it now?").c_str(),
        Localized(L"External file change").c_str(),
        MB_YESNO | MB_ICONQUESTION);
    if (answer == IDYES) OpenDocument(document_.path, false);
}

void DesktopApp::RememberCurrentDocument() {
    if (document_.origin == DocumentOrigin::GoogleDrive) {
        recentDocuments_.AddGoogleDrive(
            json::Utf8ToWide(document_.driveFileId, false),
            json::Utf8ToWide(document_.driveName, false));
    } else if (!document_.path.empty()) {
        recentDocuments_.AddLocal(document_.path);
    } else {
        return;
    }
    SendRecentDocuments();
}

std::wstring DesktopApp::DocumentDisplayName() const {
    if (document_.origin == DocumentOrigin::GoogleDrive &&
        !document_.driveName.empty()) {
        return json::Utf8ToWide(document_.driveName, false);
    }
    return document_.path.empty() ? Localized(L"Untitled")
                                  : FileName(document_.path);
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

void DesktopApp::ShowNotification(const std::wstring& message,
                                  const std::wstring& title,
                                  const char* tone) {
    const std::string notificationTone = tone ? tone : "info";
    if (browserHost_ && browserReady_) {
        SendJson("{\"type\":\"native.toast\",\"title\":" +
                 json::Quote(json::WideToUtf8(title)) +
                 ",\"message\":" + json::Quote(json::WideToUtf8(message)) +
                 ",\"tone\":" + json::Quote(notificationTone) + "}");
        return;
    }

    UINT icon = MB_ICONINFORMATION;
    if (notificationTone == "error") icon = MB_ICONERROR;
    else if (notificationTone == "warning") icon = MB_ICONWARNING;
    MessageBoxW(window_, message.c_str(), title.c_str(), MB_OK | icon);
}

void DesktopApp::ShowError(const std::wstring& message,
                           const std::wstring& title) {
    ShowNotification(message, title, "error");
}
