#include "compositor/portal/session_bus.h"

#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include <map>
#include <mutex>
#include <thread>

#include "runtime/user_folders.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Portal"
#include <hilog/log.h>

// The bus is small: every app connects to a GDBusServer here, the bus's own
// org.freedesktop.DBus methods are answered in this file, messages between
// apps are forwarded with their sender set, and the portal lives on the bus
// under the unique name :1.0. GLib clients track a well-known sender through
// GetNameOwner, so portal signals carry :1.0 as their sender.
namespace portal {
namespace {

constexpr char kBusName[] = "org.freedesktop.DBus";
constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalUnique[] = ":1.0";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kFileChooser[] = "org.freedesktop.portal.FileChooser";
constexpr char kRequest[] = "org.freedesktop.portal.Request";

struct Peer {
    GDBusConnection* connection = nullptr;
    std::string unique;
};

struct Pending {
    GDBusConnection* connection = nullptr;
    std::string destination;
    std::string handle;
};

// Owned by the bus thread, except gSink and the request queue.
GMainContext* gContext = nullptr;
GDBusServer* gServer = nullptr;
std::string gGuid;
std::map<GDBusConnection*, Peer> gPeers;
std::map<std::string, std::string> gNames;    // well-known name -> unique name
std::map<uint32_t, Pending> gPending;
std::map<uint32_t, GSocketConnection*> gFolderRequests;
uint32_t gNextPeer = 1;
uint32_t gNextRequest = 1;
RequestSink gSink;

std::string Quote(const char* text)
{
    std::string out = "\"";
    for (const char* c = text ? text : ""; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) < 0x20) continue;
        out += *c;
    }
    return out + "\"";
}

GDBusConnection* PeerByName(const std::string& name)
{
    auto owned = gNames.find(name);
    const std::string unique = owned != gNames.end() ? owned->second : name;
    for (auto& [connection, peer] : gPeers) {
        if (peer.unique == unique) return connection;
    }
    return nullptr;
}

