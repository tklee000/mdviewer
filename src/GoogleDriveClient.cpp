#include "GoogleDriveClient.h"

#include "Json.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wincred.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

namespace {

constexpr wchar_t kCredentialTarget[] = L"MdViewer.GoogleDrive.RefreshToken";
constexpr wchar_t kGoogleApiHost[] = L"www.googleapis.com";
constexpr wchar_t kGoogleTokenHost[] = L"oauth2.googleapis.com";
constexpr size_t kMaximumDriveFileBytes = 512ull * 1024 * 1024;

std::string Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (!copied || copied >= size) return {};
    value.resize(copied);
    return json::WideToUtf8(value);
}

std::string IniValue(const std::filesystem::path& path,
                     const wchar_t* key) {
    std::vector<wchar_t> buffer(4096, L'\0');
    const DWORD count = GetPrivateProfileStringW(
        L"google", key, L"", buffer.data(), static_cast<DWORD>(buffer.size()),
        path.c_str());
    return json::WideToUtf8(std::wstring(buffer.data(), count));
}

bool IsUnreserved(unsigned char value) {
    return std::isalnum(value) || value == '-' || value == '_' ||
           value == '.' || value == '~';
}

std::string UrlEncode(const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3 / 2);
    for (const unsigned char character : value) {
        if (IsUnreserved(character)) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4]);
            result.push_back(hex[character & 0x0F]);
        }
    }
    return result;
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            result.push_back(' ');
        } else if (value[index] == '%' && index + 2 < value.size()) {
            const int high = HexValue(value[index + 1]);
            const int low = HexValue(value[index + 2]);
            if (high < 0 || low < 0) return {};
            result.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            result.push_back(value[index]);
        }
    }
    return result;
}

std::map<std::string, std::string> ParseQuery(const std::string& value) {
    std::map<std::string, std::string> result;
    size_t position = 0;
    while (position <= value.size()) {
        const size_t ampersand = value.find('&', position);
        const std::string pair = value.substr(
            position, ampersand == std::string::npos ? std::string::npos
                                                     : ampersand - position);
        const size_t equals = pair.find('=');
        const std::string key = UrlDecode(pair.substr(0, equals));
        const std::string item = equals == std::string::npos
            ? std::string{} : UrlDecode(pair.substr(equals + 1));
        if (!key.empty()) result[key] = item;
        if (ampersand == std::string::npos) break;
        position = ampersand + 1;
    }
    return result;
}

std::wstring WindowsError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = size && buffer ? std::wstring(buffer, size)
                                         : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::string Base64Url(const unsigned char* bytes, DWORD size) {
    DWORD required = 0;
    if (!CryptBinaryToStringA(bytes, size,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &required)) return {};
    std::string result(required, '\0');
    if (!CryptBinaryToStringA(bytes, size,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              result.data(), &required)) return {};
    result.resize(required);
    while (!result.empty() && result.back() == '\0') result.pop_back();
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string RandomBase64Url(size_t byteCount) {
    std::vector<unsigned char> bytes(byteCount);
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return Base64Url(bytes.data(), static_cast<DWORD>(bytes.size()));
}

std::string Sha256Base64Url(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD copied = 0;
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest{};
    bool okay = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (okay) {
        okay = BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &copied, 0) >= 0;
    }
    if (okay) {
        object.resize(objectSize);
        okay = BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                                nullptr, 0, 0) >= 0;
    }
    if (okay) {
        okay = BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()), 0) >= 0;
    }
    if (okay) {
        okay = BCryptFinishHash(hash, digest.data(),
                                static_cast<ULONG>(digest.size()), 0) >= 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return okay ? Base64Url(digest.data(), static_cast<DWORD>(digest.size()))
                : std::string{};
}

std::wstring HttpError(const GoogleDriveClient::HttpResponse& response,
                       const std::wstring& fallback) {
    if (!response.error.empty()) return response.error;
    const std::string description =
        json::GetString(response.body, "error_description").value_or("");
    if (!description.empty()) return json::Utf8ToWide(description);
    const std::string message = json::GetString(response.body, "message").value_or("");
    if (!message.empty()) return json::Utf8ToWide(message);
    return fallback + L" (HTTP " + std::to_wstring(response.status) + L")";
}

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const { return value_; }
private:
    HINTERNET value_ = nullptr;
};

}  // namespace

