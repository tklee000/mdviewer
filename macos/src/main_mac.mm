#import <Cocoa/Cocoa.h>

#include "PortableCef.h"
#include "PortableCore.h"

#include "include/cef_application_mac.h"
#include "include/wrapper/cef_library_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

NSString* ToNSString(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

std::filesystem::path ToPath(NSString* value) {
    return value ? std::filesystem::path([value fileSystemRepresentation]) : std::filesystem::path();
}

std::string PreferredLanguage() {
    NSString* locale = [NSLocale preferredLanguages].firstObject ?: @"en-US";
    NSString* normalized = [locale stringByReplacingOccurrencesOfString:@"_" withString:@"-"];
    const std::string value([normalized UTF8String]);
    const std::string lower = [&value] {
        std::string result = value;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }();
    if (lower.rfind("ko", 0) == 0) return "ko-KR";
    if (lower.rfind("ja", 0) == 0) return "ja-JP";
    if (lower.rfind("fr", 0) == 0) return "fr-FR";
    if (lower.rfind("de", 0) == 0) return "de-DE";
    if (lower.rfind("ru", 0) == 0) return "ru-RU";
    if (lower.rfind("es", 0) == 0) return "es-ES";
    if (lower.rfind("pt", 0) == 0) return "pt-BR";
    if (lower.rfind("hi", 0) == 0) return "hi-IN";
    if (lower.rfind("id", 0) == 0) return "id-ID";
    if (lower.rfind("zh-hant", 0) == 0 || lower.rfind("zh-tw", 0) == 0 ||
        lower.rfind("zh-hk", 0) == 0) return "zh-TW";
    if (lower.rfind("zh", 0) == 0) return "zh-CN";
    return "en-US";
}

std::string PreferredTheme() {
    if (@available(macOS 10.14, *)) {
        NSString* match = [[NSApp effectiveAppearance]
            bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        if ([match isEqualToString:NSAppearanceNameDarkAqua]) return "dark";
    }
    return "light";
}

std::filesystem::path WebRoot() {
    if (const char* overrideRoot = std::getenv("MDVIEWER_WEB_ROOT")) {
        return std::filesystem::path(overrideRoot);
    }
    NSString* resources = [[NSBundle mainBundle] resourcePath];
    const auto installed = ToPath(resources) / "web";
    if (std::filesystem::exists(installed / "index.html")) return installed;
#ifdef MDVIEWER_SOURCE_WEB_DIR
    return std::filesystem::path(MDVIEWER_SOURCE_WEB_DIR);
#else
    return installed;
#endif
}

class MacShell;
MacShell* gShell = nullptr;

@interface MdViewerApplication : NSApplication <CefAppProtocol> {
 @private
    BOOL handlingSendEvent_;
}
@end

@implementation MdViewerApplication
- (BOOL)isHandlingSendEvent { return handlingSendEvent_; }
- (void)setHandlingSendEvent:(BOOL)value { handlingSendEvent_ = value; }
- (void)sendEvent:(NSEvent*)event {
    [self setHandlingSendEvent:YES];
    [super sendEvent:event];
    [self setHandlingSendEvent:NO];
}
@end

@interface MdViewerWindowDelegate : NSObject <NSWindowDelegate>
@end

@interface MdViewerApplicationDelegate : NSObject <NSApplicationDelegate>
@end

class MacShell final : public mdviewer::PortableCefDelegate,
                       public mdviewer::PortablePlatform {
public:
    MacShell()
        : resources_(std::make_shared<mdviewer::FileResourceProvider>(WebRoot())),
          controller_(*this, resources_, PreferredLanguage(), PreferredTheme()) {
        controller_.SetSender([this](const std::string& json) {
            mdviewer::SendPortableJson(browser_, json);
        });
    }

    std::shared_ptr<mdviewer::FileResourceProvider> Resources() const { return resources_; }

    void OpenAtStartup(const std::optional<std::filesystem::path>& path) {
        if (path) controller_.OpenInitialFile(*path);
    }

    void OpenFromFinder(const std::filesystem::path& path) {
        if (path.empty()) return;
        if (!window_ || !browser_) controller_.OpenInitialFile(path);
        else controller_.OpenRequestedFile(path);
    }

    bool HandleWindowShouldClose() {
        if (closing_) return true;
        if (!controller_.ConfirmClose()) return false;
        closing_ = true;
        if (browser_) browser_->GetHost()->CloseBrowser(true);
        else CefQuitMessageLoop();
        return false;
    }

    void ResizeBrowser() {
        if (!contentView_ || !browser_) return;
        NSView* browserView = (__bridge NSView*)browser_->GetHost()->GetWindowHandle();
        if (browserView) [browserView setFrame:[contentView_ bounds]];
        browser_->GetHost()->WasResized();
    }

    void OnCefContextInitialized() override {
        const NSRect initialFrame = NSMakeRect(0, 0, 1180, 760);
        const NSWindowStyleMask style = NSWindowStyleMaskTitled |
            NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
            NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
        window_ = [[NSWindow alloc] initWithContentRect:initialFrame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [window_ center];
        [window_ setMinSize:NSMakeSize(720, 460)];
        [window_ setTitlebarAppearsTransparent:YES];
        [window_ setTitleVisibility:NSWindowTitleHidden];
        [window_ setMovableByWindowBackground:NO];
        [[window_ standardWindowButton:NSWindowCloseButton] setHidden:YES];
        [[window_ standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
        [[window_ standardWindowButton:NSWindowZoomButton] setHidden:YES];
        windowDelegate_ = [[MdViewerWindowDelegate alloc] init];
        [window_ setDelegate:windowDelegate_];

        contentView_ = [window_ contentView];
        [contentView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [window_ setTitle:ToNSString(pendingTitle_)];
        [window_ makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        const NSRect bounds = [contentView_ bounds];
        CefWindowInfo windowInfo;
        windowInfo.SetAsChild((__bridge CefWindowHandle)contentView_,
                              CefRect(0, 0, static_cast<int>(bounds.size.width),
                                      static_cast<int>(bounds.size.height)));
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

        CefBrowserSettings browserSettings;
        browserSettings.background_color = CefColorSetARGB(255, 10, 13, 18);
        client_ = mdviewer::CreatePortableCefClient(this);
        if (!CefBrowserHost::CreateBrowser(windowInfo, client_, "https://app.mdviewer/",
                                           browserSettings, nullptr, nullptr)) {
            ShowError("CEF startup failed", "The embedded browser window could not be created.");
            CefQuitMessageLoop();
        }
    }

    void OnBrowserCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        ResizeBrowser();
    }

    void OnBrowserClosed() override {
        browser_ = nullptr;
        if (window_) {
            [window_ setDelegate:nil];
            [window_ orderOut:nil];
            [window_ close];
            window_ = nil;
        }
        CefQuitMessageLoop();
    }

    void OnBrowserMessage(const std::string& json) override {
        controller_.OnWebMessage(json);
    }

    void OnBrowserLoadError(const std::string& error) override {
        ShowError("UI load failed", error);
    }

    std::optional<std::filesystem::path> ChooseOpenFile() override {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setAllowedFileTypes:@[@"md", @"markdown"]];
        if ([panel runModal] != NSModalResponseOK) return std::nullopt;
        return ToPath(panel.URL.path);
    }

    std::optional<std::filesystem::path> ChooseSaveFile(
        const std::filesystem::path& currentPath) override {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setAllowedFileTypes:@[@"md", @"markdown"]];
        const std::string name = currentPath.empty()
            ? "Untitled.md"
            : mdviewer::PathToUtf8(currentPath.filename());
        [panel setNameFieldStringValue:ToNSString(name)];
        if (!currentPath.empty()) {
            [panel setDirectoryURL:[NSURL fileURLWithPath:ToNSString(
                mdviewer::PathToUtf8(currentPath.parent_path()))]];
        }
        if ([panel runModal] != NSModalResponseOK) return std::nullopt;
        return ToPath(panel.URL.path);
    }

    mdviewer::SavePromptResult ConfirmSaveChanges(const std::string& displayName) override {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithFormat:@"Save changes to %@?",
                                                       ToNSString(displayName)]];
        [alert setInformativeText:@"Your changes will be lost if you do not save them."];
        [alert addButtonWithTitle:@"Save"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert addButtonWithTitle:@"Discard"];
        const NSModalResponse response = [alert runModal];
        if (response == NSAlertFirstButtonReturn) return mdviewer::SavePromptResult::Save;
        if (response == NSAlertThirdButtonReturn) return mdviewer::SavePromptResult::Discard;
        return mdviewer::SavePromptResult::Cancel;
    }

    void ShowError(const std::string& title, const std::string& message) override {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert setMessageText:ToNSString(title)];
        [alert setInformativeText:ToNSString(message)];
        [alert runModal];
    }

    void ShowAbout() override {
        NSDictionary* options = @{
            NSAboutPanelOptionApplicationName: @"MdViewer",
            NSAboutPanelOptionApplicationVersion: @"0.3.0",
            NSAboutPanelOptionCredits: [[NSAttributedString alloc]
                initWithString:@"CEF-based Markdown viewer and editor"]
        };
        [NSApp orderFrontStandardAboutPanelWithOptions:options];
    }

    void SetWindowTitle(const std::string& title) override {
        pendingTitle_ = title;
        if (window_) [window_ setTitle:ToNSString(title)];
    }

    void RequestClose() override {
        if (window_) [window_ performClose:nil];
    }

    void MinimizeWindow() override {
        if (window_) [window_ miniaturize:nil];
    }

    void ToggleMaximizeWindow() override {
        if (window_) [window_ zoom:nil];
    }

    bool IsWindowMaximized() const override {
        return window_ && [window_ isZoomed];
    }

    void BeginWindowDrag() override {
        NSEvent* event = [NSApp currentEvent];
        if (window_ && event) [window_ performWindowDragWithEvent:event];
    }

    void OpenExternal(const std::string& url) override {
        NSURL* nativeUrl = [NSURL URLWithString:ToNSString(url)];
        if (!nativeUrl || ![[NSWorkspace sharedWorkspace] openURL:nativeUrl]) {
            ShowError("Open link failed", "The link could not be opened.");
        }
    }

private:
    std::shared_ptr<mdviewer::FileResourceProvider> resources_;
    mdviewer::EditorController controller_;
    NSWindow* window_ = nil;
    NSView* contentView_ = nil;
    MdViewerWindowDelegate* windowDelegate_ = nil;
    CefRefPtr<CefClient> client_;
    CefRefPtr<CefBrowser> browser_;
    std::string pendingTitle_ = "MdViewer";
    bool closing_ = false;
};

@implementation MdViewerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    return gShell ? gShell->HandleWindowShouldClose() : YES;
}
- (void)windowDidResize:(NSNotification*)notification {
    if (gShell) gShell->ResizeBrowser();
}
@end

