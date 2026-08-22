#include "GoogleDriveClient.h"
#include "Json.h"
#include "MdzArchive.h"
#include "RecentDocuments.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    bool okay = true;
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path() /
        (L"MdViewer-core-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto normalizedParent = temporaryRoot.lexically_normal().parent_path();
    std::error_code pathError;
    if (!std::filesystem::equivalent(
            normalizedParent, std::filesystem::temp_directory_path(), pathError) ||
        pathError) {
        std::cerr << "Unsafe temporary test directory\n";
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(temporaryRoot, error);
    if (error) {
        std::cerr << "Could not create temporary test directory\n";
        return 1;
    }

    {
        RecentDocuments recent(temporaryRoot);
        recent.Load();
        for (int index = 0; index < 12; ++index) {
            const std::wstring path =
                (temporaryRoot / (L"local-" + std::to_wstring(index) + L".md")).wstring();
            okay &= Expect(recent.AddLocal(path), "local recent item is persisted");
        }
        okay &= Expect(recent.Items().size() == 10, "recent list is limited to ten");
        okay &= Expect(recent.Items().front().name == L"local-11.md",
                       "newest recent item is first");
        okay &= Expect(recent.AddGoogleDrive(L"drive-file-id", L"클라우드.md"),
                       "Drive recent item is persisted");
        okay &= Expect(recent.Items().front().kind == RecentDocumentKind::GoogleDrive,
                       "Drive recent item preserves provider");
        okay &= Expect(recent.Items().front().name == L"클라우드.md",
                       "Drive recent item preserves Unicode name");
        const std::string serialized = recent.ToJson();
        okay &= Expect(serialized.find("refresh_token") == std::string::npos,
                       "recent file never contains credentials");
    }

    {
        RecentDocuments reloaded(temporaryRoot);
        reloaded.Load();
        okay &= Expect(reloaded.Items().size() == 10,
                       "recent list reloads with its limit intact");
        okay &= Expect(reloaded.Items().front().location == L"drive-file-id",
                       "Drive identity survives persistence");
        okay &= Expect(reloaded.Remove(RecentDocumentKind::GoogleDrive,
                                       L"drive-file-id"),
                       "recent item removal persists");
        okay &= Expect(reloaded.Items().size() == 9,
                       "recent item removal updates memory");
    }

    {
        RecentDocuments firstWindow(temporaryRoot);
        RecentDocuments secondWindow(temporaryRoot);
        firstWindow.Load();
        secondWindow.Load();
        okay &= Expect(firstWindow.AddGoogleDrive(L"first-window", L"first.md"),
                       "first window updates shared recent storage");
        okay &= Expect(secondWindow.AddGoogleDrive(L"second-window", L"second.md"),
                       "second window merges shared recent storage");
        RecentDocuments merged(temporaryRoot);
        merged.Load();
        const std::string json = merged.ToJson();
        okay &= Expect(json.find("first-window") != std::string::npos &&
                       json.find("second-window") != std::string::npos,
                       "multiple windows do not overwrite each other's recent items");
    }

    okay &= Expect(json::GetInteger("{\"expires_in\":3599}", "expires_in") == 3599,
                   "integer JSON values are parsed");
    okay &= Expect(!json::GetInteger("{\"expires_in\":\"3599\"}", "expires_in"),
                   "quoted JSON values are not accepted as integers");

    {
        const std::string pickerUrl = google_drive::BuildAuthorizationUrl(
            "desktop-client", "http://127.0.0.1:12345", "challenge", "state",
            google_drive::PickerMode::MarkdownFile);
        okay &= Expect(pickerUrl.find("trigger_onepick=true") != std::string::npos,
                       "desktop authorization triggers the system-browser Picker");
        okay &= Expect(pickerUrl.find("prompt=consent") != std::string::npos,
                       "desktop Picker explicitly requests consent");
        okay &= Expect(pickerUrl.find("allow_multiple=false") != std::string::npos,
                       "desktop Picker selects one document");
        okay &= Expect(pickerUrl.find("drive.file") != std::string::npos,
                       "desktop Picker requests only drive.file");
        okay &= Expect(pickerUrl.find("include_granted_scopes") == std::string::npos,
                       "desktop Picker cannot combine previously granted scopes");

        const std::string folderUrl = google_drive::BuildAuthorizationUrl(
            "desktop-client", "http://127.0.0.1:12345", "challenge", "state",
            google_drive::PickerMode::Folder);
        okay &= Expect(folderUrl.find("allow_folder_selection=true") !=
                           std::string::npos,
                       "Drive save-as Picker enables folder selection");
        okay &= Expect(folderUrl.find("application%2Fvnd.google-apps.folder") !=
                           std::string::npos,
                       "Drive save-as Picker filters for folders");
        okay &= Expect(folderUrl.find("text%2Fmarkdown") == std::string::npos,
                       "folder Picker does not include Markdown files");
        okay &= Expect(pickerUrl.find("application%2Fvnd.mdzip") !=
                           std::string::npos,
                       "Drive file Picker includes MDZip packages");

        const std::string rootMetadata =
            google_drive::BuildCreateMetadata("새 문서.md", "");
        okay &= Expect(rootMetadata.find("새 문서.md") != std::string::npos,
                       "Drive create metadata preserves Unicode file names");
        okay &= Expect(rootMetadata.find("parents") == std::string::npos,
                       "Drive root upload omits the parent folder");
        const std::string folderMetadata =
            google_drive::BuildCreateMetadata("notes.md", "folder-id");
        okay &= Expect(folderMetadata.find("\"parents\":[\"folder-id\"]") !=
                           std::string::npos,
                       "Drive folder upload includes the selected parent");
        okay &= Expect(folderMetadata.find("\"mimeType\":\"text/markdown\"") !=
                           std::string::npos,
                       "Drive create metadata identifies Markdown content");
        const std::string mdzMetadata = google_drive::BuildCreateMetadata(
            "bundle.mdz", "folder-id", mdz::kMimeType);
        okay &= Expect(mdzMetadata.find("application/vnd.mdzip") !=
                           std::string::npos,
                       "Drive create metadata identifies MDZip content");
    }

    {
        const std::string markdown =
            "# MDZip\n\n" + std::string(16000, 'A') + "\n";
        mdz::Package package = mdz::CreateDocument(markdown, "MDZip test");
        package.entries["images/pixel.png"] = mdz::Bytes{
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::string archive;
        std::wstring archiveError;
        okay &= Expect(mdz::BuildBytes(package, &archive, &archiveError),
                       "MDZ archive is built");
        okay &= Expect(archive.size() > 10 &&
                           static_cast<unsigned char>(archive[8]) == 8 &&
                           static_cast<unsigned char>(archive[9]) == 0,
                       "MDZ entries use ZIP DEFLATE rather than STORE");
        okay &= Expect(archive.size() < markdown.size(),
                       "repetitive MDZ content is actually compressed");

        mdz::Package reopened;
        okay &= Expect(mdz::ReadBytes(archive, &reopened, &archiveError),
                       "compressed MDZ archive reopens");
        okay &= Expect(reopened.entryPoint == "index.md",
                       "MDZ entry point round-trips");
        okay &= Expect(std::string(
                           reopened.entries.at("index.md").begin(),
                           reopened.entries.at("index.md").end()) == markdown,
                       "MDZ Markdown content round-trips");
        okay &= Expect(reopened.entries.at("images/pixel.png") ==
                           package.entries.at("images/pixel.png"),
                       "MDZ image bytes round-trip");

        mdz::Package unsafe = package;
        unsafe.entries["../outside.png"] = {1, 2, 3};
        std::string rejected;
        okay &= Expect(!mdz::BuildBytes(unsafe, &rejected, &archiveError),
                       "MDZ writer rejects traversal paths");
        std::string corrupt = archive;
        if (corrupt.size() > 40) corrupt[40] ^= 0x20;
        okay &= Expect(!mdz::ReadBytes(corrupt, &reopened, &archiveError),
                       "MDZ reader rejects corrupt compressed data");
    }

    {
        GoogleDriveClient drive(temporaryRoot);
        const auto result = drive.PickFile({});
        okay &= Expect(!result && result.error.find(L"google-drive.ini") != std::wstring::npos,
                       "missing Picker configuration fails before network access");
        okay &= Expect(drive.ConfigurationPath() == temporaryRoot / L"google-drive.ini",
                       "Drive configuration remains outside the repository");
    }

    std::filesystem::remove_all(temporaryRoot, error);
    if (error) {
        std::cerr << "Could not remove temporary test directory\n";
        okay = false;
    }
    if (okay) std::cout << "MdViewer core smoke tests passed.\n";
    return okay ? 0 : 1;
}
