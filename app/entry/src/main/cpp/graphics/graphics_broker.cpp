#include "graphics/graphics_broker.h"
#include "graphics/direct_scene.h"

#include "common/fs_utils.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>

#include "runtime/native_child_launcher.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GPU"

namespace ohoswayland {
namespace {
using winehua::virgl_ipc::kProtocolVersion;


int SendRequest(OHIPCRemoteProxy* proxy, uint32_t code, const OHIPCParcel* request,
                OHIPCParcel* reply)
{
    OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
    return OH_IPCRemoteProxy_SendRequest(proxy, code, request, reply, &option);
}

bool WriteVersion(OHIPCParcel* parcel)
{
    return parcel && OH_IPCParcel_WriteInt32(parcel, kProtocolVersion) == OH_IPC_SUCCESS;
}

// IPC-variant children never reach the NCP exit callback; the proxy's death
// notification is their exit signal. Cleanup runs off the IPC thread.
void OnClientDied(void* userData)
{
    const auto pid = static_cast<int32_t>(reinterpret_cast<intptr_t>(userData));
    OH_LOG_INFO(LOG_APP, "DIRECT_CLIENT_DIED pid=%{public}d", pid);
    std::thread([pid]() {
        GraphicsBroker::Instance().OnClientExit(pid);
        NativeChildLauncher::Instance().OnExit(pid);
    }).detach();
}

int32_t ParentPid(int32_t pid)
{
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* file = fopen(path, "r");
    if (!file) return -1;
    char line[512] = {};
    const bool read = fgets(line, sizeof(line), file) != nullptr;
    fclose(file);
    // The command name may contain spaces; the parent pid follows its ')'.
    const char* end = read ? strrchr(line, ')') : nullptr;
    int32_t parent = -1;
    char state = 0;
    return end && sscanf(end + 1, " %c %d", &state, &parent) == 2 ? parent : -1;
}

void SendShutdown(OHIPCRemoteProxy* proxy)
{
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    if (WriteVersion(request) && reply)
        SendRequest(proxy, winehua::virgl_ipc::kShutdownRequest, request, reply);
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
}
}

GraphicsBroker& GraphicsBroker::Instance()
{
    static GraphicsBroker broker;
    return broker;
}

void GraphicsBroker::OnHostStarted(int errorCode, OHIPCRemoteProxy* proxy)
{
    GraphicsBroker& broker = Instance();
    {
        std::lock_guard<std::mutex> lock(broker.mutex_);
        broker.callbackError_ = errorCode;
        if (errorCode == NCP_NO_ERROR && proxy) {
            broker.pendingProxy_ = proxy;
        } else if (proxy) {
            OH_IPCRemoteProxy_Destroy(proxy);
        }
        broker.callbackComplete_ = true;
    }
    broker.condition_.notify_all();
}

bool GraphicsBroker::Start(const std::string& runtimeDir)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.configured) return true;
    state_ = {};
    const std::string libraryDir = winehua::CurrentSharedObjectDir();
    if (libraryDir.empty() || !winehua::EnsureDir(runtimeDir)) {
        state_.error = "failed to prepare direct GPU runtime";
        return false;
    }
    state_.nativeLibraryDir = libraryDir;
    state_.configured = true;
    state_.socketReady = true;
    return true;
}

