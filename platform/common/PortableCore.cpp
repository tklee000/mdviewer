#include "PortableCore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace mdviewer {
namespace {

constexpr std::uintmax_t kMaximumFileSize = 32U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumMdzFileSize = 512U * 1024U * 1024U;

std::string HexByte(unsigned char value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(2, '0');
    result[0] = digits[(value >> 4U) & 0x0FU];
    result[1] = digits[value & 0x0FU];
    return result;
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void AppendUtf8(std::string& output, unsigned int codePoint) {
    if (codePoint <= 0x7FU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

std::string PercentDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = HexValue(value[i + 1]);
            const int low = HexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

std::filesystem::path Utf8Path(const std::string& value) {
    const std::u8string encoded(value.begin(), value.end());
    return std::filesystem::path(encoded);
}

bool IsInside(const std::filesystem::path& child, const std::filesystem::path& root) {
    std::error_code error;
    const auto normalizedChild = std::filesystem::weakly_canonical(child, error);
    if (error) return false;
    const auto normalizedRoot = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    auto childIt = normalizedChild.begin();
    for (auto rootIt = normalizedRoot.begin(); rootIt != normalizedRoot.end(); ++rootIt, ++childIt) {
        if (childIt == normalizedChild.end() || *childIt != *rootIt) return false;
    }
    return true;
}

std::string MimeTypeFor(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
    if (extension == ".json" || extension == ".map") return "application/json; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".woff") return "font/woff";
    if (extension == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::optional<UiResource> ReadResource(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumFileSize) return std::nullopt;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return UiResource{MimeTypeFor(path), std::move(bytes)};
}

std::string DisplayNameFor(const std::filesystem::path& path) {
    return path.empty() ? "Untitled.md" : PathToUtf8(path.filename());
}

std::string NormalizeLineEndings(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\r') {
            if (i + 1 < value.size() && value[i + 1] == '\n') ++i;
            result.push_back('\n');
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string RestoreLineEndings(const std::string& value, bool crlf) {
    if (!crlf) return value;
    std::string result;
    result.reserve(value.size() + value.size() / 16U);
    for (const char character : value) {
        if (character == '\n') result.push_back('\r');
        result.push_back(character);
    }
    return result;
}

bool DecodeUtf8Point(std::string_view input, std::size_t& cursor,
                     unsigned int& point) {
    if (cursor >= input.size()) return false;
    const unsigned char first = static_cast<unsigned char>(input[cursor++]);
    if (first <= 0x7FU) {
        point = first;
        return true;
    }

    unsigned int minimum = 0;
    int continuationCount = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        point = first & 0x1FU;
        minimum = 0x80U;
        continuationCount = 1;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        point = first & 0x0FU;
        minimum = 0x800U;
        continuationCount = 2;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        point = first & 0x07U;
        minimum = 0x10000U;
        continuationCount = 3;
    } else {
        return false;
    }

    if (input.size() - cursor < static_cast<std::size_t>(continuationCount)) {
        return false;
    }
    for (int index = 0; index < continuationCount; ++index) {
        const unsigned char next = static_cast<unsigned char>(input[cursor++]);
        if ((next & 0xC0U) != 0x80U) return false;
        point = (point << 6U) | (next & 0x3FU);
    }
    return point >= minimum && point <= 0x10FFFFU &&
           !(point >= 0xD800U && point <= 0xDFFFU);
}

bool IsValidUtf8(std::string_view input) {
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        unsigned int point = 0;
        if (!DecodeUtf8Point(input, cursor, point)) return false;
    }
    return true;
}

void AppendUtf16Unit(std::string& output, std::uint16_t unit, bool bigEndian) {
    const char low = static_cast<char>(unit & 0xFFU);
    const char high = static_cast<char>((unit >> 8U) & 0xFFU);
    if (bigEndian) {
        output.push_back(high);
        output.push_back(low);
    } else {
        output.push_back(low);
        output.push_back(high);
    }
}

bool DecodeUtf16(std::string_view bytes, bool bigEndian, std::string& output) {
    if (bytes.size() % 2U != 0) return false;
    output.clear();
    output.reserve(bytes.size());
    for (std::size_t cursor = 0; cursor < bytes.size();) {
        const auto readUnit = [&](std::size_t offset) {
            const auto first = static_cast<unsigned char>(bytes[offset]);
            const auto second = static_cast<unsigned char>(bytes[offset + 1U]);
            return static_cast<std::uint16_t>(bigEndian
                ? (static_cast<unsigned int>(first) << 8U) | second
                : (static_cast<unsigned int>(second) << 8U) | first);
        };
        const std::uint16_t first = readUnit(cursor);
        cursor += 2U;
        unsigned int point = first;
        if (first >= 0xD800U && first <= 0xDBFFU) {
            if (cursor >= bytes.size()) return false;
            const std::uint16_t second = readUnit(cursor);
            cursor += 2U;
            if (second < 0xDC00U || second > 0xDFFFU) return false;
            point = 0x10000U + ((first - 0xD800U) << 10U) +
                    (second - 0xDC00U);
        } else if (first >= 0xDC00U && first <= 0xDFFFU) {
            return false;
        }
        AppendUtf8(output, point);
    }
    return true;
}

bool EncodeUtf16(std::string_view text, bool bigEndian, std::string& output) {
    output.clear();
    output.reserve(text.size() * 2U + 2U);
    AppendUtf16Unit(output, 0xFEFFU, bigEndian);
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        unsigned int point = 0;
        if (!DecodeUtf8Point(text, cursor, point)) return false;
        if (point <= 0xFFFFU) {
            AppendUtf16Unit(output, static_cast<std::uint16_t>(point), bigEndian);
        } else {
            point -= 0x10000U;
            AppendUtf16Unit(output,
                            static_cast<std::uint16_t>(0xD800U + (point >> 10U)),
                            bigEndian);
            AppendUtf16Unit(output,
                            static_cast<std::uint16_t>(0xDC00U + (point & 0x3FFU)),
                            bigEndian);
        }
    }
    return true;
}

bool ReadFileBytes(const std::filesystem::path& path, std::uintmax_t maximumSize,
                   std::string& bytes, std::string& errorMessage) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        errorMessage = "The selected document does not exist.";
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumSize) {
        errorMessage = "The selected document exceeds the safety size limit.";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        errorMessage = "The selected document could not be opened.";
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream),
                 std::istreambuf_iterator<char>());
    return true;
}

