#include "data_device.h"
#include "protocols/wayland-server-protocol.h"
#include "compositor/wayland_server.h"

#include <accesstoken/ability_access_control.h>
#include <database/pasteboard/oh_pasteboard.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>
#include <hilog/log.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_DataDevice"

namespace {

struct DataSource {
    // Null for the HarmonyOS pasteboard, which the compositor offers itself.
    wl_resource* resource;
    std::vector<std::string> mimeTypes;
};

struct DataOffer {
    wl_resource* resource;
    DataSource* source;
};

// A client's selection being read so it can be copied to the pasteboard.
struct Capture {
    DataSource* source;
    int fd;
    wl_event_source* event;
    std::string text;
};

const char* const kReadPermission = "ohos.permission.READ_PASTEBOARD";
const char* const kPasteboardText = "text/plain";
constexpr size_t kMaxText = 16 * 1024 * 1024;

const std::vector<std::string> kTextMimes = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT",
};

std::vector<wl_resource*> devices;
std::vector<DataSource*> sources;
std::vector<DataOffer*> offers;
std::vector<Capture*> captures;
DataSource systemSource{nullptr, kTextMimes};
DataSource* selection = nullptr;

wl_display* gDisplay = nullptr;
OH_Pasteboard* gPasteboard = nullptr;
int gChangeFd = -1;
// The pasteboard's change count after our own write, whose notification
// must not replace the client selection it came from.
uint32_t gOwnChange = 0;

// A drag from HarmonyOS, offered to the app it is over as a drag the
// compositor owns. Its data arrives with the drop.
struct SystemDrag {
    DataSource source{nullptr, {}};
    std::vector<std::pair<std::string, std::string>> payloads;
    wl_resource* surface = nullptr;
    wl_listener surfaceDestroyed{};
};
SystemDrag gDrag;
std::mutex gDragMutex;
std::vector<dnd::Event> gDragEvents;
int gDragFd = -1;

std::mutex gPermissionMutex;
std::function<void()> gAskPermission;
std::vector<int> gPendingReads;
bool gAsking = false;

bool IsText(const std::string& mimeType)
{
    return std::find(kTextMimes.begin(), kTextMimes.end(), mimeType) != kTextMimes.end();
}

template<typename T>
void Remove(std::vector<T*>& items, T* item)
{
    items.erase(std::remove(items.begin(), items.end(), item), items.end());
}

void RemoveResource(std::vector<wl_resource*>& items, wl_resource* resource)
{
    items.erase(std::remove(items.begin(), items.end(), resource), items.end());
}

std::string ReadPasteboardText()
{
    OH_Pasteboard* pasteboard = OH_Pasteboard_Create();
    int status = 0;
    OH_UdmfData* data = OH_Pasteboard_GetData(pasteboard, &status);
    std::string text;
    if (data) {
        unsigned int count = 0;
        OH_UdmfRecord** records = OH_UdmfData_GetRecords(data, &count);
        for (unsigned int i = 0; i < count && text.empty(); ++i) {
            OH_UdsPlainText* plain = OH_UdsPlainText_Create();
            if (OH_UdmfRecord_GetPlainText(records[i], plain) == 0) {
                if (const char* content = OH_UdsPlainText_GetContent(plain)) text = content;
            }
            OH_UdsPlainText_Destroy(plain);
        }
        OH_UdmfData_Destroy(data);
    }
    OH_Pasteboard_Destroy(pasteboard);
    OH_LOG_INFO(LOG_APP, "[Clipboard] read pasteboard status=%{public}d bytes=%{public}zu", status, text.size());
    return text;
}

// Blocks until the client drains the pipe, so it runs off the Wayland thread.
void WriteAndClose(int fd, const std::string& text)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    close(fd);
}

// The pasteboard read is an IPC, also kept off the Wayland thread.
void ServeReads(std::vector<int> fds)
{
    const std::string text = ReadPasteboardText();
    for (int fd : fds) WriteAndClose(fd, text);
}

void ServePasteboard(int fd)
{
    if (OH_AT_CheckSelfPermission(kReadPermission)) {
        std::thread(ServeReads, std::vector<int>{fd}).detach();
        return;
    }
    std::function<void()> ask;
    {
        std::lock_guard<std::mutex> lock(gPermissionMutex);
        if (!gAskPermission) {
            close(fd);
            return;
        }
        gPendingReads.push_back(fd);
        if (!gAsking) {
            gAsking = true;
            ask = gAskPermission;
        }
    }
    if (ask) ask();
}