int32_t GraphicsBroker::LaunchDirect(const std::string& launchRecord, std::string& error)
{
    std::lock_guard<std::mutex> launchLock(launchMutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    callbackComplete_ = false;
    callbackError_ = 0;
    pendingProxy_ = nullptr;
    const int32_t createResult = OH_Ability_CreateNativeChildProcess(
        "libohos_direct_child.so", &GraphicsBroker::OnHostStarted);
    if (createResult != NCP_NO_ERROR) {
        error = "OH_Ability_CreateNativeChildProcess returned " + std::to_string(createResult);
        return -1;
    }
    if (!condition_.wait_for(lock, std::chrono::seconds(5), [this]() { return callbackComplete_; }) ||
        !pendingProxy_) {
        error = "direct host start failed callback=" + std::to_string(callbackError_);
        return -1;
    }
    OHIPCRemoteProxy* proxy = pendingProxy_;
    pendingProxy_ = nullptr;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int result = WriteVersion(request) ? OH_IPC_SUCCESS : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (result == OH_IPC_SUCCESS) result = OH_IPCParcel_WriteString(request, launchRecord.c_str());
    int32_t childResult = -1;
    int32_t pid = -1;
    if (result == OH_IPC_SUCCESS && reply) {
        result = SendRequest(proxy, winehua::virgl_ipc::kLaunchRequest, request, reply);
        if (result == OH_IPC_SUCCESS) result = OH_IPCParcel_ReadInt32(reply, &childResult);
        if (result == OH_IPC_SUCCESS && childResult == 0) result = OH_IPCParcel_ReadInt32(reply, &pid);
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH ipc=%{public}d child=%{public}d pid=%{public}d",
                result, childResult, pid);
    if (result != OH_IPC_SUCCESS || childResult != 0 || pid <= 0) {
        SendShutdown(proxy);
        OH_IPCRemoteProxy_Destroy(proxy);
        error = "direct host launch failed ipc=" + std::to_string(result) +
            " child=" + std::to_string(childResult);
        return -1;
    }
    clients_[pid] = proxy;
    OHIPCDeathRecipient* recipient = OH_IPCDeathRecipient_Create(
        &OnClientDied, nullptr, reinterpret_cast<void*>(static_cast<intptr_t>(pid)));
    if (recipient) OH_IPCRemoteProxy_AddDeathRecipient(proxy, recipient);
    return pid;
}

void GraphicsBroker::OnClientExit(int32_t pid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clients_.find(pid);
    if (it == clients_.end()) return;
    OH_IPCRemoteProxy_Destroy(it->second);
    clients_.erase(it);
    DirectScene::Instance().RemoveClient(static_cast<uint32_t>(pid));
    for (auto alias = aliases_.begin(); alias != aliases_.end();) {
        if (alias->second != pid) {
            ++alias;
            continue;
        }
        DirectScene::Instance().RemoveClient(static_cast<uint32_t>(alias->first));
        alias = aliases_.erase(alias);
    }
    OH_LOG_INFO(LOG_APP, "DIRECT_CLIENT_EXIT pid=%{public}d", pid);
}

void GraphicsBroker::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pid, proxy] : clients_) {
        SendShutdown(proxy);
        OH_IPCRemoteProxy_Destroy(proxy);
        DirectScene::Instance().RemoveClient(static_cast<uint32_t>(pid));
    }
    clients_.clear();
    aliases_.clear();
    state_.configured = false;
    state_.socketReady = false;
}

GraphicsHostState GraphicsBroker::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::vector<std::string> GraphicsBroker::GuestEnvironment() const
{
    const GraphicsHostState current = State();
    return {
        "GALLIUM_DRIVER=zink",
        "MESA_LOADER_DRIVER_OVERRIDE=zink",
        "TMPDIR=/data/storage/el2/base/cache",
        "LIBGL_DRIVERS_PATH=" + current.nativeLibraryDir,
        "LD_LIBRARY_PATH=" + current.nativeLibraryDir,
        "EGL_PLATFORM=wayland",
        // VK_OHOS_surface lives in the system loader's implicit swapchain layer.
        "ZINK_VULKAN_LIBRARY=libvulkan.so",
        "OHOS_WAYLAND_DIRECT_WINDOW=1",
        // Zink derives GL 4.0 on the Maleoon: GL 4.1+ needs 16384 textures
        // (the GPU has 8192), BPTC, and robust buffer access. Compute-based
        // clients (wgpu needs GLSL 430) work at 4.3; limits stay queryable.
        "MESA_GL_VERSION_OVERRIDE=4.3",
        "MESA_GLSL_VERSION_OVERRIDE=430",
        // The GPU lacks vertexPipelineStoresAndAtomics; Zink marks read-only
        // vertex storage buffers NonWritable, which Vulkan allows without it.
        "ZINK_READONLY_VERTEX_SSBO=1",
        // Dual-source blending renders wrong on the Maleoon: gpui's subpixel
        // text came out as hollow, colour-fringed outlines. Without it, apps
        // fall back to grayscale antialiasing, which renders correctly.
        "MESA_EXTENSION_OVERRIDE=-GL_ARB_blend_func_extended -GL_EXT_blend_func_extended",
    };
}