void Reply(GDBusConnection* connection, GDBusMessage* call, GVariant* body, const char* sender)
{
    GDBusMessage* reply = g_dbus_message_new_method_reply(call);
    if (body) g_dbus_message_set_body(reply, body);
    g_dbus_message_set_sender(reply, sender);
    g_dbus_connection_send_message(connection, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
    g_object_unref(reply);
}

void ReplyError(GDBusConnection* connection, GDBusMessage* call, const char* name, const char* message,
                const char* sender)
{
    GDBusMessage* reply = g_dbus_message_new_method_error_literal(call, name, message);
    g_dbus_message_set_sender(reply, sender);
    g_dbus_connection_send_message(connection, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
    g_object_unref(reply);
}

const char* StringArg(GDBusMessage* message)
{
    GVariant* body = g_dbus_message_get_body(message);
    if (!body || !g_variant_is_of_type(body, G_VARIANT_TYPE_TUPLE) || g_variant_n_children(body) == 0) return "";
    GVariant* first = g_variant_get_child_value(body, 0);
    const char* value = g_variant_is_of_type(first, G_VARIANT_TYPE_STRING) ? g_variant_get_string(first, nullptr) : "";
    g_variant_unref(first);   // the string stays valid while the body lives
    return value;
}

void BusMethod(GDBusConnection* connection, GDBusMessage* call)
{
    const std::string member = g_dbus_message_get_member(call) ? g_dbus_message_get_member(call) : "";
    const std::string interface = g_dbus_message_get_interface(call) ? g_dbus_message_get_interface(call) : "";
    Peer& peer = gPeers[connection];
    if (member == "Hello") {
        Reply(connection, call, g_variant_new("(s)", peer.unique.c_str()), kBusName);
    } else if (member == "RequestName") {
        gNames[StringArg(call)] = peer.unique;
        Reply(connection, call, g_variant_new("(u)", 1u), kBusName);
    } else if (member == "ReleaseName") {
        gNames.erase(StringArg(call));
        Reply(connection, call, g_variant_new("(u)", 1u), kBusName);
    } else if (member == "GetNameOwner" || member == "NameHasOwner") {
        const std::string name = StringArg(call);
        std::string owner;
        if (name == kPortalName || name == kPortalUnique) {
            owner = kPortalUnique;
        } else if (name == kBusName) {
            owner = kBusName;
        } else if (GDBusConnection* other = PeerByName(name)) {
            owner = gPeers[other].unique;
        }
        if (member == "NameHasOwner") {
            Reply(connection, call, g_variant_new("(b)", !owner.empty()), kBusName);
        } else if (owner.empty()) {
            ReplyError(connection, call, "org.freedesktop.DBus.Error.NameHasNoOwner", name.c_str(), kBusName);
        } else {
            Reply(connection, call, g_variant_new("(s)", owner.c_str()), kBusName);
        }
    } else if (member == "ListNames") {
        GVariantBuilder names;
        g_variant_builder_init(&names, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&names, "s", kBusName);
        g_variant_builder_add(&names, "s", kPortalName);
        for (const auto& [name, owner] : gNames) g_variant_builder_add(&names, "s", name.c_str());
        for (const auto& [other, info] : gPeers) g_variant_builder_add(&names, "s", info.unique.c_str());
        Reply(connection, call, g_variant_new("(as)", &names), kBusName);
    } else if (member == "ListActivatableNames") {
        Reply(connection, call, g_variant_new_parsed("(@as [],)"), kBusName);
    } else if (member == "AddMatch" || member == "RemoveMatch" || member == "Ping") {
        if (member == "AddMatch") OH_LOG_INFO(LOG_APP, "[Portal] %{public}s AddMatch %{public}s", peer.unique.c_str(), StringArg(call));
        // Signals go to every peer; each client filters its own matches.
        Reply(connection, call, nullptr, kBusName);
    } else if (member == "StartServiceByName") {
        if (std::string(StringArg(call)) == kPortalName) {
            Reply(connection, call, g_variant_new("(u)", 2u), kBusName);
        } else {
            ReplyError(connection, call, "org.freedesktop.DBus.Error.ServiceUnknown", StringArg(call), kBusName);
        }
    } else if (member == "GetId") {
        Reply(connection, call, g_variant_new("(s)", gGuid.c_str()), kBusName);
    } else if (member == "GetConnectionUnixUser") {
        Reply(connection, call, g_variant_new("(u)", static_cast<guint32>(getuid())), kBusName);
    } else if (member == "GetConnectionCredentials") {
        Reply(connection, call, g_variant_new_parsed("({'UnixUserID': <%u>},)", static_cast<guint32>(getuid())),
              kBusName);
    } else if (interface == "org.freedesktop.DBus.Properties") {
        Reply(connection, call, g_variant_new_parsed("(<@as []>,)"), kBusName);
    } else {
        ReplyError(connection, call, "org.freedesktop.DBus.Error.UnknownMethod", member.c_str(), kBusName);
    }
}

// The JSON for ArkTS: title, multiple, directory, filters (name and
// suffixes), currentName, and currentFolder.
std::string DialogJson(const char* title, GVariant* options)
{
    std::string json = "{\"title\":" + Quote(title);
    gboolean flag = FALSE;
    if (g_variant_lookup(options, "multiple", "b", &flag) && flag) json += ",\"multiple\":true";
    if (g_variant_lookup(options, "directory", "b", &flag) && flag) json += ",\"directory\":true";
    const char* text = nullptr;
    if (g_variant_lookup(options, "accept_label", "&s", &text)) json += ",\"acceptLabel\":" + Quote(text);
    if (g_variant_lookup(options, "current_name", "&s", &text)) json += ",\"currentName\":" + Quote(text);
    const char* folder = nullptr;
    if (g_variant_lookup(options, "current_folder", "^&ay", &folder)) json += ",\"currentFolder\":" + Quote(folder);
    GVariantIter* filters = nullptr;
    if (g_variant_lookup(options, "filters", "a(sa(us))", &filters)) {
        json += ",\"filters\":[";
        const char* name = nullptr;
        GVariantIter* rules = nullptr;
        bool firstFilter = true;
        while (g_variant_iter_next(filters, "(&sa(us))", &name, &rules)) {
            std::string suffixes;
            guint32 kind = 0;
            const char* pattern = nullptr;
            while (g_variant_iter_next(rules, "(u&s)", &kind, &pattern)) {
                // Glob rules of the form *.ext; MIME rules have no HarmonyOS filter.
                if (kind != 0 || !g_str_has_prefix(pattern, "*.")) continue;
                if (!suffixes.empty()) suffixes += ",";
                suffixes += Quote(pattern + 1);
            }
            g_variant_iter_free(rules);
            if (suffixes.empty()) continue;
            json += std::string(firstFilter ? "" : ",") + "{\"name\":" + Quote(name) + ",\"suffixes\":[" + suffixes + "]}";
            firstFilter = false;
        }
        g_variant_iter_free(filters);
        json += "]";
    }
    return json + "}";
}

std::string RequestPath(const std::string& unique, GVariant* options, uint32_t id)
{
    // /org/freedesktop/portal/desktop/request/SENDER/TOKEN, SENDER without
    // the leading colon and with dots as underscores.
    std::string sender = unique.substr(1);
    for (char& c : sender) {
        if (c == '.') c = '_';
    }
    const char* token = nullptr;
    std::string handle = std::string(kPortalPath) + "/request/" + sender + "/";
    if (g_variant_lookup(options, "handle_token", "&s", &token)) {
        handle += token;
    } else {
        handle += "ohos" + std::to_string(id);
    }
    return handle;
}

void PortalMethod(GDBusConnection* connection, GDBusMessage* call)
{
    const std::string member = g_dbus_message_get_member(call) ? g_dbus_message_get_member(call) : "";
    const std::string interface = g_dbus_message_get_interface(call) ? g_dbus_message_get_interface(call) : "";
    GVariant* body = g_dbus_message_get_body(call);
    if (interface == "org.freedesktop.DBus.Properties" && (member == "Get" || member == "GetAll")) {
        const char* asked = nullptr;
        g_variant_get_child(body, 0, "&s", &asked);
        guint32 version = std::string(asked) == kFileChooser ? 3 : 0;
        if (version == 0) {
            // GTK reads a failed version as "no portal" and keeps its own dialog.
            ReplyError(connection, call, "org.freedesktop.DBus.Error.UnknownInterface", asked, kPortalUnique);
        } else if (member == "Get") {
            Reply(connection, call, g_variant_new("(v)", g_variant_new_uint32(version)), kPortalUnique);
        } else {
            Reply(connection, call, g_variant_new_parsed("({'version': <%u>},)", version), kPortalUnique);
        }
        return;
    }
    if (interface == kRequest && member == "Close") {
        Reply(connection, call, nullptr, kPortalUnique);
        return;
    }
    if (interface == kFileChooser && (member == "OpenFile" || member == "SaveFile")) {
        const char* parent = nullptr;
        const char* title = nullptr;
        GVariant* options = nullptr;
        g_variant_get(body, "(&s&s@a{sv})", &parent, &title, &options);
        const uint32_t id = gNextRequest++;
        Pending pending{connection, gPeers[connection].unique, RequestPath(gPeers[connection].unique, options, id)};
        gPending[id] = pending;
        Reply(connection, call, g_variant_new("(o)", pending.handle.c_str()), kPortalUnique);
        const std::string json = DialogJson(title, options);
        g_variant_unref(options);
        OH_LOG_INFO(LOG_APP, "[Portal] %{public}s request %{public}u from %{public}s handle %{public}s", member.c_str(), id,
                    pending.destination.c_str(), pending.handle.c_str());
        if (gSink) {
            gSink(id, member == "OpenFile" ? "open" : "save", json);
        } else {
            CompleteRequest(id, 2, {});
        }
        return;
    }
    ReplyError(connection, call, "org.freedesktop.DBus.Error.UnknownMethod",
               (interface + "." + member).c_str(), kPortalUnique);
}

void Forward(GDBusConnection* from, GDBusMessage* message, GDBusConnection* to)
{
    GDBusMessage* copy = g_dbus_message_copy(message, nullptr);
    if (!copy) return;
    g_dbus_message_set_sender(copy, gPeers[from].unique.c_str());
    g_dbus_connection_send_message(to, copy, G_DBUS_SEND_MESSAGE_FLAGS_PRESERVE_SERIAL, nullptr, nullptr);
    g_object_unref(copy);
}

struct Incoming {
    GDBusConnection* connection;
    GDBusMessage* message;
};

gboolean Dispatch(gpointer data)
{
    auto* incoming = static_cast<Incoming*>(data);
    GDBusConnection* connection = incoming->connection;
    GDBusMessage* message = incoming->message;
    if (gPeers.count(connection)) {
        const char* destination = g_dbus_message_get_destination(message);
        OH_LOG_INFO(LOG_APP, "[Portal] TRACE %{public}s type=%{public}d dest=%{public}s %{public}s %{public}s.%{public}s",
                    gPeers[connection].unique.c_str(), g_dbus_message_get_message_type(message),
                    destination ? destination : "-", g_dbus_message_get_path(message) ? g_dbus_message_get_path(message) : "-",
                    g_dbus_message_get_interface(message) ? g_dbus_message_get_interface(message) : "-",
                    g_dbus_message_get_member(message) ? g_dbus_message_get_member(message) : "-");
        const GDBusMessageType type = g_dbus_message_get_message_type(message);
        if (destination && std::string(destination) == kBusName) {
            if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL) BusMethod(connection, message);
        } else if (destination && (std::string(destination) == kPortalName || std::string(destination) == kPortalUnique)) {
            if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL) PortalMethod(connection, message);
        } else if (destination) {
            if (GDBusConnection* target = PeerByName(destination)) {
                Forward(connection, message, target);
            } else if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL) {
                ReplyError(connection, message, "org.freedesktop.DBus.Error.ServiceUnknown", destination, kBusName);
            }
        } else if (type == G_DBUS_MESSAGE_TYPE_SIGNAL) {
            for (auto& [other, peer] : gPeers) {
                if (other != connection) Forward(connection, message, other);
            }
        }
    }
    g_object_unref(message);
    g_object_unref(connection);
    delete incoming;
    return G_SOURCE_REMOVE;
}