bool ReadMarkdown(const std::filesystem::path& path,
                  std::string& text,
                  TextEncoding& encoding,
                  bool& usedCrLf,
                  std::string& errorMessage) {
    std::string bytes;
    if (!ReadFileBytes(path, kMaximumFileSize, bytes, errorMessage)) return false;

    std::string decoded;
    if (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xEFU &&
        static_cast<unsigned char>(bytes[1]) == 0xBBU &&
        static_cast<unsigned char>(bytes[2]) == 0xBFU) {
        encoding = TextEncoding::Utf8Bom;
        decoded.assign(bytes.begin() + 3, bytes.end());
        if (!IsValidUtf8(decoded)) {
            errorMessage = "The Markdown file contains invalid UTF-8 text.";
            return false;
        }
    } else if (bytes.size() >= 2U &&
               static_cast<unsigned char>(bytes[0]) == 0xFFU &&
               static_cast<unsigned char>(bytes[1]) == 0xFEU) {
        encoding = TextEncoding::Utf16Le;
        if (!DecodeUtf16(std::string_view(bytes).substr(2), false, decoded)) {
            errorMessage = "The Markdown file contains invalid UTF-16 LE text.";
            return false;
        }
    } else if (bytes.size() >= 2U &&
               static_cast<unsigned char>(bytes[0]) == 0xFEU &&
               static_cast<unsigned char>(bytes[1]) == 0xFFU) {
        encoding = TextEncoding::Utf16Be;
        if (!DecodeUtf16(std::string_view(bytes).substr(2), true, decoded)) {
            errorMessage = "The Markdown file contains invalid UTF-16 BE text.";
            return false;
        }
    } else {
        encoding = TextEncoding::Utf8;
        decoded = std::move(bytes);
        if (!IsValidUtf8(decoded)) {
            errorMessage = "The Markdown file is not valid UTF-8/UTF-16 text.";
            return false;
        }
    }

    usedCrLf = decoded.find("\r\n") != std::string::npos;
    text = NormalizeLineEndings(std::move(decoded));
    return true;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::string& bytes,
                    std::string& errorMessage) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        errorMessage = "The destination directory could not be created.";
        return false;
    }

    auto temporary = path;
    temporary += ".mdviewer.tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        errorMessage = "The temporary save file could not be created.";
        return false;
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        std::filesystem::remove(temporary, error);
        errorMessage = "The Markdown file could not be written completely.";
        return false;
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        errorMessage = "The saved file could not replace the destination file.";
        return false;
    }
    return true;
}