namespace google_drive {

std::string BuildAuthorizationUrl(const std::string& clientId,
                                  const std::string& redirectUri,
                                  const std::string& codeChallenge,
                                  const std::string& state,
                                  PickerMode pickerMode) {
    std::string result =
        "https://accounts.google.com/o/oauth2/v2/auth?client_id=" +
        UrlEncode(clientId) + "&redirect_uri=" + UrlEncode(redirectUri) +
        "&response_type=code&scope=" +
        UrlEncode("https://www.googleapis.com/auth/drive.file") +
        "&code_challenge=" + UrlEncode(codeChallenge) +
        "&code_challenge_method=S256&access_type=offline&prompt=consent" +
        "&state=" + UrlEncode(state);
    if (pickerMode != PickerMode::None) {
        result += "&trigger_onepick=true&allow_multiple=false";
        if (pickerMode == PickerMode::Folder) {
            result += "&allow_folder_selection=true&mimetypes=" +
                UrlEncode("application/vnd.google-apps.folder");
        } else {
            result += "&mimetypes=" + UrlEncode(
                "text/markdown,text/x-markdown,text/plain,application/vnd.mdzip,application/zip,application/octet-stream");
        }
    }
    return result;
}

std::string BuildCreateMetadata(const std::string& name,
                                const std::string& parentFolderId,
                                const std::string& mimeType) {
    std::string result = "{\"name\":" + json::Quote(name) +
        ",\"mimeType\":" + json::Quote(mimeType);
    if (!parentFolderId.empty()) {
        result += ",\"parents\":[" + json::Quote(parentFolderId) + "]";
    }
    return result + "}";
}

}  // namespace google_drive

GoogleDriveClient::GoogleDriveClient(
    const std::filesystem::path& applicationDirectory)
    : configurationPath_(applicationDirectory / L"google-drive.ini") {}

GoogleDriveClient::Credentials GoogleDriveClient::LoadCredentials() const {
    Credentials credentials;
    credentials.clientId = Environment(L"MDVIEWER_GOOGLE_CLIENT_ID");
    credentials.clientSecret = Environment(L"MDVIEWER_GOOGLE_CLIENT_SECRET");
    if (credentials.clientId.empty()) {
        credentials.clientId = IniValue(configurationPath_, L"clientId");
    }
    if (credentials.clientSecret.empty()) {
        credentials.clientSecret = IniValue(configurationPath_, L"clientSecret");
    }
    return credentials;
}

GoogleDriveClient::HttpResponse GoogleDriveClient::Request(
    const wchar_t* method, const std::wstring& host, const std::wstring& path,
    const std::wstring& headers, const std::string& body,
    size_t maximumResponseBytes, std::stop_token stopToken) const {
    HttpResponse result;
    if (stopToken.stop_requested()) {
        result.error = L"The operation was canceled.";
        result.canceled = true;
        return result;
    }
    InternetHandle session(WinHttpOpen(
        L"MdViewer/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        result.error = WindowsError(GetLastError());
        return result;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);
    InternetHandle connection(WinHttpConnect(
        session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        result.error = WindowsError(GetLastError());
        return result;
    }
    InternetHandle request(WinHttpOpenRequest(
        connection, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        result.error = WindowsError(GetLastError());
        return result;
    }
    const BOOL sent = WinHttpSendRequest(
        request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1L),
        body.empty() ? WINHTTP_NO_REQUEST_DATA
                     : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        result.error = WindowsError(GetLastError());
        return result;
    }
    DWORD statusSize = sizeof(result.status);
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    while (!stopToken.stop_requested()) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            result.error = WindowsError(GetLastError());
            return result;
        }
        if (!available) break;
        if (result.body.size() + available > maximumResponseBytes) {
            result.error = L"The Google Drive response exceeded the safety limit.";
            return result;
        }
        const size_t offset = result.body.size();
        result.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, result.body.data() + offset,
                             available, &read)) {
            result.error = WindowsError(GetLastError());
            return result;
        }
        result.body.resize(offset + read);
    }
    if (stopToken.stop_requested()) {
        result.error = L"The operation was canceled.";
        result.canceled = true;
    }
    return result;
}

std::optional<std::string> GoogleDriveClient::ReadRefreshToken(
    const Credentials& credentials) const {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) ||
        !credential) return std::nullopt;
    std::optional<std::string> result;
    if (credential->UserName &&
        credentials.clientId == json::WideToUtf8(credential->UserName) &&
        credential->CredentialBlob && credential->CredentialBlobSize) {
        const auto* begin = reinterpret_cast<const char*>(credential->CredentialBlob);
        result = std::string(begin, begin + credential->CredentialBlobSize);
    }
    CredFree(credential);
    return result;
}

