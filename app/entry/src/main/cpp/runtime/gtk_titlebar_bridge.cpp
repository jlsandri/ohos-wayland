// Native HarmonyOS title bars for GTK apps.
//
// The direct child loads this library into any app that links GTK 4. It
// finds each window's header bar (GtkHeaderBar or AdwHeaderBar), describes
// its contents to the compositor over ohos_titlebar_v1, and hides it; the
// launcher draws the description as an ArkUI title bar under the real
// HarmonyOS window buttons. What the user activates there comes back here
// and runs the same GTK action. A header bar holding anything this file
// cannot describe stays a GTK header bar.

#include "protocols/ohos-titlebar-v1-client-protocol.h"

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>

#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr guint kPollMs = 250;
constexpr size_t kChunk = 2048;

ohos_titlebar_manager_v1* gManager = nullptr;
bool gRegistryStarted = false;

struct WindowBridge {
    GtkWindow* window = nullptr;
    ohos_titlebar_v1* titlebar = nullptr;
    GtkWidget* header = nullptr;      // the header bar this file hid
    GtkWidget* tabBar = nullptr;      // the AdwTabBar this file hid; its tabs join the title bar
    GObject* tabView = nullptr;       // that tab bar's AdwTabView
    std::string body;                 // last description sent, without serial
    uint32_t serial = 0;
    std::vector<GObject*> items;      // id -> widget or AdwTabPage, rebuilt with each description
};

std::string Quote(const char* text)
{
    std::string out = "\"";
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text ? text : ""); *c; ++c) {
        switch (*c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': break;
            default:
                if (*c < 0x20) {
                    char escaped[8];
                    g_snprintf(escaped, sizeof(escaped), "\\u%04x", *c);
                    out += escaped;
                } else {
                    out += static_cast<char>(*c);
                }
        }
    }
    return out + "\"";
}

bool HasType(GtkWidget* widget, const char* typeName)
{
    for (GType type = G_OBJECT_TYPE(widget); type != 0; type = g_type_parent(type)) {
        if (std::strcmp(g_type_name(type), typeName) == 0) return true;
    }
    return false;
}

bool IsHeaderBar(GtkWidget* widget)
{
    return GTK_IS_HEADER_BAR(widget) || HasType(widget, "AdwHeaderBar");
}

// The first widget of a type the app shows (a header bar it hid itself, as
// Ghostty does with gtk-titlebar-style = tabs, is not its title bar).
GtkWidget* FindShown(GtkWidget* widget, bool (*match)(GtkWidget*))
{
    if (!gtk_widget_get_visible(widget)) return nullptr;
    if (match(widget)) return widget;
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget* found = FindShown(child, match)) return found;
    }
    return nullptr;
}

bool IsTabBar(GtkWidget* widget)
{
    return HasType(widget, "AdwTabBar");
}

GObject* ObjectProperty(GObject* object, const char* name)
{
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(object), name)) return nullptr;
    GObject* value = nullptr;
    g_object_get(object, name, &value, nullptr);
    return value;
}

bool BoolProperty(GObject* object, const char* name)
{
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(object), name)) return false;
    gboolean value = FALSE;
    g_object_get(object, name, &value, nullptr);
    return value;
}

std::string StringProperty(GObject* object, const char* name)
{
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(object), name)) return "";
    gchar* value = nullptr;
    g_object_get(object, name, &value, nullptr);
    std::string out = value ? value : "";
    g_free(value);
    return out;
}

// The theme file for an icon name, so the title bar can draw the same glyph.
// Looked up once per name: the poll runs on the GTK main thread.
std::string IconFile(GtkWidget* widget, const char* iconName)
{
    if (!iconName || !*iconName) return "";
    static std::unordered_map<std::string, std::string> cache;
    auto cached = cache.find(iconName);
    if (cached != cache.end()) return cached->second;
    GtkIconTheme* theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(widget));
    GtkIconPaintable* paintable = gtk_icon_theme_lookup_icon(theme, iconName, nullptr, 16, 1,
                                                             GTK_TEXT_DIR_LTR, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
    std::string path;
    if (GFile* file = gtk_icon_paintable_get_file(paintable)) {
        gchar* local = g_file_get_path(file);
        if (local) path = local;
        g_free(local);
        g_object_unref(file);
    }
    g_object_unref(paintable);
    cache.emplace(iconName, path);
    return path;
}

