#include "launch_params.h"
#include <AbilityKit/native_child_process.h>
#include <gtk/gtk.h>
#include <hilog/log.h>
#include <wayland-client.h>
#include "protocols/wayland-client-protocol.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <signal.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GtkDemo"

namespace {

void ForwardClientLog(const char* path, std::atomic<bool>& stop)
{
    FILE* file = nullptr;
    char line[4096];
    while (!stop.load(std::memory_order_relaxed)) {
        if (!file) {
            file = fopen(path, "r");
            if (!file) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            OH_LOG_ERROR(LOG_APP, "GTK_NATIVE_LOG %{public}s", line);
        }
        if (feof(file)) clearerr(file);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            OH_LOG_ERROR(LOG_APP, "GTK_NATIVE_LOG %{public}s", line);
        }
        fclose(file);
    }
}

void CrashHandler(int signal, siginfo_t*, void* context)
{
    const auto* ucontext = static_cast<const ucontext_t*>(context);
    void* pc = reinterpret_cast<void*>(ucontext->uc_mcontext.pc);
    Dl_info info = {};
    if (dladdr(pc, &info) && info.dli_fname) {
        OH_LOG_ERROR(LOG_APP, "GTK_CRASH signal=%{public}d object=%{public}s symbol=%{public}s offset=%{public}p",
                     signal, info.dli_fname, info.dli_sname ? info.dli_sname : "(unknown)",
                     reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) -
                                           reinterpret_cast<uintptr_t>(info.dli_fbase)));
    } else {
        OH_LOG_ERROR(LOG_APP, "GTK_CRASH signal=%{public}d pc=%{public}p", signal, pc);
    }
    _exit(128 + signal);
}

GtkWidget* Row(const char* label, GtkWidget* control)
{
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* text = gtk_label_new(label);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_append(GTK_BOX(row), text);
    gtk_box_append(GTK_BOX(row), control);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    return row;
}

// The chosen file or folder, and whether this process can read it: the
// portal returns what the HarmonyOS picker granted.
void OnChosen(GObject* source, GAsyncResult* result, gpointer folder)
{
    GError* error = nullptr;
    GFile* file = folder ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error)
                         : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!file) {
        OH_LOG_INFO(LOG_APP, "GTK_DEMO_CHOSEN none: %{public}s", error ? error->message : "?");
        if (error) g_error_free(error);
        return;
    }
    gchar* path = g_file_get_path(file);
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, nullptr, &error);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_CHOSEN %{public}s readable=%{public}d size=%{public}lld err=%{public}s",
                path ? path : "?", info != nullptr, info ? static_cast<long long>(g_file_info_get_size(info)) : -1LL,
                error ? error->message : "");
    if (info) g_object_unref(info);
    if (error) g_error_free(error);
    g_free(path);
    g_object_unref(file);
}

// Every demo action logs its name, so a test sees which one ran.
void OnAction(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    GVariant* state = g_action_get_state(G_ACTION(action));
    if (state && g_variant_is_of_type(state, G_VARIANT_TYPE_BOOLEAN)) {
        g_simple_action_set_state(action, g_variant_new_boolean(!g_variant_get_boolean(state)));
    } else if (state && parameter) {
        g_simple_action_set_state(action, parameter);
    }
    if (state) g_variant_unref(state);
    const char* name = g_action_get_name(G_ACTION(action));
    if (std::strcmp(name, "open") == 0 || std::strcmp(name, "open-folder") == 0) {
        GtkFileDialog* dialog = gtk_file_dialog_new();
        GtkWindow* window = GTK_WINDOW(user_data);
        const bool folder = std::strcmp(name, "open-folder") == 0;
        if (folder) {
            gtk_file_dialog_select_folder(dialog, window, nullptr, OnChosen, GINT_TO_POINTER(1));
        } else {
            gtk_file_dialog_open(dialog, window, nullptr, OnChosen, nullptr);
        }
        g_object_unref(dialog);
    }
    gchar* value = parameter ? g_variant_print(parameter, FALSE) : g_strdup("");
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ACTION %{public}s %{public}s", g_action_get_name(G_ACTION(action)), value);
    g_free(value);
}

void OnAdd(GtkButton*, gpointer)
{
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ACTION add");
}

