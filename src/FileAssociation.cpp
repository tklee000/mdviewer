#include "FileAssociation.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kCanonicalProgId[] = L"MdViewer.Markdown";
constexpr wchar_t kApplicationName[] = L"MdViewer";
constexpr wchar_t kClassesRoot[] = L"Software\\Classes\\";
constexpr wchar_t kUserChoiceRoot[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\";

std::wstring QueryString(HKEY root, const std::wstring& path,
                         const wchar_t* valueName = nullptr) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(root, path.c_str(), valueName,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     &type, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, path.c_str(), valueName,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     &type, buffer.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring value(buffer.data());
    if (type == REG_EXPAND_SZ) {
        const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (required) {
            std::vector<wchar_t> expanded(required, L'\0');
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required)) {
                value.assign(expanded.data());
            }
        }
    }
    return value;
}

bool SetString(HKEY root, const std::wstring& path,
               const wchar_t* valueName, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return {};
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = std::filesystem::absolute(path, error);
    std::wstring value = error ? path : normalized.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(towlower(character));
                   });
    return value;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return NormalizePath(left) == NormalizePath(right);
}

bool IsMdViewerProgId(const std::wstring& progId) {
    if (progId.size() < std::size(kCanonicalProgId) - 1) return false;
    return _wcsnicmp(progId.c_str(), kCanonicalProgId,
                     std::size(kCanonicalProgId) - 1) == 0;
}

bool IsMdViewerExecutable(const std::wstring& path) {
    if (path.empty()) return false;
    return _wcsicmp(std::filesystem::path(path).filename().c_str(),
                    L"MdViewer.exe") == 0;
}

std::wstring EffectiveExecutable(const wchar_t* extension) {
    DWORD length = 0;
    AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE,
                      extension, nullptr, nullptr, &length);
    if (!length) return {};
    std::vector<wchar_t> buffer(length + 1, L'\0');
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE,
                                 extension, nullptr, buffer.data(), &length))) {
        return {};
    }
    return buffer.data();
}

std::wstring EffectiveProgId(const wchar_t* extension) {
    std::wstring userChoice = kUserChoiceRoot;
    userChoice += extension;
    userChoice += L"\\UserChoice";
    std::wstring progId = QueryString(HKEY_CURRENT_USER, userChoice, L"ProgId");
    if (!progId.empty()) return progId;

    std::wstring extensionKey = kClassesRoot;
    extensionKey += extension;
    progId = QueryString(HKEY_CURRENT_USER, extensionKey);
    if (!progId.empty()) return progId;
    return QueryString(HKEY_CLASSES_ROOT, extension);
}

bool HasConflictingUserChoice(const wchar_t* extension,
                              const std::wstring& executablePath) {
    std::wstring key = kUserChoiceRoot;
    key += extension;
    key += L"\\UserChoice";
    const std::wstring progId = QueryString(HKEY_CURRENT_USER, key, L"ProgId");
    if (progId.empty() || IsMdViewerProgId(progId)) return false;
    const std::wstring effective = EffectiveExecutable(extension);
    return !SamePath(effective, executablePath) && !IsMdViewerExecutable(effective);
}

bool WriteProgId(const std::wstring& progId,
                 const std::wstring& executablePath) {
    if (progId.empty()) return false;
    const std::wstring root = std::wstring(kClassesRoot) + progId;
    const std::wstring command = L"\"" + executablePath + L"\" \"%1\"";
    const std::wstring icon = L"\"" + executablePath + L"\",0";
    bool success = SetString(HKEY_CURRENT_USER, root, nullptr, L"Markdown Document");
    success = SetString(HKEY_CURRENT_USER, root + L"\\DefaultIcon", nullptr, icon) && success;
    success = SetString(HKEY_CURRENT_USER, root + L"\\shell\\open\\command",
                        nullptr, command) && success;
    return success;
}