// The icon name and label a button shows, from its own properties or its
// child (GtkImage, GtkLabel, AdwButtonContent, or a box of those).
void ButtonFace(GtkWidget* widget, std::string& icon, std::string& label)
{
    if (GTK_IS_IMAGE(widget) && gtk_image_get_storage_type(GTK_IMAGE(widget)) == GTK_IMAGE_ICON_NAME) {
        if (icon.empty()) icon = gtk_image_get_icon_name(GTK_IMAGE(widget));
        return;
    }
    if (GTK_IS_LABEL(widget)) {
        if (label.empty()) label = gtk_label_get_text(GTK_LABEL(widget));
        return;
    }
    if (HasType(widget, "AdwButtonContent")) {
        if (icon.empty()) icon = StringProperty(G_OBJECT(widget), "icon-name");
        if (label.empty()) label = StringProperty(G_OBJECT(widget), "label");
        return;
    }
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (gtk_widget_get_visible(child)) ButtonFace(child, icon, label);
    }
}

// The GActionGroup that owns a detailed action name, for state and enabled.
GActionGroup* ActionOwner(GtkWindow* window, const char* action, const char** shortName)
{
    if (g_str_has_prefix(action, "app.")) {
        *shortName = action + 4;
        GApplication* app = g_application_get_default();
        return app ? G_ACTION_GROUP(app) : nullptr;
    }
    if (g_str_has_prefix(action, "win.") && G_IS_ACTION_GROUP(window)) {
        *shortName = action + 4;
        return G_ACTION_GROUP(window);
    }
    return nullptr;
}

class Describer {
public:
    explicit Describer(WindowBridge& bridge) : bridge_(bridge) {}

    bool translatable = true;

    std::string Items(GtkWidget* parent)
    {
        std::string out;
        for (GtkWidget* child = gtk_widget_get_first_child(parent); child; child = gtk_widget_get_next_sibling(child)) {
            std::string item = Item(child);
            if (item.empty()) continue;
            if (!out.empty()) out += ",";
            out += item;
        }
        return out;
    }

    std::string title;
    std::string subtitle;

    std::string Item(GtkWidget* widget);

private:
    WindowBridge& bridge_;

    int Register(gpointer object)
    {
        bridge_.items.push_back(G_OBJECT(g_object_ref(object)));
        return static_cast<int>(bridge_.items.size()) - 1;
    }

public:
    // The tab view's pages: title, selection, and state, activated by id.
    std::string Tabs(GObject* view)
    {
        std::string out;
        GObject* pages = ObjectProperty(view, "pages");
        if (!pages || !G_IS_LIST_MODEL(pages)) {
            if (pages) g_object_unref(pages);
            return out;
        }
        GObject* selected = ObjectProperty(view, "selected-page");
        const guint count = g_list_model_get_n_items(G_LIST_MODEL(pages));
        for (guint i = 0; i < count; ++i) {
            GObject* page = static_cast<GObject*>(g_list_model_get_item(G_LIST_MODEL(pages), i));
            std::string tab = "{\"id\":" + std::to_string(Register(page)) +
                              ",\"title\":" + Quote(StringProperty(page, "title").c_str());
            if (page == selected) tab += ",\"selected\":true";
            if (BoolProperty(page, "loading")) tab += ",\"loading\":true";
            if (BoolProperty(page, "needs-attention")) tab += ",\"attention\":true";
            if (!out.empty()) out += ",";
            out += tab + "}";
            g_object_unref(page);
        }
        if (selected) g_object_unref(selected);
        g_object_unref(pages);
        return out;
    }

private:

    std::string Common(GtkWidget* widget, const std::string& icon, const std::string& label)
    {
        std::string out = "\"id\":" + std::to_string(Register(widget));
        if (!icon.empty()) {
            out += ",\"icon\":" + Quote(icon.c_str());
            const std::string file = IconFile(widget, icon.c_str());
            if (!file.empty()) out += ",\"iconFile\":" + Quote(file.c_str());
        }
        if (!label.empty()) out += ",\"label\":" + Quote(label.c_str());
        if (const char* tooltip = gtk_widget_get_tooltip_text(widget)) out += ",\"tooltip\":" + Quote(tooltip);
        if (!gtk_widget_is_sensitive(widget)) out += ",\"disabled\":true";
        if (gtk_widget_has_css_class(widget, "suggested-action")) out += ",\"style\":\"suggested\"";
        if (gtk_widget_has_css_class(widget, "destructive-action")) out += ",\"style\":\"destructive\"";
        return out;
    }

