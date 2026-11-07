#include <AbilityKit/native_child_process.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <hilog/log.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include "protocols/xdg-shell-client-protocol.h"
#include "runtime/launch_params.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GL_DEMO"

#define glCreateShader glCreateShaderFn
#define glShaderSource glShaderSourceFn
#define glCompileShader glCompileShaderFn
#define glGetShaderiv glGetShaderivFn
#define glGetShaderInfoLog glGetShaderInfoLogFn
#define glDeleteShader glDeleteShaderFn
#define glCreateProgram glCreateProgramFn
#define glAttachShader glAttachShaderFn
#define glLinkProgram glLinkProgramFn
#define glGetProgramiv glGetProgramivFn
#define glGetProgramInfoLog glGetProgramInfoLogFn
#define glDeleteProgram glDeleteProgramFn
#define glGenVertexArrays glGenVertexArraysFn
#define glBindVertexArray glBindVertexArrayFn
#define glGenBuffers glGenBuffersFn
#define glBindBuffer glBindBufferFn
#define glBufferData glBufferDataFn
#define glEnableVertexAttribArray glEnableVertexAttribArrayFn
#define glVertexAttribPointer glVertexAttribPointerFn
#define glGetUniformLocation glGetUniformLocationFn
#define glGetString glGetStringFn
#define glGetStringi glGetStringiFn
#define glGetIntegerv glGetIntegervFn
#define glDeleteBuffers glDeleteBuffersFn
#define glDeleteVertexArrays glDeleteVertexArraysFn
#define glViewport glViewportFn
#define glClearColor glClearColorFn
#define glClear glClearFn
#define glUseProgram glUseProgramFn
#define glUniform3f glUniform3fFn
#define glDrawArrays glDrawArraysFn
#define glGenFramebuffers glGenFramebuffersFn
#define glBindFramebuffer glBindFramebufferFn
#define glGenRenderbuffers glGenRenderbuffersFn
#define glBindRenderbuffer glBindRenderbufferFn
#define glRenderbufferStorage glRenderbufferStorageFn
#define glFramebufferRenderbuffer glFramebufferRenderbufferFn
#define glCheckFramebufferStatus glCheckFramebufferStatusFn
#define glBlitFramebuffer glBlitFramebufferFn
#define glReadPixels glReadPixelsFn
#define glDeleteFramebuffers glDeleteFramebuffersFn
#define glDeleteRenderbuffers glDeleteRenderbuffersFn

