#pragma once

#include <cstdint>

struct wl_display;
struct wl_resource;

// zxdg_decoration_manager_v1: which side draws a toplevel's title bar.
// Client-side means the app draws it (GTK header bars, Zed) and the HarmonyOS
// window decor is hidden; server-side keeps the HarmonyOS decor.
void RegisterXdgDecoration(wl_display* display);

enum class DecorationMode : uint32_t { Unknown = 0, Client = 1, Server = 2 };

// The negotiated mode, or Unknown when the client never asked (GTK 4 does
// not bind the protocol).
DecorationMode ToplevelDecorationMode(uint32_t toplevelId);

// Called when the xdg_toplevel goes away.
void ForgetToplevelDecoration(uint32_t toplevelId);

// Tells ArkTS which side draws the title bar ("decoration" event).
void PostDecorationEvent(uint32_t toplevelId, bool clientSide);