bool WriteMarkdown(const std::filesystem::path& path,
                   const std::string& text,
                   TextEncoding encoding,
                   bool usedCrLf,
                   std::string& errorMessage) {
    const std::string normalized = RestoreLineEndings(text, usedCrLf);
    std::string bytes;
    switch (encoding) {
        case TextEncoding::Utf8:
            if (!IsValidUtf8(normalized)) {
                errorMessage = "The document contains invalid UTF-8 text.";
                return false;
            }
            bytes = normalized;
            break;
        case TextEncoding::Utf8Bom:
            if (!IsValidUtf8(normalized)) {
                errorMessage = "The document contains invalid UTF-8 text.";
                return false;
            }
            bytes = "\xEF\xBB\xBF";
            bytes += normalized;
            break;
        case TextEncoding::Utf16Le:
            if (!EncodeUtf16(normalized, false, bytes)) {
                errorMessage = "The document contains invalid UTF-8 text.";
                return false;
            }
            break;
        case TextEncoding::Utf16Be:
            if (!EncodeUtf16(normalized, true, bytes)) {
                errorMessage = "The document contains invalid UTF-8 text.";
                return false;
            }
            break;
    }
    return WriteFileBytes(path, bytes, errorMessage);
}

std::optional<std::string> ResolveArchiveReference(
    const std::string& entryPoint, const std::string& reference) {
    if (reference.empty() || reference.front() == '/' ||
        reference.find('\\') != std::string::npos) return std::nullopt;

    std::vector<std::string> segments;
    const std::size_t baseEnd = entryPoint.find_last_of('/');
    if (baseEnd != std::string::npos) {
        std::size_t start = 0;
        while (start < baseEnd) {
            const std::size_t slash = entryPoint.find('/', start);
            const std::size_t end = (std::min)(slash, baseEnd);
            if (end > start) segments.push_back(entryPoint.substr(start, end - start));
            if (slash == std::string::npos || slash >= baseEnd) break;
            start = slash + 1U;
        }
    }
    std::size_t start = 0;
    while (start <= reference.size()) {
        const std::size_t slash = reference.find('/', start);
        const std::size_t end = slash == std::string::npos
            ? reference.size() : slash;
        const std::string segment = reference.substr(start, end - start);
        if (segment.empty() || segment == ".") {
        } else if (segment == "..") {
            if (segments.empty()) return std::nullopt;
            segments.pop_back();
        } else {
            segments.push_back(segment);
        }
        if (slash == std::string::npos) break;
        start = slash + 1U;
    }
    std::string result;
    for (const auto& segment : segments) {
        if (!result.empty()) result.push_back('/');
        result += segment;
    }
    return mdz::IsSafeArchivePath(result) ? std::optional(result) : std::nullopt;
}