namespace {
PFNGLCREATESHADERPROC glCreateShaderFn;
PFNGLSHADERSOURCEPROC glShaderSourceFn;
PFNGLCOMPILESHADERPROC glCompileShaderFn;
PFNGLGETSHADERIVPROC glGetShaderivFn;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLogFn;
PFNGLDELETESHADERPROC glDeleteShaderFn;
PFNGLCREATEPROGRAMPROC glCreateProgramFn;
PFNGLATTACHSHADERPROC glAttachShaderFn;
PFNGLLINKPROGRAMPROC glLinkProgramFn;
PFNGLGETPROGRAMIVPROC glGetProgramivFn;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLogFn;
PFNGLDELETEPROGRAMPROC glDeleteProgramFn;
PFNGLGENVERTEXARRAYSPROC glGenVertexArraysFn;
PFNGLBINDVERTEXARRAYPROC glBindVertexArrayFn;
PFNGLGENBUFFERSPROC glGenBuffersFn;
PFNGLBINDBUFFERPROC glBindBufferFn;
PFNGLBUFFERDATAPROC glBufferDataFn;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArrayFn;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointerFn;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocationFn;
const GLubyte* (*glGetStringFn)(GLenum);
const GLubyte* (*glGetStringiFn)(GLenum, GLuint);
void (*glGetIntegervFn)(GLenum, GLint*);
PFNGLDELETEBUFFERSPROC glDeleteBuffersFn;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArraysFn;
void (*glViewportFn)(GLint, GLint, GLsizei, GLsizei);
void (*glClearColorFn)(GLfloat, GLfloat, GLfloat, GLfloat);
void (*glClearFn)(GLbitfield);
PFNGLUSEPROGRAMPROC glUseProgramFn;
PFNGLUNIFORM3FPROC glUniform3fFn;
void (*glDrawArraysFn)(GLenum, GLint, GLsizei);
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffersFn;
PFNGLBINDFRAMEBUFFERPROC glBindFramebufferFn;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffersFn;
PFNGLBINDRENDERBUFFERPROC glBindRenderbufferFn;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorageFn;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbufferFn;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatusFn;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebufferFn;
void (*glReadPixelsFn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffersFn;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffersFn;

bool LoadDesktopGl()
{
#define LOAD_GL(name) name##Fn = reinterpret_cast<decltype(name##Fn)>(eglGetProcAddress(#name))
    LOAD_GL(glCreateShader);
    LOAD_GL(glShaderSource);
    LOAD_GL(glCompileShader);
    LOAD_GL(glGetShaderiv);
    LOAD_GL(glGetShaderInfoLog);
    LOAD_GL(glDeleteShader);
    LOAD_GL(glCreateProgram);
    LOAD_GL(glAttachShader);
    LOAD_GL(glLinkProgram);
    LOAD_GL(glGetProgramiv);
    LOAD_GL(glGetProgramInfoLog);
    LOAD_GL(glDeleteProgram);
    LOAD_GL(glGenVertexArrays);
    LOAD_GL(glBindVertexArray);
    LOAD_GL(glGenBuffers);
    LOAD_GL(glBindBuffer);
    LOAD_GL(glBufferData);
    LOAD_GL(glEnableVertexAttribArray);
    LOAD_GL(glVertexAttribPointer);
    LOAD_GL(glGetUniformLocation);
    LOAD_GL(glGetString);
    LOAD_GL(glGetStringi);
    LOAD_GL(glGetIntegerv);
    LOAD_GL(glDeleteBuffers);
    LOAD_GL(glDeleteVertexArrays);
    LOAD_GL(glViewport);
    LOAD_GL(glClearColor);
    LOAD_GL(glClear);
    LOAD_GL(glUseProgram);
    LOAD_GL(glUniform3f);
    LOAD_GL(glDrawArrays);
    LOAD_GL(glGenFramebuffers);
    LOAD_GL(glBindFramebuffer);
    LOAD_GL(glGenRenderbuffers);
    LOAD_GL(glBindRenderbuffer);
    LOAD_GL(glRenderbufferStorage);
    LOAD_GL(glFramebufferRenderbuffer);
    LOAD_GL(glCheckFramebufferStatus);
    LOAD_GL(glBlitFramebuffer);
    LOAD_GL(glReadPixels);
    LOAD_GL(glDeleteFramebuffers);
    LOAD_GL(glDeleteRenderbuffers);
#undef LOAD_GL
    return glCreateShaderFn && glCreateProgramFn && glGetStringFn && glGetStringiFn &&
        glGetIntegervFn && glGenVertexArraysFn && glGenBuffersFn && glDrawArraysFn;
}

struct GlDemo {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_egl_window* eglWindow = nullptr;
    wl_callback* frameCallback = nullptr;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLConfig eglConfig = nullptr;
    GLuint program = 0;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    GLint colorLocation = -1;
    int width = 640;
    int height = 360;
    bool configured = false;
    bool frameReady = true;
    bool running = true;
};

GlDemo gDemo;

void LogWayland(const char* format, va_list args)
{
    char message[2048];
    vsnprintf(message, sizeof(message), format, args);
    OH_LOG_ERROR(LOG_APP, "wayland: %{public}s", message);
}

void ForwardClientLog(const char* path, std::atomic<bool>& stop)
{
    FILE* file = nullptr;
    char line[4096];
    while (!stop.load(std::memory_order_relaxed)) {
        if (!file) {
            file = fopen(path, "r");
            if (!file) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            OH_LOG_ERROR(LOG_APP, "MESA_LOG %{public}s", line);
        }
        if (feof(file)) clearerr(file);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            OH_LOG_ERROR(LOG_APP, "MESA_LOG %{public}s", line);
        }
        fclose(file);
    }
}

void WmBasePing(void*, xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener kWmBaseListener = {WmBasePing};

void XdgConfigure(void*, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    gDemo.configured = true;
}
const xdg_surface_listener kXdgSurfaceListener = {XdgConfigure};

void ToplevelConfigure(void*, xdg_toplevel*, int32_t width, int32_t height, wl_array*)
{
    if (width > 0 && height > 0) {
        gDemo.width = width;
        gDemo.height = height;
        if (gDemo.eglWindow) wl_egl_window_resize(gDemo.eglWindow, width, height, 0, 0);
    }
}
void ToplevelClose(void*, xdg_toplevel*)
{
    gDemo.running = false;
}
const xdg_toplevel_listener kToplevelListener = {ToplevelConfigure, ToplevelClose};

void RegistryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        gDemo.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        gDemo.wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(gDemo.wmBase, &kWmBaseListener, nullptr);
    }
}
const wl_registry_listener kRegistryListener = {
    RegistryGlobal, [](void*, wl_registry*, uint32_t) {},
};