// GDBus calls filters on its worker thread; the bus state lives on the bus
// thread, so every incoming message is handed over and dropped here.
GDBusMessage* Filter(GDBusConnection* connection, GDBusMessage* message, gboolean incoming, gpointer)
{
    if (!incoming) return message;
    auto* handoff = new Incoming{G_DBUS_CONNECTION(g_object_ref(connection)), message};
    g_main_context_invoke(gContext, Dispatch, handoff);
    return nullptr;
}

void OnClosed(GDBusConnection* connection, gboolean, GError*, gpointer)
{
    auto it = gPeers.find(connection);
    if (it == gPeers.end()) return;
    for (auto name = gNames.begin(); name != gNames.end();) {
        name = name->second == it->second.unique ? gNames.erase(name) : std::next(name);
    }
    for (auto pending = gPending.begin(); pending != gPending.end();) {
        pending = pending->second.connection == connection ? gPending.erase(pending) : std::next(pending);
    }
    gPeers.erase(it);
    g_object_unref(connection);
}

gboolean OnNewConnection(GDBusServer*, GDBusConnection* connection, gpointer)
{
    Peer peer;
    peer.connection = G_DBUS_CONNECTION(g_object_ref(connection));
    peer.unique = ":1." + std::to_string(gNextPeer++);
    gPeers[connection] = peer;
    g_signal_connect(connection, "closed", G_CALLBACK(OnClosed), nullptr);
    g_dbus_connection_add_filter(connection, Filter, nullptr, nullptr);
    OH_LOG_INFO(LOG_APP, "[Portal] peer %{public}s connected", peer.unique.c_str());
    return TRUE;
}

