#include "Config.h"

#include "Json.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {

std::wstring Trim(std::wstring value) {
    const wchar_t* whitespace = L" \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::wstring SystemLanguage() {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH)) {
        return NormalizeAppLanguage(localeName);
    }
    return L"ko-KR";
}

int ToInt(const std::map<std::wstring, std::wstring>& values,
          const std::wstring& key, int fallback) {
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    try {
        return std::stoi(found->second);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

const std::vector<std::wstring>& SupportedAppLanguages() {
    static const std::vector<std::wstring> languages = {
        L"en-US", L"ko-KR", L"ja-JP", L"fr-FR", L"de-DE", L"ru-RU",
        L"zh-CN", L"zh-TW", L"es-ES", L"pt-BR", L"hi-IN", L"id-ID"};
    return languages;
}

std::wstring NormalizeAppLanguage(std::wstring language) {
    std::replace(language.begin(), language.end(), L'_', L'-');
    std::transform(language.begin(), language.end(), language.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });

    if (language.rfind(L"zh-hant", 0) == 0 ||
        language.rfind(L"zh-tw", 0) == 0 ||
        language.rfind(L"zh-hk", 0) == 0) return L"zh-TW";
    if (language.rfind(L"zh", 0) == 0) return L"zh-CN";
    if (language.rfind(L"en", 0) == 0) return L"en-US";
    if (language.rfind(L"ko", 0) == 0) return L"ko-KR";
    if (language.rfind(L"ja", 0) == 0) return L"ja-JP";
    if (language.rfind(L"fr", 0) == 0) return L"fr-FR";
    if (language.rfind(L"de", 0) == 0) return L"de-DE";
    if (language.rfind(L"ru", 0) == 0) return L"ru-RU";
    if (language.rfind(L"es", 0) == 0) return L"es-ES";
    if (language.rfind(L"pt", 0) == 0) return L"pt-BR";
    if (language.rfind(L"hi", 0) == 0) return L"hi-IN";
    if (language.rfind(L"id", 0) == 0) return L"id-ID";
    return L"ko-KR";
}

ConfigStore::ConfigStore() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &localAppData))) {
        std::filesystem::path directory(localAppData);
        CoTaskMemFree(localAppData);
        directory /= L"MdViewer";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        path_ = (directory / L"config.ini").wstring();
    } else {
        path_ = L"config.ini";
    }
}

AppConfig ConfigStore::Load() const {
    AppConfig config;
    config.language = SystemLanguage();

    std::ifstream input(path_, std::ios::binary);
    if (!input) return config;
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    std::wistringstream stream(json::Utf8ToWide(bytes));
    std::map<std::wstring, std::wstring> values;
    std::wstring section;
    std::wstring line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos) continue;
        values[section + L"." + Trim(line.substr(0, separator))] =
            Trim(line.substr(separator + 1));
    }

    config.windowWidth = (std::max)(900, ToInt(values, L"window.width", config.windowWidth));
    config.windowHeight = (std::max)(600, ToInt(values, L"window.height", config.windowHeight));
    config.maximized = ToInt(values, L"window.maximized", 0) != 0;
    const auto language = values.find(L"app.language");
    if (language != values.end()) config.language = NormalizeAppLanguage(language->second);
    const auto theme = values.find(L"app.theme");
    if (theme != values.end()) config.theme = theme->second == L"light" ? L"light" : L"dark";
    return config;
}

bool ConfigStore::Save(const AppConfig& config) const {
    const std::filesystem::path target(path_);
    const std::filesystem::path temporary = target.wstring() + L".tmp";
    std::wostringstream text;
    text << L"[window]\n"
         << L"width=" << config.windowWidth << L"\n"
         << L"height=" << config.windowHeight << L"\n"
         << L"maximized=" << (config.maximized ? 1 : 0) << L"\n\n"
         << L"[app]\n"
         << L"language=" << NormalizeAppLanguage(config.language) << L"\n"
         << L"theme=" << (config.theme == L"light" ? L"light" : L"dark") << L"\n";

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const std::string bytes = json::WideToUtf8(text.str());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return false;
    return MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