@implementation MdViewerApplicationDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return NO; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (!gShell) return NSTerminateNow;
    gShell->HandleWindowShouldClose();
    return NSTerminateCancel;
}
- (void)application:(NSApplication*)sender openFiles:(NSArray<NSString*>*)filenames {
    if (gShell) {
        for (NSString* filename in filenames) {
            gShell->OpenFromFinder(ToPath(filename));
            break;
        }
    }
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}
@end

std::optional<std::filesystem::path> InitialFile(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.empty() && argument[0] != '-') return std::filesystem::path(argument);
    }
    return std::nullopt;
}

std::filesystem::path BundleFrameworks() {
    return ToPath([[NSBundle mainBundle] privateFrameworksPath]);
}

}  // namespace

int main(int argc, char* argv[]) {
    @autoreleasepool {
        CefScopedLibraryLoader libraryLoader;
        if (!libraryLoader.LoadInMain()) return 1;

        [MdViewerApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        MdViewerApplicationDelegate* appDelegate = [[MdViewerApplicationDelegate alloc] init];
        [NSApp setDelegate:appDelegate];

        CefMainArgs mainArguments(argc, argv);
        MacShell shell;
        gShell = &shell;
        shell.OpenAtStartup(InitialFile(argc, argv));

        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = false;
        settings.log_severity = LOGSEVERITY_WARNING;

        const auto support = ToPath([[[NSFileManager defaultManager]
            URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject path]) /
            "io.mdviewer.MdViewer" / "CEF";
        std::filesystem::create_directories(support);
        CefString(&settings.cache_path) = mdviewer::PathToUtf8(support);
        CefString(&settings.root_cache_path) = mdviewer::PathToUtf8(support);
        const auto helper = BundleFrameworks() / "MdViewer Helper.app" / "Contents" /
                            "MacOS" / "MdViewer Helper";
        CefString(&settings.browser_subprocess_path) = mdviewer::PathToUtf8(helper);
        const std::string language = PreferredLanguage();
        CefString(&settings.locale) = language;
        CefString(&settings.accept_language_list) = language + ",en-US,en";

        const auto app = mdviewer::CreatePortableCefApp(&shell);
        if (!CefInitialize(mainArguments, settings, app, nullptr)) {
            gShell = nullptr;
            return 2;
        }
        if (!mdviewer::RegisterPortableResourceScheme(shell.Resources())) {
            CefShutdown();
            gShell = nullptr;
            return 3;
        }

        CefRunMessageLoop();
        CefClearSchemeHandlerFactories();
        CefShutdown();
        gShell = nullptr;
        (void)appDelegate;
    }
    return 0;
}