// Same-user peers only, over EXTERNAL; the socket is in the app's own
// runtime directory.
gboolean AuthorizePeer(GDBusAuthObserver*, GIOStream*, GCredentials* credentials, gpointer)
{
    GError* error = nullptr;
    const uid_t uid = credentials ? g_credentials_get_unix_user(credentials, &error) : static_cast<uid_t>(-1);
    if (error) g_error_free(error);
    return uid == getuid();
}

// A program asking for a user folder (runtime/user_folders.h). ArkTS shows
// the HarmonyOS permission dialog and answers like a portal request.
gboolean OnFolderRequest(GSocketService*, GSocketConnection* connection, GObject*, gpointer)
{
    GCredentials* credentials = g_socket_get_credentials(g_socket_connection_get_socket(connection), nullptr);
    const bool sameUser = credentials && g_credentials_get_unix_user(credentials, nullptr) == getuid();
    if (credentials) g_object_unref(credentials);
    char name[32] = {};
    GInputStream* input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    const gssize length = sameUser ? g_input_stream_read(input, name, sizeof(name) - 1, nullptr, nullptr) : -1;
    if (length <= 0 || name[length - 1] != '\n') return TRUE;
    name[length - 1] = '\0';
    const uint32_t id = gNextRequest++;
    gFolderRequests[id] = G_SOCKET_CONNECTION(g_object_ref(connection));
    OH_LOG_INFO(LOG_APP, "[Portal] folder request %{public}u for %{public}s", id, name);
    gSink(id, "folder", std::string("{\"folder\":") + Quote(name) + "}");
    return TRUE;
}