bool WriteApplicationRegistration(const std::wstring& executablePath) {
    const std::wstring command = L"\"" + executablePath + L"\" \"%1\"";
    const std::wstring applicationRoot =
        std::wstring(kClassesRoot) + L"Applications\\MdViewer.exe";
    bool success = SetString(HKEY_CURRENT_USER, applicationRoot,
                             L"FriendlyAppName", kApplicationName);
    success = SetString(HKEY_CURRENT_USER,
                        applicationRoot + L"\\shell\\open\\command",
                        nullptr, command) && success;
    success = SetString(HKEY_CURRENT_USER,
                        applicationRoot + L"\\SupportedTypes", L".md", L"") && success;
    success = SetString(HKEY_CURRENT_USER,
                        applicationRoot + L"\\SupportedTypes", L".markdown", L"") && success;
    success = SetString(HKEY_CURRENT_USER,
                        applicationRoot + L"\\SupportedTypes", L".mdz", L"") && success;

    const std::wstring capabilities = L"Software\\MdViewer\\Capabilities";
    success = SetString(HKEY_CURRENT_USER, capabilities,
                        L"ApplicationName", kApplicationName) && success;
    success = SetString(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations",
                        L".md", kCanonicalProgId) && success;
    success = SetString(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations",
                        L".markdown", kCanonicalProgId) && success;
    success = SetString(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations",
                        L".mdz", kCanonicalProgId) && success;
    success = SetString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
                        kApplicationName, capabilities) && success;
    return success;
}

bool WriteOpenWithRegistration(const wchar_t* extension) {
    const std::wstring extensionRoot = std::wstring(kClassesRoot) + extension;
    return SetString(HKEY_CURRENT_USER, extensionRoot + L"\\OpenWithProgids",
                     kCanonicalProgId, L"");
}

void NotifyAssociationChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

}  // namespace

FileAssociationState CheckAndRepairMarkdownAssociation(
    const std::wstring& executablePath) {
    const std::wstring effectiveExecutable = EffectiveExecutable(L".md");
    const std::wstring effectiveProgId = EffectiveProgId(L".md");
    std::wstring mdzExecutable = EffectiveExecutable(L".mdz");
    std::wstring mdzProgId = EffectiveProgId(L".mdz");

    WriteProgId(kCanonicalProgId, executablePath);
    WriteApplicationRegistration(executablePath);
    WriteOpenWithRegistration(L".md");
    WriteOpenWithRegistration(L".markdown");
    WriteOpenWithRegistration(L".mdz");
    if (mdzProgId.empty()) {
        const std::wstring extensionRoot = std::wstring(kClassesRoot) + L".mdz";
        if (SetString(HKEY_CURRENT_USER, extensionRoot, nullptr,
                      kCanonicalProgId)) {
            mdzProgId = kCanonicalProgId;
            mdzExecutable = executablePath;
            NotifyAssociationChanged();
        }
    }

    if (!effectiveProgId.empty() && !mdzProgId.empty() &&
        SamePath(effectiveExecutable, executablePath) &&
        SamePath(mdzExecutable, executablePath)) {
        return FileAssociationState::Current;
    }
    if (!effectiveProgId.empty() && !mdzProgId.empty() &&
        (IsMdViewerProgId(effectiveProgId) ||
         IsMdViewerExecutable(effectiveExecutable)) &&
        (IsMdViewerProgId(mdzProgId) || IsMdViewerExecutable(mdzExecutable))) {
        if (!effectiveProgId.empty()) WriteProgId(effectiveProgId, executablePath);
        if (!mdzProgId.empty() && mdzProgId != effectiveProgId) {
            WriteProgId(mdzProgId, executablePath);
        }
        NotifyAssociationChanged();
        return FileAssociationState::RepairedMdViewer;
    }
    return FileAssociationState::NeedsUserConsent;
}

FileAssociationResult AssociateMarkdownFiles(
    const std::wstring& executablePath) {
    FileAssociationResult result;
    result.success = WriteProgId(kCanonicalProgId, executablePath);
    result.success = WriteApplicationRegistration(executablePath) && result.success;
    for (const wchar_t* extension : {L".md", L".markdown", L".mdz"}) {
        const std::wstring extensionRoot = std::wstring(kClassesRoot) + extension;
        result.success = SetString(HKEY_CURRENT_USER, extensionRoot,
                                   nullptr, kCanonicalProgId) && result.success;
        result.success = WriteOpenWithRegistration(extension) && result.success;
        result.needsSystemConfirmation =
            HasConflictingUserChoice(extension, executablePath) ||
            result.needsSystemConfirmation;
    }
    NotifyAssociationChanged();
    return result;
}

void OpenDefaultAppsSettings(HWND owner) {
    HINSTANCE result = ShellExecuteW(
        owner, L"open", L"ms-settings:defaultapps?registeredAppUser=MdViewer",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShellExecuteW(owner, L"open", L"ms-settings:defaultapps",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
}
