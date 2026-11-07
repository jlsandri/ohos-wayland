// libohos_vkwsi.so: the Vulkan library Zink loads (ZINK_VULKAN_LIBRARY) in a
// program the direct child execs. It forwards everything to the system
// loader except the OHOS surface and swapchain. Swapchain images are ordinary
// images; presenting one copies it into a buffer of the window the host owns
// (remote_wsi_protocol.h), which the host then flushes to the compositor.

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>

#include <hilog/log.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <thread>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "runtime/remote_wsi_protocol.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_VkWsi"

namespace {
namespace wsi = ohoswayland::remote_wsi;

// linux/dma-buf.h: CPU access brackets for a mapped dma-buf.
struct DmaBufSync {
    uint64_t flags;
};
constexpr uint64_t kSyncWrite = 2;
constexpr uint64_t kSyncStart = 0;
constexpr uint64_t kSyncEnd = 4;
constexpr unsigned long kDmaBufIoctlSync = _IOW('b', 0, DmaBufSync);

void* Loader()
{
    static void* library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return library;
}

template <typename Fn> Fn Export(const char* name)
{
    return reinterpret_cast<Fn>(dlsym(Loader(), name));
}

PFN_vkGetInstanceProcAddr RealGipa()
{
    static const auto gipa = Export<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    return gipa;
}

PFN_vkGetDeviceProcAddr RealGdpa()
{
    static const auto gdpa = Export<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    return gdpa;
}

#define DEVICE_FN(device, name) reinterpret_cast<PFN_##name>(RealGdpa()(device, #name))

struct Device {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    // A queue of our own for signalling acquire semaphores, so we never
    // submit to a queue Zink is using under its own lock.
    VkQueue signalQueue = VK_NULL_HANDLE;
    std::mutex signalMutex;
    std::unordered_map<VkQueue, uint32_t> queueFamilies;
};

struct Surface {
    uint64_t surfaceKey;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // Host-visible copy of the image, read when it is presented.
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    const uint8_t* pixels = nullptr;
    bool coherent = true;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    // Signalled when the readback copy has finished.
    VkFence fence = VK_NULL_HANDLE;
    // Presented and not yet delivered to the host.
    bool busy = false;
};

// A window buffer the host lent us, mapped once.
struct Slot {
    uint8_t* data = nullptr;
    size_t size = 0;
    int fd = -1;
    uint32_t stride = 0;
};

struct Swapchain {
    VkDevice device;
    int socket = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Image> images;
    std::vector<Slot> slots;
    uint32_t next = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t poolFamily = UINT32_MAX;
    // Presents run in two halves: the app's thread records each image's
    // readback, and the worker waits for it and delivers the pixels, so the
    // app draws its next frame during the copy.
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<uint32_t> presented;
    bool stopping = false;
    std::atomic<bool> lost{false};
    uint64_t frames = 0;
    // Microseconds spent per stage since the last timing log.
    uint64_t readbackUs = 0, acquireUs = 0, copyUs = 0, flushUs = 0, frameStartUs = 0;
};

void DeliverLoop(Swapchain* swapchain);

uint64_t NowUs()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

std::mutex gMutex;
std::unordered_map<VkDevice, Device*> gDevices;

Device* DeviceFor(VkDevice device)
{
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gDevices.find(device);
    return it == gDevices.end() ? nullptr : it->second;
}

Surface* AsSurface(VkSurfaceKHR surface)
{
    return reinterpret_cast<Surface*>(surface);
}

Swapchain* AsSwapchain(VkSwapchainKHR swapchain)
{
    return reinterpret_cast<Swapchain*>(swapchain);
}

int Connect()
{
    const char* path = getenv(wsi::kSocketEnv);
    if (!path) return -1;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        OH_LOG_ERROR(LOG_APP, "VKWSI_CONNECT_FAILED %{public}s errno=%{public}d", path, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

bool Call(int socket, const wsi::Request& request, int32_t& result)
{
    return send(socket, &request, sizeof(request), MSG_NOSIGNAL) == sizeof(request) &&
           recv(socket, &result, sizeof(result), 0) == sizeof(result);
}

bool ReceiveAcquire(int socket, wsi::AcquireReply& reply, int& fd)
{
    iovec io = {&reply, sizeof(reply)};
    char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != sizeof(reply)) return false;
    fd = -1;
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
        std::memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return true;
}

// Signals what vkAcquireNextImageKHR would. Presenting finishes all work on
// an image before it returns, so every image is ready when acquired.
VkResult SignalAcquire(VkDevice device, VkSemaphore semaphore, VkFence fence)
{
    if (!semaphore && !fence) return VK_SUCCESS;
    Device* state = DeviceFor(device);
    if (!state || !state->signalQueue) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.signalSemaphoreCount = semaphore ? 1 : 0;
    submit.pSignalSemaphores = &semaphore;
    std::lock_guard<std::mutex> lock(state->signalMutex);
    return DEVICE_FN(device, vkQueueSubmit)(state->signalQueue, 1, &submit, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* device)
{
    // Ask for a second queue in Zink's graphics family for SignalAcquire.
    uint32_t familyCount = 0;
    const auto getFamilies = Export<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        "vkGetPhysicalDeviceQueueFamilyProperties");
    getFamilies(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    getFamilies(physical, &familyCount, families.data());
    std::vector<VkDeviceQueueCreateInfo> queues(info->pQueueCreateInfos,
                                                info->pQueueCreateInfos + info->queueCreateInfoCount);
    std::vector<float> priorities;
    int signalFamily = -1;
    uint32_t signalIndex = 0;
    for (auto& queue : queues) {
        if (queue.queueFamilyIndex < familyCount &&
            (families[queue.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            families[queue.queueFamilyIndex].queueCount > queue.queueCount && queue.flags == 0) {
            priorities.assign(queue.pQueuePriorities, queue.pQueuePriorities + queue.queueCount);
            priorities.push_back(priorities.empty() ? 1.0f : priorities.back());
            signalFamily = static_cast<int>(queue.queueFamilyIndex);
            signalIndex = queue.queueCount;
            queue.queueCount += 1;
            queue.pQueuePriorities = priorities.data();
            break;
        }
    }
    VkDeviceCreateInfo patched = *info;
    patched.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    patched.pQueueCreateInfos = queues.data();

    const auto create = Export<PFN_vkCreateDevice>("vkCreateDevice");
    const VkResult result = create(physical, &patched, allocator, device);
    if (result != VK_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "VKWSI_CREATE_DEVICE_FAILED result=%{public}d", result);
        return result;
    }
    auto* state = new Device;
    state->physical = physical;
    if (signalFamily >= 0) {
        DEVICE_FN(*device, vkGetDeviceQueue)(*device, static_cast<uint32_t>(signalFamily), signalIndex,
                                            &state->signalQueue);
    }
    OH_LOG_INFO(LOG_APP, "VKWSI_DEVICE signal_queue=%{public}d", state->signalQueue != VK_NULL_HANDLE);
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices[*device] = state;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto it = gDevices.find(device);
        if (it != gDevices.end()) {
            delete it->second;
            gDevices.erase(it);
        }
    }
    DEVICE_FN(device, vkDestroyDevice)(device, allocator);
}

// Records each queue's family so presents can record copies for it.
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    DEVICE_FN(device, vkGetDeviceQueue)(device, family, index, queue);
    if (Device* state = DeviceFor(device)) {
        std::lock_guard<std::mutex> lock(gMutex);
        state->queueFamilies[*queue] = family;
    }
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    DEVICE_FN(device, vkGetDeviceQueue2)(device, info, queue);
    if (Device* state = DeviceFor(device)) {
        std::lock_guard<std::mutex> lock(gMutex);
        state->queueFamilies[*queue] = info->queueFamilyIndex;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSurfaceOHOS(VkInstance instance, const VkSurfaceCreateInfoOHOS* info,
                                                 const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface)
{
    const auto* window = reinterpret_cast<const wsi::RemoteWindow*>(info->window);
    if (!window || window->magic != wsi::kRemoteWindowMagic) {
        // A real window: this process got one some other way.
        const auto create = reinterpret_cast<PFN_vkCreateSurfaceOHOS>(RealGipa()(instance, "vkCreateSurfaceOHOS"));
        return create ? create(instance, info, allocator, surface) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    *surface = reinterpret_cast<VkSurfaceKHR>(new Surface{window->surfaceKey});
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*)
{
    delete AsSurface(surface);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceSupport(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* supported)
{
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceCapabilities(VkPhysicalDevice, VkSurfaceKHR,
                                                      VkSurfaceCapabilitiesKHR* caps)
{
    *caps = {};
    caps->minImageCount = 2;
    caps->maxImageCount = 4;
    // Like Wayland, the swapchain's extent sets the surface size.
    caps->currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
    caps->minImageExtent = {1, 1};
    caps->maxImageExtent = {8192, 8192};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceFormats(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                 VkSurfaceFormatKHR* formats)
{
    // The window's buffers are RGBA.
    static const VkSurfaceFormatKHR kFormats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    if (!formats) {
        *count = 2;
        return VK_SUCCESS;
    }
    const uint32_t written = *count < 2 ? *count : 2;
    std::memcpy(formats, kFormats, written * sizeof(kFormats[0]));
    *count = written;
    return written < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSurfacePresentModes(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                      VkPresentModeKHR* modes)
{
    if (!modes) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1) return VK_INCOMPLETE;
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}

uint32_t MemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags want,
                    VkMemoryPropertyFlags need, VkMemoryPropertyFlags* found)
{
    VkPhysicalDeviceMemoryProperties properties;
    Export<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &properties);
    for (const VkMemoryPropertyFlags flags : {want | need, need}) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
                if (found) *found = properties.memoryTypes[i].propertyFlags;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

void Destroy(VkDevice device, Swapchain* swapchain)
{
    if (swapchain->worker.joinable()) {
        {
            std::lock_guard<std::mutex> lock(swapchain->mutex);
            swapchain->stopping = true;
        }
        swapchain->changed.notify_all();
        swapchain->worker.join();
    }
    for (auto& image : swapchain->images) {
        if (image.fence) DEVICE_FN(device, vkDestroyFence)(device, image.fence, nullptr);
        if (image.pixels) DEVICE_FN(device, vkUnmapMemory)(device, image.readbackMemory);
        DEVICE_FN(device, vkDestroyBuffer)(device, image.readback, nullptr);
        DEVICE_FN(device, vkFreeMemory)(device, image.readbackMemory, nullptr);
        DEVICE_FN(device, vkDestroyImage)(device, image.image, nullptr);
        DEVICE_FN(device, vkFreeMemory)(device, image.memory, nullptr);
    }
    for (auto& slot : swapchain->slots) {
        if (slot.data) munmap(slot.data, slot.size);
        if (slot.fd >= 0) close(slot.fd);
    }
    if (swapchain->pool) DEVICE_FN(device, vkDestroyCommandPool)(device, swapchain->pool, nullptr);
    if (swapchain->socket >= 0) close(swapchain->socket);
    delete swapchain;
}

VkResult CreateImage(VkDevice device, Device* state, const VkSwapchainCreateInfoKHR* info, Image& image)
{
    VkImageFormatListCreateInfo formats = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    for (auto* item = static_cast<const VkBaseInStructure*>(info->pNext); item; item = item->pNext) {
        if (item->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            formats = *reinterpret_cast<const VkImageFormatListCreateInfo*>(item);
    }
    formats.pNext = nullptr;
    VkImageCreateInfo create = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.pNext = formats.viewFormatCount ? &formats : nullptr;
    create.flags = (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = info->imageFormat;
    create.extent = {info->imageExtent.width, info->imageExtent.height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = DEVICE_FN(device, vkCreateImage)(device, &create, nullptr, &image.image);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    DEVICE_FN(device, vkGetImageMemoryRequirements)(device, image.image, &requirements);
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(state->physical, requirements.memoryTypeBits, 0,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
    result = DEVICE_FN(device, vkAllocateMemory)(device, &allocate, nullptr, &image.memory);
    if (result == VK_SUCCESS) result = DEVICE_FN(device, vkBindImageMemory)(device, image.image, image.memory, 0);
    if (result != VK_SUCCESS) return result;

    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = static_cast<VkDeviceSize>(info->imageExtent.width) * info->imageExtent.height * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = DEVICE_FN(device, vkCreateBuffer)(device, &buffer, nullptr, &image.readback);
    if (result != VK_SUCCESS) return result;
    DEVICE_FN(device, vkGetBufferMemoryRequirements)(device, image.readback, &requirements);
    VkMemoryPropertyFlags flags = 0;
    allocate.allocationSize = requirements.size;
    // Cached memory makes the CPU read of each frame fast.
    allocate.memoryTypeIndex = MemoryType(state->physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &flags);
    image.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    result = DEVICE_FN(device, vkAllocateMemory)(device, &allocate, nullptr, &image.readbackMemory);
    if (result == VK_SUCCESS)
        result = DEVICE_FN(device, vkBindBufferMemory)(device, image.readback, image.readbackMemory, 0);
    void* mapped = nullptr;
    if (result == VK_SUCCESS)
        result = DEVICE_FN(device, vkMapMemory)(device, image.readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    image.pixels = static_cast<const uint8_t*>(mapped);
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (result == VK_SUCCESS) result = DEVICE_FN(device, vkCreateFence)(device, &fence, nullptr, &image.fence);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                               const VkAllocationCallbacks*, VkSwapchainKHR* out)
{
    Device* state = DeviceFor(device);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    auto* swapchain = new Swapchain;
    swapchain->device = device;
    swapchain->width = info->imageExtent.width;
    swapchain->height = info->imageExtent.height;
    // One image being drawn, one being copied, and one spare.
    const uint32_t count = info->minImageCount < 3 ? 3 : (info->minImageCount > 4 ? 4 : info->minImageCount);
    swapchain->images.resize(count);
    VkResult result = VK_SUCCESS;
    for (auto& image : swapchain->images) {
        if (result == VK_SUCCESS) result = CreateImage(device, state, info, image);
    }
    if (result == VK_SUCCESS) {
        swapchain->socket = Connect();
        const wsi::Request request = {wsi::kCreate, 0, AsSurface(info->surface)->surfaceKey, swapchain->width,
                                      swapchain->height};
        int32_t reply = VK_ERROR_SURFACE_LOST_KHR;
        if (swapchain->socket < 0 || !Call(swapchain->socket, request, reply)) reply = VK_ERROR_SURFACE_LOST_KHR;
        result = static_cast<VkResult>(reply);
    }
    OH_LOG_INFO(LOG_APP, "VKWSI_SWAPCHAIN key=%{public}llu size=%{public}ux%{public}u format=%{public}d images=%{public}u result=%{public}d",
                static_cast<unsigned long long>(AsSurface(info->surface)->surfaceKey), swapchain->width,
                swapchain->height, info->imageFormat, count, result);
    if (result != VK_SUCCESS) {
        Destroy(device, swapchain);
        return result;
    }
    swapchain->worker = std::thread(DeliverLoop, swapchain);
    *out = reinterpret_cast<VkSwapchainKHR>(swapchain);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR handle, const VkAllocationCallbacks*)
{
    if (Swapchain* swapchain = AsSwapchain(handle)) Destroy(device, swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImages(VkDevice, VkSwapchainKHR handle, uint32_t* count, VkImage* images)
{
    Swapchain* swapchain = AsSwapchain(handle);
    const uint32_t total = static_cast<uint32_t>(swapchain->images.size());
    if (!images) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t written = *count < total ? *count : total;
    for (uint32_t i = 0; i < written; ++i) images[i] = swapchain->images[i].image;
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage(VkDevice device, VkSwapchainKHR handle, uint64_t timeout,
                                                VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
    Swapchain* swapchain = AsSwapchain(handle);
    {
        std::unique_lock<std::mutex> lock(swapchain->mutex);
        Image& image = swapchain->images[swapchain->next];
        if (image.busy && timeout == 0) return VK_NOT_READY;
        swapchain->changed.wait(lock, [&]() { return !image.busy || swapchain->lost; });
        if (swapchain->lost) return VK_ERROR_SURFACE_LOST_KHR;
        *index = swapchain->next;
        swapchain->next = (swapchain->next + 1) % swapchain->images.size();
    }
    return SignalAcquire(device, semaphore, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                 uint32_t* index)
{
    return AcquireNextImage(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

VkResult EnsureCommands(Swapchain* swapchain, uint32_t family)
{
    if (swapchain->poolFamily == family) return VK_SUCCESS;
    VkDevice device = swapchain->device;
    if (swapchain->pool) DEVICE_FN(device, vkDestroyCommandPool)(device, swapchain->pool, nullptr);
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family;
    VkResult result = DEVICE_FN(device, vkCreateCommandPool)(device, &pool, nullptr, &swapchain->pool);
    if (result != VK_SUCCESS) return result;
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = swapchain->pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    std::vector<VkCommandBuffer> buffers(swapchain->images.size());
    allocate.commandBufferCount = static_cast<uint32_t>(buffers.size());
    result = DEVICE_FN(device, vkAllocateCommandBuffers)(device, &allocate, buffers.data());
    if (result != VK_SUCCESS) return result;
    for (size_t i = 0; i < buffers.size(); ++i) swapchain->images[i].commands = buffers[i];
    swapchain->poolFamily = family;
    return VK_SUCCESS;
}

// Records and submits the copy of image into its readback buffer on queue,
// after the waits. Its fence signals when the pixels can be read.
VkResult Readback(VkQueue queue, Swapchain* swapchain, Image& image, uint32_t waitCount, const VkSemaphore* waits)
{
    VkDevice device = swapchain->device;
    Device* state = DeviceFor(device);
    uint32_t family = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto it = state->queueFamilies.find(queue);
        if (it != state->queueFamilies.end()) family = it->second;
    }
    VkResult result = EnsureCommands(swapchain, family);
    if (result != VK_SUCCESS) return result;
    VkCommandBuffer commands = image.commands;
    DEVICE_FN(device, vkResetCommandBuffer)(commands, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEVICE_FN(device, vkBeginCommandBuffer)(commands, &begin);
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    DEVICE_FN(device, vkCmdPipelineBarrier)(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {swapchain->width, swapchain->height, 1};
    DEVICE_FN(device, vkCmdCopyImageToBuffer)(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              image.readback, 1, &region);
    VkBufferMemoryBarrier host = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = image.readback;
    host.size = VK_WHOLE_SIZE;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    DEVICE_FN(device, vkCmdPipelineBarrier)(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                            nullptr, 1, &host, 1, &barrier);
    DEVICE_FN(device, vkEndCommandBuffer)(commands);

    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    return DEVICE_FN(device, vkQueueSubmit)(queue, 1, &submit, image.fence);
}

// Fills the next window buffer with image's pixels and hands it to the host.
VkResult Deliver(Swapchain* swapchain, const Image& image)
{
    wsi::Request request = {wsi::kAcquire, 0, 0, swapchain->width, swapchain->height};
    wsi::AcquireReply reply = {};
    int fd = -1;
    uint64_t start = NowUs();
    if (send(swapchain->socket, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request) ||
        !ReceiveAcquire(swapchain->socket, reply, fd)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (reply.result != VK_SUCCESS) return static_cast<VkResult>(reply.result);
    swapchain->acquireUs += NowUs() - start;
    start = NowUs();
    if (reply.slot >= swapchain->slots.size()) swapchain->slots.resize(reply.slot + 1);
    Slot& slot = swapchain->slots[reply.slot];
    if (fd >= 0) {
        if (slot.data) munmap(slot.data, slot.size);
        if (slot.fd >= 0) close(slot.fd);
        slot.fd = fd;
        slot.size = reply.size;
        slot.stride = reply.stride;
        void* data = mmap(nullptr, slot.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        slot.data = data == MAP_FAILED ? nullptr : static_cast<uint8_t*>(data);
        if (!slot.data) OH_LOG_ERROR(LOG_APP, "VKWSI_MAP_FAILED errno=%{public}d", errno);
    }
    if (slot.data) {
        DmaBufSync sync = {kSyncStart | kSyncWrite};
        ioctl(slot.fd, kDmaBufIoctlSync, &sync);
        const size_t row = static_cast<size_t>(swapchain->width) * 4;
        const uint32_t rows = reply.height < swapchain->height ? reply.height : swapchain->height;
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(slot.data + static_cast<size_t>(y) * slot.stride, image.pixels + y * row, row);
        sync.flags = kSyncEnd | kSyncWrite;
        ioctl(slot.fd, kDmaBufIoctlSync, &sync);
    }
    swapchain->copyUs += NowUs() - start;
    start = NowUs();
    request.op = wsi::kFlush;
    request.slot = reply.slot;
    int32_t result = VK_ERROR_SURFACE_LOST_KHR;
    if (!Call(swapchain->socket, request, result)) return VK_ERROR_SURFACE_LOST_KHR;
    swapchain->flushUs += NowUs() - start;
    return static_cast<VkResult>(result);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* info)
{
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        Swapchain* swapchain = AsSwapchain(info->pSwapchains[i]);
        const uint32_t index = info->pImageIndices[i];
        VkResult result = swapchain->lost ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
        if (result == VK_SUCCESS) {
            const uint64_t start = NowUs();
            result = Readback(queue, swapchain, swapchain->images[index], i == 0 ? info->waitSemaphoreCount : 0,
                              info->pWaitSemaphores);
            swapchain->readbackUs += NowUs() - start;
        }
        if (result == VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(swapchain->mutex);
            swapchain->images[index].busy = true;
            swapchain->presented.push_back(index);
        }
        swapchain->changed.notify_all();
        if (info->pResults) info->pResults[i] = result;
        if (result != VK_SUCCESS) overall = result;
    }
    return overall;
}

// Delivers presented images to the host in order (see Swapchain).
void DeliverLoop(Swapchain* swapchain)
{
    VkDevice device = swapchain->device;
    while (true) {
        uint32_t index = 0;
        {
            std::unique_lock<std::mutex> lock(swapchain->mutex);
            swapchain->changed.wait(lock, [&]() { return swapchain->stopping || !swapchain->presented.empty(); });
            if (swapchain->presented.empty()) return;
            index = swapchain->presented.front();
            swapchain->presented.pop_front();
        }
        Image& image = swapchain->images[index];
        VkResult result = DEVICE_FN(device, vkWaitForFences)(device, 1, &image.fence, VK_TRUE, UINT64_MAX);
        DEVICE_FN(device, vkResetFences)(device, 1, &image.fence);
        if (result == VK_SUCCESS && !image.coherent) {
            VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = image.readbackMemory;
            range.size = VK_WHOLE_SIZE;
            result = DEVICE_FN(device, vkInvalidateMappedMemoryRanges)(device, 1, &range);
        }
        if (result == VK_SUCCESS && !swapchain->lost) result = Deliver(swapchain, image);
        if (result == VK_ERROR_SURFACE_LOST_KHR) swapchain->lost = true;
        ++swapchain->frames;
        if (swapchain->frames == 1 || result != VK_SUCCESS) {
            OH_LOG_INFO(LOG_APP, "VKWSI_PRESENT frame=%{public}llu result=%{public}d",
                        static_cast<unsigned long long>(swapchain->frames), result);
        }
        constexpr uint64_t kTimingFrames = 240;
        if (swapchain->frames % kTimingFrames == 0) {
            const uint64_t now = NowUs();
            const uint64_t elapsed = swapchain->frameStartUs ? now - swapchain->frameStartUs : 0;
            OH_LOG_INFO(LOG_APP, "VKWSI_TIMING fps=%{public}.1f readback=%{public}.2fms acquire=%{public}.2fms copy=%{public}.2fms flush=%{public}.2fms",
                        elapsed ? kTimingFrames * 1e6 / elapsed : 0.0, swapchain->readbackUs / 1000.0 / kTimingFrames,
                        swapchain->acquireUs / 1000.0 / kTimingFrames, swapchain->copyUs / 1000.0 / kTimingFrames,
                        swapchain->flushUs / 1000.0 / kTimingFrames);
            swapchain->readbackUs = swapchain->acquireUs = swapchain->copyUs = swapchain->flushUs = 0;
            swapchain->frameStartUs = now;
        }
        {
            std::lock_guard<std::mutex> lock(swapchain->mutex);
            image.busy = false;
        }
        swapchain->changed.notify_all();
    }
}

PFN_vkVoidFunction Override(const char* name)
{
    struct Entry {
        const char* name;
        PFN_vkVoidFunction function;
    };
#define ENTRY(name, fn) {name, reinterpret_cast<PFN_vkVoidFunction>(fn)}
    static const Entry kOverrides[] = {
        ENTRY("vkCreateDevice", CreateDevice),
        ENTRY("vkDestroyDevice", DestroyDevice),
        ENTRY("vkGetDeviceQueue", GetDeviceQueue),
        ENTRY("vkGetDeviceQueue2", GetDeviceQueue2),
        ENTRY("vkCreateSurfaceOHOS", CreateSurfaceOHOS),
        ENTRY("vkDestroySurfaceKHR", DestroySurface),
        ENTRY("vkGetPhysicalDeviceSurfaceSupportKHR", GetSurfaceSupport),
        ENTRY("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", GetSurfaceCapabilities),
        ENTRY("vkGetPhysicalDeviceSurfaceFormatsKHR", GetSurfaceFormats),
        ENTRY("vkGetPhysicalDeviceSurfacePresentModesKHR", GetSurfacePresentModes),
        ENTRY("vkCreateSwapchainKHR", CreateSwapchain),
        ENTRY("vkDestroySwapchainKHR", DestroySwapchain),
        ENTRY("vkGetSwapchainImagesKHR", GetSwapchainImages),
        ENTRY("vkAcquireNextImageKHR", AcquireNextImage),
        ENTRY("vkAcquireNextImage2KHR", AcquireNextImage2),
        ENTRY("vkQueuePresentKHR", QueuePresent),
    };
#undef ENTRY
    for (const auto& entry : kOverrides) {
        if (std::strcmp(entry.name, name) == 0) return entry.function;
    }
    return nullptr;
}
} // namespace

extern "C" {
__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                                    const char* name);

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                                                      const char* name)
{
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (PFN_vkVoidFunction function = Override(name)) return function;
    return RealGipa()(instance, name);
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                                    const char* name)
{
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (PFN_vkVoidFunction function = Override(name)) return function;
    return RealGdpa()(device, name);
}
}