std::string EncodingName(TextEncoding encoding) {
    switch (encoding) {
        case TextEncoding::Utf8: return "UTF-8";
        case TextEncoding::Utf8Bom: return "UTF-8 BOM";
        case TextEncoding::Utf16Le: return "UTF-16 LE";
        case TextEncoding::Utf16Be: return "UTF-16 BE";
    }
    return "UTF-8";
}

std::string NarrowError(const std::wstring& value) {
    return std::string(value.begin(), value.end());
}

}  // namespace

FileResourceProvider::FileResourceProvider(std::filesystem::path webRoot)
    : webRoot_(std::move(webRoot)) {}

std::optional<UiResource> FileResourceProvider::Load(const std::string& url) const {
    constexpr const char* prefix = "https://app.mdviewer/";
    if (url.rfind(prefix, 0) != 0) return std::nullopt;

    std::string relative = url.substr(std::char_traits<char>::length(prefix));
    if (relative.empty()) relative = "index.html";
    const auto queryOffset = relative.find('?');
    const std::string query = queryOffset == std::string::npos ? std::string() : relative.substr(queryOffset + 1);
    relative = relative.substr(0, queryOffset);

    std::filesystem::path root = webRoot_;
    std::filesystem::path candidate;
    if (relative == "__asset") {
        constexpr const char* pathKey = "path=";
        if (query.rfind(pathKey, 0) != 0) return std::nullopt;
        const std::string reference = PercentDecode(query.substr(5));
        std::shared_ptr<const mdz::Entries> archive;
        std::string entryPoint;
        {
            std::lock_guard<std::mutex> lock(documentMutex_);
            root = documentDirectory_;
            archive = documentArchive_;
            entryPoint = archiveEntryPoint_;
        }
        if (archive) {
            const auto archivePath = ResolveArchiveReference(entryPoint, reference);
            if (!archivePath) return std::nullopt;
            const auto item = archive->find(*archivePath);
            if (item == archive->end()) return std::nullopt;
            const std::string mimeType = MimeTypeFor(Utf8Path(*archivePath));
            if (mimeType.rfind("image/", 0) != 0) return std::nullopt;
            return UiResource{
                mimeType,
                std::string(reinterpret_cast<const char*>(item->second.data()),
                            item->second.size())};
        }
        if (root.empty()) return std::nullopt;
        candidate = root / Utf8Path(reference);
    } else {
        candidate = root / Utf8Path(PercentDecode(relative));
    }

    if (!IsInside(candidate, root)) return std::nullopt;
    return ReadResource(candidate);
}

void FileResourceProvider::SetDocumentDirectory(std::filesystem::path directory) {
    std::lock_guard<std::mutex> lock(documentMutex_);
    documentDirectory_ = std::move(directory);
    documentArchive_.reset();
    archiveEntryPoint_.clear();
}

void FileResourceProvider::SetDocumentArchive(
    std::shared_ptr<const mdz::Entries> entries, std::string entryPoint) {
    std::lock_guard<std::mutex> lock(documentMutex_);
    documentDirectory_.clear();
    documentArchive_ = std::move(entries);
    archiveEntryPoint_ = std::move(entryPoint);
}

EditorController::EditorController(PortablePlatform& platform,
                                   std::shared_ptr<FileResourceProvider> resources,
                                   std::string language,
                                   std::string theme)
    : platform_(platform),
      resources_(std::move(resources)),
      language_(std::move(language)),
      theme_(std::move(theme)) {
    UpdateTitle();
}

void EditorController::SetSender(SendJson sender) {
    sender_ = std::move(sender);
}

bool EditorController::OpenInitialFile(const std::filesystem::path& path) {
    if (path.empty()) return true;
    return OpenDocument(path);
}

bool EditorController::OpenRequestedFile(const std::filesystem::path& path) {
    if (path.empty() || !ConfirmDiscardOrSave()) return false;
    return OpenDocument(path);
}

