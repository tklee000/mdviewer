#pragma once

#include <filesystem>
#include <functional>
#include <memory>
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

private:
    std::filesystem::path webRoot_;
    std::filesystem::path documentDirectory_;
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
    virtual std::optional<std::filesystem::path> ChooseSaveFile(
        const std::filesystem::path& currentPath) = 0;
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
    bool OpenDocument(const std::filesystem::path& path);
    bool SaveDocument(bool forceSaveAs);
    void NewDocument();
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
    bool hadBom_ = false;
    bool usedCrLf_ = false;
};

std::string JsonQuote(const std::string& value);
std::optional<std::string> JsonString(const std::string& json, const std::string& key);
std::optional<bool> JsonBool(const std::string& json, const std::string& key);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace mdviewer
