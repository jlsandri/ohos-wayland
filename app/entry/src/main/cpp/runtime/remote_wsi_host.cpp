// The direct child's half of the remote swapchain (remote_wsi_protocol.h).
// A program the host execs cannot hold an OHNativeWindow, so this file keeps
// the window the compositor attached for the program's surface, lends the
// program its buffers, and flushes each one the program filled.

#include "runtime/remote_wsi_host.h"

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "runtime/remote_wsi_protocol.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_RemoteWsi"

extern "C" OHNativeWindow* OhosDirectWindowAcquire(uint64_t key, int32_t timeoutMs);

namespace ohoswayland {
namespace {
namespace wsi = remote_wsi;

constexpr int32_t kSuccess = 0;
constexpr int32_t kOutOfDate = -1000001004;  // VK_ERROR_OUT_OF_DATE_KHR
constexpr int32_t kSurfaceLost = -1000000000; // VK_ERROR_SURFACE_LOST_KHR

void WaitFence(int fence)
{
    if (fence < 0) return;
    pollfd item = {fence, POLLIN, 0};
    poll(&item, 1, 1000);
    close(fence);
}

bool SendWithFd(int socket, const void* data, size_t size, int fd)
{
    iovec io = {const_cast<void*>(data), size};
    msghdr message = {};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))] = {};
    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
    }
    return sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
}

// One client swapchain: the window it draws into and the buffers it has seen.
class Connection {
public:
    explicit Connection(int socket) : socket_(socket) {}

    ~Connection()
    {
        // A client that dies between acquire and flush leaves a buffer out.
        if (pending_) OH_NativeWindow_NativeWindowAbortBuffer(window_, pending_);
        close(socket_);
    }

    void Run()
    {
        wsi::Request request;
        uint64_t frames = 0;
        while (recv(socket_, &request, sizeof(request), 0) == sizeof(request)) {
            if (request.op == wsi::kCreate) {
                const int32_t result = Create(request);
                OH_LOG_INFO(LOG_APP, "REMOTE_WSI_CREATE key=%{public}llu size=%{public}ux%{public}u result=%{public}d",
                            static_cast<unsigned long long>(request.surfaceKey), request.width, request.height, result);
                if (send(socket_, &result, sizeof(result), MSG_NOSIGNAL) != sizeof(result)) return;
            } else if (request.op == wsi::kAcquire) {
                int fd = -1;
                const wsi::AcquireReply reply = Acquire(fd);
                if (!SendWithFd(socket_, &reply, sizeof(reply), fd)) return;
            } else if (request.op == wsi::kFlush) {
                const int32_t result = Flush(request.slot);
                if (++frames == 1 || result != kSuccess) {
                    OH_LOG_INFO(LOG_APP, "REMOTE_WSI_PRESENT key=%{public}llu frame=%{public}llu result=%{public}d",
                                static_cast<unsigned long long>(key_), static_cast<unsigned long long>(frames), result);
                }
                if (send(socket_, &result, sizeof(result), MSG_NOSIGNAL) != sizeof(result)) return;
            } else {
                return;
            }
        }
    }

private:
    int32_t Create(const wsi::Request& request)
    {
        window_ = OhosDirectWindowAcquire(request.surfaceKey, 5000);
        if (!window_) return kSurfaceLost;
        key_ = request.surfaceKey;
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, static_cast<int32_t>(request.width),
                                              static_cast<int32_t>(request.height));
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_USAGE,
                                              static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ |
                                                                    NATIVEBUFFER_USAGE_CPU_WRITE |
                                                                    NATIVEBUFFER_USAGE_MEM_DMA |
                                                                    NATIVEBUFFER_USAGE_HW_TEXTURE));
        width_ = request.width;
        height_ = request.height;
        return kSuccess;
    }

    wsi::AcquireReply Acquire(int& fd)
    {
        wsi::AcquireReply reply = {};
        if (!window_ || pending_) {
            reply.result = kSurfaceLost;
            return reply;
        }
        OHNativeWindowBuffer* buffer = nullptr;
        int fence = -1;
        if (OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fence) != 0 || !buffer) {
            reply.result = kSurfaceLost;
            return reply;
        }
        WaitFence(fence);
        BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
        if (!handle || handle->width != static_cast<int32_t>(width_) || handle->height != static_cast<int32_t>(height_)) {
            // A buffer from before a resize; the client makes a new swapchain.
            OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
            reply.result = kOutOfDate;
            return reply;
        }
        auto [it, added] = slots_.emplace(handle, static_cast<uint32_t>(slots_.size()));
        pending_ = buffer;
        reply.result = kSuccess;
        reply.slot = it->second;
        reply.hasFd = added ? 1 : 0;
        reply.stride = static_cast<uint32_t>(handle->stride);
        reply.width = static_cast<uint32_t>(handle->width);
        reply.height = static_cast<uint32_t>(handle->height);
        reply.size = static_cast<uint64_t>(handle->size);
        if (added) fd = handle->fd;
        return reply;
    }

    int32_t Flush(uint32_t)
    {
        if (!pending_) return kSurfaceLost;
        Region region = {nullptr, 0};
        const int32_t result = OH_NativeWindow_NativeWindowFlushBuffer(window_, pending_, -1, region);
        pending_ = nullptr;
        return result == 0 ? kSuccess : kSurfaceLost;
    }

    int socket_;
    OHNativeWindow* window_ = nullptr;
    uint64_t key_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    OHNativeWindowBuffer* pending_ = nullptr;
    // Window buffers by handle; the client maps each slot once.
    std::map<const BufferHandle*, uint32_t> slots_;
};
} // namespace

bool StartRemoteWsiServer(const std::string& path)
{
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (listener < 0 || path.size() >= sizeof(address.sun_path)) return false;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    unlink(path.c_str());
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        OH_LOG_ERROR(LOG_APP, "REMOTE_WSI_LISTEN_FAILED %{public}s errno=%{public}d", path.c_str(), errno);
        close(listener);
        return false;
    }
    std::thread([listener]() {
        while (true) {
            const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            std::thread([client]() { Connection(client).Run(); }).detach();
        }
    }).detach();
    return true;
}

} // namespace ohoswayland