void WritePasteboardText(const std::string& text)
{
    OH_UdmfData* data = OH_UdmfData_Create();
    OH_UdmfRecord* record = OH_UdmfRecord_Create();
    OH_UdsPlainText* plain = OH_UdsPlainText_Create();
    OH_UdsPlainText_SetContent(plain, text.c_str());
    OH_UdmfRecord_AddPlainText(record, plain);
    OH_UdmfData_AddRecord(data, record);
    const int result = OH_Pasteboard_SetData(gPasteboard, data);
    gOwnChange = OH_Pasteboard_GetChangeCount(gPasteboard);
    OH_UdsPlainText_Destroy(plain);
    OH_UdmfRecord_Destroy(record);
    OH_UdmfData_Destroy(data);
    OH_LOG_INFO(LOG_APP, "[Clipboard] wrote pasteboard result=%{public}d bytes=%{public}zu", result, text.size());
}

void EndCapture(Capture* capture)
{
    wl_event_source_remove(capture->event);
    close(capture->fd);
    Remove(captures, capture);
    delete capture;
}

int OnCaptureReadable(int fd, uint32_t, void* data)
{
    auto* capture = static_cast<Capture*>(data);
    char buffer[65536];
    while (true) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0 && capture->text.size() + static_cast<size_t>(n) <= kMaxText) {
            capture->text.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        // A newer selection replaced this one while it was being read.
        if (n == 0 && selection == capture->source) WritePasteboardText(capture->text);
        EndCapture(capture);
        return 0;
    }
}

void CaptureToPasteboard(DataSource* source)
{
    auto mimeType = std::find_if(source->mimeTypes.begin(), source->mimeTypes.end(), IsText);
    if (mimeType == source->mimeTypes.end()) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return;
    wl_data_source_send_send(source->resource, mimeType->c_str(), fds[1]);
    close(fds[1]);
    auto* capture = new Capture{source, fds[0], nullptr, {}};
    capture->event = wl_event_loop_add_fd(wl_display_get_event_loop(gDisplay), fds[0],
                                          WL_EVENT_READABLE, OnCaptureReadable, capture);
    captures.push_back(capture);
}

void DestroyOffer(wl_resource* resource)
{
    auto* offer = static_cast<DataOffer*>(wl_resource_get_user_data(resource));
    Remove(offers, offer);
    delete offer;
}

void OfferAccept(wl_client*, wl_resource*, uint32_t, const char*)
{
}

void OfferReceive(wl_client*, wl_resource* resource, const char* mimeType, int32_t fd)
{
    auto* offer = static_cast<DataOffer*>(wl_resource_get_user_data(resource));
    if (offer->source && mimeType &&
        std::find(offer->source->mimeTypes.begin(), offer->source->mimeTypes.end(), mimeType) !=
            offer->source->mimeTypes.end()) {
        if (offer->source == &systemSource) {
            ServePasteboard(fd);
            return;
        }
        if (offer->source == &gDrag.source) {
            for (const auto& payload : gDrag.payloads) {
                if (payload.first != mimeType) continue;
                std::thread(WriteAndClose, fd, payload.second).detach();
                return;
            }
            close(fd);
            return;
        }
        wl_data_source_send_send(offer->source->resource, mimeType, fd);
    }
    close(fd);
}

void OfferDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void OfferFinish(wl_client*, wl_resource*)
{
}