bool GoogleDriveClient::StoreRefreshToken(
    const Credentials& credentials, const std::string& refreshToken) const {
    CREDENTIALW credential{};
    const std::wstring clientId = json::Utf8ToWide(credentials.clientId, false);
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(refreshToken.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(refreshToken.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(clientId.c_str());
    return CredWriteW(&credential, 0) != FALSE;
}

void GoogleDriveClient::DeleteRefreshToken() const {
    CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0);
}

void GoogleDriveClient::ForgetAuthorization() {
    accessToken_.clear();
    accessTokenExpiry_ = {};
    DeleteRefreshToken();
}

GoogleDriveResult<std::string> GoogleDriveClient::RefreshAccessToken(
    const Credentials& credentials, const std::string& refreshToken,
    std::stop_token stopToken, bool* invalidGrant) {
    if (invalidGrant) *invalidGrant = false;
    std::string form = "client_id=" + UrlEncode(credentials.clientId) +
        "&refresh_token=" + UrlEncode(refreshToken) +
        "&grant_type=refresh_token";
    if (!credentials.clientSecret.empty()) {
        form += "&client_secret=" + UrlEncode(credentials.clientSecret);
    }
    const HttpResponse response = Request(
        L"POST", kGoogleTokenHost, L"/token",
        L"Content-Type: application/x-www-form-urlencoded\r\n", form,
        1024 * 1024, stopToken);
    const std::string token = json::GetString(response.body, "access_token").value_or("");
    if (!response.error.empty() || response.status != 200 || token.empty()) {
        if (invalidGrant) {
            *invalidGrant = json::GetString(response.body, "error").value_or("") ==
                            "invalid_grant";
        }
        return {{}, HttpError(response, L"Google authorization could not be refreshed.")};
    }
    const auto expires = json::GetInteger(response.body, "expires_in").value_or(3600);
    accessToken_ = token;
    accessTokenExpiry_ = std::chrono::system_clock::now() +
        std::chrono::seconds((std::max<std::int64_t>)(60, expires - 60));
    return {token, {}};
}

GoogleDriveResult<std::string> GoogleDriveClient::InteractiveAuthorize(
    const Credentials& credentials, std::stop_token stopToken,
    google_drive::PickerMode pickerMode, std::string* pickedFileId) {
    if (pickedFileId) pickedFileId->clear();
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
        return {{}, L"The local OAuth callback could not be started."};
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return {{}, L"The local OAuth callback could not be started."};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return {{}, L"The local OAuth callback port could not be opened."};
    }
    int addressLength = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
    const unsigned short port = ntohs(address.sin_port);
    const std::string redirect = "http://127.0.0.1:" + std::to_string(port);
    const std::string verifier = RandomBase64Url(64);
    const std::string challenge = Sha256Base64Url(verifier);
    const std::string state = RandomBase64Url(32);
    if (verifier.empty() || challenge.empty() || state.empty()) {
        closesocket(listener);
        WSACleanup();
        return {{}, L"Secure OAuth values could not be generated."};
    }
    const std::string requestUrl = google_drive::BuildAuthorizationUrl(
        credentials.clientId, redirect, challenge, state, pickerMode);
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", json::Utf8ToWide(requestUrl, false).c_str(),
        nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        closesocket(listener);
        WSACleanup();
        return {{}, L"The default browser could not be opened for Google authorization."};
    }

    std::string requestText;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!stopToken.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{0, 250000};
        const int selected = select(0, &readable, nullptr, nullptr, &timeout);
        if (selected <= 0) continue;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::array<char, 4096> buffer{};
        while (requestText.size() < 16 * 1024) {
            const int count = recv(client, buffer.data(),
                                   static_cast<int>(buffer.size()), 0);
            if (count <= 0) break;
            requestText.append(buffer.data(), static_cast<size_t>(count));
            if (requestText.find("\r\n\r\n") != std::string::npos) break;
        }
        const std::string body =
            "<!doctype html><meta charset=utf-8><title>MdViewer</title>"
            "<p>Google Drive authorization has returned to MdViewer. "
            "You may close this tab.</p>";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(client, response.data(), static_cast<int>(response.size()), 0);
        closesocket(client);
        break;
    }
    closesocket(listener);
    WSACleanup();
    if (stopToken.stop_requested()) return {{}, {}, true};
    if (requestText.empty()) {
        return {{}, L"Google authorization timed out. Please try again."};
    }
    const size_t targetStart = requestText.find(' ');
    const size_t targetEnd = targetStart == std::string::npos
        ? std::string::npos : requestText.find(' ', targetStart + 1);
    if (targetStart == std::string::npos || targetEnd == std::string::npos) {
        return {{}, L"The Google authorization response was invalid."};
    }
    const std::string target = requestText.substr(
        targetStart + 1, targetEnd - targetStart - 1);
    const size_t question = target.find('?');
    const auto query = ParseQuery(question == std::string::npos
                                      ? std::string{} : target.substr(question + 1));
    const auto returnedState = query.find("state");
    if (returnedState == query.end() || returnedState->second != state) {
        return {{}, L"The Google authorization state did not match."};
    }
    if (const auto error = query.find("error"); error != query.end()) {
        if (error->second == "access_denied") return {{}, {}, true};
        return {{}, L"Google authorization failed: " +
                    json::Utf8ToWide(error->second)};
    }
    const auto code = query.find("code");
    if (code == query.end() || code->second.empty()) {
        return {{}, L"Google did not return an authorization code."};
    }
    if (pickerMode != google_drive::PickerMode::None) {
        const auto picked = query.find("picked_file_ids");
        if (picked == query.end() || picked->second.empty()) {
            return {{}, {}, true};
        }
        const size_t separator = picked->second.find(',');
        const std::string firstId = picked->second.substr(0, separator);
        if (firstId.empty() || firstId.size() > 1024) {
            return {{}, L"Google Picker returned an invalid file ID."};
        }
        if (pickedFileId) *pickedFileId = firstId;
    }

    std::string form = "client_id=" + UrlEncode(credentials.clientId) +
        "&code=" + UrlEncode(code->second) +
        "&code_verifier=" + UrlEncode(verifier) +
        "&grant_type=authorization_code&redirect_uri=" + UrlEncode(redirect);
    if (!credentials.clientSecret.empty()) {
        form += "&client_secret=" + UrlEncode(credentials.clientSecret);
    }
    const HttpResponse tokenResponse = Request(
        L"POST", kGoogleTokenHost, L"/token",
        L"Content-Type: application/x-www-form-urlencoded\r\n", form,
        1024 * 1024, stopToken);
    const std::string token =
        json::GetString(tokenResponse.body, "access_token").value_or("");
    if (!tokenResponse.error.empty() || tokenResponse.status != 200 || token.empty()) {
        return {{}, HttpError(tokenResponse,
                              L"Google authorization could not be completed.")};
    }
    const std::string refresh =
        json::GetString(tokenResponse.body, "refresh_token").value_or("");
    if (!refresh.empty() && !StoreRefreshToken(credentials, refresh)) {
        return {{}, L"The Google refresh token could not be stored securely in Windows."};
    }
    const auto expires =
        json::GetInteger(tokenResponse.body, "expires_in").value_or(3600);
    accessToken_ = token;
    accessTokenExpiry_ = std::chrono::system_clock::now() +
        std::chrono::seconds((std::max<std::int64_t>)(60, expires - 60));
    return {token, {}};
}

