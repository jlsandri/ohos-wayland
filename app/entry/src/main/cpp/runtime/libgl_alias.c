// libGL.so.1 for distribution programs (ohos_wl.py RUNTIME_ROOTS). Desktop GL
// runs on the runtime's Mesa through EGL: libepoxy and similar loaders take
// GL 1.x entry points from libGL.so.1 and the rest from eglGetProcAddress.
// This library depends on Mesa's libGLESv2.so.2, whose entry points dispatch
// to whichever context is current, so lookups through it reach the runtime's
// driver instead of a second Mesa. HarmonyOS has no X server, so the GLX
// entry points that libraries such as GStreamer GL link fail.

void* glXChooseFBConfig(void) { return 0; }
void* glXChooseVisual(void) { return 0; }
void* glXCopyContext(void) { return 0; }
void* glXCreateContext(void) { return 0; }
void* glXCreateGLXPixmap(void) { return 0; }
void* glXCreateNewContext(void) { return 0; }
void* glXCreatePbuffer(void) { return 0; }
void* glXCreatePixmap(void) { return 0; }
void* glXCreateWindow(void) { return 0; }
void* glXDestroyContext(void) { return 0; }
void* glXDestroyGLXPixmap(void) { return 0; }
void* glXDestroyPbuffer(void) { return 0; }
void* glXDestroyPixmap(void) { return 0; }
void* glXDestroyWindow(void) { return 0; }
void* glXGetClientString(void) { return 0; }
void* glXGetConfig(void) { return 0; }
void* glXGetCurrentContext(void) { return 0; }
void* glXGetCurrentDisplay(void) { return 0; }
void* glXGetCurrentDrawable(void) { return 0; }
void* glXGetCurrentReadDrawable(void) { return 0; }
void* glXGetFBConfigAttrib(void) { return 0; }
void* glXGetFBConfigs(void) { return 0; }
void* glXGetProcAddress(void) { return 0; }
void* glXGetProcAddressARB(void) { return 0; }
void* glXGetSelectedEvent(void) { return 0; }
void* glXGetVisualFromFBConfig(void) { return 0; }
void* glXIsDirect(void) { return 0; }
void* glXMakeContextCurrent(void) { return 0; }
void* glXMakeCurrent(void) { return 0; }
void* glXQueryContext(void) { return 0; }
void* glXQueryDrawable(void) { return 0; }
void* glXQueryExtension(void) { return 0; }
void* glXQueryExtensionsString(void) { return 0; }
void* glXQueryServerString(void) { return 0; }
void* glXQueryVersion(void) { return 0; }
void* glXSelectEvent(void) { return 0; }
void* glXSwapBuffers(void) { return 0; }
void* glXUseXFont(void) { return 0; }
void* glXWaitGL(void) { return 0; }
void* glXWaitX(void) { return 0; }