void EditorController::OnWebMessage(const std::string& message) {
    const auto type = JsonString(message, "type");
    if (!type) return;

    if (*type == "ready") {
        if (const auto value = JsonString(message, "language")) language_ = *value;
        if (const auto value = JsonString(message, "theme")) theme_ = *value;
        ready_ = true;
        SendInitialState();
        Send(std::string("{\"type\":\"window.stateChanged\",\"maximized\":") +
             (platform_.IsWindowMaximized() ? "true}" : "false}"));
        SendMdzPasswordRequest();
        return;
    }
    if (*type == "document.changed") {
        bool changed = false;
        if (const auto text = JsonString(message, "text")) {
            if (*text != text_) {
                text_ = *text;
                changed = true;
            }
        }
        if (const auto eol = JsonString(message, "eol")) {
            const bool crlf = *eol == "CRLF";
            if ((*eol == "LF" || *eol == "CRLF") && crlf != usedCrLf_) {
                usedCrLf_ = crlf;
                changed = true;
            }
        }
        const auto reportedDirty = JsonBool(message, "dirty");
        if (reportedDirty || changed) {
            dirty_ = reportedDirty.value_or(true);
            UpdateTitle();
        }
        return;
    }
    if (*type == "editor.modeChanged") {
        if (const auto mode = JsonString(message, "mode")) editorMode_ = *mode;
        return;
    }
    if (*type == "mdz.passwordChanged") {
        const std::string password = JsonString(message, "password").value_or("");
        if (password.size() <= 1024) ChangeMdzPassword(password);
        return;
    }
    if (*type == "mdz.passwordResponse") {
        const std::string password = JsonString(message, "password").value_or("");
        if (!pendingMdzPath_ || password.empty() || password.size() > 1024) return;
        const auto path = *pendingMdzPath_;
        pendingMdzPath_.reset();
        OpenDocument(path, password);
        return;
    }
    if (*type == "mdz.passwordCanceled") {
        pendingMdzPath_.reset();
        pendingMdzPasswordIncorrect_ = false;
        return;
    }
    if (*type == "settings.languageChanged") {
        if (const auto value = JsonString(message, "locale")) {
            language_ = *value;
            Send("{\"type\":\"language.changed\",\"locale\":" +
                 JsonQuote(language_) + "}");
        }
        return;
    }
    if (*type == "settings.themeChanged") {
        if (const auto value = JsonString(message, "theme")) {
            theme_ = *value == "light" ? "light" : "dark";
            Send("{\"type\":\"theme.changed\",\"theme\":" +
                 JsonQuote(theme_) + "}");
        }
        return;
    }
    if (*type == "openExternal") {
        if (const auto url = JsonString(message, "url")) {
            if (url->rfind("https://", 0) == 0 || url->rfind("http://", 0) == 0 ||
                url->rfind("mailto:", 0) == 0) {
                platform_.OpenExternal(*url);
            }
        }
        return;
    }
    if (*type != "command") return;

    const auto name = JsonString(message, "name");
    if (!name) return;
    if (*name == "file.new") {
        if (ConfirmDiscardOrSave()) NewDocument();
    } else if (*name == "file.open") {
        if (!ConfirmDiscardOrSave()) return;
        if (const auto selected = platform_.ChooseOpenFile()) OpenDocument(*selected);
    } else if (*name == "file.save") {
        SaveDocument(false);
    } else if (*name == "file.saveAs") {
        SaveDocument(true);
    } else if (*name == "app.exit" || *name == "window.close") {
        platform_.RequestClose();
    } else if (*name == "app.about") {
        platform_.ShowAbout();
    } else if (*name == "window.minimize") {
        platform_.MinimizeWindow();
    } else if (*name == "window.maximizeToggle") {
        platform_.ToggleMaximizeWindow();
        Send(std::string("{\"type\":\"window.stateChanged\",\"maximized\":") +
             (platform_.IsWindowMaximized() ? "true}" : "false}"));
    } else if (*name == "window.drag") {
        platform_.BeginWindowDrag();
    }
}