GoogleDriveResult<std::string> GoogleDriveClient::EnsureAccessToken(
    const Credentials& credentials, std::stop_token stopToken) {
    if (credentials.clientId.empty()) {
        return {{}, L"Google Drive is not configured. Add clientId to " +
                    configurationPath_.wstring() + L"."};
    }
    if (!accessToken_.empty() &&
        std::chrono::system_clock::now() < accessTokenExpiry_) {
        return {accessToken_, {}};
    }
    if (const auto refresh = ReadRefreshToken(credentials)) {
        bool invalidGrant = false;
        auto refreshed = RefreshAccessToken(
            credentials, *refresh, stopToken, &invalidGrant);
        if (refreshed || refreshed.canceled || !invalidGrant) return refreshed;
        DeleteRefreshToken();
    }
    return InteractiveAuthorize(credentials, stopToken);
}

GoogleDriveResult<GoogleDrivePickerSelection>
GoogleDriveClient::PickFile(std::stop_token stopToken) {
    const Credentials credentials = LoadCredentials();
    if (credentials.clientId.empty()) {
        return {{}, L"Google Drive is not configured. Add clientId to " +
                    configurationPath_.wstring() + L"."};
    }
    std::string fileId;
    const auto token = InteractiveAuthorize(
        credentials, stopToken, google_drive::PickerMode::MarkdownFile, &fileId);
    if (!token) return {{}, token.error, token.canceled};
    if (fileId.empty()) return {{}, {}, true};
    return {GoogleDrivePickerSelection{std::move(fileId)}, {}};
}

