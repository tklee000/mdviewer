#include "Json.h"

#include <windows.h>

#include <cctype>
#include <cstdio>

namespace {

size_t SkipSpace(const std::string& text, size_t position) {
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    return position;
}

std::optional<size_t> FindValue(const std::string& object,
                                const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t position = 0;
    while ((position = object.find(needle, position)) != std::string::npos) {
        const size_t colon = object.find(':', position + needle.size());
        if (colon == std::string::npos) return std::nullopt;
        return SkipSpace(object, colon + 1);
    }
    return std::nullopt;
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<unsigned int> ParseHex4(const std::string& text,
                                      size_t position) {
    if (position + 4 > text.size()) return std::nullopt;
    unsigned int result = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int digit = HexValue(text[position + i]);
        if (digit < 0) return std::nullopt;
        result = (result << 4) | static_cast<unsigned int>(digit);
    }
    return result;
}

void AppendUtf8(std::string& output, unsigned int point) {
    if (point <= 0x7F) {
        output.push_back(static_cast<char>(point));
    } else if (point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (point >> 6)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
}

}  // namespace

namespace json {

std::string Escape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
                output += escaped;
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    return output;
}

std::string Quote(const std::string& value) {
    return "\"" + Escape(value) + "\"";
}

std::optional<std::string> GetString(const std::string& object,
                                     const std::string& key) {
    const auto start = FindValue(object, key);
    if (!start || *start >= object.size() || object[*start] != '"') {
        return std::nullopt;
    }

    std::string result;
    for (size_t i = *start + 1; i < object.size(); ++i) {
        const char character = object[i];
        if (character == '"') return result;
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (++i >= object.size()) return std::nullopt;
        switch (object[i]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            const auto first = ParseHex4(object, i + 1);
            if (!first) return std::nullopt;
            i += 4;
            unsigned int point = *first;
            if (point >= 0xD800 && point <= 0xDBFF && i + 6 < object.size() &&
                object[i + 1] == '\\' && object[i + 2] == 'u') {
                const auto second = ParseHex4(object, i + 3);
                if (second && *second >= 0xDC00 && *second <= 0xDFFF) {
                    point = 0x10000 + ((point - 0xD800) << 10) +
                            (*second - 0xDC00);
                    i += 6;
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

std::optional<bool> GetBool(const std::string& object,
                            const std::string& key) {
    const auto start = FindValue(object, key);
    if (!start) return std::nullopt;
    if (object.compare(*start, 4, "true") == 0) return true;
    if (object.compare(*start, 5, "false") == 0) return false;
    return std::nullopt;
}

std::optional<std::int64_t> GetInteger(const std::string& object,
                                       const std::string& key) {
    const auto start = FindValue(object, key);
    if (!start || *start >= object.size()) return std::nullopt;
    size_t end = *start;
    if (object[end] == '-') ++end;
    const size_t digits = end;
    while (end < object.size() &&
           std::isdigit(static_cast<unsigned char>(object[end]))) ++end;
    if (end == digits) return std::nullopt;
    try {
        return std::stoll(object.substr(*start, end - *start));
    } catch (...) {
        return std::nullopt;
    }
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), output.data(), size,
                        nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value, bool allowAnsiFallback) {
    if (value.empty()) return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(
        codePage, flags, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0 && allowAnsiFallback) {
        codePage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(
            codePage, flags, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
    }
    if (size <= 0) return {};
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(codePage, flags, value.data(),
                        static_cast<int>(value.size()), output.data(), size);
    return output;
}

bool IsValidUtf8(const std::string& value) {
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

}  // namespace json
