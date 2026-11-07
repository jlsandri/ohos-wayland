#pragma once

#include <string>

namespace ohoswayland {

// Serves remote swapchains (remote_wsi_protocol.h) on a Unix socket at path
// for programs this host execs. Returns false when the socket cannot be bound.
bool StartRemoteWsiServer(const std::string& path);

} // namespace ohoswayland
