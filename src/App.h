#pragma once

#include "BrowserHost.h"
#include "Config.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

class UiResourceProvider;

const wchar_t* MdViewerWindowClassName();
constexpr ULONG_PTR kMdViewerOpenFileMessage = 0x4D445646;  // "MDVF"

class DesktopApp final : public BrowserHostDelegate {
public:
    DesktopApp(HINSTANCE instance, std::wstring initialFile);
    ~DesktopApp() override;

    int Run(int showCommand);
    bool ShutdownBrowser();

    void OnBrowserCreated() override;
    void OnBrowserMessage(const std::string& message) override;
    void OnBrowserLoadError(const std::wstring& message) override;

private:
    struct Document {
        std::wstring path;
        std::string text;
        bool dirty = false;
        bool utf8Bom = false;
        bool crlf = true;
        std::wstring encoding = L"UTF-8";
        std::filesystem::file_time_type diskWriteTime{};
        bool hasDiskWriteTime = false;
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam);
    LRESULT HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    bool CreateMainWindow(int showCommand);
    void InitializeBrowser();
    void ResizeBrowser();
    void BeginBrowserClose();
    void UpdateWindowTitle();
    void SaveWindowState();
    void PostToUi(std::function<void()> callback);

    void HandleBrowserMessage(const std::string& message);
    void SendJson(const std::string& json);
    void SendDocumentState(const char* type = "app.init");
    void SendWindowState();
    void SetLanguage(const std::wstring& language);
    void SetTheme(const std::wstring& theme);
    void ApplyNativeTheme();

    bool ConfirmSaveChanges();
    void NewDocument();
    void ChooseAndOpenDocument();
    bool OpenDocument(const std::wstring& path, bool confirmCurrent = true);
    bool ReadDocument(const std::wstring& path, Document* result,
                      std::wstring* errorMessage) const;
    bool SaveDocument();
    bool SaveDocumentAs();
    bool WriteDocument(const std::wstring& path, std::wstring* errorMessage);
    std::wstring ChooseFileToOpen() const;
    std::wstring ChooseFileToSave() const;
    void CheckExternalFileChange();

    std::wstring Localized(const std::wstring& english) const;
    std::wstring Localized(const std::string& english,
                           std::initializer_list<std::pair<std::string, std::string>> parameters = {}) const;
    void ShowError(const std::wstring& message,
                   const std::wstring& title) const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::wstring initialFile_;
    ConfigStore configStore_;
    AppConfig config_;
    Document document_;
    std::string editorMode_ = "source";
    std::shared_ptr<UiResourceProvider> resources_;
    std::unique_ptr<BrowserHost> browserHost_;
    bool browserReady_ = false;
    bool browserCreated_ = false;
    bool startupComplete_ = false;
    bool closeRequested_ = false;
    bool browserCanClose_ = false;
    bool alive_ = true;
    bool externalChangeReported_ = false;
};