void GraphicsBroker::PublishPresentSurface(uint32_t clientPid, uint32_t surfaceId)
{
    DirectScene::Instance().SetToplevel((static_cast<uint64_t>(clientPid) << 32) | surfaceId);
    OH_LOG_INFO(LOG_APP, "GPU_PRESENT_HANDSHAKE pid=%{public}u surface=%{public}u", clientPid, surfaceId);
}

void GraphicsBroker::UpdatePresentSurfaceSize(uint64_t surfaceKey, uint32_t width, uint32_t height)
{
    DirectScene::Instance().SetConfiguredSize(surfaceKey, width, height);
}

bool GraphicsBroker::IsDirectSurface(uint64_t surfaceKey)
{
    return DirectScene::Instance().Contains(surfaceKey);
}

bool GraphicsBroker::HasClient(uint64_t surfaceKey) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ProxyForKeyLocked(surfaceKey) != nullptr;
}

OHIPCRemoteProxy* GraphicsBroker::ProxyForKeyLocked(uint64_t surfaceKey) const
{
    const auto pid = static_cast<int32_t>(surfaceKey >> 32);
    auto it = clients_.find(pid);
    if (it != clients_.end()) return it->second;
    const auto alias = aliases_.find(pid);
    if (alias != aliases_.end()) {
        it = clients_.find(alias->second);
        return it == clients_.end() ? nullptr : it->second;
    }
    // A program the host execs (and its own children) draws from its own pid;
    // the window still goes to the host it descends from.
    int32_t ancestor = pid;
    for (int depth = 0; depth < 8 && ancestor > 1; ++depth) {
        ancestor = ParentPid(ancestor);
        it = clients_.find(ancestor);
        if (it != clients_.end()) {
            aliases_[pid] = ancestor;
            OH_LOG_INFO(LOG_APP, "DIRECT_CLIENT_ALIAS pid=%{public}d host=%{public}d", pid, ancestor);
            return it->second;
        }
    }
    return nullptr;
}

bool GraphicsBroker::SendAttachLocked(OHIPCRemoteProxy* proxy, uint64_t surfaceKey,
                                      OHNativeWindow* window, uint64_t framePeriodNs)
{
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int result = WriteVersion(request) ? OH_IPC_SUCCESS : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (result == OH_IPC_SUCCESS)
        result = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (result == OH_IPC_SUCCESS)
        result = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(framePeriodNs));
    if (result == OH_IPC_SUCCESS)
        result = OH_IPCParcel_WriteInt32(request, winehua::virgl_ipc::kSurfaceVulkan);
    if (result == OH_IPC_SUCCESS) result = OH_NativeWindow_WriteToParcel(window, request);
    int32_t childResult = -1;
    if (result == OH_IPC_SUCCESS && reply) {
        result = SendRequest(proxy, winehua::virgl_ipc::kAttachSurfaceRequest, request, reply);
        if (result == OH_IPC_SUCCESS) result = OH_IPCParcel_ReadInt32(reply, &childResult);
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    OH_LOG_INFO(LOG_APP, "DIRECT_ATTACH key=%{public}llu ipc=%{public}d child=%{public}d",
                static_cast<unsigned long long>(surfaceKey), result, childResult);
    return result == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::Attach(uint64_t surfaceKey, OHNativeWindow* window, uint64_t framePeriodNs)
{
    if (!surfaceKey || !window || !framePeriodNs) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    OHIPCRemoteProxy* proxy = ProxyForKeyLocked(surfaceKey);
    return proxy && SendAttachLocked(proxy, surfaceKey, window, framePeriodNs);
}

bool GraphicsBroker::SetFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return surfaceKey && framePeriodNs && ProxyForKeyLocked(surfaceKey);
}

void GraphicsBroker::Detach(uint64_t surfaceKey)
{
    std::lock_guard<std::mutex> lock(mutex_);
    OHIPCRemoteProxy* proxy = ProxyForKeyLocked(surfaceKey);
    if (!proxy) return;
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    if (WriteVersion(request))
        OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (reply) SendRequest(proxy, winehua::virgl_ipc::kDetachSurfaceRequest, request, reply);
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
}

} // namespace ohoswayland