GoogleDriveResult<GoogleDrivePickerSelection>
GoogleDriveClient::PickFolder(std::stop_token stopToken) {
    const Credentials credentials = LoadCredentials();
    if (credentials.clientId.empty()) {
        return {{}, L"Google Drive is not configured. Add clientId to " +
                    configurationPath_.wstring() + L"."};
    }
    std::string folderId;
    const auto token = InteractiveAuthorize(
        credentials, stopToken, google_drive::PickerMode::Folder, &folderId);
    if (!token) return {{}, token.error, token.canceled};
    if (folderId.empty()) return {{}, {}, true};
    return {GoogleDrivePickerSelection{std::move(folderId)}, {}};
}

GoogleDriveClient::HttpResponse GoogleDriveClient::AuthorizedRequest(
    const Credentials& credentials, const wchar_t* method,
    const std::wstring& path, const std::wstring& contentType,
    const std::string& body, size_t maximumResponseBytes,
    std::stop_token stopToken) {
    auto token = EnsureAccessToken(credentials, stopToken);
    if (!token) return {0, {}, token.error, token.canceled};
    auto perform = [&](const std::string& accessToken) {
        std::wstring headers = L"Authorization: Bearer " +
            json::Utf8ToWide(accessToken, false) + L"\r\n";
        if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";
        return Request(method, kGoogleApiHost, path, headers, body,
                       maximumResponseBytes, stopToken);
    };
    HttpResponse response = perform(*token.value);
    if (response.status != 401 || stopToken.stop_requested()) return response;
    accessToken_.clear();
    accessTokenExpiry_ = {};
    token = EnsureAccessToken(credentials, stopToken);
    return token ? perform(*token.value)
                 : HttpResponse{0, {}, token.error, token.canceled};
}

GoogleDriveResult<GoogleDriveFile> GoogleDriveClient::DownloadFile(
    const std::string& fileId, std::stop_token stopToken) {
    if (fileId.empty()) return {{}, L"The Google Drive file ID is empty."};
    const Credentials credentials = LoadCredentials();
    const std::wstring encodedId = json::Utf8ToWide(UrlEncode(fileId), false);
    const std::wstring metadataPath = L"/drive/v3/files/" + encodedId +
        L"?fields=id,name,mimeType,modifiedTime";
    const HttpResponse metadata = AuthorizedRequest(
        credentials, L"GET", metadataPath, {}, {}, 1024 * 1024, stopToken);
    if (!metadata.error.empty() || metadata.status != 200) {
        return {{}, HttpError(metadata, L"The Google Drive file metadata could not be read."),
                metadata.canceled};
    }
    GoogleDriveFile file;
    file.id = json::GetString(metadata.body, "id").value_or(fileId);
    file.name = json::GetString(metadata.body, "name").value_or("Untitled.md");
    file.mimeType = json::GetString(metadata.body, "mimeType").value_or("");
    file.modifiedTime = json::GetString(metadata.body, "modifiedTime").value_or("");
    const HttpResponse content = AuthorizedRequest(
        credentials, L"GET", L"/drive/v3/files/" + encodedId + L"?alt=media",
        {}, {}, kMaximumDriveFileBytes + 1, stopToken);
    if (!content.error.empty() || content.status != 200) {
        return {{}, HttpError(content, L"The Google Drive file could not be downloaded."),
                content.canceled};
    }
    if (content.body.size() > kMaximumDriveFileBytes) {
        return {{}, L"The Google Drive file is larger than the 512 MB safety limit."};
    }
    file.bytes = content.body;
    return {std::move(file), {}};
}

