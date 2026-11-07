#pragma once

#include <wayland-server.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

void RegisterDataDeviceManager(wl_display* display);

// The selection is shared with the HarmonyOS pasteboard. Reading it needs
// ohos.permission.READ_PASTEBOARD, which only ArkTS can ask the user for.
namespace clipboard {

// Called on the Wayland thread when a client pastes before the permission
// is granted. The answer comes back through OnPermissionResult.
void SetPermissionRequester(std::function<void()> ask);
void OnPermissionResult(bool granted);

} // namespace clipboard

// Drags from HarmonyOS into the app (ArkUI onDragEnter to onDrop). Posted
// from the UI thread, delivered on the Wayland thread to the toplevel's
// surface at surface-local x, y.
namespace dnd {

enum class Kind { Enter, Motion, Leave, Drop };

struct Event {
    Kind kind;
    uint32_t toplevel;
    wl_fixed_t x;
    wl_fixed_t y;
    std::vector<std::string> mimeTypes;
    std::vector<std::pair<std::string, std::string>> payloads;
};

void Post(Event event);

} // namespace dnd
