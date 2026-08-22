#pragma once

#include "BrowserHost.h"
#include "Config.h"
#include "GoogleDriveClient.h"
#include "MdzArchive.h"
#include "RecentDocuments.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

class UiResourceProvider;

const wchar_t* MdViewerWindowClassName();
constexpr ULONG_PTR kMdViewerOpenFileMessage = 0x4D445646;  // "MDVF"
constexpr DWORD_PTR kMdViewerOpenedNewWindowResult = 2;

enum class TextEncoding {
    Ansi,
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16Be,
};

class DesktopApp final : public BrowserHostDelegate {
public:
    DesktopApp(HINSTANCE instance, std::wstring initialFile);
    ~DesktopApp() override;

    int Run(int showCommand);
    bool ShutdownBrowser();

    void OnBrowserCreated() override;
    void OnBrowserMessage(const std::string& message) override;
    void OnFilesDropped(const std::vector<std::wstring>& paths) override;
    void OnBrowserLoadError(const std::wstring& message) override;

private:
    enum class DocumentOrigin {
        Local,
        GoogleDrive,
    };

    enum class DocumentFormat {
        Markdown,
        Mdz,
    };

    struct Document {
        DocumentOrigin origin = DocumentOrigin::Local;
        DocumentFormat format = DocumentFormat::Markdown;
        std::wstring path;
        std::string driveFileId;
        std::string driveName;
        std::string driveMimeType;
        std::string driveModifiedTime;
        std::string text;
        std::shared_ptr<mdz::Entries> mdzEntries;
        std::string mdzEntryPoint;
        std::map<std::string, mdz::Bytes> mdzManagedAssets;
        bool dirty = false;
        bool crlf = true;
        TextEncoding encoding = TextEncoding::Utf8;
        std::filesystem::file_time_type diskWriteTime{};
        bool hasDiskWriteTime = false;
    };

    struct SaveSelection {
        std::wstring path;
        TextEncoding encoding = TextEncoding::Utf8;
        DocumentFormat format = DocumentFormat::Markdown;
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
    void UpdateShutdownProtection();
    void SaveWindowState();
    void PostToUi(std::function<void()> callback);

    void HandleBrowserMessage(const std::string& message);
    void SendJson(const std::string& json);
    void SendDocumentState(const char* type = "app.init");
    void UpdateDocumentResources();
    void SynchronizeMdzEntry();
    void ReconcileMdzManagedAssets();
    void SendRecentDocuments();
    void SendWindowState();
    void SetLanguage(const std::wstring& language);
    void SetTheme(const std::wstring& theme);
    void ApplyNativeTheme();

    bool ConfirmSaveChanges(std::function<void()> afterSave = {});
    void NewDocument();
    void ChooseAndOpenDocument();
    void ChooseAndOpenGoogleDriveDocument();
    void SaveDocumentToGoogleDriveAs(std::string fileName,
                                     bool chooseFolder);
    void OpenRecentDocument(const std::string& kind,
                            const std::wstring& location);
    void BeginGoogleDrivePicker();
    void BeginGoogleDriveFolderPicker(std::string fileName);
    void BeginGoogleDriveDownload(const std::string& fileId);
    void BeginGoogleDriveCreate(std::string fileName,
                                std::string parentFolderId);
    void BeginGoogleDriveSave(std::function<void()> afterSave = {},
                              bool overwriteConflict = false);
    bool StartGoogleDriveOperation(
        std::function<void(std::stop_token)> operation);
    void FinishGoogleDriveOperation();
    void SendGoogleDriveSavedSnapshot(const std::string& savedText,
                                      bool savedCrlf);
    bool OpenExternalDocuments(const std::vector<std::wstring>& paths,
                               bool activateCurrentWindow);
    bool LaunchNewWindow(const std::wstring& path);
    bool OpenDocument(const std::wstring& path, bool confirmCurrent = true);
    bool ReadDocument(const std::wstring& path, Document* result,
                      std::wstring* errorMessage) const;
    bool DecodeDocumentBytes(std::string bytes, Document* result,
                             std::wstring* errorMessage) const;
    bool DecodeMdzBytes(std::string bytes, Document* result,
                        std::wstring* errorMessage) const;
    bool BuildDocumentBytes(const Document& document, std::string* bytes,
                            std::wstring* errorMessage) const;
    void EmbedImageInMdz(const std::string& dataUrl,
                         const std::string& fileName,
                         const std::string& alt);
    bool SaveDocument();
    bool SaveDocumentAs();
    bool WriteDocument(const std::wstring& path, std::wstring* errorMessage);
    std::wstring ChooseFileToOpen() const;
    std::optional<SaveSelection> ChooseFileToSave() const;
    void CheckExternalFileChange();
    void RememberCurrentDocument();
    std::wstring DocumentDisplayName() const;

    std::wstring Localized(const std::wstring& english) const;
    std::wstring Localized(const std::string& english,
                           std::initializer_list<std::pair<std::string, std::string>> parameters = {}) const;
    void ShowNotification(const std::wstring& message,
                          const std::wstring& title,
                          const char* tone);
    void ShowError(const std::wstring& message,
                   const std::wstring& title);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::wstring initialFile_;
    ConfigStore configStore_;
    AppConfig config_;
    RecentDocuments recentDocuments_;
    GoogleDriveClient googleDrive_;
    Document document_;
    std::uint64_t documentGeneration_ = 0;
    std::string editorMode_ = "preview";
    std::shared_ptr<UiResourceProvider> resources_;
    std::unique_ptr<BrowserHost> browserHost_;
    bool browserReady_ = false;
    bool browserCreated_ = false;
    bool startupComplete_ = false;
    bool closeRequested_ = false;
    bool browserCanClose_ = false;
    bool alive_ = true;
    bool externalChangeReported_ = false;
    std::atomic_bool googleDriveBusy_{false};
    std::jthread googleDriveWorker_;
    bool googleDriveSaveInProgress_ = false;
    std::function<void()> pendingGoogleDriveSaveContinuation_;
};
