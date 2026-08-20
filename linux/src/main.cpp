#include "PortableCef.h"
#include "PortableCore.h"

#include "include/cef_browser.h"
#include "include/cef_command_line.h"

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <limits.h>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

std::filesystem::path ExecutablePath() {
    char buffer[PATH_MAX + 1]{};
    const ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (length <= 0) return std::filesystem::current_path() / "mdviewer";
    buffer[length] = '\0';
    return std::filesystem::path(buffer);
}

std::filesystem::path WebRoot(const std::filesystem::path& executable) {
    if (const char* overrideRoot = std::getenv("MDVIEWER_WEB_ROOT")) {
        return std::filesystem::path(overrideRoot);
    }
    const auto installed = executable.parent_path().parent_path() / "share" / "mdviewer" / "web";
    if (std::filesystem::exists(installed / "index.html")) return installed;
#ifdef MDVIEWER_SOURCE_WEB_DIR
    return std::filesystem::path(MDVIEWER_SOURCE_WEB_DIR);
#else
    return installed;
#endif
}

std::filesystem::path IconPath(const std::filesystem::path& executable) {
    return executable.parent_path().parent_path() / "share" / "icons" / "hicolor" /
           "256x256" / "apps" / "mdviewer.png";
}

std::string InitialLanguage() {
    const char* locale = std::setlocale(LC_ALL, "");
    if (!locale) return "en-US";
    std::string value(locale);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value.rfind("ko", 0) == 0) return "ko-KR";
    if (value.rfind("ja", 0) == 0) return "ja-JP";
    if (value.rfind("fr", 0) == 0) return "fr-FR";
    if (value.rfind("de", 0) == 0) return "de-DE";
    if (value.rfind("ru", 0) == 0) return "ru-RU";
    if (value.rfind("es", 0) == 0) return "es-ES";
    if (value.rfind("pt", 0) == 0) return "pt-BR";
    if (value.rfind("hi", 0) == 0) return "hi-IN";
    if (value.rfind("id", 0) == 0) return "id-ID";
    if (value.rfind("zh_tw", 0) == 0 || value.rfind("zh_hk", 0) == 0 ||
        value.rfind("zh-hant", 0) == 0) return "zh-TW";
    if (value.rfind("zh", 0) == 0) return "zh-CN";
    return "en-US";
}

std::string InitialTheme() {
    const char* desktop = std::getenv("GTK_THEME");
    if (desktop) {
        std::string value(desktop);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value.find("dark") != std::string::npos) return "dark";
    }
    return "light";
}

std::optional<std::filesystem::path> InitialFile(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.empty() && argument[0] != '-') return std::filesystem::path(argument);
    }
    return std::nullopt;
}