    std::string Menu(GMenuModel* model)
    {
        std::string out;
        const int count = g_menu_model_get_n_items(model);
        for (int i = 0; i < count; ++i) {
            std::string entry;
            gchar* label = nullptr;
            g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
            if (GMenuModel* section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION)) {
                entry = "{\"kind\":\"section\"";
                if (label) entry += ",\"label\":" + Quote(label);
                entry += ",\"items\":[" + Menu(section) + "]}";
                g_object_unref(section);
            } else if (GMenuModel* submenu = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU)) {
                entry = "{\"kind\":\"submenu\",\"label\":" + Quote(label) + ",\"items\":[" + Menu(submenu) + "]}";
                g_object_unref(submenu);
            } else {
                entry = MenuItem(model, i, label);
            }
            g_free(label);
            if (entry.empty()) continue;
            if (!out.empty()) out += ",";
            out += entry;
        }
        return out;
    }

    std::string MenuItem(GMenuModel* model, int index, const char* label)
    {
        gchar* action = nullptr;
        if (!g_menu_model_get_item_attribute(model, index, G_MENU_ATTRIBUTE_ACTION, "s", &action)) {
            // A custom widget slot (GtkPopoverMenu "custom") has no action.
            gchar* custom = nullptr;
            if (g_menu_model_get_item_attribute(model, index, "custom", "s", &custom)) translatable = false;
            g_free(custom);
            return "";
        }
        std::string entry = "{\"kind\":\"item\",\"label\":" + Quote(label) + ",\"action\":" + Quote(action);
        GVariant* target = g_menu_model_get_item_attribute_value(model, index, G_MENU_ATTRIBUTE_TARGET, nullptr);
        if (target) {
            gchar* text = g_variant_print(target, TRUE);
            entry += ",\"target\":" + Quote(text);
            g_free(text);
        }
        const char* shortName = nullptr;
        if (GActionGroup* owner = ActionOwner(bridge_.window, action, &shortName)) {
            if (!g_action_group_get_action_enabled(owner, shortName)) entry += ",\"disabled\":true";
            if (GVariant* state = g_action_group_get_action_state(owner, shortName)) {
                if (g_variant_is_of_type(state, G_VARIANT_TYPE_BOOLEAN)) {
                    entry += g_variant_get_boolean(state) ? ",\"checked\":true" : ",\"checked\":false";
                } else if (target && g_variant_equal(state, target)) {
                    entry += ",\"checked\":true";
                } else if (target) {
                    entry += ",\"checked\":false";
                }
                g_variant_unref(state);
            }
        }
        GtkApplication* app = gtk_window_get_application(bridge_.window);
        if (app) {
            gchar* detailed = g_action_print_detailed_name(action, target);
            gchar** accels = gtk_application_get_accels_for_action(app, detailed);
            if (accels && accels[0]) {
                guint key = 0;
                GdkModifierType mods = static_cast<GdkModifierType>(0);
                if (gtk_accelerator_parse(accels[0], &key, &mods)) {
                    gchar* shown = gtk_accelerator_get_label(key, mods);
                    entry += ",\"accel\":" + Quote(shown);
                    g_free(shown);
                }
            }
            g_strfreev(accels);
            g_free(detailed);
        }
        if (target) g_variant_unref(target);
        g_free(action);
        return entry + "}";
    }

};

