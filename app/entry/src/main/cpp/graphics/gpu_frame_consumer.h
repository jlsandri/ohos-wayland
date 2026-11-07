#pragma once

#include "graphics/direct_scene.h"
#include "graphics/present_stats.h"
#include "compositor/frame/geometry.h"

#include <GLES3/gl3.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OH_NativeImage;
struct NativeWindow;
using OHNativeWindow = NativeWindow;

namespace ohoswayland {

// Presents the direct-GPU scene: each surface renders into its own
// NativeImage producer window, and the toplevel plus its popups are drawn as
// external OES textures into the XComponent.
class GpuFrameConsumer {
public:
    GpuFrameConsumer() = default;
    ~GpuFrameConsumer();

    GpuFrameConsumer(const GpuFrameConsumer&) = delete;
    GpuFrameConsumer& operator=(const GpuFrameConsumer&) = delete;

    bool Initialize();
    bool Render(int outputWidth, int outputHeight, int64_t framePeriodNs, const ContentInsets& insets);
    void Shutdown();
    // Logical size of the toplevel's window geometry; input maps through the same fit.
    int SourceWidth() const { return sourceWidth_; }
    int SourceHeight() const { return sourceHeight_; }
    // Writes the next composed frame to path (binary PPM).
    void RequestCapture(const std::string& path);
    bool CapturePending();
    // Writes the composed frame if a capture is pending; any path that draws may call it.
    void CaptureFrame(int outputWidth, int outputHeight);
    // Whether the last Render latched a new client frame for the toplevel,
    // and the toplevel buffer it drew.
    bool TopFrameLatched() const { return topLatched_; }
    const PresentedBuffer& TopBuffer() const { return topBuffer_; }

private:
    static constexpr float kNoExtend[4] = {0, 0, 1, 1};
    static constexpr float kNoEdge[2] = {0, 0};
    struct Layer {
        uint64_t key = 0;
        OH_NativeImage* image = nullptr;
        OHNativeWindow* producerWindow = nullptr;
        GLuint texture = 0;
        std::atomic<bool> frameAvailable{false};
        bool listenerSet = false;
        bool hasFrame = false;
        uint64_t lastAttachNs = 0;
        uint64_t frames = 0;
        float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    static void OnFrameAvailable(void* context);
    void SyncLayers(const std::vector<DirectLayer>& scene, int64_t framePeriodNs);
    bool Attach(Layer& layer, const DirectLayer& info, int64_t framePeriodNs);
    bool Update(Layer& layer);
    void Draw(const Layer& layer, int x, int y, int width, int height, int outputHeight, const float crop[4],
              const float extend[4] = kNoExtend, const float edge[2] = kNoEdge);
    void ReleaseLayer(Layer& layer);

    std::vector<std::unique_ptr<Layer>> layers_;
    GLuint program_ = 0;
    GLuint vertexBuffer_ = 0;
    GLint transformLocation_ = -1;
    GLint cropLocation_ = -1;
    GLint extendLocation_ = -1;
    GLint edgeLocation_ = -1;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    bool topLatched_ = false;
    PresentedBuffer topBuffer_;
    std::mutex captureMutex_;
    std::string capturePath_;
};

} // namespace ohoswayland
