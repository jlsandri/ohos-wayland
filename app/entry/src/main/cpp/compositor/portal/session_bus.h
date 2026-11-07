#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A D-Bus session bus for the apps, with xdg-desktop-portal on it, so that
// toolkits (GTK, libadwaita, Qt, Electron, zbus/ashpd) show HarmonyOS
// dialogs. Apps reach it at DBUS_SESSION_BUS_ADDRESS. A portal call that
// needs HarmonyOS UI becomes a request for ArkTS; ArkTS answers with
// CompleteRequest.
namespace portal {

// kind is "open" or "save" (FileChooser); json describes the dialog.
using RequestSink = std::function<void(uint32_t id, const std::string& kind, const std::string& json)>;

bool StartSessionBus(const std::string& socketPath, RequestSink sink);

// Any thread. response follows org.freedesktop.portal.Request: 0 success,
// 1 cancelled, 2 failed. uris are file:// URIs of the chosen files.
void CompleteRequest(uint32_t id, uint32_t response, const std::vector<std::string>& uris);

} // namespace portal
