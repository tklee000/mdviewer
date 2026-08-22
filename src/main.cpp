#include "App.h"
#include "CefRuntime.h"
#include "Config.h"
#include "Localization.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

std::wstring InitialMarkdownFile() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring result;
    if (arguments) {
        for (int index = 1; index < count; ++index) {
            const std::wstring argument = arguments[index];
            if (!argument.empty() && argument[0] != L'-') {
                result = argument;
                break;
            }
        }
        LocalFree(arguments);
    }
    return result;
}

bool HasCommandLineSwitch(const wchar_t* expected) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    if (arguments) {
        for (int index = 1; index < count; ++index) {
            if (_wcsicmp(arguments[index], expected) == 0) {
                found = true;
                break;
            }
        }
        LocalFree(arguments);
    }
    return found;
}

bool IsProcessRunning(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

void CleanupStaleSecondaryProfiles(const std::filesystem::path& root,
                                   DWORD currentProcessId) {
    if (root.empty()) return;
    std::error_code iteratorError;
    for (std::filesystem::directory_iterator iterator(root, iteratorError), end;
         !iteratorError && iterator != end; iterator.increment(iteratorError)) {
        if (!iterator->is_directory(iteratorError) || iteratorError) continue;
        const std::wstring name = iterator->path().filename().wstring();
        constexpr wchar_t prefix[] = L"CEF-";
        if (name.rfind(prefix, 0) != 0 || name.size() <= 4 ||
            !std::all_of(name.begin() + 4, name.end(),
                         [](wchar_t value) { return value >= L'0' && value <= L'9'; })) {
            continue;
        }
        DWORD processId = 0;
        try {
            processId = static_cast<DWORD>(std::stoul(name.substr(4)));
        } catch (...) {
            continue;
        }
        if (!processId || processId == currentProcessId || IsProcessRunning(processId)) {
            continue;
        }
        const std::filesystem::path target = iterator->path().lexically_normal();
        if (target.parent_path() != root || target.filename() != name) continue;
        std::error_code cleanupError;
        std::filesystem::remove_all(target, cleanupError);
    }
}

bool ForwardToExistingInstance(const std::wstring& initialFile) {
    HWND existing = nullptr;
    for (int attempt = 0; attempt < 50 && !existing; ++attempt) {
        existing = FindWindowW(MdViewerWindowClassName(), nullptr);
        if (!existing) Sleep(100);
    }
    if (!existing) return false;

    DWORD_PTR result = 0;
    if (!initialFile.empty()) {
        COPYDATASTRUCT copyData{};
        copyData.dwData = kMdViewerOpenFileMessage;
        copyData.cbData = static_cast<DWORD>(
            (initialFile.size() + 1) * sizeof(wchar_t));
        copyData.lpData = const_cast<wchar_t*>(initialFile.c_str());
        if (!SendMessageTimeoutW(existing, WM_COPYDATA, 0,
                                 reinterpret_cast<LPARAM>(&copyData),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                                 &result)) return false;
    }
    if (result != kMdViewerOpenedNewWindowResult) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
    }
    return true;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    const int subprocessResult = cef_runtime::ExecuteSubprocess(instance);
    if (subprocessResult >= 0) return subprocessResult;

    const std::wstring initialFile = InitialMarkdownFile();
    const bool forceNewWindow = HasCommandLineSwitch(L"--new-window");
    HANDLE singleInstance = CreateMutexW(
        nullptr, FALSE, L"Local\\MdViewer.SingleInstance.4D445646");
    const bool alreadyRunning = singleInstance && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning && !forceNewWindow && ForwardToExistingInstance(initialFile)) {
        CloseHandle(singleInstance);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const ConfigStore configStore;
    const AppConfig config = configStore.Load();
    if (FAILED(comResult)) {
        MessageBoxW(nullptr,
            localization::Text(instance, config.language,
                L"Windows components could not be initialized.").c_str(),
            localization::Text(instance, config.language,
                L"MdViewer startup error").c_str(),
            MB_OK | MB_ICONERROR);
        if (singleInstance) CloseHandle(singleInstance);
        return 1;
    }

    // Chromium profiles are single-process resources. Give every secondary
    // window a short-lived, isolated profile so even a secondary window can
    // open another document without contending for a CEF profile lock.
    std::filesystem::path secondaryProfileRoot;
    std::filesystem::path cefDataPath;
    std::error_code pathError;
    secondaryProfileRoot = std::filesystem::absolute(
        std::filesystem::temp_directory_path(pathError) / L"MdViewer",
        pathError).lexically_normal();
    if (pathError) secondaryProfileRoot.clear();
    CleanupStaleSecondaryProfiles(secondaryProfileRoot, GetCurrentProcessId());
    if (forceNewWindow) {
        if (!secondaryProfileRoot.empty()) {
            cefDataPath = secondaryProfileRoot /
                (L"CEF-" + std::to_wstring(GetCurrentProcessId()));
        }
    } else {
        cefDataPath =
            std::filesystem::path(configStore.Path()).parent_path() / L"CEF";
    }
    const auto cleanupSecondaryProfile = [&] {
        if (!forceNewWindow || cefDataPath.empty() ||
            cefDataPath.parent_path() != secondaryProfileRoot ||
            cefDataPath.filename() !=
                L"CEF-" + std::to_wstring(GetCurrentProcessId())) return;
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::error_code cleanupError;
            std::filesystem::remove_all(cefDataPath, cleanupError);
            std::error_code existsError;
            if (!std::filesystem::exists(cefDataPath, existsError)) return;
            Sleep(50);
        }
    };
    std::wstring cefError;
    if (!cef_runtime::Initialize(instance, cefDataPath.wstring(),
                                 config.language, &cefError)) {
        MessageBoxW(nullptr,
            (localization::Text(instance, config.language,
                L"The CEF runtime could not be initialized.") + L"\n" + cefError).c_str(),
            localization::Text(instance, config.language,
                L"MdViewer startup error").c_str(),
            MB_OK | MB_ICONERROR);
        CoUninitialize();
        cleanupSecondaryProfile();
        if (singleInstance) CloseHandle(singleInstance);
        return 1;
    }

    int result = 1;
    bool browserClosedCleanly = true;
    {
        DesktopApp app(instance, initialFile);
        result = app.Run(showCommand);
        browserClosedCleanly = app.ShutdownBrowser();
    }
    cef_runtime::Shutdown(browserClosedCleanly);
    if (browserClosedCleanly) cleanupSecondaryProfile();
    CoUninitialize();
    if (singleInstance) CloseHandle(singleInstance);
    return result;
}
