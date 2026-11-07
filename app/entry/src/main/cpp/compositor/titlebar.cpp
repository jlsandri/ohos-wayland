#include "compositor/titlebar.h"

#include "compositor/toplevel/toplevel_event_bus.h"
#include "compositor/wayland_server.h"
#include "protocols/ohos-titlebar-v1-server-protocol.h"

#include <wayland-server-core.h>

#include <fcntl.h>
#include <unistd.h>

#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Titlebar"
#include <hilog/log.h>

namespace {

struct Titlebar {
    wl_resource* resource = nullptr;
    wl_resource* surface = nullptr;
    wl_listener surfaceDestroy{};
    uint32_t toplevelId = 0;
    std::string pending;
};

// Wayland thread owns the titlebars; activations arrive from the UI thread
// through the queue and the pipe.
std::unordered_map<uint32_t, Titlebar*> gByToplevel;
std::mutex gQueueMutex;
std::deque<std::pair<uint32_t, std::string>> gQueue;
int gPipe[2] = {-1, -1};

uint32_t ResolveToplevel(Titlebar* bar)
{
    if (bar->toplevelId == 0 && bar->surface) {
        bar->toplevelId = WaylandServer::GetInstance()->GetToplevelManager().FindToplevelBySurface(bar->surface);
        if (bar->toplevelId != 0) gByToplevel[bar->toplevelId] = bar;
    }
    return bar->toplevelId;
}

void Post(Titlebar* bar, const std::string& json)
{
    const uint32_t id = ResolveToplevel(bar);
    if (id == 0) {
        OH_LOG_WARN(LOG_APP, "[Titlebar] description for an unmapped surface dropped");
        return;
    }
    WaylandServer::GetInstance()->PostToplevelEvent(id, ToplevelEventType::Titlebar, json);
}

void OnSurfaceDestroy(wl_listener* listener, void*)
{
    Titlebar* bar = wl_container_of(listener, bar, surfaceDestroy);
    bar->surface = nullptr;
    wl_list_remove(&bar->surfaceDestroy.link);
    wl_list_init(&bar->surfaceDestroy.link);
}

void TitlebarResourceDestroy(wl_resource* resource)
{
    auto* bar = static_cast<Titlebar*>(wl_resource_get_user_data(resource));
    if (!bar) return;
    if (bar->toplevelId != 0) {
        gByToplevel.erase(bar->toplevelId);
        // The toolkit draws its header bar again.
        WaylandServer::GetInstance()->PostToplevelEvent(bar->toplevelId, ToplevelEventType::Titlebar,
                                                        "{\"native\":false}");
    }
    wl_list_remove(&bar->surfaceDestroy.link);
    delete bar;
}

void TitlebarDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void TitlebarAppend(wl_client*, wl_resource* resource, const char* chunk)
{
    auto* bar = static_cast<Titlebar*>(wl_resource_get_user_data(resource));
    if (bar && chunk) bar->pending += chunk;
}

void TitlebarCommit(wl_client*, wl_resource* resource)
{
    auto* bar = static_cast<Titlebar*>(wl_resource_get_user_data(resource));
    if (!bar) return;
    std::string json;
    json.swap(bar->pending);
    Post(bar, json);
}

const struct ohos_titlebar_v1_interface kTitlebarImpl = {
    .destroy = TitlebarDestroy,
    .append = TitlebarAppend,
    .commit = TitlebarCommit,
};

void ManagerDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void ManagerGetTitlebar(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surface)
{
    wl_resource* resource = wl_resource_create(client, &ohos_titlebar_v1_interface,
                                               wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* bar = new Titlebar();
    bar->resource = resource;
    bar->surface = surface;
    bar->surfaceDestroy.notify = OnSurfaceDestroy;
    wl_resource_add_destroy_listener(surface, &bar->surfaceDestroy);
    wl_resource_set_implementation(resource, &kTitlebarImpl, bar, TitlebarResourceDestroy);
    ResolveToplevel(bar);
    OH_LOG_INFO(LOG_APP, "[Titlebar] bound for toplevel %{public}u", bar->toplevelId);
}

const struct ohos_titlebar_manager_v1_interface kManagerImpl = {
    .destroy = ManagerDestroy,
    .get_titlebar = ManagerGetTitlebar,
};

void ManagerBind(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &ohos_titlebar_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagerImpl, nullptr, nullptr);
}

int OnActivationsReady(int fd, uint32_t, void*)
{
    char drain[64];
    while (read(fd, drain, sizeof(drain)) > 0) {
    }
    std::deque<std::pair<uint32_t, std::string>> ready;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        ready.swap(gQueue);
    }
    for (const auto& [toplevelId, json] : ready) {
        auto it = gByToplevel.find(toplevelId);
        if (it == gByToplevel.end()) continue;
        ohos_titlebar_v1_send_activate(it->second->resource, json.c_str());
        wl_client_flush(wl_resource_get_client(it->second->resource));
    }
    return 0;
}

} // namespace

void RegisterTitlebar(wl_display* display)
{
    wl_global_create(display, &ohos_titlebar_manager_v1_interface, 1, nullptr, ManagerBind);
    if (pipe2(gPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        wl_event_loop_add_fd(wl_display_get_event_loop(display), gPipe[0], WL_EVENT_READABLE,
                             OnActivationsReady, nullptr);
    }
}

void ActivateTitlebar(uint32_t toplevelId, const std::string& json)
{
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        gQueue.emplace_back(toplevelId, json);
    }
    if (gPipe[1] >= 0) {
        const char wake = 1;
        (void)write(gPipe[1], &wake, 1);
    }
}