// The primary menu: sections, a check item, a radio group, and shortcuts.
GMenuModel* PrimaryMenu()
{
    GMenu* menu = g_menu_new();
    GMenu* file = g_menu_new();
    g_menu_append(file, "_New Window", "win.new-window");
    g_menu_append(file, "_Open…", "win.open");
    g_menu_append(file, "Open _Folder…", "win.open-folder");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(file));
    GMenu* view = g_menu_new();
    g_menu_append(view, "Show _Grid", "win.show-grid");
    g_menu_append(view, "Compact", "win.density::compact");
    g_menu_append(view, "Comfortable", "win.density::comfortable");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));
    GMenu* help = g_menu_new();
    g_menu_append(help, "_About", "win.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(help));
    g_object_unref(file);
    g_object_unref(view);
    g_object_unref(help);
    return G_MENU_MODEL(menu);
}

// A showcase of the common widgets (header bar, stack switcher, buttons,
// entry, switch, check, list) so the runtime's GTK style can be checked.
GtkWidget* ScrollMeasure(GtkWidget* page);

// Clipboard probe (GTK_DEMO_CLIPBOARD, the clipboard-copy and
// clipboard-paste tests): "copy:<text>" puts text on the clipboard, which
// the compositor hands to the HarmonyOS pasteboard; "paste" logs what the
// clipboard holds, which in a fresh run comes from the pasteboard.
void LogPasted(GObject* clipboard, GAsyncResult* result, gpointer)
{
    GError* error = nullptr;
    char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(clipboard), result, &error);
    OH_LOG_INFO(LOG_APP, "GTK_CLIPBOARD pasted=%{public}s error=%{public}s",
                text ? text : "", error ? error->message : "none");
    g_free(text);
    if (error) g_error_free(error);
}

gboolean RunClipboardProbe(gpointer)
{
    const char* probe = getenv("GTK_DEMO_CLIPBOARD");
    GdkClipboard* clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    if (strncmp(probe, "copy:", 5) == 0) {
        gdk_clipboard_set_text(clipboard, probe + 5);
        OH_LOG_INFO(LOG_APP, "GTK_CLIPBOARD copied");
    } else {
        gdk_clipboard_read_text_async(clipboard, nullptr, LogPasted, nullptr);
    }
    return G_SOURCE_REMOVE;
}

// Drop probe (GTK_DEMO_DROP, the drop-text test): the window takes dropped
// text and logs it.
gboolean LogDropped(GtkDropTarget*, const GValue* value, double x, double y, gpointer)
{
    OH_LOG_INFO(LOG_APP, "GTK_DROP text=%{public}s at=%{public}.0f,%{public}.0f",
                G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : "", x, y);
    return TRUE;
}

gboolean RunThemeProbe(gpointer window)
{
    static GtkCssProvider* provider = nullptr;
    static int step = 0;
    const char* color = step++ % 2 == 0 ? "#284058" : "#406080";
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    char css[64];
    g_snprintf(css, sizeof(css), "window { background: %s; }", color);
    gtk_css_provider_load_from_string(provider, css);
    OH_LOG_INFO(LOG_APP, "GTK_THEME %s", color);
    return G_SOURCE_CONTINUE;
}

