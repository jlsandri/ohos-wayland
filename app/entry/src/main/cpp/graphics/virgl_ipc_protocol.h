#pragma once

#include <cstdint>

namespace winehua::virgl_ipc {

constexpr int32_t kProtocolVersion = 10;

enum RequestCode : uint32_t {
    kAttachSurfaceRequest = 2,
    kDetachSurfaceRequest = 3,
    kShutdownRequest = 4,
    kSetFramePeriodRequest = 6,
    // Direct-window host: carries an encoded launch record, replies result and pid.
    kLaunchRequest = 7,
};

enum SurfaceFlags : uint32_t {
    kSurfaceAttached = 1u << 0,
    kSurfaceVulkan = 1u << 1,
};

struct SurfaceInfo {
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
    uint32_t flags = 0;
};

} // namespace winehua::virgl_ipc