void FrameDone(void* data, wl_callback* callback, uint32_t)
{
    GlDemo* demo = static_cast<GlDemo*>(data);
    demo->frameReady = true;
    if (demo->frameCallback == callback) demo->frameCallback = nullptr;
    wl_callback_destroy(callback);
}
const wl_callback_listener kFrameListener = {FrameDone};

GLuint CompileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR shader_compile: %{public}s", log);
    glDeleteShader(shader);
    return 0;
}

bool CreateProgram(GlDemo& demo)
{
    static const char vertexSource[] = R"(#version 400
#extension GL_ARB_shader_storage_buffer_object : require
#extension GL_ARB_shading_language_420pack : require
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
    static const char fragmentSource[] = R"(#version 400
#extension GL_ARB_shader_storage_buffer_object : require
#extension GL_ARB_shading_language_420pack : require
in vec3 vColor;
out vec4 outColor;
uniform vec3 uPulse;
void main() {
    outColor = vec4(vColor * uPulse, 1.0);
}
)";
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    demo.program = glCreateProgram();
    glAttachShader(demo.program, vertex);
    glAttachShader(demo.program, fragment);
    glLinkProgram(demo.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(demo.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(demo.program, sizeof(log), nullptr, log);
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR program_link: %{public}s", log);
        return false;
    }
    static const float vertices[] = {
        -0.72f, -0.62f, 0.95f, 0.16f, 0.12f,
         0.72f, -0.62f, 0.12f, 0.85f, 0.28f,
         0.00f,  0.72f, 0.18f, 0.32f, 0.96f,
    };
    glGenVertexArrays(1, &demo.vertexArray);
    glBindVertexArray(demo.vertexArray);
    glGenBuffers(1, &demo.vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, demo.vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5,
                          reinterpret_cast<void*>(sizeof(float) * 2));
    demo.colorLocation = glGetUniformLocation(demo.program, "uPulse");
    return true;
}