bool EditorController::ConfirmClose() {
    return ConfirmDiscardOrSave();
}

bool EditorController::ConfirmDiscardOrSave() {
    if (!dirty_) return true;
    const auto result = platform_.ConfirmSaveChanges(DisplayNameFor(path_));
    if (result == SavePromptResult::Cancel) return false;
    if (result == SavePromptResult::Discard) return true;
    return SaveDocument(false);
}

bool EditorController::OpenDocument(const std::filesystem::path& path,
                                    const std::string& password) {
    std::string loaded;
    TextEncoding encoding = TextEncoding::Utf8;
    bool usedCrLf = false;
    std::string error;
    std::shared_ptr<mdz::Package> archive;
    const DocumentFormat format = mdz::IsMdzPath(path.wstring())
        ? DocumentFormat::Mdz : DocumentFormat::Markdown;
    if (format == DocumentFormat::Mdz) {
        std::string bytes;
        if (!ReadFileBytes(path, kMaximumMdzFileSize, bytes, error)) {
            platform_.ShowError("Open failed", error);
            return false;
        }
        mdz::Package package;
        std::wstring archiveError;
        mdz::ReadStatus readStatus = mdz::ReadStatus::Error;
        if (!mdz::ReadBytes(bytes, &package, &archiveError,
                            password, &readStatus)) {
            if (readStatus == mdz::ReadStatus::PasswordRequired ||
                readStatus == mdz::ReadStatus::IncorrectPassword) {
                pendingMdzPath_ = path;
                pendingMdzPasswordIncorrect_ =
                    readStatus == mdz::ReadStatus::IncorrectPassword;
                SendMdzPasswordRequest();
                return true;
            }
            platform_.ShowError("Open failed", NarrowError(archiveError));
            return false;
        }
        const auto entry = package.entries.find(package.entryPoint);
        if (entry == package.entries.end()) {
            platform_.ShowError("Open failed", "The MDZ Markdown entry is missing.");
            return false;
        }
        loaded.assign(reinterpret_cast<const char*>(entry->second.data()),
                      entry->second.size());
        usedCrLf = loaded.find("\r\n") != std::string::npos;
        loaded = NormalizeLineEndings(std::move(loaded));
        archive = std::make_shared<mdz::Package>(std::move(package));
    } else if (!ReadMarkdown(path, loaded, encoding, usedCrLf, error)) {
        platform_.ShowError("Open failed", error);
        return false;
    }
    path_ = std::filesystem::absolute(path);
    text_ = std::move(loaded);
    encoding_ = encoding;
    format_ = format;
    usedCrLf_ = usedCrLf;
    archive_ = std::move(archive);
    mdzPassword_ = format == DocumentFormat::Mdz ? password : std::string{};
    mdzPasswordDirty_ = false;
    pendingMdzPath_.reset();
    pendingMdzPasswordIncorrect_ = false;
    dirty_ = false;
    editorMode_ = "preview";
    if (archive_) {
        resources_->SetDocumentArchive(
            std::shared_ptr<const mdz::Entries>(archive_, &archive_->entries),
            archive_->entryPoint);
    } else {
        resources_->SetDocumentDirectory(path_.parent_path());
    }
    UpdateTitle();
    if (ready_) SendDocumentState("document.opened");
    return true;
}

