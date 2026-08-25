#pragma once

#include "MdzArchive.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mdviewer {

struct UiResource {
    std::string mimeType;
    std::string bytes;
};

class FileResourceProvider {
public:
    explicit FileResourceProvider(std::filesystem::path webRoot);

    std::optional<UiResource> Load(const std::string& url) const;
    void SetDocumentDirectory(std::filesystem::path directory);
    void SetDocumentArchive(std::shared_ptr<const mdz::Entries> entries,
                            std::string entryPoint);

private:
    std::filesystem::path webRoot_;
    mutable std::mutex documentMutex_;
    std::filesystem::path documentDirectory_;
    std::shared_ptr<const mdz::Entries> documentArchive_;
    std::string archiveEntryPoint_;
};

enum class TextEncoding {
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16Be,
};

enum class DocumentFormat {
    Markdown,
    Mdz,
};

struct SaveSelection {
    std::filesystem::path path;
    TextEncoding encoding = TextEncoding::Utf8;
    DocumentFormat format = DocumentFormat::Markdown;
};

enum class SavePromptResult {
    Save,
    Discard,
    Cancel,
};

class PortablePlatform {
public:
    virtual ~PortablePlatform() = default;

    virtual std::optional<std::filesystem::path> ChooseOpenFile() = 0;
    virtual std::optional<SaveSelection> ChooseSaveFile(
        const std::filesystem::path& currentPath,
        TextEncoding currentEncoding,
        DocumentFormat currentFormat) = 0;
    virtual SavePromptResult ConfirmSaveChanges(const std::string& displayName) = 0;
    virtual void ShowError(const std::string& title, const std::string& message) = 0;
    virtual void ShowAbout() = 0;
    virtual void SetWindowTitle(const std::string& title) = 0;
    virtual void RequestClose() = 0;
    virtual void MinimizeWindow() = 0;
    virtual void ToggleMaximizeWindow() = 0;
    virtual bool IsWindowMaximized() const = 0;
    virtual void BeginWindowDrag() = 0;
    virtual void OpenExternal(const std::string& url) = 0;
};

class EditorController {
public:
    using SendJson = std::function<void(const std::string&)>;

    EditorController(PortablePlatform& platform,
                     std::shared_ptr<FileResourceProvider> resources,
                     std::string language,
                     std::string theme);

    void SetSender(SendJson sender);
    bool OpenInitialFile(const std::filesystem::path& path);
    bool OpenRequestedFile(const std::filesystem::path& path);
    void OnWebMessage(const std::string& message);
    bool ConfirmClose();

private:
    bool ConfirmDiscardOrSave();
    bool OpenDocument(const std::filesystem::path& path,
                      const std::string& password = {});
    bool SaveDocument(bool forceSaveAs);
    void NewDocument();
    void SendMdzPasswordRequest();
    void ChangeMdzPassword(const std::string& password);
    void SendInitialState();
    void SendDocumentState(const char* messageType);
    void SendSettings();
    void UpdateTitle();
    void Send(const std::string& json) const;

    PortablePlatform& platform_;
    std::shared_ptr<FileResourceProvider> resources_;
    SendJson sender_;
    std::filesystem::path path_;
    std::string text_;
    std::string language_;
    std::string theme_;
    std::string editorMode_ = "preview";
    bool dirty_ = false;
    bool ready_ = false;
    TextEncoding encoding_ = TextEncoding::Utf8;
    DocumentFormat format_ = DocumentFormat::Markdown;
    bool usedCrLf_ = false;
    std::shared_ptr<mdz::Package> archive_;
    std::string mdzPassword_;
    bool mdzPasswordDirty_ = false;
    std::optional<std::filesystem::path> pendingMdzPath_;
    bool pendingMdzPasswordIncorrect_ = false;
};

std::string JsonQuote(const std::string& value);
std::optional<std::string> JsonString(const std::string& json, const std::string& key);
std::optional<bool> JsonBool(const std::string& json, const std::string& key);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace mdviewer