bool CreateEgl(GlDemo& demo)
{
    demo.eglDisplay = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(demo.display));
    if (demo.eglDisplay == EGL_NO_DISPLAY || !eglInitialize(demo.eglDisplay, nullptr, nullptr)) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR eglInitialize error=0x%{public}x", eglGetError());
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR eglBindAPI OpenGL error=0x%{public}x", eglGetError());
        return false;
    }
    if (!LoadDesktopGl()) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR eglGetProcAddress could not load desktop GL");
        return false;
    }
    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE,
    };
    EGLConfig& config = demo.eglConfig;
    EGLint count = 0;
    if (!eglChooseConfig(demo.eglDisplay, configAttributes, &config, 1, &count) || count != 1) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR eglChooseConfig OpenGL error=0x%{public}x", eglGetError());
        return false;
    }
    const EGLint contextAttributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    demo.eglContext = eglCreateContext(demo.eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
    if (demo.eglContext == EGL_NO_CONTEXT) {
        OH_LOG_ERROR(LOG_APP, "GPU_GL40_UNAVAILABLE eglCreateContext 4.0 core error=0x%{public}x",
                     eglGetError());
        return false;
    }
    demo.eglSurface = eglCreateWindowSurface(
        demo.eglDisplay, config,
        reinterpret_cast<EGLNativeWindowType>(demo.eglWindow),
        nullptr);
    if (demo.eglSurface == EGL_NO_SURFACE ||
        eglMakeCurrent(demo.eglDisplay, demo.eglSurface, demo.eglSurface, demo.eglContext) != EGL_TRUE) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR EGL window/current error=0x%{public}x", eglGetError());
        return false;
    }
    return true;
}

bool CheckDesktopGl40Extensions()
{
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    OH_LOG_INFO(LOG_APP, "GL_VERSION=%{public}s", version ? version : "(null)");
    OH_LOG_INFO(LOG_APP, "GL_VENDOR=%{public}s", vendor ? vendor : "(null)");
    OH_LOG_INFO(LOG_APP, "GL_RENDERER=%{public}s", renderer ? renderer : "(null)");
    static const char* required[] = {
        "GL_ARB_shader_storage_buffer_object",
        "GL_ARB_vertex_attrib_binding",
        "GL_ARB_shading_language_420pack",
    };
    bool present[3] = {false, false, false};
    GLint extensionCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
    for (GLint i = 0; i < extensionCount; ++i) {
        const char* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        OH_LOG_INFO(LOG_APP, "GL_EXTENSION=%{public}s", extension ? extension : "(null)");
        if (!extension) continue;
        for (size_t j = 0; j < 3; ++j) {
            if (strcmp(extension, required[j]) == 0) present[j] = true;
        }
    }
    bool supported = true;
    for (size_t j = 0; j < 3; ++j) {
        OH_LOG_INFO(LOG_APP, "GL_EXTENSION_REQUIRED %{public}s=%{public}s",
                    required[j], present[j] ? "present" : "missing");
        supported = supported && present[j];
    }
    if (!supported) {
        OH_LOG_ERROR(LOG_APP, "GPU_EXTENSION_GATE_UNAVAILABLE GL_VERSION=%{public}s",
                     version ? version : "(null)");
        return false;
    }
    OH_LOG_INFO(LOG_APP, "GPU_EXTENSION_GATE_PASS GL_VERSION=%{public}s",
                version ? version : "(null)");
    return true;
}

