#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "protocols/xdg-shell-client-protocol.h"
#include "runtime/launch_params.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Demo"

namespace {
struct Demo {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    size_t size = 0;
    int width = 640;
    int height = 360;
    int pointerX = 320;
    int pointerY = 180;
    uint32_t color = 0x244b76;
    uint32_t lastKey = 0;
    bool configured = false;
    bool running = true;
    bool busy = false;
    bool dirty = false;
};

Demo gDemo;

void LogWayland(const char* format, va_list args)
{
    char message[2048];
    vsnprintf(message, sizeof(message), format, args);
    OH_LOG_ERROR(LOG_APP, "wayland: %{public}s", message);
}

void Paint(Demo& demo)
{
    if (!demo.pixels) return;
    if (demo.busy) {
        demo.dirty = true;
        return;
    }
    for (int y = 0; y < demo.height; ++y) {
        for (int x = 0; x < demo.width; ++x) {
            const uint32_t stripe = getenv("DEMO_THEME") ? 0 : ((x / 32) + (y / 32)) % 2 == 0 ? 0x080808 : 0;
            demo.pixels[y * demo.width + x] = demo.color + stripe;
        }
    }
    if (getenv("DEMO_THEME")) OH_LOG_INFO(LOG_APP, "DEMO_PAINT color=%06x", demo.color);
    for (int d = -12; d <= 12; ++d) {
        const int verticalY = demo.pointerY + d;
        const int horizontalX = demo.pointerX + d;
        if (verticalY >= 0 && verticalY < demo.height)
            demo.pixels[verticalY * demo.width + demo.pointerX] = 0xffffff;
        if (horizontalX >= 0 && horizontalX < demo.width)
            demo.pixels[demo.pointerY * demo.width + horizontalX] = 0xffffff;
    }
    wl_surface_attach(demo.surface, demo.buffer, 0, 0);
    wl_surface_damage(demo.surface, 0, 0, demo.width, demo.height);
    wl_surface_commit(demo.surface);
    demo.busy = true;
    demo.dirty = false;
}

void BufferRelease(void*, wl_buffer*)
{
    OH_LOG_INFO(LOG_APP, "buffer release begin");
    gDemo.busy = false;
    if (gDemo.dirty) Paint(gDemo);
    OH_LOG_INFO(LOG_APP, "buffer release end");
}
const wl_buffer_listener kBufferListener = {BufferRelease};

bool CreateBuffer(Demo& demo)
{
    if (demo.buffer) wl_buffer_destroy(demo.buffer);
    if (demo.pixels) munmap(demo.pixels, demo.size);
    demo.buffer = nullptr;
    demo.pixels = nullptr;
    demo.busy = false;
    demo.size = static_cast<size_t>(demo.width) * demo.height * 4;
    char name[64];
    snprintf(name, sizeof(name), "/ohos-wayland-demo-%d", getpid());
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "shm_open failed errno=%{public}d", errno);
        return false;
    }
    shm_unlink(name);
    if (ftruncate(fd, static_cast<off_t>(demo.size)) != 0) {
        close(fd);
        return false;
    }
    demo.pixels = static_cast<uint32_t*>(mmap(nullptr, demo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (demo.pixels == MAP_FAILED) {
        demo.pixels = nullptr;
        close(fd);
        return false;
    }
    wl_shm_pool* pool = wl_shm_create_pool(demo.shm, fd, static_cast<int32_t>(demo.size));
    demo.buffer = wl_shm_pool_create_buffer(pool, 0, demo.width, demo.height, demo.width * 4,
                                            WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!demo.buffer) return false;
    wl_buffer_add_listener(demo.buffer, &kBufferListener, nullptr);
    return true;
}

void XdgSurfaceConfigure(void*, xdg_surface* surface, uint32_t serial)
{
    OH_LOG_INFO(LOG_APP, "xdg configure begin serial=%{public}u", serial);
    xdg_surface_ack_configure(surface, serial);
    gDemo.configured = true;
    if (CreateBuffer(gDemo)) Paint(gDemo);
    OH_LOG_INFO(LOG_APP, "xdg configure end");
}
const xdg_surface_listener kXdgSurfaceListener = {XdgSurfaceConfigure};

void ToplevelConfigure(void*, xdg_toplevel*, int32_t width, int32_t height, wl_array*)
{
    OH_LOG_INFO(LOG_APP, "toplevel configure %{public}dx%{public}d", width, height);
    if (width > 0 && height > 0 && (width != gDemo.width || height != gDemo.height)) {
        gDemo.width = width;
        gDemo.height = height;
        gDemo.pointerX = width / 2;
        gDemo.pointerY = height / 2;
    }
}
void ToplevelClose(void*, xdg_toplevel*)
{
    gDemo.running = false;
}
const xdg_toplevel_listener kToplevelListener = {ToplevelConfigure, ToplevelClose};

void WmBasePing(void*, xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener kWmBaseListener = {WmBasePing};

void PointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y)
{
    OH_LOG_INFO(LOG_APP, "pointer enter");
    gDemo.pointerX = wl_fixed_to_int(x);
    gDemo.pointerY = wl_fixed_to_int(y);
}
void PointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void PointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    gDemo.pointerX = std::clamp(wl_fixed_to_int(x), 0, gDemo.width - 1);
    gDemo.pointerY = std::clamp(wl_fixed_to_int(y), 0, gDemo.height - 1);
    if (gDemo.configured) Paint(gDemo);
}
void PointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state)
{
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        gDemo.color = button == 0x110 ? 0x28754a : button == 0x111 ? 0x813f4f : 0x71622c;
        OH_LOG_INFO(LOG_APP, "DEMO_BUTTON x=%{public}d y=%{public}d", gDemo.pointerX, gDemo.pointerY);
        if (gDemo.configured) Paint(gDemo);
    }
}
void PointerAxis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value)
{
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        const int delta = wl_fixed_to_int(value);
        gDemo.color = static_cast<uint32_t>(static_cast<int32_t>(gDemo.color) + delta * 0x030201);
        if (gDemo.configured) Paint(gDemo);
    }
}
void PointerFrame(void*, wl_pointer*) {}
const wl_pointer_listener kPointerListener = {
    .enter = PointerEnter,
    .leave = PointerLeave,
    .motion = PointerMotion,
    .button = PointerButton,
    .axis = PointerAxis,
    .frame = PointerFrame,
};

void KeyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t size)
{
    OH_LOG_INFO(LOG_APP, "keyboard keymap size=%{public}u", size);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) munmap(map, size);
    close(fd);
}
void KeyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*)
{
    OH_LOG_INFO(LOG_APP, "keyboard enter");
}
void KeyboardKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        gDemo.lastKey = key;
        gDemo.color = 0x243e70 + (key % 8) * 0x050403;
        if (gDemo.configured) Paint(gDemo);
        OH_LOG_INFO(LOG_APP, "key pressed evdev=%{public}u", key);
    }
}
const wl_keyboard_listener kKeyboardListener = {
    KeyboardKeymap,
    KeyboardEnter,
    [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
    KeyboardKey,
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_keyboard*, int32_t, int32_t) {},
};

void SeatCapabilities(void*, wl_seat* seat, uint32_t capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !gDemo.pointer) {
        gDemo.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(gDemo.pointer, &kPointerListener, nullptr);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !gDemo.keyboard) {
        gDemo.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(gDemo.keyboard, &kKeyboardListener, nullptr);
    }
}
const wl_seat_listener kSeatListener = {SeatCapabilities, [](void*, wl_seat*, const char*) {}};

void RegistryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        gDemo.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        gDemo.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        gDemo.wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(gDemo.wmBase, &kWmBaseListener, nullptr);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        gDemo.seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(gDemo.seat, &kSeatListener, nullptr);
    }
}
const wl_registry_listener kRegistryListener = {
    RegistryGlobal, [](void*, wl_registry*, uint32_t) {},
};

int RunDemo()
{
    gDemo = Demo{};
    Demo& demo = gDemo;
    demo.display = wl_display_connect(nullptr);
    if (!demo.display) {
        OH_LOG_ERROR(LOG_APP, "wl_display_connect failed");
        return 1;
    }
    demo.registry = wl_display_get_registry(demo.display);
    wl_registry_add_listener(demo.registry, &kRegistryListener, nullptr);
    if (wl_display_roundtrip(demo.display) < 0 || !demo.compositor || !demo.shm || !demo.wmBase) return 2;
    demo.surface = wl_compositor_create_surface(demo.compositor);
    demo.xdgSurface = xdg_wm_base_get_xdg_surface(demo.wmBase, demo.surface);
    xdg_surface_add_listener(demo.xdgSurface, &kXdgSurfaceListener, nullptr);
    demo.toplevel = xdg_surface_get_toplevel(demo.xdgSurface);
    xdg_toplevel_add_listener(demo.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(demo.toplevel, "Native Wayland wl_shm diagnostic");
    xdg_toplevel_set_app_id(demo.toplevel, "com.exploration.ohoswayland.demo");
    wl_surface_commit(demo.surface);
    while (demo.running) {
        OH_LOG_INFO(LOG_APP, "dispatch enter");
        const int dispatchResult = wl_display_dispatch(demo.display);
        OH_LOG_INFO(LOG_APP, "dispatch exit result=%{public}d errno=%{public}d", dispatchResult, errno);
        if (dispatchResult < 0) break;
    }
    OH_LOG_INFO(LOG_APP, "cleanup begin");
    if (demo.buffer) wl_buffer_destroy(demo.buffer);
    if (demo.pixels) munmap(demo.pixels, demo.size);
    if (demo.pointer) wl_pointer_destroy(demo.pointer);
    if (demo.keyboard) wl_keyboard_destroy(demo.keyboard);
    if (demo.seat) wl_seat_destroy(demo.seat);
    xdg_toplevel_destroy(demo.toplevel);
    xdg_surface_destroy(demo.xdgSurface);
    wl_surface_destroy(demo.surface);
    xdg_wm_base_destroy(demo.wmBase);
    wl_shm_destroy(demo.shm);
    wl_compositor_destroy(demo.compositor);
    wl_registry_destroy(demo.registry);
    wl_display_disconnect(demo.display);
    return 0;
}
}

extern "C" __attribute__((visibility("default")))
void OhosWaylandDemoMain(NativeChildProcess_Args args)
{
    wl_log_set_handler_client(LogWayland);
    if (!ohoswayland::ApplyLaunchParams(args.entryParams)) {
        OH_LOG_ERROR(LOG_APP, "invalid launch parameters");
        return;
    }
    const int result = RunDemo();
    OH_LOG_INFO(LOG_APP, "demo exited result=%{public}d", result);
}
