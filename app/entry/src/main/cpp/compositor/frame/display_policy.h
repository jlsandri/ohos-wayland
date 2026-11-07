#pragma once
#include <cstdint>

// How a subsurface is carried: inside its parent's window frame, or as its
// own popup window. WineHua's desktop compositing mode, the other half of
// this policy, was removed.
struct DisplayPolicy {
    // Judged from protocol facts, not heuristics (winewayland.drv: a client
    // surface has an empty input region and sets a viewport destination but
    // never a source; menus and child windows set a source on every attach).
    //
    //   InlineClient — composed into the parent window's frame, input passes
    //                  through to the parent, not clipped to the window
    //   Popup        — its own popup window, may extend past the parent
    enum class SubsurfaceRoute { InlineClient, Popup };

    static bool IsInlineClientSurface(bool inputRegionEmpty, int32_t vpSrcW) {
        return inputRegionEmpty && vpSrcW <= 0;
    }

    SubsurfaceRoute RouteForSubsurface(bool inputRegionEmpty, int32_t vpSrcW) const {
        return IsInlineClientSurface(inputRegionEmpty, vpSrcW) ? SubsurfaceRoute::InlineClient
                                                                : SubsurfaceRoute::Popup;
    }
};
