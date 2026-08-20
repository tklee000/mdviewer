#include "App.h"
#include "CefRuntime.h"
#include "Config.h"
#include "Localization.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

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
    ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
    return true;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    const int subprocessResult = cef_runtime::ExecuteSubprocess(instance);
    if (subprocessResult >= 0) return subprocessResult;

    const std::wstring initialFile = InitialMarkdownFile();
    HANDLE singleInstance = CreateMutexW(
        nullptr, FALSE, L"Local\\MdViewer.SingleInstance.4D445646");
    const bool alreadyRunning = singleInstance && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning && ForwardToExistingInstance(initialFile)) {
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

    const std::filesystem::path cefDataPath =
        std::filesystem::path(configStore.Path()).parent_path() / L"CEF";
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
    CoUninitialize();
    if (singleInstance) CloseHandle(singleInstance);
    return result;
}
