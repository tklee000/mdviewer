#include "PortableCef.h"

#include "include/wrapper/cef_library_loader.h"

int main(int argc, char* argv[]) {
    CefScopedLibraryLoader libraryLoader;
    if (!libraryLoader.LoadInHelper()) return 1;
    CefMainArgs mainArguments(argc, argv);
    return CefExecuteProcess(mainArguments, mdviewer::CreatePortableCefApp(nullptr), nullptr);
}