GoogleDriveResult<GoogleDriveUpload> GoogleDriveClient::CreateMarkdownFile(
    const std::string& name, const std::string& parentFolderId,
    const std::string& bytes, const std::string& mimeType,
    std::stop_token stopToken) {
    if (name.empty() || name.size() > 1024 ||
        name.find('\0') != std::string::npos ||
        name.find('\r') != std::string::npos ||
        name.find('\n') != std::string::npos) {
        return {{}, L"The Google Drive file name is invalid."};
    }
    if (parentFolderId.size() > 1024 ||
        parentFolderId.find('\0') != std::string::npos ||
        parentFolderId.find('\r') != std::string::npos ||
        parentFolderId.find('\n') != std::string::npos) {
        return {{}, L"The Google Drive folder ID is invalid."};
    }
    if (bytes.size() > kMaximumDriveFileBytes) {
        return {{}, L"The Google Drive file is larger than the 512 MB safety limit."};
    }
    const bool safeMimeType = !mimeType.empty() && mimeType.size() <= 128 &&
        mimeType.find('\r') == std::string::npos &&
        mimeType.find('\n') == std::string::npos;
    const std::string uploadMimeType = safeMimeType
        ? mimeType : "application/octet-stream";
    std::string boundary;
    do {
        boundary = "mdviewer_" + RandomBase64Url(24);
    } while (!boundary.empty() && bytes.find(boundary) != std::string::npos);
    if (boundary.empty()) {
        return {{}, L"The Google Drive upload could not be prepared securely."};
    }
    const std::string metadata =
        google_drive::BuildCreateMetadata(name, parentFolderId, uploadMimeType);
    std::string body;
    body.reserve(metadata.size() + bytes.size() + 256);
    body += "--" + boundary +
        "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
        metadata + "\r\n--" + boundary + "\r\nContent-Type: " +
        uploadMimeType + "\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";

    const Credentials credentials = LoadCredentials();
    const std::wstring contentType = L"multipart/related; boundary=" +
        json::Utf8ToWide(boundary, false);
    const HttpResponse upload = AuthorizedRequest(
        credentials, L"POST",
        L"/upload/drive/v3/files?uploadType=multipart&supportsAllDrives=true&fields=id,name,mimeType,modifiedTime",
        contentType, body, 1024 * 1024, stopToken);
    if (!upload.error.empty() ||
        (upload.status != 200 && upload.status != 201)) {
        return {{}, HttpError(upload,
                              L"The Google Drive file could not be created."),
                upload.canceled};
    }
    GoogleDriveUpload result;
    result.id = json::GetString(upload.body, "id").value_or("");
    result.name = json::GetString(upload.body, "name").value_or(name);
    result.mimeType =
        json::GetString(upload.body, "mimeType").value_or(uploadMimeType);
    result.modifiedTime =
        json::GetString(upload.body, "modifiedTime").value_or("");
    if (result.id.empty()) {
        return {{}, L"Google Drive did not return the new file ID."};
    }
    return {std::move(result), {}};
}

GoogleDriveResult<GoogleDriveUpload> GoogleDriveClient::UploadFile(
    const std::string& fileId, const std::string& bytes,
    const std::string& mimeType, const std::string& expectedModifiedTime,
    bool overwriteConflict,
    std::stop_token stopToken) {
    if (fileId.empty()) return {{}, L"The Google Drive file ID is empty."};
    const Credentials credentials = LoadCredentials();
    const std::wstring encodedId = json::Utf8ToWide(UrlEncode(fileId), false);
    if (!overwriteConflict && !expectedModifiedTime.empty()) {
        const HttpResponse metadata = AuthorizedRequest(
            credentials, L"GET", L"/drive/v3/files/" + encodedId +
                L"?fields=modifiedTime", {}, {}, 1024 * 1024, stopToken);
        if (!metadata.error.empty() || metadata.status != 200) {
            return {{}, HttpError(metadata,
                                  L"The Google Drive file could not be checked before saving."),
                    metadata.canceled};
        }
        const std::string current =
            json::GetString(metadata.body, "modifiedTime").value_or("");
        if (!current.empty() && current != expectedModifiedTime) {
            return {{}, L"The Google Drive file was changed elsewhere.", false, true};
        }
    }
    const bool safeMimeType = !mimeType.empty() && mimeType.size() <= 128 &&
        mimeType.find('\r') == std::string::npos &&
        mimeType.find('\n') == std::string::npos;
    const std::wstring contentType = safeMimeType
        ? json::Utf8ToWide(mimeType, false) : L"application/octet-stream";
    const HttpResponse upload = AuthorizedRequest(
        credentials, L"PATCH", L"/upload/drive/v3/files/" + encodedId +
            L"?uploadType=media&fields=name,modifiedTime",
        contentType, bytes, 1024 * 1024, stopToken);
    if (!upload.error.empty() || upload.status != 200) {
        return {{}, HttpError(upload, L"The Google Drive file could not be saved."),
                upload.canceled};
    }
    GoogleDriveUpload result;
    result.name = json::GetString(upload.body, "name").value_or("");
    result.mimeType = mimeType;
    result.modifiedTime =
        json::GetString(upload.body, "modifiedTime").value_or("");
    return {std::move(result), {}};
}
