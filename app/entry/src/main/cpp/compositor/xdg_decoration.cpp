#include "compositor/xdg_decoration.h"

#include "compositor/wayland_server.h"
#include "compositor/xdg_configure.h"
#include "compositor/xdg_shell.h"
#include "protocols/xdg-decoration-unstable-v1-server-protocol.h"
#include "protocols/xdg-shell-server-protocol.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace {

struct DecorationState {
    DecorationMode mode = DecorationMode::Unknown;
    wl_resource* toplevel = nullptr;
};

// Wayland thread writes; the commit path reads the mode.
std::mutex gMutex;
std::unordered_map<uint32_t, DecorationState> gStates;

uint32_t ToplevelIdOf(wl_resource* decoration)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wl_resource_get_user_data(decoration)));
}

// The app draws its own title bar unless it asks the compositor to, the way
// apps look on macOS; the HarmonyOS decor then stays hidden.
void Apply(wl_resource* decoration, uint32_t requested)
{
    const uint32_t id = ToplevelIdOf(decoration);
    const uint32_t mode = requested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    wl_resource* toplevel = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto& state = gStates[id];
        state.mode = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? DecorationMode::Server
                                                                          : DecorationMode::Client;
        toplevel = state.toplevel;
    }
    zxdg_toplevel_decoration_v1_send_configure(decoration, mode);
    // The decoration mode takes effect with the next xdg_surface.configure.
    if (toplevel) {
        auto* data = static_cast<ToplevelData*>(wl_resource_get_user_data(toplevel));
        auto* server = WaylandServer::GetInstance();
        const int32_t scale = std::max(1, server->OutputScale());
        if (data && data->xdgSurface) {
            XdgConfigureSend(toplevel, data->xdgSurface, server->OutputWidth() / scale,
                             server->OutputHeight() / scale, {XDG_TOPLEVEL_STATE_ACTIVATED});
        }
    }
    PostDecorationEvent(id, mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

void DecorationDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void DecorationSetMode(wl_client*, wl_resource* resource, uint32_t mode)
{
    Apply(resource, mode);
}

void DecorationUnsetMode(wl_client*, wl_resource* resource)
{
    Apply(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

const struct zxdg_toplevel_decoration_v1_interface kDecorationImpl = {
    .destroy = DecorationDestroy,
    .set_mode = DecorationSetMode,
    .unset_mode = DecorationUnsetMode,
};

void ManagerDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void ManagerGetToplevelDecoration(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* toplevel)
{
    auto* data = static_cast<ToplevelData*>(wl_resource_get_user_data(toplevel));
    const uint32_t toplevelId = data ? data->toplevelId : 0;
    wl_resource* decoration = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface,
                                                 wl_resource_get_version(manager), id);
    wl_resource_set_implementation(decoration, &kDecorationImpl,
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(toplevelId)), nullptr);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gStates[toplevelId].toplevel = toplevel;
    }
    // Until the client states a preference, the app draws its title bar.
    Apply(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

const struct zxdg_decoration_manager_v1_interface kManagerImpl = {
    .destroy = ManagerDestroy,
    .get_toplevel_decoration = ManagerGetToplevelDecoration,
};

void ManagerBind(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
    wl_resource_set_implementation(resource, &kManagerImpl, nullptr, nullptr);
}

} // namespace

void RegisterXdgDecoration(wl_display* display)
{
    wl_global_create(display, &zxdg_decoration_manager_v1_interface, 1, nullptr, ManagerBind);
}

DecorationMode ToplevelDecorationMode(uint32_t toplevelId)
{
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gStates.find(toplevelId);
    return it == gStates.end() ? DecorationMode::Unknown : it->second.mode;
}

void ForgetToplevelDecoration(uint32_t toplevelId)
{
    std::lock_guard<std::mutex> lock(gMutex);
    gStates.erase(toplevelId);
}

void PostDecorationEvent(uint32_t toplevelId, bool clientSide)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        toplevelId, ToplevelEventType::Decoration, clientSide ? "{\"client\":true}" : "{\"client\":false}");
}
