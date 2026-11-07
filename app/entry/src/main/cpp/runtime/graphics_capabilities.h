#pragma once

namespace ohoswayland {

struct GraphicsCapabilities {
    bool egl = false;
    bool gles3 = false;
    bool nativeVsync = false;
    bool zeroCopyPresentation = false;
    bool desktopGl = false;
    bool vulkan = false;
    bool shmDiagnostic = true;
};

} // namespace ohoswayland
