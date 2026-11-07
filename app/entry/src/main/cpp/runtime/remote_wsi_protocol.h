#pragma once

// Wire format between libohos_vkwsi.so, which stands in for the Vulkan
// swapchain inside an exec'd client, and the direct child host that owns the
// client's OHNativeWindow (remote_wsi_host.cpp).
//
// An exec'd process has no IPC channel to the app, so it cannot receive an
// OHNativeWindow, and the Maleoon driver shares images between processes
// only as OH_NativeBuffers, which need IPC too. So the host requests buffers
// from the real window and passes their dma-buf fds to the client once; the
// client copies each presented image into one and the host flushes it. One
// SOCK_SEQPACKET connection carries one swapchain.

#include <cstdint>

namespace ohoswayland::remote_wsi {

// The host's socket path, set in the exec'd client's environment.
constexpr const char* kSocketEnv = "OHOS_WAYLAND_WSI_SOCKET";

// The title bar bridge exec_preload.cpp loads into GTK 4 programs.
constexpr const char* kTitlebarBridgeEnv = "OHOS_WAYLAND_TITLEBAR_BRIDGE";

// What OhosDirectWindowAcquire returns inside an exec'd client (exec_preload.cpp).
// Mesa passes it to Kopper as the VkSurfaceCreateInfoOHOS window, and
// libohos_vkwsi.so reads the surface key back from it.
constexpr uint32_t kRemoteWindowMagic = 0x4f575349; // "OWSI"

struct RemoteWindow {
    uint32_t magic;
    uint64_t surfaceKey;
};

enum Op : uint32_t {
    // Sizes the window for the swapchain. Reply: result.
    kCreate = 1,
    // Requests the next window buffer. Reply: AcquireReply, with the
    // buffer's fd as SCM_RIGHTS the first time a slot is used.
    kAcquire = 2,
    // Queues the acquired buffer to the compositor. Reply: result.
    kFlush = 3,
};

struct Request {
    uint32_t op;
    uint32_t slot;
    uint64_t surfaceKey;
    uint32_t width;
    uint32_t height;
};

struct AcquireReply {
    // A VkResult.
    int32_t result;
    uint32_t slot;
    // 1 when this reply carries the slot's fd.
    uint32_t hasFd;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint64_t size;
};

} // namespace ohoswayland::remote_wsi