void Activate(GtkApplication* app, gpointer)
{
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ACTIVATE begin");
    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Native GTK on HarmonyOS");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    GtkWidget* stack = gtk_stack_new();
    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), switcher);
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "New item");
    g_signal_connect(add, "clicked", G_CALLBACK(OnAdd), nullptr);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), add);
    static const GActionEntry kActions[] = {
        {"new-window", OnAction, nullptr, nullptr, nullptr, {}},
        {"open", OnAction, nullptr, nullptr, nullptr, {}},
        {"open-folder", OnAction, nullptr, nullptr, nullptr, {}},
        {"show-grid", OnAction, nullptr, "true", nullptr, {}},
        {"density", OnAction, "s", "'comfortable'", nullptr, {}},
        {"about", OnAction, nullptr, nullptr, nullptr, {}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window), kActions, G_N_ELEMENTS(kActions), window);
    const char* newWindow[] = {"<Control>n", nullptr};
    const char* open[] = {"<Control>o", nullptr};
    gtk_application_set_accels_for_action(app, "win.new-window", newWindow);
    gtk_application_set_accels_for_action(app, "win.open", open);
    GtkWidget* menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
    GMenuModel* model = PrimaryMenu();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), model);
    g_object_unref(model);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(page, 32);
    gtk_widget_set_margin_end(page, 32);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    GtkWidget* title = gtk_label_new("GTK 4 on the shared Wayland runtime");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget* detail = gtk_label_new("HarmonyOS Sans • arm64 • direct GPU • 中文字体");
    gtk_widget_add_css_class(detail, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(detail), 0);
    gtk_box_append(GTK_BOX(page), title);
    gtk_box_append(GTK_BOX(page), detail);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* plain = gtk_button_new_with_label("Button");
    GtkWidget* suggested = gtk_button_new_with_label("Suggested");
    gtk_widget_add_css_class(suggested, "suggested-action");
    GtkWidget* destructive = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(destructive, "destructive-action");
    gtk_box_append(GTK_BOX(buttons), plain);
    gtk_box_append(GTK_BOX(buttons), suggested);
    gtk_box_append(GTK_BOX(buttons), destructive);
    gtk_box_append(GTK_BOX(page), buttons);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search or type here");
    gtk_box_append(GTK_BOX(page), entry);

    GtkWidget* list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    GtkWidget* toggle = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    gtk_list_box_append(GTK_LIST_BOX(list), Row("Wi-Fi style switch", toggle));
    GtkWidget* check = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
    gtk_list_box_append(GTK_LIST_BOX(list), Row("Check button", check));
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(scale), 60);
    gtk_widget_set_size_request(scale, 240, -1);
    gtk_list_box_append(GTK_LIST_BOX(list), Row("Scale", scale));
    gtk_box_append(GTK_BOX(page), list);


    GtkWidget* content = page;
    if (getenv("GTK_DEMO_SCROLL")) content = ScrollMeasure(page);
    gtk_stack_add_titled(GTK_STACK(stack), content, "widgets", "Widgets");
    GtkWidget* second = gtk_label_new("Second page");
    gtk_stack_add_titled(GTK_STACK(stack), second, "about", "About");
    gtk_window_set_child(GTK_WINDOW(window), stack);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ACTIVATE present");
    gtk_window_present(GTK_WINDOW(window));
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_PRESENT title=Native GTK on HarmonyOS");
    if (getenv("GTK_DEMO_CLIPBOARD")) g_timeout_add(1500, RunClipboardProbe, nullptr);
    if (getenv("GTK_DEMO_THEME")) g_timeout_add(1000, RunThemeProbe, window);
    if (getenv("GTK_DEMO_DROP")) {
        GtkDropTarget* target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
        g_signal_connect(target, "drop", G_CALLBACK(LogDropped), nullptr);
        gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(target));
    }
}

// Scroll measurement (GTK_DEMO_SCROLL=1, the gtk-scroll-replay test): the
// page becomes a tall scrolled list starting mid-way, and every change of
// its position is logged, plus the settled position 500 ms after the last
// change, so kinetic scrolling counts too.
guint gSettleTimer = 0;
double gScrollStart = 0;

gboolean LogSettled(gpointer adjustment)
{
    const double value = gtk_adjustment_get_value(GTK_ADJUSTMENT(adjustment));
    OH_LOG_INFO(LOG_APP, "GTK_SCROLL settled value=%{public}.1f moved=%{public}.1f", value, value - gScrollStart);
    gSettleTimer = 0;
    return G_SOURCE_REMOVE;
}

void OnScrollValue(GtkAdjustment* adjustment, gpointer)
{
    OH_LOG_INFO(LOG_APP, "GTK_SCROLL value=%{public}.1f", gtk_adjustment_get_value(adjustment));
    if (gSettleTimer) g_source_remove(gSettleTimer);
    gSettleTimer = g_timeout_add(500, LogSettled, adjustment);
}

gboolean StartMidway(gpointer adjustment)
{
    auto* adj = GTK_ADJUSTMENT(adjustment);
    gScrollStart = (gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)) / 2;
    gtk_adjustment_set_value(adj, gScrollStart);
    g_signal_connect(adj, "value-changed", G_CALLBACK(OnScrollValue), nullptr);
    OH_LOG_INFO(LOG_APP, "GTK_SCROLL start value=%{public}.1f", gScrollStart);
    return G_SOURCE_REMOVE;
}

