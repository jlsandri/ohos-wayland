#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <native_window/external_window.h>
#include <cstdint>

namespace ohoswayland {

enum class FrameClockState {
    Closed,
    Idle,
    Pending,
    Rendering,
    Presenting,
};

struct OhosFrame {
    OHNativeWindow* window = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLSync presentFence = EGL_NO_SYNC;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
    FrameClockState clock = FrameClockState::Closed;
};

} // namespace ohoswayland