bool ListenForFolders(const std::string& path)
{
    unlink(path.c_str());
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 8) != 0) {
        if (fd >= 0) close(fd);
        return false;
    }
    GSocket* socket = g_socket_new_from_fd(fd, nullptr);
    GSocketService* service = g_socket_service_new();
    const bool added = socket && g_socket_listener_add_socket(G_SOCKET_LISTENER(service), socket, nullptr, nullptr);
    if (socket) g_object_unref(socket);
    if (!added) return false;
    g_signal_connect(service, "incoming", G_CALLBACK(OnFolderRequest), nullptr);
    g_socket_service_start(service);
    return true;
}

struct Completion {
    uint32_t id;
    uint32_t response;
    std::vector<std::string> uris;
};

gboolean SendResponse(gpointer data)
{
    auto* completion = static_cast<Completion*>(data);
    auto folder = gFolderRequests.find(completion->id);
    if (folder != gFolderRequests.end()) {
        const char answer = completion->response == 0 ? '1' : '0';
        GOutputStream* output = g_io_stream_get_output_stream(G_IO_STREAM(folder->second));
        g_output_stream_write(output, &answer, 1, nullptr, nullptr);
        g_io_stream_close(G_IO_STREAM(folder->second), nullptr, nullptr);
        g_object_unref(folder->second);
        OH_LOG_INFO(LOG_APP, "[Portal] folder request %{public}u answered %{public}c", completion->id, answer);
        gFolderRequests.erase(folder);
    }
    auto it = gPending.find(completion->id);
    if (it != gPending.end()) {
        // Always send uris, empty on cancel: GTK's response_cb reads it
        // unchecked and walks the array (gtkfilechoosernativeportal.c).
        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        GVariantBuilder uris;
        g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
        for (const auto& uri : completion->uris) g_variant_builder_add(&uris, "s", uri.c_str());
        g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
        GDBusMessage* signal = g_dbus_message_new_signal(it->second.handle.c_str(), kRequest, "Response");
        g_dbus_message_set_body(signal, g_variant_new("(ua{sv})", completion->response, &results));
        g_dbus_message_set_sender(signal, kPortalUnique);
        g_dbus_message_set_destination(signal, it->second.destination.c_str());
        g_dbus_connection_send_message(it->second.connection, signal, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
        g_object_unref(signal);
        OH_LOG_INFO(LOG_APP, "[Portal] request %{public}u answered %{public}u with %{public}zu uri(s)",
                    completion->id, completion->response, completion->uris.size());
        gPending.erase(it);
    }
    delete completion;
    return G_SOURCE_REMOVE;
}

} // namespace

bool StartSessionBus(const std::string& socketPath, RequestSink sink)
{
    if (gServer) return true;
    gSink = std::move(sink);
    unlink(socketPath.c_str());
    gContext = g_main_context_new();
    gchar* guid = g_dbus_generate_guid();
    gGuid = guid;
    GDBusAuthObserver* observer = g_dbus_auth_observer_new();
    g_signal_connect(observer, "authorize-authenticated-peer", G_CALLBACK(AuthorizePeer), nullptr);
    GError* error = nullptr;
    const std::string address = "unix:path=" + socketPath;
    g_main_context_push_thread_default(gContext);
    gServer = g_dbus_server_new_sync(address.c_str(), G_DBUS_SERVER_FLAGS_NONE, guid, observer, nullptr, &error);
    if (gServer) {
        g_signal_connect(gServer, "new-connection", G_CALLBACK(OnNewConnection), nullptr);
        g_dbus_server_start(gServer);
        const std::string folders = socketPath.substr(0, socketPath.rfind('/') + 1) + ohoswayland::kFolderSocket;
        if (!ListenForFolders(folders)) OH_LOG_ERROR(LOG_APP, "[Portal] folder socket failed: %{public}s", folders.c_str());
    }
    g_main_context_pop_thread_default(gContext);
    g_free(guid);
    g_object_unref(observer);
    if (!gServer) {
        OH_LOG_ERROR(LOG_APP, "[Portal] session bus failed: %{public}s", error ? error->message : "?");
        if (error) g_error_free(error);
        return false;
    }
    std::thread([]() {
        g_main_context_push_thread_default(gContext);
        GMainLoop* loop = g_main_loop_new(gContext, FALSE);
        g_main_loop_run(loop);
    }).detach();
    OH_LOG_INFO(LOG_APP, "[Portal] session bus at %{public}s", socketPath.c_str());
    return true;
}

void CompleteRequest(uint32_t id, uint32_t response, const std::vector<std::string>& uris)
{
    if (!gContext) return;
    g_main_context_invoke(gContext, SendResponse, new Completion{id, response, uris});
}

} // namespace portal