class LinuxShell final : public mdviewer::PortableCefDelegate,
                         public mdviewer::PortablePlatform {
public:
    explicit LinuxShell(std::filesystem::path executable)
        : executable_(std::move(executable)),
          resources_(std::make_shared<mdviewer::FileResourceProvider>(WebRoot(executable_))),
          controller_(*this, resources_, InitialLanguage(), InitialTheme()) {
        controller_.SetSender([this](const std::string& json) {
            mdviewer::SendPortableJson(browser_, json);
        });
    }

    std::shared_ptr<mdviewer::FileResourceProvider> Resources() const { return resources_; }

    void OpenAtStartup(const std::optional<std::filesystem::path>& path) {
        if (path) controller_.OpenInitialFile(*path);
    }

    void OnCefContextInitialized() override {
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(window_), 1180, 760);
        gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);
        gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
        gtk_widget_set_size_request(window_, 720, 460);
        gtk_widget_add_events(window_, GDK_STRUCTURE_MASK);

        const auto icon = IconPath(executable_);
        if (std::filesystem::exists(icon)) {
            gtk_window_set_icon_from_file(GTK_WINDOW(window_), icon.string().c_str(), nullptr);
        }

        g_signal_connect(window_, "delete-event", G_CALLBACK(OnDeleteEvent), this);
        g_signal_connect(window_, "size-allocate", G_CALLBACK(OnSizeAllocate), this);
        gtk_widget_realize(window_);
        gtk_widget_show_all(window_);

        const Window parent = GDK_WINDOW_XID(gtk_widget_get_window(window_));
        GtkAllocation allocation{};
        gtk_widget_get_allocation(window_, &allocation);

        CefWindowInfo windowInfo;
        windowInfo.SetAsChild(
            static_cast<CefWindowHandle>(parent),
            CefRect(0, 0, (std::max)(1, allocation.width), (std::max)(1, allocation.height)));
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
            gtk_widget_destroy(window_);
            window_ = nullptr;
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
        GtkWidget* dialog = gtk_file_chooser_dialog_new(
            "Open Markdown", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Markdown (*.md, *.markdown)");
        gtk_file_filter_add_pattern(filter, "*.md");
        gtk_file_filter_add_pattern(filter, "*.markdown");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

        std::optional<std::filesystem::path> result;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (name) {
                result = std::filesystem::path(name);
                g_free(name);
            }
        }
        gtk_widget_destroy(dialog);
        return result;
    }

    std::optional<std::filesystem::path> ChooseSaveFile(
        const std::filesystem::path& currentPath) override {
        GtkWidget* dialog = gtk_file_chooser_dialog_new(
            "Save Markdown", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        const std::string suggested = currentPath.empty()
            ? "Untitled.md"
            : mdviewer::PathToUtf8(currentPath.filename());
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested.c_str());
        if (!currentPath.empty()) {
            gtk_file_chooser_set_current_folder(
                GTK_FILE_CHOOSER(dialog), currentPath.parent_path().string().c_str());
        }

        std::optional<std::filesystem::path> result;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (name) {
                result = std::filesystem::path(name);
                g_free(name);
            }
        }
        gtk_widget_destroy(dialog);
        return result;
    }

    mdviewer::SavePromptResult ConfirmSaveChanges(const std::string& displayName) override {
        const std::string message = "Save changes to " + displayName + "?";
        GtkWidget* dialog = gtk_message_dialog_new(
            GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "%s", message.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL,
                               "_Discard", GTK_RESPONSE_REJECT,
                               "_Save", GTK_RESPONSE_ACCEPT, nullptr);
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response == GTK_RESPONSE_ACCEPT) return mdviewer::SavePromptResult::Save;
        if (response == GTK_RESPONSE_REJECT) return mdviewer::SavePromptResult::Discard;
        return mdviewer::SavePromptResult::Cancel;
    }

    void ShowError(const std::string& title, const std::string& message) override {
        GtkWidget* dialog = gtk_message_dialog_new(
            window_ ? GTK_WINDOW(window_) : nullptr, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
        gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    void ShowAbout() override {
        gtk_show_about_dialog(
            window_ ? GTK_WINDOW(window_) : nullptr,
            "program-name", "MdViewer", "version", "0.2.0",
            "comments", "CEF-based Markdown viewer and editor",
            "copyright", "Copyright © MdViewer contributors", nullptr);
    }

    void SetWindowTitle(const std::string& title) override {
        pendingTitle_ = title;
        if (window_) gtk_window_set_title(GTK_WINDOW(window_), pendingTitle_.c_str());
    }

    void RequestClose() override {
        if (window_) gtk_window_close(GTK_WINDOW(window_));
    }

    void MinimizeWindow() override {
        if (window_) gtk_window_iconify(GTK_WINDOW(window_));
    }

    void ToggleMaximizeWindow() override {
        if (!window_) return;
        GdkWindow* gdkWindow = gtk_widget_get_window(window_);
        const GdkWindowState state = gdk_window_get_state(gdkWindow);
        if ((state & GDK_WINDOW_STATE_MAXIMIZED) != 0) gtk_window_unmaximize(GTK_WINDOW(window_));
        else gtk_window_maximize(GTK_WINDOW(window_));
    }

    bool IsWindowMaximized() const override {
        if (!window_) return false;
        GdkWindow* gdkWindow = gtk_widget_get_window(window_);
        return gdkWindow &&
            (gdk_window_get_state(gdkWindow) & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    }

    void BeginWindowDrag() override {
        if (!window_) return;
        GdkDisplay* display = gtk_widget_get_display(window_);
        GdkSeat* seat = gdk_display_get_default_seat(display);
        GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
        gint x = 0;
        gint y = 0;
        if (pointer) gdk_device_get_position(pointer, nullptr, &x, &y);
        gtk_window_begin_move_drag(GTK_WINDOW(window_), 1, x, y, GDK_CURRENT_TIME);
    }

    void OpenExternal(const std::string& url) override {
        GError* error = nullptr;
        if (!gtk_show_uri_on_window(GTK_WINDOW(window_), url.c_str(), GDK_CURRENT_TIME, &error)) {
            const std::string message = error ? error->message : "The link could not be opened.";
            if (error) g_error_free(error);
            ShowError("Open link failed", message);
        }
    }

private:
    static gboolean OnDeleteEvent(GtkWidget* widget, GdkEvent* event, gpointer data) {
        auto* self = static_cast<LinuxShell*>(data);
        if (self->closing_) return TRUE;
        if (!self->controller_.ConfirmClose()) return TRUE;
        self->closing_ = true;
        if (self->browser_) self->browser_->GetHost()->CloseBrowser(true);
        else CefQuitMessageLoop();
        return TRUE;
    }

    static void OnSizeAllocate(GtkWidget* widget, GtkAllocation* allocation, gpointer data) {
        static_cast<LinuxShell*>(data)->ResizeBrowser();
    }

    void ResizeBrowser() {
        if (!window_ || !browser_) return;
        GtkAllocation allocation{};
        gtk_widget_get_allocation(window_, &allocation);
        const CefWindowHandle handle = browser_->GetHost()->GetWindowHandle();
        if (handle) {
            XResizeWindow(GDK_DISPLAY_XDISPLAY(gtk_widget_get_display(window_)),
                          static_cast<Window>(handle),
                          static_cast<unsigned int>((std::max)(1, allocation.width)),
                          static_cast<unsigned int>((std::max)(1, allocation.height)));
        }
    }

    std::filesystem::path executable_;
    std::shared_ptr<mdviewer::FileResourceProvider> resources_;
    mdviewer::EditorController controller_;
    GtkWidget* window_ = nullptr;
    CefRefPtr<CefClient> client_;
    CefRefPtr<CefBrowser> browser_;
    std::string pendingTitle_ = "MdViewer";
    bool closing_ = false;
};

}  // namespace

int main(int argc, char* argv[]) {
    CefMainArgs mainArguments(argc, argv);
    const auto subprocessApp = mdviewer::CreatePortableCefApp(nullptr);
    const int subprocessCode = CefExecuteProcess(mainArguments, subprocessApp, nullptr);
    if (subprocessCode >= 0) return subprocessCode;

    XInitThreads();
    if (!std::getenv("GDK_BACKEND")) setenv("GDK_BACKEND", "x11", 0);
    if (!gtk_init_check(&argc, &argv)) return 2;

    const auto executable = ExecutablePath();
    LinuxShell shell(executable);
    shell.OpenAtStartup(InitialFile(argc, argv));

    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_WARNING;
    const auto cache = std::filesystem::path(g_get_user_cache_dir()) / "MdViewer" / "CEF";
    std::filesystem::create_directories(cache);
    CefString(&settings.cache_path) = cache.string();
    CefString(&settings.root_cache_path) = cache.string();
    const std::string language = InitialLanguage();
    CefString(&settings.locale) = language;
    CefString(&settings.accept_language_list) = language + ",en-US,en";

    const auto app = mdviewer::CreatePortableCefApp(&shell);
    if (!CefInitialize(mainArguments, settings, app, nullptr)) return 3;
    if (!mdviewer::RegisterPortableResourceScheme(shell.Resources())) {
        CefShutdown();
        return 4;
    }

    CefRunMessageLoop();
    CefClearSchemeHandlerFactories();
    CefShutdown();
    return 0;
}
