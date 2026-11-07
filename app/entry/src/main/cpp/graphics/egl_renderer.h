#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <native_window/external_window.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "compositor/frame/direct_pass_policy.h"
#include "compositor/frame/geometry.h"
#include "graphics/gpu_frame_consumer.h"
#include "runtime/graphics_capabilities.h"
#include "runtime/ohos_frame.h"

class DesktopCompositor;

class EglRenderer : public winehua::DirectPassPolicy {
public:
    explicit EglRenderer(DesktopCompositor& compositor);

    static EGLDisplay GetSharedDisplay();
    static ohoswayland::GraphicsCapabilities ProbeCapabilities();

    bool Init(OHNativeWindow* window, int w, int h);
    void Shutdown();
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    uint32_t GetToplevelId() const { return toplevelId_; }
    // The XComponent size. Input maps view pixels through frame_, which the
    // direct-GPU path does not otherwise refresh (its EGL size query can
    // report 1x1), so a hidden title bar left input on the old size.
    void SetSize(int w, int h)
    {
        expectW_ = w;
        expectH_ = h;
        frame_.width = w;
        frame_.height = h;
    }
    void SetStretchFill(bool on) { stretchFill_.store(on); }
    void SetContentInsets(const ContentInsets& insets)
    {
        std::lock_guard<std::mutex> lock(insetsMutex_);
        insets_ = insets;
    }
    ContentInsets GetContentInsets() const
    {
        std::lock_guard<std::mutex> lock(insetsMutex_);
        return insets_;
    }
    void SetGpuDemoEnabled(bool enabled) { gpuDemoEnabled_.store(enabled); }
    bool IsValid() const { return running_; }
    int GetWidth() const { return frame_.width; }
    int GetHeight() const { return frame_.height; }
    int GetFrameWidth() const { return frameW_; }
    int GetFrameHeight() const { return frameH_; }
    FitRect GetInputLetterbox() const;
    uint32_t DirectPassCapabilities() const override;

private:
    void RenderLoop();
    static void OnVSync(long long timestamp, void* data);
    FitRect ComputeFrameDisplayRect(int drawW, int drawH) const;

    ohoswayland::OhosFrame frame_;
    int expectW_ = 0;
    int expectH_ = 0;
    int lastDrawW_ = 0;
    int lastDrawH_ = 0;
    int frameW_ = 0;
    int frameH_ = 0;
    int contentW_ = 0;
    int contentH_ = 0;
    int texW_ = 0;
    int texH_ = 0;
    bool frameArgb_ = false;
    FitRect letterbox_;
    std::atomic<bool> stretchFill_{false};

    GLuint texture_ = 0;
    GLuint program_ = 0;
    GLuint vbo_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
    uint64_t vsyncSequence_ = 0;
    std::atomic<long long> vsyncPeriodNs_{16666667};
    std::atomic<bool> gpuDemoEnabled_{false};
    bool persistentContext_ = false;
    uint32_t toplevelId_ = 0;
    mutable std::mutex insetsMutex_;
    ContentInsets insets_;
    DesktopCompositor& compositor_;
};

// Diagnostic: writes the next composed direct-GPU frame to path (PPM).
void RequestGpuFrameCapture(const std::string& path);
