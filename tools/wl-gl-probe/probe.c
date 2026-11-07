// Minimal foreign Wayland + EGL desktop-GL client. It is built outside the
// HAP to prove that `ohos-wl install` can run an unmodified executable on the
// direct GPU path. It cycles the clear colour and logs frame counts.

#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xdg-shell-client-protocol.h"

#define GL_COLOR_BUFFER_BIT 0x00004000

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_egl_window *egl_window;
static int width = 640, height = 400;
static int configured, running = 1;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
   if (!strcmp(interface, wl_compositor_interface.name))
      compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
   else if (!strcmp(interface, xdg_wm_base_interface.name))
      wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
   xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
   xdg_surface_ack_configure(surface, serial);
   configured = 1;
}

static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                               struct wl_array *states)
{
   if (w > 0 && h > 0) {
      width = w;
      height = h;
      if (egl_window)
         wl_egl_window_resize(egl_window, width, height, 0, 0);
   }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};

int main(int argc, char **argv)
{
   struct wl_display *display = wl_display_connect(NULL);
   if (!display) {
      fprintf(stderr, "WL_GL_PROBE no wayland display\n");
      return 1;
   }
   struct wl_registry *registry = wl_display_get_registry(display);
   wl_registry_add_listener(registry, &registry_listener, NULL);
   wl_display_roundtrip(display);
   if (!compositor || !wm_base) {
      fprintf(stderr, "WL_GL_PROBE missing globals\n");
      return 2;
   }
   xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
   struct wl_surface *surface = wl_compositor_create_surface(compositor);
   struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
   xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
   struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
   xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
   xdg_toplevel_set_title(toplevel, "wl-gl-probe");
   wl_surface_commit(surface);
   while (!configured)
      wl_display_dispatch(display);

   EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)display);
   if (!eglInitialize(egl_display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) {
      fprintf(stderr, "WL_GL_PROBE EGL init failed 0x%x\n", eglGetError());
      return 3;
   }
   const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
   };
   EGLConfig config;
   EGLint count = 0;
   if (!eglChooseConfig(egl_display, config_attributes, &config, 1, &count) || count < 1) {
      fprintf(stderr, "WL_GL_PROBE no EGL config 0x%x\n", eglGetError());
      return 4;
   }
   EGLContext context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, NULL);
   egl_window = wl_egl_window_create(surface, width, height);
   EGLSurface egl_surface =
      eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)egl_window, NULL);
   if (context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
       !eglMakeCurrent(egl_display, egl_surface, egl_surface, context)) {
      fprintf(stderr, "WL_GL_PROBE EGL surface failed 0x%x\n", eglGetError());
      return 5;
   }
   void (*clear_color)(float, float, float, float) =
      (void (*)(float, float, float, float))eglGetProcAddress("glClearColor");
   void (*clear)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glClear");
   const char *(*get_string)(unsigned) = (const char *(*)(unsigned))eglGetProcAddress("glGetString");
   printf("WL_GL_PROBE renderer=%s\n", get_string ? get_string(0x1F01) : "?");

   unsigned long frame = 0;
   while (running) {
      const double t = frame / 60.0;
      clear_color(0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.1f), 0.5f + 0.5f * sinf(t + 4.2f), 1);
      clear(GL_COLOR_BUFFER_BIT);
      eglSwapBuffers(egl_display, egl_surface);
      if (++frame % 120 == 1)
         printf("WL_GL_PROBE frame=%lu size=%dx%d\n", frame, width, height);
      if (wl_display_dispatch_pending(display) < 0)
         break;
   }
   return 0;
}