std::string Describer::Item(GtkWidget* widget)
{
    if (!gtk_widget_get_visible(widget)) return "";
    if (GTK_IS_WINDOW_CONTROLS(widget)) return "";
    if (HasType(widget, "AdwWindowTitle")) {
        title = StringProperty(G_OBJECT(widget), "title");
        subtitle = StringProperty(G_OBJECT(widget), "subtitle");
        return "";
    }
    if (GTK_IS_LABEL(widget)) {
        if (gtk_widget_has_css_class(widget, "title")) {
            title = gtk_label_get_text(GTK_LABEL(widget));
            return "";
        }
        return "{\"kind\":\"label\"," + Common(widget, "", gtk_label_get_text(GTK_LABEL(widget))) + "}";
    }
    if (HasType(widget, "AdwSplitButton")) {
        // A button with a dropdown menu (Ghostty's new tab / new split).
        GMenuModel* model = G_MENU_MODEL(ObjectProperty(G_OBJECT(widget), "menu-model"));
        std::string icon = StringProperty(G_OBJECT(widget), "icon-name");
        std::string label = StringProperty(G_OBJECT(widget), "label");
        std::string out = "{\"kind\":\"split\"," + Common(widget, icon, label);
        const std::string dropdown = StringProperty(G_OBJECT(widget), "dropdown-tooltip");
        if (!dropdown.empty()) out += ",\"dropdownTooltip\":" + Quote(dropdown.c_str());
        if (model) {
            out += ",\"items\":[" + Menu(model) + "]";
            g_object_unref(model);
        }
        return out + "}";
    }
    if (GTK_IS_MENU_BUTTON(widget)) {
        GtkMenuButton* button = GTK_MENU_BUTTON(widget);
        GMenuModel* model = gtk_menu_button_get_menu_model(button);
        if (!model) {
            translatable = false;
            return "";
        }
        std::string icon = gtk_menu_button_get_icon_name(button) ? gtk_menu_button_get_icon_name(button) : "";
        std::string label = gtk_menu_button_get_label(button) ? gtk_menu_button_get_label(button) : "";
        if (icon.empty() && label.empty()) icon = "open-menu-symbolic";
        return "{\"kind\":\"menu\"," + Common(widget, icon, label) + ",\"items\":[" + Menu(model) + "]}";
    }
    if (GTK_IS_STACK_SWITCHER(widget)) {
        std::string options;
        for (GtkWidget* child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (!gtk_widget_get_visible(child) || !GTK_IS_TOGGLE_BUTTON(child)) continue;
            std::string icon, label;
            ButtonFace(child, icon, label);
            if (!options.empty()) options += ",";
            options += "{" + Common(child, icon, label) +
                       (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child)) ? ",\"active\":true}" : "}");
        }
        return "{\"kind\":\"switcher\",\"options\":[" + options + "]}";
    }
    if (GTK_IS_BUTTON(widget)) {
        std::string icon, label;
        if (const char* name = gtk_button_get_icon_name(GTK_BUTTON(widget))) icon = name;
        if (const char* text = gtk_button_get_label(GTK_BUTTON(widget))) label = text;
        if (icon.empty() && label.empty()) ButtonFace(widget, icon, label);
        if (icon.empty() && label.empty()) {
            translatable = false;
            return "";
        }
        std::string out = "{\"kind\":\"button\"," + Common(widget, icon, label);
        if (GTK_IS_TOGGLE_BUTTON(widget)) {
            out += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) ? ",\"active\":true" : ",\"active\":false";
        }
        return out + "}";
    }
    if (GTK_IS_SEPARATOR(widget)) return "{\"kind\":\"separator\",\"id\":" + std::to_string(Register(widget)) + "}";
    if (GTK_IS_IMAGE(widget)) {
        std::string icon, label;
        ButtonFace(widget, icon, label);
        return icon.empty() ? "" : "{\"kind\":\"icon\"," + Common(widget, icon, "") + "}";
    }
    // Layout containers hold the items; anything else with no children is
    // a control the title bar cannot draw.
    if (gtk_widget_get_first_child(widget)) return Items(widget);
    if (GTK_IS_SPINNER(widget) || HasType(widget, "AdwSpinner")) return "";
    translatable = false;
    return "";
}

// Start, title, and end parts of a header bar: the children of its center box.
void Sections(GtkWidget* header, GtkWidget*& start, GtkWidget*& center, GtkWidget*& end)
{
    std::vector<GtkWidget*> stack = {header};
    while (!stack.empty()) {
        GtkWidget* widget = stack.back();
        stack.pop_back();
        if (GTK_IS_CENTER_BOX(widget)) {
            start = gtk_center_box_get_start_widget(GTK_CENTER_BOX(widget));
            center = gtk_center_box_get_center_widget(GTK_CENTER_BOX(widget));
            end = gtk_center_box_get_end_widget(GTK_CENTER_BOX(widget));
            return;
        }
        for (GtkWidget* child = gtk_widget_get_last_child(widget); child; child = gtk_widget_get_prev_sibling(child)) {
            stack.push_back(child);
        }
    }
    start = header;
}