bool EditorController::SaveDocument(bool forceSaveAs) {
    std::filesystem::path destination = path_;
    TextEncoding destinationEncoding = encoding_;
    DocumentFormat destinationFormat = format_;
    if (forceSaveAs || destination.empty()) {
        const auto selected = platform_.ChooseSaveFile(
            destination, destinationEncoding, destinationFormat);
        if (!selected) return false;
        destination = selected->path;
        destinationEncoding = selected->encoding;
        destinationFormat = selected->format;
        if (destination.extension().empty()) {
            destination += destinationFormat == DocumentFormat::Mdz ? ".mdz" : ".md";
        }
    }

    std::string error;
    bool saved = false;
    std::shared_ptr<mdz::Package> savedArchive;
    const std::string savedMdzPassword =
        destinationFormat == DocumentFormat::Mdz &&
        format_ == DocumentFormat::Mdz ? mdzPassword_ : std::string{};
    if (destinationFormat == DocumentFormat::Mdz) {
        mdz::Package package = archive_
            ? *archive_
            : mdz::CreateDocument(text_, PathToUtf8(destination.stem()));
        const std::string markdown = RestoreLineEndings(text_, usedCrLf_);
        package.entries[package.entryPoint] =
            mdz::Bytes(markdown.begin(), markdown.end());
        std::string bytes;
        std::wstring archiveError;
        if (!mdz::BuildBytes(package, &bytes, &archiveError,
                             savedMdzPassword)) {
            error = NarrowError(archiveError);
        } else if ((saved = WriteFileBytes(destination, bytes, error))) {
            savedArchive = std::make_shared<mdz::Package>(std::move(package));
        }
        destinationEncoding = TextEncoding::Utf8;
    } else {
        saved = WriteMarkdown(
            destination, text_, destinationEncoding, usedCrLf_, error);
    }
    if (!saved) {
        if (error.empty()) error = "The document could not be saved.";
        platform_.ShowError("Save failed", error);
        return false;
    }
    path_ = std::filesystem::absolute(destination);
    encoding_ = destinationEncoding;
    format_ = destinationFormat;
    archive_ = std::move(savedArchive);
    mdzPassword_ = destinationFormat == DocumentFormat::Mdz
        ? savedMdzPassword : std::string{};
    mdzPasswordDirty_ = false;
    dirty_ = false;
    if (archive_) {
        resources_->SetDocumentArchive(
            std::shared_ptr<const mdz::Entries>(archive_, &archive_->entries),
            archive_->entryPoint);
    } else {
        resources_->SetDocumentDirectory(path_.parent_path());
    }
    UpdateTitle();
    if (ready_) SendDocumentState("document.saved");
    return true;
}

void EditorController::NewDocument() {
    path_.clear();
    text_.clear();
    dirty_ = false;
    encoding_ = TextEncoding::Utf8;
    format_ = DocumentFormat::Markdown;
    usedCrLf_ = false;
    archive_.reset();
    mdzPassword_.clear();
    mdzPasswordDirty_ = false;
    editorMode_ = "preview";
    resources_->SetDocumentDirectory({});
    UpdateTitle();
    if (ready_) SendDocumentState("document.opened");
}

void EditorController::SendInitialState() {
    SendDocumentState("app.init");
}

void EditorController::SendMdzPasswordRequest() {
    if (!ready_ || !pendingMdzPath_) return;
    Send("{\"type\":\"mdz.passwordRequired\",\"name\":" +
         JsonQuote(DisplayNameFor(*pendingMdzPath_)) +
         ",\"incorrect\":" +
         (pendingMdzPasswordIncorrect_ ? "true}" : "false}"));
}

void EditorController::ChangeMdzPassword(const std::string& password) {
    if (format_ != DocumentFormat::Mdz || password.size() > 1024) return;
    const bool changed = password != mdzPassword_;
    if (changed) {
        mdzPassword_ = password;
        mdzPasswordDirty_ = true;
        dirty_ = true;
        UpdateTitle();
    }
    Send("{\"type\":\"mdz.passwordChanged\",\"encrypted\":" +
         std::string(mdzPassword_.empty() ? "false" : "true") +
         ",\"changed\":" + (changed ? "true" : "false") +
         ",\"dirty\":" + (dirty_ ? "true}" : "false}"));
}

