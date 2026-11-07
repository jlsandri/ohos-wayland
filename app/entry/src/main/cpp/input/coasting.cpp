#include "coasting.h"

#include <arkui/native_interface.h>
#include <arkui/native_node.h>
#include <arkui/native_node_napi.h>
#include <arkui/ui_input_event.h>
#include <hilog/log.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Coasting"

namespace {

// ArkUI calls node event receivers on the UI thread, the thread that owns
// this env, so the callback runs directly.
napi_env gEnv = nullptr;
napi_ref gCallback = nullptr;

void OnNodeEvent(ArkUI_NodeEvent* event)
{
    if (OH_ArkUI_NodeEvent_GetEventType(event) != NODE_ON_COASTING_AXIS_EVENT || !gCallback) return;
    ArkUI_CoastingAxisEvent* coasting =
        OH_ArkUI_UIInputEvent_GetCoastingAxisEvent(OH_ArkUI_NodeEvent_GetInputEvent(event));
    if (!coasting) return;
    napi_handle_scope scope;
    napi_open_handle_scope(gEnv, &scope);
    napi_value callback;
    napi_value args[3];
    napi_get_reference_value(gEnv, gCallback, &callback);
    napi_create_int32(gEnv, OH_ArkUI_CoastingAxisEvent_GetPhase(coasting), &args[0]);
    napi_create_double(gEnv, OH_ArkUI_CoastingAxisEvent_GetDeltaX(coasting), &args[1]);
    napi_create_double(gEnv, OH_ArkUI_CoastingAxisEvent_GetDeltaY(coasting), &args[2]);
    napi_call_function(gEnv, nullptr, callback, 3, args, nullptr);
    napi_close_handle_scope(gEnv, scope);
}

int32_t SetNumber(ArkUI_NativeNodeAPI_1* api, ArkUI_NodeHandle node, ArkUI_NodeAttributeType type,
                  ArkUI_NumberValue value)
{
    ArkUI_AttributeItem item = {&value, 1, nullptr, nullptr};
    return api->setAttribute(node, type, &item);
}

} // namespace

// ArkUI delivers C-API events only to nodes the C API created (an ArkTS
// node refuses registerNodeEvent with 401), so coasting is watched on a
// full-size node over the surface that lets every other event through.
napi_value WatchCoasting(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t result = ARKUI_ERROR_CODE_PARAM_INVALID;
    ArkUI_NodeContentHandle content = nullptr;
    ArkUI_NativeNodeAPI_1* api = nullptr;
    OH_ArkUI_GetModuleInterface(ARKUI_NATIVE_NODE, ArkUI_NativeNodeAPI_1, api);
    if (argc == 2 && api && OH_ArkUI_GetNodeContentFromNapiValue(env, args[0], &content) == 0 && content) {
        ArkUI_NodeHandle node = api->createNode(ARKUI_NODE_STACK);
        SetNumber(api, node, NODE_WIDTH_PERCENT, {.f32 = 1.0f});
        SetNumber(api, node, NODE_HEIGHT_PERCENT, {.f32 = 1.0f});
        SetNumber(api, node, NODE_HIT_TEST_BEHAVIOR, {.i32 = ARKUI_HIT_TEST_MODE_TRANSPARENT});
        result = api->registerNodeEvent(node, NODE_ON_COASTING_AXIS_EVENT, 0, nullptr);
        if (result == ARKUI_ERROR_CODE_NO_ERROR) result = api->addNodeEventReceiver(node, OnNodeEvent);
        if (result == ARKUI_ERROR_CODE_NO_ERROR) result = OH_ArkUI_NodeContent_AddNode(content, node);
        if (result == ARKUI_ERROR_CODE_NO_ERROR) {
            if (gCallback) napi_delete_reference(gEnv, gCallback);
            gEnv = env;
            napi_create_reference(env, args[1], 1, &gCallback);
        }
    }
    OH_LOG_INFO(LOG_APP, "COASTING_WATCH result=%{public}d", result);
    napi_value value;
    napi_create_int32(env, result, &value);
    return value;
}