void ClearItems(WindowBridge& bridge)
{
    for (GObject* object : bridge.items) g_object_unref(object);
    bridge.items.clear();
}

void Send(WindowBridge& bridge, const std::string& json)
{
    for (size_t offset = 0; offset < json.size(); offset += kChunk) {
        ohos_titlebar_v1_append(bridge.titlebar, json.substr(offset, kChunk).c_str());
    }
    ohos_titlebar_v1_commit(bridge.titlebar);
    wl_display_flush(gdk_wayland_display_get_wl_display(gtk_widget_get_display(GTK_WIDGET(bridge.window))));
}

void Restore(WindowBridge& bridge)
{
    if (bridge.header) gtk_widget_set_visible(bridge.header, TRUE);
    if (bridge.tabBar) gtk_widget_set_visible(bridge.tabBar, TRUE);
    bridge.header = nullptr;
    bridge.tabBar = nullptr;
    if (bridge.tabView) g_object_unref(bridge.tabView);
    bridge.tabView = nullptr;
}

void Describe(WindowBridge& bridge)
{
    const gint64 started = g_get_monotonic_time();
    GtkWidget* header = bridge.header ? bridge.header : FindShown(GTK_WIDGET(bridge.window), IsHeaderBar);
    std::string body;
    if (!header) {
        body = "\"native\":false";
        ClearItems(bridge);
    } else {
        ClearItems(bridge);
        Describer describer(bridge);
        GtkWidget *start = nullptr, *center = nullptr, *end = nullptr;
        Sections(header, start, center, end);
        const std::string startItems = start ? describer.Items(start) : "";
        const std::string centerItems = center ? describer.Item(center) : "";
        const std::string endItems = end ? describer.Items(end) : "";
        if (describer.translatable) {
            std::string title = describer.title;
            if (title.empty() && gtk_window_get_title(bridge.window)) title = gtk_window_get_title(bridge.window);
            body = "\"native\":true,\"title\":" + Quote(title.c_str());
            if (!describer.subtitle.empty()) body += ",\"subtitle\":" + Quote(describer.subtitle.c_str());
            body += ",\"start\":[" + startItems + "],\"center\":[" + centerItems + "],\"end\":[" + endItems + "]";
            // A libadwaita tab bar's tabs join the title bar, the way
            // HarmonyOS apps show tabs in theirs.
            GtkWidget* tabBar = bridge.tabBar ? bridge.tabBar : FindShown(GTK_WIDGET(bridge.window), IsTabBar);
            if (tabBar && !bridge.tabView) bridge.tabView = ObjectProperty(G_OBJECT(tabBar), "view");
            if (tabBar && bridge.tabView) {
                body += ",\"tabs\":[" + describer.Tabs(bridge.tabView) + "]";
                if (gtk_widget_get_visible(tabBar)) gtk_widget_set_visible(tabBar, FALSE);
                bridge.tabBar = tabBar;
            }
            if (gtk_widget_get_visible(header)) gtk_widget_set_visible(header, FALSE);
            bridge.header = header;
        } else {
            body = "\"native\":false";
            Restore(bridge);
        }
    }
    const gint64 spent = g_get_monotonic_time() - started;
    // A slow poll shows as a hitch in the app's animations.
    if (spent > 4000) g_message("ohos-titlebar: describing the header bar took %lld us", static_cast<long long>(spent));
    if (body == bridge.body) return;
    bridge.body = body;
    ++bridge.serial;
    Send(bridge, "{\"serial\":" + std::to_string(bridge.serial) + "," + body + "}");
}

