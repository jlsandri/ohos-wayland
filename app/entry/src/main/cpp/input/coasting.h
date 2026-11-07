#pragma once

#include <napi/native_api.h>

// watchCoasting(content, callback): puts a node in the NodeContent that
// delivers HarmonyOS touchpad coasting (the momentum ArkUI generates after a
// two-finger fling) to callback(phase, deltaX, deltaY). Returns the ArkUI
// result code.
napi_value WatchCoasting(napi_env env, napi_callback_info info);