GtkWidget* ScrollMeasure(GtkWidget* page)
{
    for (int i = 1; i <= 400; ++i) {
        char text[32];
        g_snprintf(text, sizeof(text), "Row %d", i);
        GtkWidget* label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_append(GTK_BOX(page), label);
    }
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
    gtk_widget_set_vexpand(scrolled, TRUE);
    g_timeout_add(1500, StartMidway, gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)));
    return scrolled;
}

void LogGtk(const char* domain, GLogLevelFlags, const char* message, gpointer)
{
    OH_LOG_ERROR(LOG_APP, "GTK_LOG %{public}s: %{public}s", domain ? domain : "gtk", message ? message : "(null)");
}

void LogPrint(const char* message)
{
    OH_LOG_ERROR(LOG_APP, "GTK_PRINT %{public}s", message ? message : "(null)");
}

} // namespace

extern "C" __attribute__((visibility("default")))
void OhosWaylandGtkDemoMain(NativeChildProcess_Args args)
{
    struct sigaction action = {};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ENTRY begin");
    if (!ohoswayland::ApplyLaunchParams(args.entryParams)) {
        OH_LOG_ERROR(LOG_APP, "GTK_DEMO_ERROR invalid launch parameters");
        return;
    }
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ENTRY environment applied");
    setenv("GSK_RENDERER", "gl", 1);
    setenv("GDK_DEBUG", "opengl", 1);
    setenv("GSK_DEBUG", "renderer", 1);
    g_log_set_default_handler(LogGtk, nullptr);
    g_set_print_handler(LogPrint);
    g_set_printerr_handler(LogPrint);
    const char* xkbRoot = getenv("XKB_CONFIG_ROOT");
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_XKB root=%{public}s access=%{public}d",
                xkbRoot ? xkbRoot : "(null)", xkbRoot ? access(xkbRoot, F_OK) : -2);
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_WAYLAND display=%{public}s runtime=%{public}s",
                waylandDisplay ? waylandDisplay : "(null)", runtimeDir ? runtimeDir : "(null)");
    wl_display* display = wl_display_connect(nullptr);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_WAYLAND connect=%{public}p", display);
    if (display) {
        wl_registry* registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {
            [](void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
                if (strcmp(interface, "wl_data_device_manager") == 0) {
                    auto* manager = static_cast<wl_data_device_manager*>(
                        wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
                    OH_LOG_INFO(LOG_APP, "GTK_DEMO_DATA manager bound");
                    wl_data_source* source = wl_data_device_manager_create_data_source(manager);
                    wl_data_source_offer(source, "text/plain");
                    wl_data_device* device = wl_data_device_manager_get_data_device(manager, nullptr);
                    wl_data_device_set_selection(device, source, 1);
                    OH_LOG_INFO(LOG_APP, "GTK_DEMO_DATA selection passed");
                    wl_data_device_release(device);
                    wl_data_source_destroy(source);
                    wl_data_device_manager_destroy(manager);
                }
            },
            [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, nullptr);
        wl_display_roundtrip(display);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
    }
    if (!gtk_init_check()) {
        OH_LOG_ERROR(LOG_APP, "GTK_DEMO_ERROR gtk_init_check failed");
        return;
    }
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ENTRY init passed");
    if (freopen("/data/storage/el2/base/cache/ohos_wayland_gtk_client.log", "w", stderr))
        setvbuf(stderr, nullptr, _IONBF, 0);
    std::atomic<bool> logStop{false};
    std::thread logThread(ForwardClientLog,
                          "/data/storage/el2/base/cache/ohos_wayland_gtk_client.log",
                          std::ref(logStop));
    GtkApplication* app = gtk_application_new("com.exploration.ohoswayland.gtkdemo", G_APPLICATION_NON_UNIQUE);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ENTRY application created");
    g_signal_connect(app, "activate", G_CALLBACK(Activate), nullptr);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_ENTRY run begin");
    const int result = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    OH_LOG_INFO(LOG_APP, "GTK_DEMO_EXIT result=%{public}d", result);
    logStop.store(true, std::memory_order_relaxed);
    if (logThread.joinable()) logThread.join();
}
