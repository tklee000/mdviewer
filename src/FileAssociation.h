#pragma once

#include <windows.h>

#include <string>

enum class FileAssociationState {
    Current,
    RepairedMdViewer,
    NeedsUserConsent,
};

struct FileAssociationResult {
    bool success = false;
    bool needsSystemConfirmation = false;
};

FileAssociationState CheckAndRepairMarkdownAssociation(
    const std::wstring& executablePath);
FileAssociationResult AssociateMarkdownFiles(
    const std::wstring& executablePath);
void OpenDefaultAppsSettings(HWND owner);