void EditorController::SendDocumentState(const char* messageType) {
    Send("{\"type\":" + JsonQuote(messageType) +
         ",\"language\":" + JsonQuote(language_) +
         ",\"theme\":" + JsonQuote(theme_) +
         ",\"mode\":" + JsonQuote(editorMode_) +
         ",\"capabilities\":{\"googleDrive\":false}" +
         ",\"document\":{\"path\":" + JsonQuote(PathToUtf8(path_)) +
         ",\"name\":" + JsonQuote(DisplayNameFor(path_)) +
         ",\"text\":" + JsonQuote(text_) +
         ",\"dirty\":" + (dirty_ ? "true" : "false") +
         ",\"mdzEncrypted\":" + (!mdzPassword_.empty() ? "true" : "false") +
         ",\"mdzPasswordDirty\":" + (mdzPasswordDirty_ ? "true" : "false") +
         ",\"format\":" + JsonQuote(format_ == DocumentFormat::Mdz
             ? "mdz" : "markdown") +
         ",\"encoding\":" + JsonQuote(EncodingName(encoding_)) + ",\"eol\":" +
         JsonQuote(usedCrLf_ ? "CRLF" : "LF") + "}}");
}

void EditorController::SendSettings() {
    Send("{\"type\":\"settings.changed\",\"language\":" + JsonQuote(language_) +
         ",\"theme\":" + JsonQuote(theme_) + "}");
}

void EditorController::UpdateTitle() {
    platform_.SetWindowTitle(DisplayNameFor(path_) + (dirty_ ? " * - MdViewer" : " - MdViewer"));
}

void EditorController::Send(const std::string& json) const {
    if (sender_) sender_(json);
}

std::string JsonQuote(const std::string& value) {
    std::string result = "\"";
    result.reserve(value.size() + 2U);
    for (const unsigned char character : value) {
        switch (character) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) result += "\\u00" + HexByte(character);
                else result.push_back(static_cast<char>(character));
                break;
        }
    }
    result.push_back('\"');
    return result;
}

std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    if (cursor == std::string::npos) return std::nullopt;
    cursor = json.find(':', cursor + token.size());
    if (cursor == std::string::npos) return std::nullopt;
    cursor = json.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor == std::string::npos || json[cursor] != '\"') return std::nullopt;
    ++cursor;

    std::string result;
    while (cursor < json.size()) {
        const char character = json[cursor++];
        if (character == '\"') return result;
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (cursor >= json.size()) return std::nullopt;
        const char escaped = json[cursor++];
        switch (escaped) {
            case '\"': result.push_back('\"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (cursor + 4 > json.size()) return std::nullopt;
                unsigned int point = 0;
                for (int i = 0; i < 4; ++i) {
                    const int value = HexValue(json[cursor++]);
                    if (value < 0) return std::nullopt;
                    point = (point << 4U) | static_cast<unsigned int>(value);
                }
                if (point >= 0xD800U && point <= 0xDBFFU && cursor + 6 <= json.size() &&
                    json[cursor] == '\\' && json[cursor + 1] == 'u') {
                    cursor += 2;
                    unsigned int low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int value = HexValue(json[cursor++]);
                        if (value < 0) return std::nullopt;
                        low = (low << 4U) | static_cast<unsigned int>(value);
                    }
                    if (low >= 0xDC00U && low <= 0xDFFFU) {
                        point = 0x10000U + ((point - 0xD800U) << 10U) + (low - 0xDC00U);
                    }
                }
                AppendUtf8(result, point);
                break;
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> JsonBool(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    if (cursor == std::string::npos) return std::nullopt;
    cursor = json.find(':', cursor + token.size());
    if (cursor == std::string::npos) return std::nullopt;
    cursor = json.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor == std::string::npos) return std::nullopt;
    if (json.compare(cursor, 4, "true") == 0) return true;
    if (json.compare(cursor, 5, "false") == 0) return false;
    return std::nullopt;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

}  // namespace mdviewer