// A drag from HarmonyOS is always a copy.
void OfferSetActions(wl_client*, wl_resource* resource, uint32_t actions, uint32_t)
{
    auto* offer = static_cast<DataOffer*>(wl_resource_get_user_data(resource));
    if (!offer->source || offer->source != &gDrag.source) return;
    wl_data_offer_send_action(resource, (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
        ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}

const struct wl_data_offer_interface offerInterface = {
    OfferAccept,
    OfferReceive,
    OfferDestroy,
    OfferFinish,
    OfferSetActions,
};

void SendSelection(wl_resource* device)
{
    if (!selection) {
        wl_data_device_send_selection(device, nullptr);
        return;
    }
    wl_resource* offerResource = wl_resource_create(
        wl_resource_get_client(device), &wl_data_offer_interface,
        wl_resource_get_version(device), 0);
    if (!offerResource) return;
    auto* offer = new DataOffer{offerResource, selection};
    offers.push_back(offer);
    wl_resource_set_implementation(offerResource, &offerInterface, offer, DestroyOffer);
    wl_data_device_send_data_offer(device, offerResource);
    for (const auto& mimeType : selection->mimeTypes) {
        wl_data_offer_send_offer(offerResource, mimeType.c_str());
    }
    wl_data_device_send_selection(device, offerResource);
}

void SetSelection(DataSource* next)
{
    if (selection && selection != next && selection->resource) {
        wl_data_source_send_cancelled(selection->resource);
    }
    selection = next;
    for (wl_resource* device : devices) SendSelection(device);
}

uint32_t NowMs()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

std::vector<wl_resource*> DragDevices()
{
    std::vector<wl_resource*> found;
    if (!gDrag.surface) return found;
    wl_client* client = wl_resource_get_client(gDrag.surface);
    for (wl_resource* device : devices) {
        if (wl_resource_get_client(device) == client) found.push_back(device);
    }
    return found;
}

void ForgetDragSurface()
{
    if (!gDrag.surface) return;
    wl_list_remove(&gDrag.surfaceDestroyed.link);
    gDrag.surface = nullptr;
}

void OnDragSurfaceDestroyed(wl_listener*, void*)
{
    wl_list_init(&gDrag.surfaceDestroyed.link);
    gDrag.surface = nullptr;
}

void LeaveDrag()
{
    for (wl_resource* device : DragDevices()) wl_data_device_send_leave(device);
    for (DataOffer* offer : offers) {
        if (offer->source == &gDrag.source) offer->source = nullptr;
    }
    ForgetDragSurface();
}

void EnterDrag(const dnd::Event& event)
{
    LeaveDrag();
    wl_resource* surface = WaylandServer::GetInstance()->GetToplevelManager().GetSurfaceForToplevel(event.toplevel);
    if (!surface) return;
    gDrag.source.mimeTypes = event.mimeTypes;
    gDrag.payloads.clear();
    gDrag.surface = surface;
    gDrag.surfaceDestroyed.notify = OnDragSurfaceDestroyed;
    wl_resource_add_destroy_listener(surface, &gDrag.surfaceDestroyed);
    const uint32_t serial = wl_display_next_serial(gDisplay);
    for (wl_resource* device : DragDevices()) {
        const int version = wl_resource_get_version(device);
        wl_resource* offerResource = wl_resource_create(wl_resource_get_client(device),
                                                        &wl_data_offer_interface, version, 0);
        if (!offerResource) continue;
        auto* offer = new DataOffer{offerResource, &gDrag.source};
        offers.push_back(offer);
        wl_resource_set_implementation(offerResource, &offerInterface, offer, DestroyOffer);
        wl_data_device_send_data_offer(device, offerResource);
        for (const auto& mimeType : event.mimeTypes) wl_data_offer_send_offer(offerResource, mimeType.c_str());
        if (version >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
            wl_data_offer_send_source_actions(offerResource, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
        wl_data_device_send_enter(device, serial, surface, event.x, event.y, offerResource);
    }
    OH_LOG_INFO(LOG_APP, "[DnD] enter tl=%{public}u types=%{public}zu", event.toplevel, event.mimeTypes.size());
}

void DeliverDrag(const dnd::Event& event)
{
    switch (event.kind) {
        case dnd::Kind::Enter:
            EnterDrag(event);
            break;
        case dnd::Kind::Motion:
            for (wl_resource* device : DragDevices()) wl_data_device_send_motion(device, NowMs(), event.x, event.y);
            break;
        case dnd::Kind::Leave:
            LeaveDrag();
            break;
        case dnd::Kind::Drop:
            // No leave follows: GTK drops its drop object on leave while it
            // may still be reading. The next enter replaces this drag.
            gDrag.payloads = event.payloads;
            for (wl_resource* device : DragDevices()) wl_data_device_send_drop(device);
            OH_LOG_INFO(LOG_APP, "[DnD] drop payloads=%{public}zu", event.payloads.size());
            ForgetDragSurface();
            break;
    }
}

int OnDragEvents(int fd, uint32_t, void*)
{
    uint64_t count = 0;
    while (read(fd, &count, sizeof(count)) > 0) {}
    std::vector<dnd::Event> events;
    {
        std::lock_guard<std::mutex> lock(gDragMutex);
        events.swap(gDragEvents);
    }
    for (const auto& event : events) DeliverDrag(event);
    return 0;
}

DataSource* PasteboardSelection()
{
    return OH_Pasteboard_HasType(gPasteboard, kPasteboardText) ? &systemSource : nullptr;
}

void SourceOffer(wl_client*, wl_resource* resource, const char* mimeType)
{
    if (!mimeType) return;
    auto* source = static_cast<DataSource*>(wl_resource_get_user_data(resource));
    if (std::find(source->mimeTypes.begin(), source->mimeTypes.end(), mimeType) == source->mimeTypes.end()) {
        source->mimeTypes.emplace_back(mimeType);
    }
}

void SourceDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void SourceSetActions(wl_client*, wl_resource*, uint32_t)
{
}

const struct wl_data_source_interface sourceInterface = {
    SourceOffer,
    SourceDestroy,
    SourceSetActions,
};

void DestroySource(wl_resource* resource)
{
    auto* source = static_cast<DataSource*>(wl_resource_get_user_data(resource));
    for (Capture* capture : std::vector<Capture*>(captures)) {
        if (capture->source == source) EndCapture(capture);
    }
    for (DataOffer* offer : offers) {
        if (offer->source == source) offer->source = nullptr;
    }
    Remove(sources, source);
    // The copied text lives on in the pasteboard after its app is gone.
    if (selection == source) {
        selection = nullptr;
        SetSelection(PasteboardSelection());
    }
    delete source;
}

void DeviceStartDrag(wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*, uint32_t)
{
}

void DeviceSetSelection(wl_client*, wl_resource*, wl_resource* sourceResource, uint32_t)
{
    auto* source = sourceResource
        ? static_cast<DataSource*>(wl_resource_get_user_data(sourceResource))
        : nullptr;
    SetSelection(source);
    if (source) CaptureToPasteboard(source);
}

void DeviceRelease(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct wl_data_device_interface deviceInterface = {
    DeviceStartDrag,
    DeviceSetSelection,
    DeviceRelease,
};

void DestroyDevice(wl_resource* resource)
{
    RemoveResource(devices, resource);
}

void ManagerCreateSource(wl_client* client, wl_resource* resource, uint32_t id)
{
    wl_resource* sourceResource = wl_resource_create(
        client, &wl_data_source_interface, wl_resource_get_version(resource), id);
    if (!sourceResource) return;
    auto* source = new DataSource{sourceResource, {}};
    sources.push_back(source);
    wl_resource_set_implementation(sourceResource, &sourceInterface, source, DestroySource);
}

void ManagerGetDevice(wl_client* client, wl_resource* resource, uint32_t id, wl_resource*)
{
    wl_resource* device = wl_resource_create(
        client, &wl_data_device_interface, wl_resource_get_version(resource), id);
    if (!device) return;
    devices.push_back(device);
    wl_resource_set_implementation(device, &deviceInterface, nullptr, DestroyDevice);
    SendSelection(device);
}

const struct wl_data_device_manager_interface managerInterface = {
    ManagerCreateSource,
    ManagerGetDevice,
};

void ManagerBind(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    if (!resource) return;
    wl_resource_set_implementation(resource, &managerInterface, nullptr, nullptr);
    OH_LOG_INFO(LOG_APP, "data device manager bound version=%{public}u", version);
}

// Another app copied something: offer it, unless the change is our own write.
int OnPasteboardChanged(int fd, uint32_t, void*)
{
    uint64_t count = 0;
    while (read(fd, &count, sizeof(count)) > 0) {}
    if (OH_Pasteboard_GetChangeCount(gPasteboard) == gOwnChange) return 0;
    SetSelection(PasteboardSelection());
    return 0;
}

void NotifyPasteboardChanged(void*, Pasteboard_NotifyType)
{
    const uint64_t one = 1;
    write(gChangeFd, &one, sizeof(one));
}

} // namespace

void RegisterDataDeviceManager(wl_display* display)
{
    gDisplay = display;
    if (!gPasteboard) {
        gPasteboard = OH_Pasteboard_Create();
        gChangeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        OH_PasteboardObserver* observer = OH_PasteboardObserver_Create();
        OH_PasteboardObserver_SetData(observer, nullptr, NotifyPasteboardChanged, nullptr);
        OH_Pasteboard_Subscribe(gPasteboard, NOTIFY_LOCAL_DATA_CHANGE, observer);
        OH_Pasteboard_Subscribe(gPasteboard, NOTIFY_REMOTE_DATA_CHANGE, observer);
    }
    wl_event_loop_add_fd(wl_display_get_event_loop(display), gChangeFd, WL_EVENT_READABLE,
                         OnPasteboardChanged, nullptr);
    if (gDragFd < 0) gDragFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    wl_event_loop_add_fd(wl_display_get_event_loop(display), gDragFd, WL_EVENT_READABLE, OnDragEvents, nullptr);
    gDrag.surface = nullptr;
    selection = PasteboardSelection();
    wl_global_create(display, &wl_data_device_manager_interface, 3, nullptr, ManagerBind);
    OH_LOG_INFO(LOG_APP, "data device manager global registered");
}

namespace clipboard {

void SetPermissionRequester(std::function<void()> ask)
{
    std::lock_guard<std::mutex> lock(gPermissionMutex);
    gAskPermission = std::move(ask);
}

void OnPermissionResult(bool granted)
{
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(gPermissionMutex);
        fds.swap(gPendingReads);
        gAsking = false;
    }
    OH_LOG_INFO(LOG_APP, "[Clipboard] read permission granted=%{public}d waiting=%{public}zu", granted, fds.size());
    if (granted) {
        std::thread(ServeReads, std::move(fds)).detach();
        return;
    }
    for (int fd : fds) close(fd);
}

} // namespace clipboard

namespace dnd {

void Post(Event event)
{
    {
        std::lock_guard<std::mutex> lock(gDragMutex);
        gDragEvents.push_back(std::move(event));
    }
    const uint64_t one = 1;
    write(gDragFd, &one, sizeof(one));
}

} // namespace dnd