void Destroy(GlDemo& demo)
{
    if (demo.eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(demo.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (demo.vertexBuffer) glDeleteBuffers(1, &demo.vertexBuffer);
        if (demo.vertexArray) glDeleteVertexArrays(1, &demo.vertexArray);
        if (demo.program) glDeleteProgram(demo.program);
        if (demo.eglSurface != EGL_NO_SURFACE) eglDestroySurface(demo.eglDisplay, demo.eglSurface);
        if (demo.eglContext != EGL_NO_CONTEXT) eglDestroyContext(demo.eglDisplay, demo.eglContext);
    }
    if (demo.eglWindow) wl_egl_window_destroy(demo.eglWindow);
    if (demo.frameCallback) wl_callback_destroy(demo.frameCallback);
    if (demo.toplevel) xdg_toplevel_destroy(demo.toplevel);
    if (demo.xdgSurface) xdg_surface_destroy(demo.xdgSurface);
    if (demo.surface) wl_surface_destroy(demo.surface);
    if (demo.wmBase) xdg_wm_base_destroy(demo.wmBase);
    if (demo.compositor) wl_compositor_destroy(demo.compositor);
    if (demo.registry) wl_registry_destroy(demo.registry);
    if (demo.display) wl_display_disconnect(demo.display);
}

int Run()
{
    gDemo = GlDemo{};
    GlDemo& demo = gDemo;
    demo.display = wl_display_connect(nullptr);
    if (!demo.display) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR wl_display_connect failed");
        return 1;
    }
    demo.registry = wl_display_get_registry(demo.display);
    wl_registry_add_listener(demo.registry, &kRegistryListener, nullptr);
    if (wl_display_roundtrip(demo.display) < 0 || !demo.compositor || !demo.wmBase) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR required Wayland globals missing");
        return 2;
    }
    demo.surface = wl_compositor_create_surface(demo.compositor);
    demo.xdgSurface = xdg_wm_base_get_xdg_surface(demo.wmBase, demo.surface);
    xdg_surface_add_listener(demo.xdgSurface, &kXdgSurfaceListener, nullptr);
    demo.toplevel = xdg_surface_get_toplevel(demo.xdgSurface);
    xdg_toplevel_add_listener(demo.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(demo.toplevel, "Native Zink GL 4.0 extension demo");
    xdg_toplevel_set_app_id(demo.toplevel, "com.exploration.ohoswayland.gldemo");
    wl_surface_commit(demo.surface);
    if (wl_display_roundtrip(demo.display) < 0 || !demo.configured) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR initial XDG configure missing");
        return 3;
    }
    demo.eglWindow = wl_egl_window_create(demo.surface, demo.width, demo.height);
    if (!demo.eglWindow) return 4;
    if (!CreateEgl(demo)) return 4;
    if (!CheckDesktopGl40Extensions()) return 5;
    if (!CreateProgram(demo)) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR GL program creation failed");
        return 6;
    }

    uint64_t frame = 0;
    while (demo.running) {
        const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(frame) * 0.05f);
        glViewport(0, 0, demo.width, demo.height);
        glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(demo.program);
        glUniform3f(demo.colorLocation, pulse, pulse, pulse);
        glBindVertexArray(demo.vertexArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (eglSwapBuffers(demo.eglDisplay, demo.eglSurface) != EGL_TRUE) {
            OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR eglSwapBuffers error=0x%{public}x", eglGetError());
            return 7;
        }
        if (++frame == 1 || frame % 600 == 0) {
            OH_LOG_INFO(LOG_APP, "GPU_CLIENT_FRAME count=%{public}llu size=%{public}dx%{public}d",
                        static_cast<unsigned long long>(frame), demo.width, demo.height);
        }
        if (wl_display_dispatch_pending(demo.display) < 0 || wl_display_flush(demo.display) < 0) {
            OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR Wayland dispatch failed");
            return 8;
        }
    }
    return 0;
}
}

extern "C" __attribute__((visibility("default")))
void OhosWaylandGlDemoMain(NativeChildProcess_Args args)
{
    if (freopen("/data/storage/el2/base/cache/ohos_wayland_gl_client.log", "w", stderr))
        setvbuf(stderr, nullptr, _IONBF, 0);
    wl_log_set_handler_client(LogWayland);
    if (!ohoswayland::ApplyLaunchParams(args.entryParams)) {
        OH_LOG_ERROR(LOG_APP, "GPU_DEMO_ERROR invalid launch parameters");
        return;
    }
    std::atomic<bool> logStop{false};
    std::thread logThread(ForwardClientLog,
                          "/data/storage/el2/base/cache/ohos_wayland_gl_client.log",
                          std::ref(logStop));
    const int result = Run();
    OH_LOG_INFO(LOG_APP, "GPU demo exited result=%{public}d", result);
    Destroy(gDemo);
    logStop.store(true, std::memory_order_relaxed);
    if (logThread.joinable()) logThread.join();
}
