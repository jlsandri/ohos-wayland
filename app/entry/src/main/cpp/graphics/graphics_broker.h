#pragma once

#include "graphics/virgl_ipc_protocol.h"

#include <native_window/external_window.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct OHIPCRemoteProxy;

namespace ohoswayland {

struct GraphicsHostState {
    bool configured = false;
    bool socketReady = false;
    std::string socketPath;
    std::string nativeLibraryDir;
    std::string error;
};

class GraphicsBroker {
public:
    static GraphicsBroker& Instance();

    bool Start(const std::string& runtimeDir);
    void Stop();
    GraphicsHostState State() const;
    std::vector<std::string> GuestEnvironment() const;
    static void PublishPresentSurface(uint32_t clientPid, uint32_t surfaceId);
    static void UpdatePresentSurfaceSize(uint64_t surfaceKey, uint32_t width, uint32_t height);
    static bool IsDirectSurface(uint64_t surfaceKey);

    // Starts libohos_direct_child.so and hands it the encoded launch record.
    // Returns the client pid, or -1 with error set.
    int32_t LaunchDirect(const std::string& launchRecord, std::string& error);
    void OnClientExit(int32_t pid);

    bool Attach(uint64_t surfaceKey, OHNativeWindow* window, uint64_t framePeriodNs);
    bool SetFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs);
    void Detach(uint64_t surfaceKey);
    // Only clients started through the direct host can receive a window.
    bool HasClient(uint64_t surfaceKey) const;

private:
    GraphicsBroker() = default;

    static void OnHostStarted(int errorCode, OHIPCRemoteProxy* proxy);
    OHIPCRemoteProxy* ProxyForKeyLocked(uint64_t surfaceKey) const;
    bool SendAttachLocked(OHIPCRemoteProxy* proxy, uint64_t surfaceKey, OHNativeWindow* window,
                          uint64_t framePeriodNs);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // One direct host launch is in flight at a time because the NCP callback
    // carries no context.
    std::mutex launchMutex_;
    OHIPCRemoteProxy* pendingProxy_ = nullptr;
    std::map<int32_t, OHIPCRemoteProxy*> clients_;
    // Descendant pid to the host pid in clients_ it was resolved to.
    mutable std::map<int32_t, int32_t> aliases_;
    GraphicsHostState state_;
    bool callbackComplete_ = false;
    int callbackError_ = 0;
};

} // namespace ohoswayland
