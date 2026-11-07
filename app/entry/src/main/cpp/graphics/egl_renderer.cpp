#include "egl_renderer.h"

#include "common/fps_counter.h"
#include "common/perf_utils.h"
#include "compositor/toplevel/desktop_compositor.h"
#include "graphics/present_stats.h"
#include "input/input_manager.h"
#include "shader_utils.h"

#include <algorithm>
#include <chrono>
#include <EGL/eglext.h>
#include <native_buffer/native_buffer.h>
#include <native_vsync/native_vsync.h>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"
#include <hilog/log.h>

using winehua::CompileShader;
using winehua::kFullscreenQuadFS;
using winehua::kFullscreenQuadVS;
using winehua::PerfClock;
using winehua::PerfNowUs;

namespace {
EGLDisplay gSharedDisplay = EGL_NO_DISPLAY;
std::once_flag gDisplayOnce;

// The system EGL exports the EGL 1.5 eglCreateSync but its driver does not
// implement it, and every call logs "eglCreateSync is not found". The
// EGL_KHR_fence_sync entry points work; without them frames go unfenced.
struct FenceProcs {
    PFNEGLCREATESYNCKHRPROC create = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC wait = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy = nullptr;
};

const FenceProcs& Fences()
{
    static const FenceProcs procs = [] {
        FenceProcs result;
        result.create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        result.wait = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
        result.destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        if (!result.create || !result.wait || !result.destroy) result = FenceProcs();
        return result;
    }();
    return procs;
}

EGLSync CreatePresentFence(EGLDisplay display)
{
    const FenceProcs& fences = Fences();
    return fences.create ? static_cast<EGLSync>(fences.create(display, EGL_SYNC_FENCE_KHR, nullptr)) : EGL_NO_SYNC;
}

void WaitPresentFence(EGLDisplay display, EGLSync fence)
{
    Fences().wait(display, static_cast<EGLSyncKHR>(fence), 0, EGL_FOREVER_KHR);
    Fences().destroy(display, static_cast<EGLSyncKHR>(fence));
}
}

EglRenderer::EglRenderer(DesktopCompositor& compositor) : compositor_(compositor) {}

EGLDisplay EglRenderer::GetSharedDisplay()
{
    std::call_once(gDisplayOnce, []() {
        gSharedDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gSharedDisplay == EGL_NO_DISPLAY) return;
        EGLint major = 0;
        EGLint minor = 0;
        if (!eglInitialize(gSharedDisplay, &major, &minor)) {
            gSharedDisplay = EGL_NO_DISPLAY;
            return;
        }
        OH_LOG_INFO(LOG_APP, "[EGL] initialized %{public}d.%{public}d", major, minor);
    });
    return gSharedDisplay;
}

ohoswayland::GraphicsCapabilities EglRenderer::ProbeCapabilities()
{
    ohoswayland::GraphicsCapabilities caps;
    caps.egl = GetSharedDisplay() != EGL_NO_DISPLAY;
    if (caps.egl) {
        const char* apis = eglQueryString(gSharedDisplay, EGL_CLIENT_APIS);
        caps.gles3 = apis && std::string(apis).find("OpenGL_ES") != std::string::npos;
    }
    const char name[] = "OhosWaylandCapabilities";
    OH_NativeVSync* vsync = OH_NativeVSync_Create(name, sizeof(name) - 1);
    caps.nativeVsync = vsync != nullptr;
    if (vsync) OH_NativeVSync_Destroy(vsync);
    return caps;
}

namespace {
// A NativeImage stays bound to the EGL context it was created in, so the GPU
// consumer and its context outlive individual renderers. XComponent surface
// recreation then keeps the client's window. The context serves one renderer
// at a time; a renderer created while it is taken gets a private context.
EGLContext gGpuContext = EGL_NO_CONTEXT;
std::atomic<bool> gGpuContextTaken{false};
ohoswayland::GpuFrameConsumer& SharedGpuConsumer()
{
    static ohoswayland::GpuFrameConsumer consumer;
    return consumer;
}
bool gGpuConsumerInitialized = false;
}

