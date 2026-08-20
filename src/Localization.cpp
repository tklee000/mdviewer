#include "Localization.h"

#include "Json.h"
#include "resource.h"

namespace {

std::string LoadCatalog(HINSTANCE instance, int resourceId) {
    const HRSRC resource =
        FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return {};
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* bytes =
        static_cast<const char*>(loaded ? LockResource(loaded) : nullptr);
    return bytes && size ? std::string(bytes, bytes + size) : std::string{};
}

const std::string& Catalog(HINSTANCE instance, const std::wstring& locale) {
    static const std::string empty;
    if (locale == L"ko-KR") { static const auto v = LoadCatalog(instance, IDR_LOCALE_KO_KR); return v; }
    if (locale == L"ja-JP") { static const auto v = LoadCatalog(instance, IDR_LOCALE_JA_JP); return v; }
    if (locale == L"fr-FR") { static const auto v = LoadCatalog(instance, IDR_LOCALE_FR_FR); return v; }
    if (locale == L"de-DE") { static const auto v = LoadCatalog(instance, IDR_LOCALE_DE_DE); return v; }
    if (locale == L"zh-CN") { static const auto v = LoadCatalog(instance, IDR_LOCALE_ZH_CN); return v; }
    if (locale == L"zh-TW") { static const auto v = LoadCatalog(instance, IDR_LOCALE_ZH_TW); return v; }
    if (locale == L"es-ES") { static const auto v = LoadCatalog(instance, IDR_LOCALE_ES_ES); return v; }
    if (locale == L"pt-BR") { static const auto v = LoadCatalog(instance, IDR_LOCALE_PT_BR); return v; }
    if (locale == L"hi-IN") { static const auto v = LoadCatalog(instance, IDR_LOCALE_HI_IN); return v; }
    if (locale == L"id-ID") { static const auto v = LoadCatalog(instance, IDR_LOCALE_ID_ID); return v; }
    if (locale == L"ru-RU") { static const auto v = LoadCatalog(instance, IDR_LOCALE_RU_RU); return v; }
    return empty;
}

std::string ReplaceParameters(std::string text,
                              localization::Parameters parameters) {
    for (const auto& parameter : parameters) {
        const std::string marker = "{" + parameter.first + "}";
        size_t position = 0;
        while ((position = text.find(marker, position)) != std::string::npos) {
            text.replace(position, marker.size(), parameter.second);
            position += parameter.second.size();
        }
    }
    return text;
}

}  // namespace

namespace localization {

std::string Text(HINSTANCE instance, const std::wstring& locale,
                 const std::string& english, Parameters parameters) {
    const std::string& catalog = Catalog(instance, locale);
    const auto translated = catalog.empty()
        ? std::optional<std::string>{}
        : json::GetString(catalog, english);
    return ReplaceParameters(translated.value_or(english), parameters);
}

std::wstring Text(HINSTANCE instance, const std::wstring& locale,
                  const std::wstring& english) {
    return json::Utf8ToWide(
        Text(instance, locale, json::WideToUtf8(english)));
}

}  // namespace localization
