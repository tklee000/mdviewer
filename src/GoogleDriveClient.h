#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace google_drive {

enum class PickerMode {
    None,
    MarkdownFile,
    Folder,
};

std::string BuildAuthorizationUrl(const std::string& clientId,
                                  const std::string& redirectUri,
                                  const std::string& codeChallenge,
                                  const std::string& state,
                                  PickerMode pickerMode);
std::string BuildCreateMetadata(const std::string& name,
                                const std::string& parentFolderId,
                                const std::string& mimeType = "text/markdown");

}  // namespace google_drive

template <typename T>
struct GoogleDriveResult {
    std::optional<T> value;
    std::wstring error;
    bool canceled = false;
    bool conflict = false;

    explicit operator bool() const { return value.has_value(); }
};

struct GoogleDrivePickerSelection {
    std::string fileId;
};

struct GoogleDriveFile {
    std::string id;
    std::string name;
    std::string mimeType;
    std::string modifiedTime;
    std::string bytes;
};

struct GoogleDriveUpload {
    std::string id;
    std::string name;
    std::string mimeType;
    std::string modifiedTime;
};

class GoogleDriveClient {
public:
    struct HttpResponse {
        unsigned long status = 0;
        std::string body;
        std::wstring error;
        bool canceled = false;
    };

    explicit GoogleDriveClient(const std::filesystem::path& applicationDirectory);

    GoogleDriveResult<GoogleDrivePickerSelection> PickFile(
        std::stop_token stopToken);
    GoogleDriveResult<GoogleDrivePickerSelection> PickFolder(
        std::stop_token stopToken);
    GoogleDriveResult<GoogleDriveFile> DownloadFile(
        const std::string& fileId, std::stop_token stopToken);
    GoogleDriveResult<GoogleDriveUpload> CreateMarkdownFile(
        const std::string& name, const std::string& parentFolderId,
        const std::string& bytes, const std::string& mimeType,
        std::stop_token stopToken);
    GoogleDriveResult<GoogleDriveUpload> UploadFile(
        const std::string& fileId, const std::string& bytes,
        const std::string& mimeType, const std::string& expectedModifiedTime,
        bool overwriteConflict,
        std::stop_token stopToken);

    const std::filesystem::path& ConfigurationPath() const {
        return configurationPath_;
    }
    void ForgetAuthorization();

private:
    struct Credentials {
        std::string clientId;
        std::string clientSecret;
    };
    Credentials LoadCredentials() const;
    GoogleDriveResult<std::string> EnsureAccessToken(
        const Credentials& credentials, std::stop_token stopToken);
    GoogleDriveResult<std::string> RefreshAccessToken(
        const Credentials& credentials, const std::string& refreshToken,
        std::stop_token stopToken, bool* invalidGrant);
    GoogleDriveResult<std::string> InteractiveAuthorize(
        const Credentials& credentials, std::stop_token stopToken,
        google_drive::PickerMode pickerMode = google_drive::PickerMode::None,
        std::string* pickedFileId = nullptr);
    HttpResponse Request(const wchar_t* method, const std::wstring& host,
                         const std::wstring& path,
                         const std::wstring& headers,
                         const std::string& body,
                         size_t maximumResponseBytes,
                         std::stop_token stopToken) const;
    HttpResponse AuthorizedRequest(
        const Credentials& credentials, const wchar_t* method,
        const std::wstring& path, const std::wstring& contentType,
        const std::string& body, size_t maximumResponseBytes,
        std::stop_token stopToken);
    std::optional<std::string> ReadRefreshToken(
        const Credentials& credentials) const;
    bool StoreRefreshToken(const Credentials& credentials,
                           const std::string& refreshToken) const;
    void DeleteRefreshToken() const;

    std::filesystem::path configurationPath_;
    std::string accessToken_;
    std::chrono::system_clock::time_point accessTokenExpiry_{};
};