// "serial\tid\taction\ttarget": a button or switcher option by id, a menu
// item's action on its menu button, or a tab ("close" closes it, otherwise
// it is selected).
void OnActivate(void* data, ohos_titlebar_v1*, const char* message)
{
    auto* bridge = static_cast<WindowBridge*>(data);
    gchar** fields = g_strsplit(message, "\t", 4);
    const guint count = g_strv_length(fields);
    if (count >= 2 && g_ascii_strtoull(fields[0], nullptr, 10) == bridge->serial) {
        const guint64 id = g_ascii_strtoull(fields[1], nullptr, 10);
        GObject* object = id < bridge->items.size() ? bridge->items[id] : nullptr;
        GtkWidget* widget = object && GTK_IS_WIDGET(object) ? GTK_WIDGET(object) : nullptr;
        const char* action = count >= 3 ? fields[2] : "";
        if (object && !widget && bridge->tabView) {
            if (std::strcmp(action, "close") == 0) {
                using ClosePage = void (*)(GObject*, GObject*);
                if (auto close = reinterpret_cast<ClosePage>(dlsym(RTLD_DEFAULT, "adw_tab_view_close_page"))) {
                    close(bridge->tabView, object);
                }
            } else {
                g_object_set(bridge->tabView, "selected-page", object, nullptr);
            }
        } else if (widget && *action) {
            GVariant* target = nullptr;
            if (count >= 4 && *fields[3]) target = g_variant_parse(nullptr, fields[3], nullptr, nullptr, nullptr);
            gtk_widget_activate_action_variant(widget, action, target);
        } else if (widget) {
            gtk_widget_activate(widget);
        }
    }
    g_strfreev(fields);
}

const ohos_titlebar_v1_listener kTitlebarListener = {
    .activate = OnActivate,
};

void FreeBridge(gpointer data)
{
    auto* bridge = static_cast<WindowBridge*>(data);
    if (bridge->titlebar) ohos_titlebar_v1_destroy(bridge->titlebar);
    ClearItems(*bridge);
    if (bridge->tabView) g_object_unref(bridge->tabView);
    delete bridge;
}

void Attach(GtkWindow* window)
{
    if (g_object_get_data(G_OBJECT(window), "ohos-titlebar")) return;
    if (!gtk_widget_get_mapped(GTK_WIDGET(window))) return;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!surface || !GDK_IS_WAYLAND_TOPLEVEL(surface)) return;
    auto* bridge = new WindowBridge();
    bridge->window = window;
    bridge->titlebar = ohos_titlebar_manager_v1_get_titlebar(gManager, gdk_wayland_surface_get_wl_surface(surface));
    ohos_titlebar_v1_add_listener(bridge->titlebar, &kTitlebarListener, bridge);
    g_object_set_data_full(G_OBJECT(window), "ohos-titlebar", bridge, FreeBridge);
}

void OnGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t)
{
    if (std::strcmp(interface, ohos_titlebar_manager_v1_interface.name) == 0 && !gManager) {
        gManager = static_cast<ohos_titlebar_manager_v1*>(
            wl_registry_bind(registry, name, &ohos_titlebar_manager_v1_interface, 1));
    }
}

void OnGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {
    .global = OnGlobal,
    .global_remove = OnGlobalRemove,
};

gboolean Poll(gpointer)
{
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return G_SOURCE_CONTINUE;
    if (!GDK_IS_WAYLAND_DISPLAY(display)) return G_SOURCE_REMOVE;
    if (!gRegistryStarted) {
        gRegistryStarted = true;
        wl_registry* registry = wl_display_get_registry(gdk_wayland_display_get_wl_display(display));
        wl_registry_add_listener(registry, &kRegistryListener, nullptr);
        wl_display_flush(gdk_wayland_display_get_wl_display(display));
        return G_SOURCE_CONTINUE;
    }
    if (!gManager) return G_SOURCE_CONTINUE;
    GListModel* windows = gtk_window_get_toplevels();
    const guint count = g_list_model_get_n_items(windows);
    for (guint i = 0; i < count; ++i) {
        auto* window = static_cast<GtkWindow*>(g_list_model_get_item(windows, i));
        // Dialogs and popups keep their own header bars.
        if (!gtk_window_get_transient_for(window)) {
            Attach(window);
            if (auto* bridge = static_cast<WindowBridge*>(g_object_get_data(G_OBJECT(window), "ohos-titlebar"))) {
                Describe(*bridge);
            }
        }
        g_object_unref(window);
    }
    return G_SOURCE_CONTINUE;
}

__attribute__((constructor)) void StartBridge()
{
    g_timeout_add(kPollMs, Poll, nullptr);
}

} // namespace
