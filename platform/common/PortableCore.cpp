#include "PortableCore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace mdviewer {
namespace {

constexpr std::uintmax_t kMaximumFileSize = 32U * 1024U * 1024U;

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
    const auto normalizedChild = std::filesystem::weakly_canonical(child);
    const auto normalizedRoot = std::filesystem::weakly_canonical(root);
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

bool ReadMarkdown(const std::filesystem::path& path,
                  std::string& text,
                  bool& hadBom,
                  bool& usedCrLf,
                  std::string& errorMessage) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        errorMessage = "The selected Markdown file does not exist.";
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumFileSize) {
        errorMessage = "The Markdown file is larger than the 32 MB safety limit.";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        errorMessage = "The Markdown file could not be opened.";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    hadBom = bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xEFU &&
             static_cast<unsigned char>(bytes[1]) == 0xBBU &&
             static_cast<unsigned char>(bytes[2]) == 0xBFU;
    if (hadBom) bytes.erase(0, 3);
    usedCrLf = bytes.find("\r\n") != std::string::npos;
    text = NormalizeLineEndings(std::move(bytes));
    return true;
}

bool WriteMarkdown(const std::filesystem::path& path,
                   const std::string& text,
                   bool hadBom,
                   bool usedCrLf,
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
    if (hadBom) stream.write("\xEF\xBB\xBF", 3);
    const std::string bytes = RestoreLineEndings(text, usedCrLf);
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
        if (query.rfind(pathKey, 0) != 0 || documentDirectory_.empty()) return std::nullopt;
        root = documentDirectory_;
        candidate = root / Utf8Path(PercentDecode(query.substr(5)));
    } else {
        candidate = root / Utf8Path(PercentDecode(relative));
    }

    try {
        if (!IsInside(candidate, root)) return std::nullopt;
        return ReadResource(candidate);
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

void FileResourceProvider::SetDocumentDirectory(std::filesystem::path directory) {
    documentDirectory_ = std::move(directory);
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

bool EditorController::OpenDocument(const std::filesystem::path& path) {
    std::string loaded;
    bool hadBom = false;
    bool usedCrLf = false;
    std::string error;
    if (!ReadMarkdown(path, loaded, hadBom, usedCrLf, error)) {
        platform_.ShowError("Open failed", error);
        return false;
    }
    path_ = std::filesystem::absolute(path);
    text_ = std::move(loaded);
    hadBom_ = hadBom;
    usedCrLf_ = usedCrLf;
    dirty_ = false;
    editorMode_ = "preview";
    resources_->SetDocumentDirectory(path_.parent_path());
    UpdateTitle();
    if (ready_) SendDocumentState("document.opened");
    return true;
}

bool EditorController::SaveDocument(bool forceSaveAs) {
    std::filesystem::path destination = path_;
    if (forceSaveAs || destination.empty()) {
        const auto selected = platform_.ChooseSaveFile(destination);
        if (!selected) return false;
        destination = *selected;
        if (destination.extension().empty()) destination += ".md";
    }

    std::string error;
    if (!WriteMarkdown(destination, text_, hadBom_, usedCrLf_, error)) {
        platform_.ShowError("Save failed", error);
        return false;
    }
    path_ = std::filesystem::absolute(destination);
    dirty_ = false;
    resources_->SetDocumentDirectory(path_.parent_path());
    UpdateTitle();
    if (ready_) SendDocumentState("document.saved");
    return true;
}

void EditorController::NewDocument() {
    path_.clear();
    text_.clear();
    dirty_ = false;
    hadBom_ = false;
    usedCrLf_ = false;
    editorMode_ = "preview";
    resources_->SetDocumentDirectory({});
    UpdateTitle();
    if (ready_) SendDocumentState("document.opened");
}

void EditorController::SendInitialState() {
    SendDocumentState("app.init");
}

void EditorController::SendDocumentState(const char* messageType) {
    Send("{\"type\":" + JsonQuote(messageType) +
         ",\"language\":" + JsonQuote(language_) +
         ",\"theme\":" + JsonQuote(theme_) +
         ",\"mode\":" + JsonQuote(editorMode_) +
         ",\"document\":{\"path\":" + JsonQuote(PathToUtf8(path_)) +
         ",\"name\":" + JsonQuote(DisplayNameFor(path_)) +
         ",\"text\":" + JsonQuote(text_) +
         ",\"dirty\":" + (dirty_ ? "true" : "false") +
         ",\"encoding\":\"UTF-8\",\"eol\":" +
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
