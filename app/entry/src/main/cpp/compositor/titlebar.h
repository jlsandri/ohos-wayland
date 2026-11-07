#pragma once

#include <cstdint>
#include <string>

struct wl_display;

// ohos_titlebar_manager_v1: a toolkit header bar drawn as a native HarmonyOS
// title bar. The client describes its header bar in JSON; the description
// goes to ArkTS as a "titlebar" toplevel event, and what the user activates
// there comes back to the client.
void RegisterTitlebar(wl_display* display);

// Any thread: delivers an activation to the toplevel's client.
void ActivateTitlebar(uint32_t toplevelId, const std::string& json);
