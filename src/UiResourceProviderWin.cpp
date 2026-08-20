#include "UiResourceProvider.h"

#include "Json.h"
#include "resource.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

namespace {

constexpr std::string_view kUiOrigin = "https://app.mdviewer/";
constexpr std::string_view kAssetPrefix = "https://app.mdviewer/__asset?path=";
constexpr wchar_t kDevelopmentUiEnvironment[] = L"MDVIEWER_DEVELOPMENT_UI";

struct ResourceDescriptor {
    std::string_view path;
    int id;
    const char* mimeType;
    const wchar_t* developmentPath;
};

constexpr std::array<ResourceDescriptor, 15> kResources = {{
    {"index.html", IDR_UI_INDEX, "text/html", L"index.html"},
    {"styles.css", IDR_UI_STYLES, "text/css", L"styles.css"},
    {"i18n.js", IDR_UI_I18N, "application/javascript", L"i18n.js"},
    {"app.js", IDR_UI_SCRIPT, "application/javascript", L"app.js"},
    {"locales/ko-KR.json", IDR_LOCALE_KO_KR, "application/json", L"locales\\ko-KR.json"},
    {"locales/ja-JP.json", IDR_LOCALE_JA_JP, "application/json", L"locales\\ja-JP.json"},
    {"locales/fr-FR.json", IDR_LOCALE_FR_FR, "application/json", L"locales\\fr-FR.json"},
    {"locales/de-DE.json", IDR_LOCALE_DE_DE, "application/json", L"locales\\de-DE.json"},
    {"locales/zh-CN.json", IDR_LOCALE_ZH_CN, "application/json", L"locales\\zh-CN.json"},
    {"locales/zh-TW.json", IDR_LOCALE_ZH_TW, "application/json", L"locales\\zh-TW.json"},
    {"locales/es-ES.json", IDR_LOCALE_ES_ES, "application/json", L"locales\\es-ES.json"},
    {"locales/pt-BR.json", IDR_LOCALE_PT_BR, "application/json", L"locales\\pt-BR.json"},
    {"locales/hi-IN.json", IDR_LOCALE_HI_IN, "application/json", L"locales\\hi-IN.json"},
    {"locales/id-ID.json", IDR_LOCALE_ID_ID, "application/json", L"locales\\id-ID.json"},
    {"locales/ru-RU.json", IDR_LOCALE_RU_RU, "application/json", L"locales\\ru-RU.json"},
}};

std::optional<std::filesystem::path> DevelopmentUiDirectory() {
    const DWORD length =
        GetEnvironmentVariableW(kDevelopmentUiEnvironment, nullptr, 0);
    if (!length) return std::nullopt;
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        kDevelopmentUiEnvironment, value.data(), length);
    if (!copied || copied >= length) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(value);
}

std::vector<unsigned char> ReadFile(const std::filesystem::path& path,
                                    size_t maximumSize = 32 * 1024 * 1024) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumSize) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

std::vector<unsigned char> ReadEmbedded(HINSTANCE instance, int id) {
    const HRSRC resource =
        FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return {};
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* data = static_cast<const unsigned char*>(
        loaded ? LockResource(loaded) : nullptr);
    return data && size ? std::vector<unsigned char>(data, data + size)
                        : std::vector<unsigned char>{};
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> PercentDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) return std::nullopt;
        const int high = HexValue(value[index + 1]);
        const int low = HexValue(value[index + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return result;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool IsInside(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
    std::wstring rootText = Lower(root.lexically_normal().wstring());
    std::wstring candidateText = Lower(candidate.lexically_normal().wstring());
    if (!rootText.empty() && rootText.back() != L'\\') rootText.push_back(L'\\');
    return candidateText.compare(0, rootText.size(), rootText) == 0;
}

const char* ImageMimeType(const std::filesystem::path& path) {
    const std::wstring extension = Lower(path.extension().wstring());
    if (extension == L".png") return "image/png";
    if (extension == L".jpg" || extension == L".jpeg") return "image/jpeg";
    if (extension == L".gif") return "image/gif";
    if (extension == L".webp") return "image/webp";
    if (extension == L".bmp") return "image/bmp";
    return nullptr;
}

class WindowsUiResourceProvider final : public UiResourceProvider {
public:
    explicit WindowsUiResourceProvider(HINSTANCE instance)
        : instance_(instance), developmentDirectory_(DevelopmentUiDirectory()) {}

    UiResource Load(const std::string& url) const override {
        if (url.compare(0, kAssetPrefix.size(), kAssetPrefix) == 0) {
            return LoadDocumentAsset(url);
        }

        UiResource result;
        if (url.compare(0, kUiOrigin.size(), kUiOrigin) != 0) return result;
        std::string_view path(url.data() + kUiOrigin.size(),
                              url.size() - kUiOrigin.size());
        const size_t suffix = path.find_first_of("?#");
        if (suffix != std::string_view::npos) path = path.substr(0, suffix);
        if (path.empty()) path = "index.html";

        for (const auto& resource : kResources) {
            if (path != resource.path) continue;
            result.statusCode = 200;
            result.statusText = "OK";
            result.mimeType = resource.mimeType;
            result.bytes = developmentDirectory_
                ? ReadFile(*developmentDirectory_ / resource.developmentPath)
                : ReadEmbedded(instance_, resource.id);
            if (result.bytes.empty()) {
                result.statusCode = 500;
                result.statusText = "UI Resource Missing";
                result.mimeType = "text/plain";
                static constexpr char error[] = "Embedded UI resource is missing.";
                result.bytes.assign(error, error + sizeof(error) - 1);
            }
            return result;
        }

        static constexpr char notFound[] = "Not Found";
        result.bytes.assign(notFound, notFound + sizeof(notFound) - 1);
        return result;
    }

    void SetDocumentDirectory(const std::wstring& directory) override {
        std::lock_guard<std::mutex> lock(documentMutex_);
        std::error_code error;
        documentDirectory_ = directory.empty()
            ? std::filesystem::path{}
            : std::filesystem::weakly_canonical(directory, error);
        if (error) documentDirectory_.clear();
    }

private:
    UiResource LoadDocumentAsset(const std::string& url) const {
        UiResource result;
        const auto decoded = PercentDecode(
            std::string_view(url).substr(kAssetPrefix.size()));
        if (!decoded || !json::IsValidUtf8(*decoded)) return result;

        std::filesystem::path root;
        {
            std::lock_guard<std::mutex> lock(documentMutex_);
            root = documentDirectory_;
        }
        if (root.empty()) return result;

        const std::filesystem::path relative(json::Utf8ToWide(*decoded, false));
        if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
            return result;
        }
        std::error_code error;
        const std::filesystem::path candidate =
            std::filesystem::weakly_canonical(root / relative, error);
        if (error || !IsInside(root, candidate)) return result;
        const char* mimeType = ImageMimeType(candidate);
        if (!mimeType) return result;

        result.bytes = ReadFile(candidate);
        if (result.bytes.empty()) return result;
        result.statusCode = 200;
        result.statusText = "OK";
        result.mimeType = mimeType;
        result.charset.clear();
        return result;
    }

    HINSTANCE instance_ = nullptr;
    std::optional<std::filesystem::path> developmentDirectory_;
    mutable std::mutex documentMutex_;
    std::filesystem::path documentDirectory_;
};

}  // namespace

std::shared_ptr<UiResourceProvider> CreatePlatformUiResourceProvider(
    void* nativeApplicationHandle) {
    return std::make_shared<WindowsUiResourceProvider>(
        static_cast<HINSTANCE>(nativeApplicationHandle));
}