void RequestGpuFrameCapture(const std::string& path)
{
    SharedGpuConsumer().RequestCapture(path);
}

namespace {
}

bool EglRenderer::Init(OHNativeWindow* window, int w, int h)
{
    frame_.window = window;
    frame_.width = w;
    frame_.height = h;
    frame_.display = GetSharedDisplay();
    if (frame_.display == EGL_NO_DISPLAY) return false;

    EGLConfig config = nullptr;
    EGLint configCount = 0;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    if (!eglChooseConfig(frame_.display, attributes, &config, 1, &configCount) || configCount != 1)
        return false;
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    bool expected = false;
    persistentContext_ = gGpuContextTaken.compare_exchange_strong(expected, true);
    if (persistentContext_ && gGpuContext != EGL_NO_CONTEXT) {
        frame_.context = gGpuContext;
    } else {
        frame_.context = eglCreateContext(frame_.display, config, EGL_NO_CONTEXT, contextAttributes);
        if (persistentContext_) gGpuContext = frame_.context;
    }
    if (frame_.context == EGL_NO_CONTEXT) return false;

    OH_NativeWindow_NativeWindowHandleOpt(frame_.window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    frame_.surface = eglCreateWindowSurface(
        frame_.display, config, reinterpret_cast<EGLNativeWindowType>(frame_.window), nullptr);
    if (frame_.surface == EGL_NO_SURFACE) {
        if (!persistentContext_) eglDestroyContext(frame_.display, frame_.context);
        if (persistentContext_) gGpuContextTaken.store(false);
        frame_.context = EGL_NO_CONTEXT;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    return true;
}

void EglRenderer::OnVSync(long long timestamp, void* data)
{
    static_cast<void>(timestamp);
    auto* renderer = static_cast<EglRenderer*>(data);
    {
        std::lock_guard<std::mutex> lock(renderer->vsyncMutex_);
        ++renderer->vsyncSequence_;
    }
    renderer->vsyncCv_.notify_one();
}

FitRect EglRenderer::GetInputLetterbox() const
{
    if (contentW_ > 0 && contentH_ > 0) {
        FitRect result;
        if (ComputeInsetFitRect(frame_.width, frame_.height, contentW_, contentH_, GetContentInsets(), result)) return result;
    }
    return letterbox_;
}

FitRect EglRenderer::ComputeFrameDisplayRect(int drawW, int drawH) const
{
    if (!stretchFill_.load() || drawW <= 0 || drawH <= 0) return letterbox_;
    FitRect result = letterbox_;
    result.offX = 0;
    result.offY = 0;
    result.dstW = drawW;
    result.dstH = drawH;
    return result;
}

uint32_t EglRenderer::DirectPassCapabilities() const
{
    return winehua::kDirectPassCapabilitiesAll;
}

void EglRenderer::RenderLoop()
{
    frame_.clock = ohoswayland::FrameClockState::Idle;
    if (!eglMakeCurrent(frame_.display, frame_.surface, frame_.surface, frame_.context)) {
        frame_.clock = ohoswayland::FrameClockState::Closed;
        return;
    }
    eglSwapInterval(frame_.display, 0);

    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kFullscreenQuadVS);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFullscreenQuadFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glBindAttribLocation(program_, 0, "aPos");
    glBindAttribLocation(program_, 1, "aUV");
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] shader link failed");
        frame_.clock = ohoswayland::FrameClockState::Closed;
        return;
    }

    const float quad[] = {
        -1,-1, 0,1, 1,-1, 1,1, -1,1, 0,0,
        1,-1, 1,1, 1,1, 1,0, -1,1, 0,0,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);

    FpsCounter fps("render");
    // Per-stage time of the GPU loop, logged every kTimingFrames frames: the
    // stages must fit one vsync period for the loop to keep the display rate.
    constexpr int kTimingFrames = 240;
    int timingFrames = 0;
    long long fenceUs = 0, renderUs = 0, swapUs = 0;
    auto sinceUs = [](PerfClock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(PerfClock::now() - start).count();
    };
    std::vector<uint8_t> pixels;
    bool rendered = false;
    bool firstFrameLogged = false;
    long long vsyncPeriodNs = vsyncPeriodNs_.load(std::memory_order_relaxed);
    long long loggedPeriodNs = 0;
    unsigned int vsyncFailures = 0;
    uint64_t vsyncTicks = 0;
    auto lastVsyncTick = PerfClock::time_point{};
    auto fallbackDeadline = PerfClock::now();
    const char vsyncName[] = "OhosWaylandRenderer";
    OH_NativeVSync* nativeVsync = OH_NativeVSync_Create(vsyncName, sizeof(vsyncName) - 1);
    if (nativeVsync) {
        OH_NativeVSync_ExpectedRateRange rate = {60, 120, 120};
        OH_NativeVSync_SetExpectedFrameRateRange(nativeVsync, &rate);
    }

    auto waitForFrameTick = [&]() -> bool {
        if (!running_) return false;
        frame_.clock = ohoswayland::FrameClockState::Pending;
        if (nativeVsync) {
            uint64_t requestedSequence;
            {
                std::lock_guard<std::mutex> lock(vsyncMutex_);
                requestedSequence = vsyncSequence_;
            }
            const int requestResult = OH_NativeVSync_RequestFrame(nativeVsync, &EglRenderer::OnVSync, this);
            if (requestResult == 0) {
                std::unique_lock<std::mutex> lock(vsyncMutex_);
                const bool signaled = vsyncCv_.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                    return !running_ || vsyncSequence_ != requestedSequence;
                });
                lock.unlock();
                if (!running_) return false;
                if (signaled) {
                    long long period = 0;
                    if (OH_NativeVSync_GetPeriod(nativeVsync, &period) == 0 && period > 0) {
                        vsyncPeriodNs = period;
                        vsyncPeriodNs_.store(period, std::memory_order_relaxed);
                        if (loggedPeriodNs == 0 || std::abs(period - loggedPeriodNs) >= 500000) {
                            OH_LOG_INFO(LOG_APP, "[EGL] NativeVSync period=%{public}lldns", period);
                            loggedPeriodNs = period;
                            ohoswayland::PresentStats::Instance().OnDisplayPeriod(period);
                        }
                    }
                    vsyncFailures = 0;
                    fallbackDeadline = PerfClock::now();
                    ++vsyncTicks;
                    const auto tickCadenceUs = lastVsyncTick.time_since_epoch().count() == 0
                        ? 0 : std::chrono::duration_cast<std::chrono::microseconds>(
                              fallbackDeadline - lastVsyncTick).count();
                    lastVsyncTick = fallbackDeadline;
                    if (vsyncTicks == 1 || vsyncTicks % 120 == 0) {
                        OH_LOG_INFO(LOG_APP,
                                    "[EGL] NativeVSync cadence ticks=%{public}llu "
                                    "period=%{public}lldns cadence=%{public}lldus",
                                    static_cast<unsigned long long>(vsyncTicks), vsyncPeriodNs,
                                    static_cast<long long>(tickCadenceUs));
                    }
                    frame_.clock = ohoswayland::FrameClockState::Idle;
                    return true;
                }
            }
            ++vsyncFailures;
        }
        const auto period = std::chrono::nanoseconds(vsyncPeriodNs > 0 ? vsyncPeriodNs : 16666667);
        const auto now = PerfClock::now();
        fallbackDeadline += period;
        if (fallbackDeadline <= now || fallbackDeadline - now > period * 2)
            fallbackDeadline = now + period;
        std::this_thread::sleep_until(fallbackDeadline);
        frame_.clock = ohoswayland::FrameClockState::Idle;
        return running_;
    };

    while (running_) {
        frame_.clock = ohoswayland::FrameClockState::Rendering;
        auto stageStart = PerfClock::now();
        if (frame_.presentFence != EGL_NO_SYNC) {
            WaitPresentFence(frame_.display, frame_.presentFence);
            frame_.presentFence = EGL_NO_SYNC;
        }
        fenceUs += sinceUs(stageStart);
        const ContentInsets insets = GetContentInsets();

        if (gpuDemoEnabled_.load(std::memory_order_relaxed)) {
            if (!gGpuConsumerInitialized) gGpuConsumerInitialized = SharedGpuConsumer().Initialize();
            EGLint surfaceW = 0;
            EGLint surfaceH = 0;
            eglQuerySurface(frame_.display, frame_.surface, EGL_WIDTH, &surfaceW);
            eglQuerySurface(frame_.display, frame_.surface, EGL_HEIGHT, &surfaceH);
            stageStart = PerfClock::now();
            const bool presentedGpu = gGpuConsumerInitialized &&
                SharedGpuConsumer().Render(surfaceW, surfaceH, vsyncPeriodNs, insets);
            renderUs += sinceUs(stageStart);
            // Each display tick releases the direct clients' frame callbacks.
            InputManager::GetInstance()->NotifyFramePresented();
            // Pointer input maps view pixels to surface coordinates through
            // the same fit the consumer draws with.
            if (presentedGpu) {
                contentW_ = SharedGpuConsumer().SourceWidth();
                contentH_ = SharedGpuConsumer().SourceHeight();
            }
            frame_.clock = ohoswayland::FrameClockState::Presenting;
            stageStart = PerfClock::now();
            const bool swapped = presentedGpu && eglSwapBuffers(frame_.display, frame_.surface) == EGL_TRUE;
            swapUs += sinceUs(stageStart);
            if (++timingFrames == kTimingFrames) {
                OH_LOG_INFO(LOG_APP, "[EGL] loop timing fence=%{public}.2fms render=%{public}.2fms swap=%{public}.2fms",
                            fenceUs / 1000.0 / kTimingFrames, renderUs / 1000.0 / kTimingFrames,
                            swapUs / 1000.0 / kTimingFrames);
                timingFrames = 0;
                fenceUs = renderUs = swapUs = 0;
            }
            if (swapped) {
                lastDrawW_ = surfaceW;
                lastDrawH_ = surfaceH;
                ++frame_.generation;
                if (frame_.presentFence == EGL_NO_SYNC)
                    frame_.presentFence = CreatePresentFence(frame_.display);
                fps.Tick();
                if (SharedGpuConsumer().TopFrameLatched()) {
                    ohoswayland::PresentStats::Instance().OnClientFrame(ohoswayland::SteadyNowNs(),
                                                                        SharedGpuConsumer().TopBuffer());
                }
            }
            frame_.clock = ohoswayland::FrameClockState::Idle;
            if (!waitForFrameTick()) break;
            continue;
        }

        uint32_t useToplevel = toplevelId_;
        PresentedFrame presented;
        const bool haveFrame = useToplevel != 0 &&
            compositor_.TakeToplevelFrame(useToplevel, pixels, presented);
        if (haveFrame) {
            frameW_ = presented.w;
            frameH_ = presented.h;
            frameArgb_ = !presented.opaque;
            contentW_ = presented.contentW;
            contentH_ = presented.contentH;
            if (!firstFrameLogged) {
                OH_LOG_INFO(LOG_APP, "[EGL] first wl_shm diagnostic frame %{public}dx%{public}d",
                            frameW_, frameH_);
                firstFrameLogged = true;
            }
            glBindTexture(GL_TEXTURE_2D, texture_);
            const int rowLength = frameH_ > 0 ? static_cast<int>(pixels.size()) / frameH_ / 4 : frameW_;
            if (rowLength != frameW_) glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            if (frameW_ != texW_ || frameH_ != texH_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameW_, frameH_, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                texW_ = frameW_;
                texH_ = frameH_;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameW_, frameH_,
                                GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            }
            if (rowLength != frameW_) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            rendered = true;
        }

        EGLint surfaceW = 0;
        EGLint surfaceH = 0;
        eglQuerySurface(frame_.display, frame_.surface, EGL_WIDTH, &surfaceW);
        eglQuerySurface(frame_.display, frame_.surface, EGL_HEIGHT, &surfaceH);
        if (surfaceW > 0 && surfaceH > 0) {
            frame_.width = surfaceW;
            frame_.height = surfaceH;
        }
        if (!haveFrame && rendered && surfaceW == lastDrawW_ && surfaceH == lastDrawH_ &&
            !SharedGpuConsumer().CapturePending()) {
            if (!waitForFrameTick()) break;
            continue;
        }

        const int drawW = frame_.width;
        const int drawH = frame_.height;
        if (!ComputeInsetFitRect(drawW, drawH, frameW_, frameH_, insets, letterbox_)) letterbox_ = FitRect{};
        glClearColor(0, 0, 0, frameArgb_ ? 0 : 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ohoswayland::PresentedBuffer shmBuffer;
        if (rendered && drawW > 0 && drawH > 0) {
            const FitRect display = ComputeFrameDisplayRect(drawW, drawH);
            shmBuffer = {ohoswayland::PresentPath::Shm, frameW_, frameH_,
                         contentW_ > 0 ? static_cast<double>(frameW_) / contentW_ : 0,
                         frameW_ > 0 ? static_cast<double>(display.dstW) / frameW_ : 0};
            // With insets the quad covers the output and the frame's edge
            // pixels continue across them.
            if (insets.Any()) {
                glViewport(0, 0, drawW, drawH);
            } else {
                glViewport(display.offX, display.offY, display.dstW, display.dstH);
            }
            glUseProgram(program_);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glUniform1f(glGetUniformLocation(program_, "uForceOpaque"), frameArgb_ ? 0.0f : 1.0f);
            if (insets.Any()) {
                glUniform4f(glGetUniformLocation(program_, "uExtend"),
                            -static_cast<float>(letterbox_.offX) / letterbox_.dstW,
                            -static_cast<float>(letterbox_.offY) / letterbox_.dstH,
                            static_cast<float>(drawW) / letterbox_.dstW, static_cast<float>(drawH) / letterbox_.dstH);
            } else {
                glUniform4f(glGetUniformLocation(program_, "uExtend"), 0, 0, 1, 1);
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
            SharedGpuConsumer().CaptureFrame(drawW, drawH);
        }

        frame_.clock = ohoswayland::FrameClockState::Presenting;
        if (eglSwapBuffers(frame_.display, frame_.surface) == EGL_TRUE) {
            lastDrawW_ = drawW;
            lastDrawH_ = drawH;
            ++frame_.generation;
            if (frame_.presentFence == EGL_NO_SYNC)
                frame_.presentFence = CreatePresentFence(frame_.display);
            fps.Tick();
            if (haveFrame) ohoswayland::PresentStats::Instance().OnClientFrame(ohoswayland::SteadyNowNs(), shmBuffer);
        }
        frame_.clock = ohoswayland::FrameClockState::Idle;
        if (!waitForFrameTick()) break;
    }

    if (nativeVsync) OH_NativeVSync_Destroy(nativeVsync);
    glDeleteTextures(1, &texture_);
    glDeleteBuffers(1, &vbo_);
    glDeleteProgram(program_);
    texture_ = 0;
    vbo_ = 0;
    program_ = 0;
    frame_.clock = ohoswayland::FrameClockState::Closed;
    // Release the persistent context from this thread so the next renderer can use it.
    if (persistentContext_) eglMakeCurrent(frame_.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void EglRenderer::Shutdown()
{
    running_ = false;
    vsyncCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (frame_.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(frame_.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (frame_.presentFence != EGL_NO_SYNC) {
            Fences().destroy(frame_.display, static_cast<EGLSyncKHR>(frame_.presentFence));
            frame_.presentFence = EGL_NO_SYNC;
        }
        if (frame_.surface != EGL_NO_SURFACE) {
            eglDestroySurface(frame_.display, frame_.surface);
            frame_.surface = EGL_NO_SURFACE;
        }
        if (persistentContext_) gGpuContextTaken.store(false);
        if (frame_.context != EGL_NO_CONTEXT && !persistentContext_) {
            eglDestroyContext(frame_.display, frame_.context);
            frame_.context = EGL_NO_CONTEXT;
        }
    }
    if (frame_.window) {
        OH_NativeWindow_DestroyNativeWindow(frame_.window);
        frame_.window = nullptr;
    }
}
