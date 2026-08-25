#include "PortableCore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

class FakePlatform final : public mdviewer::PortablePlatform {
public:
    std::optional<std::filesystem::path> ChooseOpenFile() override {
        return std::nullopt;
    }

    std::optional<mdviewer::SaveSelection> ChooseSaveFile(
        const std::filesystem::path&, mdviewer::TextEncoding,
        mdviewer::DocumentFormat) override {
        const auto result = nextSave;
        nextSave.reset();
        return result;
    }

    mdviewer::SavePromptResult ConfirmSaveChanges(const std::string&) override {
        return mdviewer::SavePromptResult::Discard;
    }

    void ShowError(const std::string&, const std::string& message) override {
        errors.push_back(message);
    }
    void ShowAbout() override {}
    void SetWindowTitle(const std::string&) override {}
    void RequestClose() override {}
    void MinimizeWindow() override {}
    void ToggleMaximizeWindow() override {}
    bool IsWindowMaximized() const override { return false; }
    void BeginWindowDrag() override {}
    void OpenExternal(const std::string&) override {}

    std::optional<mdviewer::SaveSelection> nextSave;
    std::vector<std::string> errors;
};

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool Require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

}  // namespace

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("mdviewer-portable-core-" + std::to_string(suffix));
    std::error_code filesystemError;
    std::filesystem::create_directories(temporary, filesystemError);
    if (!Require(!filesystemError, "Could not create the test directory")) return 1;

    FakePlatform platform;
    auto resources = std::make_shared<mdviewer::FileResourceProvider>(temporary);
    mdviewer::EditorController controller(platform, resources, "ko-KR", "light");
    std::vector<std::string> messages;
    controller.SetSender([&](const std::string& message) {
        messages.push_back(message);
    });
    controller.OnWebMessage("{\"type\":\"ready\"}");

    const std::string markdown = "# 휴대용 테스트 😀\n\n본문입니다.\n";
    controller.OnWebMessage(
        "{\"type\":\"document.changed\",\"text\":" +
        mdviewer::JsonQuote(markdown) + ",\"dirty\":true,\"eol\":\"LF\"}");

    const auto utf16Path = temporary / "unicode.md";
    platform.nextSave = mdviewer::SaveSelection{
        utf16Path, mdviewer::TextEncoding::Utf16Le,
        mdviewer::DocumentFormat::Markdown};
    controller.OnWebMessage(
        "{\"type\":\"command\",\"name\":\"file.saveAs\"}");
    const std::string utf16Bytes = ReadBytes(utf16Path);
    if (!Require(utf16Bytes.size() > 4 &&
                 static_cast<unsigned char>(utf16Bytes[0]) == 0xFF &&
                 static_cast<unsigned char>(utf16Bytes[1]) == 0xFE,
                 "UTF-16 LE save did not write its BOM")) return 1;

    const auto mdzPath = temporary / "document.mdz";
    platform.nextSave = mdviewer::SaveSelection{
        mdzPath, mdviewer::TextEncoding::Utf16Be,
        mdviewer::DocumentFormat::Mdz};
    controller.OnWebMessage(
        "{\"type\":\"command\",\"name\":\"file.saveAs\"}");
    mdz::Package package;
    std::wstring mdzError;
    if (!Require(mdz::ReadBytes(ReadBytes(mdzPath), &package, &mdzError),
                 "Saved MDZ could not be read")) return 1;
    const auto markdownEntry = package.entries.find(package.entryPoint);
    if (!Require(markdownEntry != package.entries.end(),
                 "Saved MDZ has no Markdown entry")) return 1;
    const std::string savedMarkdown(
        reinterpret_cast<const char*>(markdownEntry->second.data()),
        markdownEntry->second.size());
    if (!Require(savedMarkdown == markdown,
                 "Saved MDZ Markdown text changed unexpectedly")) return 1;

    controller.OnWebMessage(
        "{\"type\":\"mdz.passwordChanged\",\"password\":\"portable-secret\"}");
    controller.OnWebMessage("{\"type\":\"command\",\"name\":\"file.save\"}");
    mdz::ReadStatus protectedStatus = mdz::ReadStatus::Error;
    if (!Require(!mdz::ReadBytes(ReadBytes(mdzPath), &package, &mdzError,
                                {}, &protectedStatus) &&
                 protectedStatus == mdz::ReadStatus::PasswordRequired,
                 "Portable controller did not encrypt the MDZ after setting a password")) {
        return 1;
    }
    if (!Require(mdz::ReadBytes(ReadBytes(mdzPath), &package, &mdzError,
                                "portable-secret", &protectedStatus),
                 "Portable controller password did not reopen the MDZ")) return 1;
    controller.OnWebMessage(
        "{\"type\":\"mdz.passwordChanged\",\"password\":\"\"}");
    controller.OnWebMessage("{\"type\":\"command\",\"name\":\"file.save\"}");
    if (!Require(mdz::ReadBytes(ReadBytes(mdzPath), &package, &mdzError,
                                {}, &protectedStatus),
                 "Blank portable MDZ password did not remove encryption")) return 1;

    package.entries["images/pixel.png"] = mdz::Bytes{0x89, 'P', 'N', 'G'};
    package.entries[package.entryPoint] = mdz::Bytes{
        '!', '[', 'x', ']', '(', 'i', 'm', 'a', 'g', 'e', 's', '/',
        'p', 'i', 'x', 'e', 'l', '.', 'p', 'n', 'g', ')', '\n'};
    std::string packagedBytes;
    if (!Require(mdz::BuildBytes(package, &packagedBytes, &mdzError) &&
                 WriteBytes(mdzPath, packagedBytes),
                 "Could not prepare the MDZ asset test")) return 1;

    FakePlatform reopenPlatform;
    auto reopenedResources =
        std::make_shared<mdviewer::FileResourceProvider>(temporary);
    mdviewer::EditorController reopened(
        reopenPlatform, reopenedResources, "ko-KR", "light");
    std::vector<std::string> reopenedMessages;
    reopened.SetSender([&](const std::string& message) {
        reopenedMessages.push_back(message);
    });
    if (!Require(reopened.OpenInitialFile(mdzPath), "Could not reopen the MDZ")) return 1;
    reopened.OnWebMessage("{\"type\":\"ready\"}");
    if (!Require(!reopenedMessages.empty() &&
                 reopenedMessages.front().find("\"format\":\"mdz\"") !=
                     std::string::npos &&
                 reopenedMessages.front().find("\"encoding\":\"UTF-8\"") !=
                     std::string::npos &&
                 reopenedMessages.front().find(
                     "\"capabilities\":{\"googleDrive\":false}") !=
                     std::string::npos,
                 "Reopened MDZ state does not report its format/encoding")) return 1;
    const auto asset = reopenedResources->Load(
        "https://app.mdviewer/__asset?path=images%2Fpixel.png");
    if (!Require(asset && asset->mimeType == "image/png" &&
                 asset->bytes == std::string("\x89PNG", 4),
                 "MDZ relative image asset was not served")) return 1;

    const auto protectedPath = temporary / "protected.mdz";
    std::string protectedBytes;
    if (!Require(mdz::BuildBytes(package, &protectedBytes, &mdzError,
                                 "open-secret") &&
                 WriteBytes(protectedPath, protectedBytes),
                 "Could not prepare the protected portable MDZ")) return 1;
    FakePlatform protectedPlatform;
    auto protectedResources =
        std::make_shared<mdviewer::FileResourceProvider>(temporary);
    mdviewer::EditorController protectedController(
        protectedPlatform, protectedResources, "ko-KR", "light");
    std::vector<std::string> protectedMessages;
    protectedController.SetSender([&](const std::string& message) {
        protectedMessages.push_back(message);
    });
    if (!Require(protectedController.OpenInitialFile(protectedPath),
                 "Protected portable MDZ open was not queued")) return 1;
    protectedController.OnWebMessage("{\"type\":\"ready\"}");
    if (!Require(!protectedMessages.empty() &&
                 protectedMessages.back().find("mdz.passwordRequired") !=
                     std::string::npos,
                 "Protected portable MDZ did not request a password")) return 1;
    protectedController.OnWebMessage(
        "{\"type\":\"mdz.passwordResponse\",\"password\":\"wrong\"}");
    if (!Require(protectedMessages.back().find("\"incorrect\":true") !=
                     std::string::npos,
                 "Incorrect portable MDZ password did not request another attempt")) {
        return 1;
    }
    protectedController.OnWebMessage(
        "{\"type\":\"mdz.passwordResponse\",\"password\":\"open-secret\"}");
    if (!Require(protectedMessages.back().find("\"type\":\"document.opened\"") !=
                     std::string::npos &&
                 protectedMessages.back().find("\"format\":\"mdz\"") !=
                     std::string::npos &&
                 protectedMessages.back().find("\"mdzEncrypted\":true") !=
                     std::string::npos,
                 "Correct portable MDZ password did not open the document")) return 1;

    const auto utf16BePath = temporary / "roundtrip.md";
    reopenPlatform.nextSave = mdviewer::SaveSelection{
        utf16BePath, mdviewer::TextEncoding::Utf16Be,
        mdviewer::DocumentFormat::Markdown};
    reopened.OnWebMessage(
        "{\"type\":\"command\",\"name\":\"file.saveAs\"}");
    const std::string utf16BeBytes = ReadBytes(utf16BePath);
    if (!Require(utf16BeBytes.size() > 4 &&
                 static_cast<unsigned char>(utf16BeBytes[0]) == 0xFE &&
                 static_cast<unsigned char>(utf16BeBytes[1]) == 0xFF,
                 "UTF-16 BE save did not write its BOM")) return 1;

    if (!Require(platform.errors.empty() && reopenPlatform.errors.empty() &&
                 protectedPlatform.errors.empty(),
                 "A document operation reported an unexpected error")) return 1;
    std::filesystem::remove_all(temporary, filesystemError);
    std::cout << "portable core MDZ/encoding smoke test passed\n";
    return 0;
}
