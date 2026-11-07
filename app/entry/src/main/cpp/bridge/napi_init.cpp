#include <napi/native_api.h>

#include "bridge/plugin_manager.h"
#include "compositor/data_device.h"
#include "compositor/input/input_space_mapper.h"
#include "compositor/portal/session_bus.h"
#include "compositor/titlebar.h"
#include "compositor/wayland_server.h"
#include "graphics/egl_renderer.h"
#include "graphics/graphics_broker.h"
#include "graphics/present_stats.h"
#include "input/coasting.h"
#include "input/input_manager.h"
#include "input/text_input.h"
#include "runtime/app_registry.h"
#include "runtime/native_child_launcher.h"
#include "runtime/tar_extract.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <pty.h>
#include <unistd.h>
#include <array>
#include <fstream>

extern "C" {
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct engine_st ENGINE;
EVP_PKEY_CTX* EVP_PKEY_CTX_new_id(int id, ENGINE* engine);
int EVP_PKEY_keygen_init(EVP_PKEY_CTX* context);
int EVP_PKEY_keygen(EVP_PKEY_CTX* context, EVP_PKEY** key);
int EVP_PKEY_CTX_set_rsa_keygen_bits(EVP_PKEY_CTX* context, int bits);
typedef struct bignum_st BIGNUM;
int EVP_PKEY_get_bn_param(const EVP_PKEY* key, const char* name, BIGNUM** value);
int BN_num_bits(const BIGNUM* value);
int BN_bn2bin(const BIGNUM* value, unsigned char* output);
void BN_free(BIGNUM* value);
int EVP_PKEY_get_raw_public_key(const EVP_PKEY* key, unsigned char* output, size_t* length);
int EVP_PKEY_get_raw_private_key(const EVP_PKEY* key, unsigned char* output, size_t* length);
int EVP_EncodeBlock(unsigned char* output, const unsigned char* input, int length);
void EVP_PKEY_CTX_free(EVP_PKEY_CTX* context);
void EVP_PKEY_free(EVP_PKEY* key);
int PEM_write_PrivateKey(FILE* file, const EVP_PKEY* key, const void* cipher,
    unsigned char* password, int length, void* callback, void* userdata);
}

void AppendUint32(std::vector<unsigned char>& output, uint32_t value)
{
    output.insert(output.end(), {
        static_cast<unsigned char>(value >> 24),
        static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value),
    });
}

void AppendString(std::vector<unsigned char>& output, const void* data, size_t length)
{
    AppendUint32(output, length);
    const auto* bytes = static_cast<const unsigned char*>(data);
    output.insert(output.end(), bytes, bytes + length);
}

void AppendString(std::vector<unsigned char>& output, const char* value)
{
    AppendString(output, value, strlen(value));
}

void WriteOpenSshKey(const char* path, const std::array<unsigned char, 32>& publicKey,
    const std::array<unsigned char, 32>& privateSeed)
{
    std::vector<unsigned char> publicBlob;
    AppendString(publicBlob, "ssh-ed25519");
    AppendString(publicBlob, publicKey.data(), publicKey.size());

    std::vector<unsigned char> privateBlob;
    AppendUint32(privateBlob, 0x47484f53);
    AppendUint32(privateBlob, 0x47484f53);
    AppendString(privateBlob, "ssh-ed25519");
    AppendString(privateBlob, publicKey.data(), publicKey.size());
    std::array<unsigned char, 64> expandedPrivate{};
    memcpy(expandedPrivate.data(), privateSeed.data(), privateSeed.size());
    memcpy(expandedPrivate.data() + privateSeed.size(), publicKey.data(), publicKey.size());
    AppendString(privateBlob, expandedPrivate.data(), expandedPrivate.size());
    AppendString(privateBlob, "ghostty-hap-runtime");
    for (unsigned char padding = 1; privateBlob.size() % 8 != 0; ++padding) {
        privateBlob.push_back(padding);
    }

    std::vector<unsigned char> container;
    AppendString(container, "openssh-key-v1\0", 15);
    AppendString(container, "none");
    AppendString(container, "none");
    AppendString(container, "", 0);
    AppendUint32(container, 1);
    AppendString(container, publicBlob.data(), publicBlob.size());
    AppendString(container, privateBlob.data(), privateBlob.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(container.data()), container.size());
}

namespace {
std::string StringArg(napi_env env, napi_value value);
}

// ensureSshKey(directory): the app's key for the HiShell sshd, kept in the
// app's ~/.ssh so it survives a cache clear and goes with a backup. A key
// from the cache, where earlier runtimes kept it, moves there and stays
// authorized.
napi_value EnsureSshKey(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        napi_throw_error(env, nullptr, "ensureSshKey requires a directory");
        return nullptr;
    }
    const std::string directory = StringArg(env, args[0]);
    mkdir(directory.c_str(), S_IRWXU);
    const std::string privateKey = directory + "/hishell_rsa";
    const std::string publicKey = privateKey + ".pub";
    const char* privatePath = privateKey.c_str();
    const char* publicPath = publicKey.c_str();
    if (access(privatePath, R_OK) != 0 &&
        rename("/data/storage/el2/base/cache/ghostty_hap_rsa", privatePath) == 0) {
        rename("/data/storage/el2/base/cache/ghostty_hap_rsa.pub", publicPath);
    }
    if (access(privatePath, R_OK) != 0 || access(publicPath, R_OK) != 0) {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(6, nullptr);
        EVP_PKEY* key = nullptr;
        if (!context || EVP_PKEY_keygen_init(context) != 1 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 3072) != 1 ||
            EVP_PKEY_keygen(context, &key) != 1) {
            EVP_PKEY_CTX_free(context);
            napi_throw_error(env, nullptr, "OpenSSL RSA key generation failed");
            return nullptr;
        }
        FILE* privateFile = fopen(privatePath, "w");
        const int writeResult = privateFile
            ? PEM_write_PrivateKey(privateFile, key, nullptr, nullptr, 0, nullptr, nullptr)
            : 0;
        if (privateFile) fclose(privateFile);
        BIGNUM* modulus = nullptr;
        BIGNUM* exponent = nullptr;
        if (writeResult != 1 ||
            EVP_PKEY_get_bn_param(key, "n", &modulus) != 1 ||
            EVP_PKEY_get_bn_param(key, "e", &exponent) != 1) {
            EVP_PKEY_free(key);
            EVP_PKEY_CTX_free(context);
            napi_throw_error(env, nullptr, "Cannot write or inspect app RSA key");
            return nullptr;
        }
        std::vector<unsigned char> publicBlob;
        AppendString(publicBlob, "ssh-rsa");
        for (BIGNUM* parameter : {exponent, modulus}) {
            std::vector<unsigned char> bytes((BN_num_bits(parameter) + 7) / 8);
            BN_bn2bin(parameter, bytes.data());
            if (!bytes.empty() && (bytes[0] & 0x80)) bytes.insert(bytes.begin(), 0);
            AppendString(publicBlob, bytes.data(), bytes.size());
        }
        BN_free(modulus);
        BN_free(exponent);
        std::array<unsigned char, 640> encoded{};
        if (EVP_EncodeBlock(encoded.data(), publicBlob.data(), publicBlob.size()) <= 0) {
            EVP_PKEY_free(key);
            EVP_PKEY_CTX_free(context);
            napi_throw_error(env, nullptr, "Cannot encode app RSA public key");
            return nullptr;
        }
        std::ofstream publicFile(publicPath);
        publicFile << "ssh-rsa " << encoded.data() << " ghostty-hap-runtime\n";
        EVP_PKEY_free(key);
        EVP_PKEY_CTX_free(context);
    }
    if (chmod(privatePath, S_IRUSR | S_IWUSR) != 0) {
        napi_throw_error(env, nullptr, "Cannot restrict app SSH private key permissions");
        return nullptr;
    }
    std::ifstream publicFile(publicPath);
    std::string line;
    std::getline(publicFile, line);
    napi_value result;
    napi_create_string_utf8(env, line.c_str(), line.size(), &result);
    return result;
}

napi_value SendText(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string text = StringArg(env, args[0]);
    TextInput::GetInstance()->SendCommitString(text.c_str());
    return nullptr;
}

napi_value ProbePty(napi_env env, napi_callback_info)
{
    int master = -1;
    int slave = -1;
    int result = openpty(&master, &slave, nullptr, nullptr, nullptr);
    int error = result == 0 ? 0 : errno;
    if (result == 0) {
        close(master);
        close(slave);
    }
    napi_value value;
    napi_create_int32(env, error, &value);
    return value;
}

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

namespace {
std::atomic<napi_threadsafe_function> gStateCallback{nullptr};
std::atomic<napi_threadsafe_function> gToplevelCallback{nullptr};
std::atomic<uint32_t> gToplevelId{0};
// Last XComponent surface size, so a toplevel bound later is configured to fill it.
std::atomic<int32_t> gRendererWidth{0};
std::atomic<int32_t> gRendererHeight{0};
ContentInsets gContentInsets;

// The size apps lay out at: the view without the insets.
int32_t ContentWidth() { return gRendererWidth.load() - gContentInsets.left - gContentInsets.right; }
int32_t ContentHeight() { return gRendererHeight.load() - gContentInsets.top - gContentInsets.bottom; }

struct ToplevelEvent {
    uint32_t id;
    std::string name;
    std::string json;
};

void Throw(napi_env env, const std::string& message)
{
    napi_throw_error(env, nullptr, message.c_str());
}

std::string StringArg(napi_env env, napi_value value)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
    std::string result(length, '\0');
    napi_get_value_string_utf8(env, value, result.data(), length + 1, nullptr);
    return result;
}

std::vector<std::string> StringArray(napi_env env, napi_value value)
{
    std::vector<std::string> result;
    bool isArray = false;
    napi_is_array(env, value, &isArray);
    if (!isArray) return result;
    uint32_t length = 0;
    napi_get_array_length(env, value, &length);
    result.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        napi_value item = nullptr;
        napi_get_element(env, value, i, &item);
        result.push_back(StringArg(env, item));
    }
    return result;
}

void CallState(napi_env env, napi_value callback, void*, void* data)
{
    auto* state = static_cast<std::string*>(data);
    if (env && callback) {
        napi_value undefined;
        napi_value value;
        napi_get_undefined(env, &undefined);
        napi_create_string_utf8(env, state->c_str(), NAPI_AUTO_LENGTH, &value);
        napi_call_function(env, undefined, callback, 1, &value, nullptr);
    }
    delete state;
}

void CallToplevel(napi_env env, napi_value callback, void*, void* data)
{
    auto* event = static_cast<ToplevelEvent*>(data);
    if (env && callback) {
        napi_value undefined;
        napi_value id;
        napi_value name;
        napi_value json;
        napi_get_undefined(env, &undefined);
        napi_create_uint32(env, event->id, &id);
        napi_create_string_utf8(env, event->name.c_str(), NAPI_AUTO_LENGTH, &name);
        napi_create_string_utf8(env, event->json.c_str(), NAPI_AUTO_LENGTH, &json);
        napi_value args[] = {id, name, json};
        napi_call_function(env, undefined, callback, 3, args, nullptr);
    }
    delete event;
}

napi_value SetStateCallback(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        Throw(env, "setStateCallback requires one function");
        return nullptr;
    }
    if (auto old = gStateCallback.exchange(nullptr)) napi_release_threadsafe_function(old, napi_tsfn_release);
    napi_value name;
    napi_create_string_utf8(env, "OhosWaylandState", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function callback = nullptr;
    if (napi_create_threadsafe_function(env, args[0], nullptr, name, 0, 1, nullptr,
                                        nullptr, nullptr, CallState, &callback) != napi_ok) return nullptr;
    gStateCallback.store(callback);
    WaylandServer::GetInstance()->SetStateCallback([](const char* state) {
        if (auto callback = gStateCallback.load())
            napi_call_threadsafe_function(callback, new std::string(state), napi_tsfn_nonblocking);
    });
    return nullptr;
}

// Portal dialogs (compositor/portal): requests reuse the toplevel event shape,
// with the request id, the dialog kind, and its JSON description.
std::atomic<napi_threadsafe_function> gPortalCallback{nullptr};

// startSessionBus(socketPath, callback(id, kind, json)): the apps' D-Bus
// session bus with the portal; HarmonyOS dialogs are shown by the callback.
napi_value StartSessionBus(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    napi_value result;
    if (argc != 2) {
        Throw(env, "startSessionBus requires a socket path and a function");
        return nullptr;
    }
    if (auto old = gPortalCallback.exchange(nullptr)) napi_release_threadsafe_function(old, napi_tsfn_release);
    napi_value name;
    napi_create_string_utf8(env, "OhosWaylandPortal", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function callback = nullptr;
    if (napi_create_threadsafe_function(env, args[1], nullptr, name, 0, 1, nullptr,
                                        nullptr, nullptr, CallToplevel, &callback) != napi_ok) return nullptr;
    gPortalCallback.store(callback);
    const bool started = portal::StartSessionBus(StringArg(env, args[0]),
        [](uint32_t id, const std::string& kind, const std::string& json) {
            if (auto callback = gPortalCallback.load()) {
                napi_call_threadsafe_function(callback, new ToplevelEvent{id, kind, json}, napi_tsfn_nonblocking);
            }
        });
    napi_get_boolean(env, started, &result);
    return result;
}

// portalRespond(id, response, uris): the dialog's answer (0 chosen,
// 1 cancelled, 2 failed) and the chosen files as file:// URIs.
napi_value PortalRespond(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 3) return nullptr;
    uint32_t id = 0;
    uint32_t response = 2;
    napi_get_value_uint32(env, args[0], &id);
    napi_get_value_uint32(env, args[1], &response);
    std::vector<std::string> uris;
    uint32_t count = 0;
    napi_get_array_length(env, args[2], &count);
    for (uint32_t i = 0; i < count; ++i) {
        napi_value element;
        napi_get_element(env, args[2], i, &element);
        uris.push_back(StringArg(env, element));
    }
    portal::CompleteRequest(id, response, uris);
    return nullptr;
}

std::atomic<napi_threadsafe_function> gPasteboardCallback{nullptr};

void CallPasteboard(napi_env env, napi_value callback, void*, void*)
{
    if (!env || !callback) return;
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, callback, 0, nullptr, nullptr);
}

// setPasteboardCallback(callback()): called when an app pastes before
// ohos.permission.READ_PASTEBOARD is granted; ArkTS asks the user and
// answers with pasteboardPermission(granted).
napi_value SetPasteboardCallback(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        Throw(env, "setPasteboardCallback requires one function");
        return nullptr;
    }
    if (auto old = gPasteboardCallback.exchange(nullptr)) napi_release_threadsafe_function(old, napi_tsfn_release);
    napi_value name;
    napi_create_string_utf8(env, "OhosWaylandPasteboard", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function callback = nullptr;
    if (napi_create_threadsafe_function(env, args[0], nullptr, name, 0, 1, nullptr,
                                        nullptr, nullptr, CallPasteboard, &callback) != napi_ok) return nullptr;
    gPasteboardCallback.store(callback);
    clipboard::SetPermissionRequester([] {
        if (auto callback = gPasteboardCallback.load()) {
            napi_call_threadsafe_function(callback, nullptr, napi_tsfn_nonblocking);
        }
    });
    return nullptr;
}

napi_value PasteboardPermission(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool granted = false;
    if (argc == 1) napi_get_value_bool(env, args[0], &granted);
    clipboard::OnPermissionResult(granted);
    return nullptr;
}

napi_value SetToplevelCallback(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        Throw(env, "setToplevelCallback requires one function");
        return nullptr;
    }
    if (auto old = gToplevelCallback.exchange(nullptr)) napi_release_threadsafe_function(old, napi_tsfn_release);
    napi_value name;
    napi_create_string_utf8(env, "OhosWaylandToplevel", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function callback = nullptr;
    if (napi_create_threadsafe_function(env, args[0], nullptr, name, 0, 1, nullptr,
                                        nullptr, nullptr, CallToplevel, &callback) != napi_ok) return nullptr;
    gToplevelCallback.store(callback);
    WaylandServer::GetInstance()->SetToplevelCallback([](uint32_t id, const char* name, const char* json) {
        if (auto callback = gToplevelCallback.load()) {
            napi_call_threadsafe_function(callback, new ToplevelEvent{id, name, json}, napi_tsfn_nonblocking);
        }
    });
    return nullptr;
}

napi_value StartServer(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        Throw(env, "startServer requires a socket path");
        return nullptr;
    }
    const std::string path = StringArg(env, args[0]);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) mkdir(path.substr(0, slash).c_str(), 0700);
    gToplevelId.store(0);
    const bool started = !path.empty() && WaylandServer::GetInstance()->Start(path);
    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

napi_value StopServer(napi_env env, napi_callback_info)
{
    WaylandServer::GetInstance()->Stop();
    gToplevelId.store(0);
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}

napi_value CreateRenderer(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t surfaceId = 0;
    bool lossless = false;
    if (argc != 1 || napi_get_value_bigint_int64(env, args[0], &surfaceId, &lossless) != napi_ok || !lossless) {
        Throw(env, "createRenderer requires a surfaceId bigint");
        return nullptr;
    }
    PluginManager::GetInstance()->CreateRenderer(gToplevelId.load(), surfaceId);
    return nullptr;
}

napi_value ResizeRenderer(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t width = 0;
    int32_t height = 0;
    if (argc != 2 || napi_get_value_int32(env, args[0], &width) != napi_ok ||
        napi_get_value_int32(env, args[1], &height) != napi_ok || width <= 0 || height <= 0) {
        Throw(env, "resizeRenderer requires positive dimensions");
        return nullptr;
    }
    gRendererWidth.store(width);
    gRendererHeight.store(height);
    // GTK limits toplevels to the advertised output mode, so it tracks the view.
    const int32_t contentWidth = ContentWidth();
    const int32_t contentHeight = ContentHeight();
    if (contentWidth > 0 && contentHeight > 0) WaylandServer::GetInstance()->SetOutputSize(contentWidth, contentHeight);
    PluginManager::GetInstance()->ResizeRenderer(gToplevelId.load(), width, height);
    const int32_t scale = WaylandServer::GetInstance()->OutputScale();
    WaylandServer::GetInstance()->NotifyToplevelResize(gToplevelId.load(), contentWidth / scale, contentHeight / scale);
    return nullptr;
}

napi_value DestroyRenderer(napi_env env, napi_callback_info)
{
    PluginManager::GetInstance()->DestroyToplevel(gToplevelId.load());
    return nullptr;
}

// setToplevelMaximized(on): the HarmonyOS window was maximized or restored.
// The client hears it in a configure, so CSD apps drop corners and shadows.
napi_value SetToplevelMaximized(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool on = false;
    if (argc == 1) napi_get_value_bool(env, args[0], &on);
    const uint32_t id = gToplevelId.load();
    const int32_t width = ContentWidth();
    const int32_t height = ContentHeight();
    const int32_t scale = WaylandServer::GetInstance()->OutputScale();
    if (id == 0 || width <= 0 || height <= 0) return nullptr;
    WaylandServer::GetInstance()->SetToplevelMaximizedState(id, on);
    WaylandServer::GetInstance()->NotifyToplevelResize(id, width / scale, height / scale);
    return nullptr;
}

// activateTitlebar(message): the user activated an item of the native title
// bar ("serial\tid\taction\ttarget", see ohos-titlebar-v1.xml).
napi_value ActivateTitlebarItem(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    size_t length = 0;
    if (argc != 1 || napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok) return nullptr;
    std::string json(length, '\0');
    napi_get_value_string_utf8(env, args[0], json.data(), length + 1, &length);
    ActivateTitlebar(gToplevelId.load(), json);
    return nullptr;
}

// closeToplevel(): asks the app to close its window, as its own close button
// would. Returns false when no toplevel is bound.
napi_value CloseToplevel(napi_env env, napi_callback_info)
{
    const uint32_t id = gToplevelId.load();
    if (id != 0) WaylandServer::GetInstance()->SendToplevelClose(id);
    napi_value result;
    napi_get_boolean(env, id != 0, &result);
    return result;
}

napi_value BindToplevel(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc == 1) napi_get_value_uint32(env, args[0], &id);
    const uint32_t previous = gToplevelId.exchange(id);
    if (previous != id) PluginManager::GetInstance()->MoveRendererToToplevel(previous, id);
    const int32_t width = ContentWidth();
    const int32_t height = ContentHeight();
    const int32_t scale = WaylandServer::GetInstance()->OutputScale();
    if (id && previous != id && width > 0 && height > 0)
        WaylandServer::GetInstance()->NotifyToplevelResize(id, width / scale, height / scale);
    return nullptr;
}

// extractTar(fd, offset, length, dest): unpacks a rawfile tar (from
// ResourceManager getRawFd) into dest. Returns entries written or -errno.
napi_value ExtractTar(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t fd = -1;
    int64_t offset = 0;
    int64_t length = 0;
    char dest[1024] = {};
    size_t destLength = 0;
    int32_t result = -EINVAL;
    if (argc == 4 && napi_get_value_int32(env, args[0], &fd) == napi_ok &&
        napi_get_value_int64(env, args[1], &offset) == napi_ok &&
        napi_get_value_int64(env, args[2], &length) == napi_ok &&
        napi_get_value_string_utf8(env, args[3], dest, sizeof(dest), &destLength) == napi_ok) {
        result = ohoswayland::ExtractTar(fd, offset, length, std::string(dest, destLength));
    }
    napi_value value;
    napi_create_int32(env, result, &value);
    return value;
}

napi_value CaptureFrame(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {};
    size_t length = 0;
    if (argc == 1 && napi_get_value_string_utf8(env, args[0], path, sizeof(path), &length) == napi_ok) {
        RequestGpuFrameCapture(std::string(path, length));
    }
    return nullptr;
}

napi_value SetOutputScale(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t scale = 1;
    if (argc == 1) napi_get_value_int32(env, args[0], &scale);
    WaylandServer::GetInstance()->SetOutputScale(scale);
    return nullptr;
}

// setContentInsets(left, top, right, bottom): output pixels between the view's
// edges and the app. The renderer continues the app's edge pixels across them.
napi_value SetContentInsets(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    ContentInsets insets;
    if (argc != 4 || napi_get_value_int32(env, args[0], &insets.left) != napi_ok ||
        napi_get_value_int32(env, args[1], &insets.top) != napi_ok ||
        napi_get_value_int32(env, args[2], &insets.right) != napi_ok ||
        napi_get_value_int32(env, args[3], &insets.bottom) != napi_ok) {
        Throw(env, "setContentInsets requires left, top, right, and bottom");
        return nullptr;
    }
    gContentInsets = insets;
    PluginManager::GetInstance()->SetContentInsets(insets);
    const int32_t width = ContentWidth();
    const int32_t height = ContentHeight();
    if (width <= 0 || height <= 0) return nullptr;
    WaylandServer::GetInstance()->SetOutputSize(width, height);
    const int32_t scale = WaylandServer::GetInstance()->OutputScale();
    if (gToplevelId.load()) WaylandServer::GetInstance()->NotifyToplevelResize(gToplevelId.load(), width / scale, height / scale);
    return nullptr;
}

napi_value SetOutputSize(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t width = 0;
    int32_t height = 0;
    if (argc == 2) {
        napi_get_value_int32(env, args[0], &width);
        napi_get_value_int32(env, args[1], &height);
    }
    if (width > 0 && height > 0) WaylandServer::GetInstance()->SetOutputSize(width, height);
    return nullptr;
}

napi_value SendPointerEvent(napi_env env, napi_callback_info info)
{
    size_t argc = 7;
    napi_value args[7];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    int32_t action = 0;
    int32_t button = 0;
    double x = 0;
    double y = 0;
    double rawX = 0;
    double rawY = 0;
    napi_get_value_int32(env, args[0], &action);
    napi_get_value_double(env, args[1], &x);
    napi_get_value_double(env, args[2], &y);
    napi_get_value_int32(env, args[3], &button);
    if (argc >= 6) {
        napi_get_value_double(env, args[4], &rawX);
        napi_get_value_double(env, args[5], &rawY);
    }
    bool fromMouse = true;
    if (argc >= 7) napi_get_value_bool(env, args[6], &fromMouse);
    InputManager::GetInstance()->SendPointerEvent(gToplevelId.load(), action, x, y, button,
                                                  rawX, rawY, fromMouse);
    return nullptr;
}

napi_value SendKeyEvent(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 2) return nullptr;
    int32_t key = 0;
    bool pressed = false;
    napi_get_value_int32(env, args[0], &key);
    napi_get_value_bool(env, args[1], &pressed);
    InputManager::GetInstance()->SendKeyEvent(gToplevelId.load(), key, pressed);
    return nullptr;
}

napi_value SendScrollEvent(napi_env env, napi_callback_info info)
{
    size_t argc = 7;
    napi_value args[7];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 7) return nullptr;
    int32_t axis = 0;
    int32_t step = 0;
    double value = 0;
    double x = 0;
    double y = 0;
    napi_get_value_int32(env, args[0], &axis);
    napi_get_value_double(env, args[1], &value);
    napi_get_value_int32(env, args[2], &step);
    napi_get_value_double(env, args[3], &x);
    napi_get_value_double(env, args[4], &y);
    uint32_t source = 0;
    bool stop = false;
    napi_get_value_uint32(env, args[5], &source);
    napi_get_value_bool(env, args[6], &stop);
    InputManager::GetInstance()->SendScrollEvent(gToplevelId.load(), axis, value, step, x, y, source, stop);
    return nullptr;
}

// dragEvent(kind, x, y, mimeTypes, payloads): a HarmonyOS drag over the app,
// kind 0 enter, 1 move, 2 leave, 3 drop, at physical px on the surface.
// payloads pairs with mimeTypes and is read only on a drop.
napi_value DragEvent(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 5) return nullptr;
    int32_t kind = 0;
    double x = 0;
    double y = 0;
    napi_get_value_int32(env, args[0], &kind);
    napi_get_value_double(env, args[1], &x);
    napi_get_value_double(env, args[2], &y);
    if (kind < 0 || kind > 3) return nullptr;
    const uint32_t toplevel = gToplevelId.load();
    wl_fixed_t surfaceX = 0;
    wl_fixed_t surfaceY = 0;
    InputSpaceMapper::GetInstance()->CoordTransform(x, y, toplevel, &surfaceX, &surfaceY);
    dnd::Event event{static_cast<dnd::Kind>(kind), toplevel, surfaceX, surfaceY, StringArray(env, args[3]), {}};
    const std::vector<std::string> payloads = StringArray(env, args[4]);
    for (size_t i = 0; i < payloads.size() && i < event.mimeTypes.size(); ++i) {
        event.payloads.emplace_back(event.mimeTypes[i], payloads[i]);
    }
    dnd::Post(std::move(event));
    return nullptr;
}

// presentedFps(): client frames presented in the last second, for the overlay's numeral,
// which reads it every display frame.
napi_value PresentedFps(napi_env env, napi_callback_info)
{
    napi_value result;
    napi_create_int32(env, ohoswayland::PresentStats::Instance().FramesInLastSecond(ohoswayland::SteadyNowNs()),
                      &result);
    return result;
}

// Resident memory of a process in bytes, or -1 when /proc does not show it.
double ResidentBytes(int32_t pid)
{
    if (pid <= 0) return -1;
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) return std::strtod(line.c_str() + 6, nullptr) * 1024;
    }
    return -1;
}

// getDiagnostics(pid): what the diagnostics overlay shows (PresentStats),
// with the resident memory of the app process pid.
napi_value GetDiagnostics(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t pid = 0;
    if (argc == 1) napi_get_value_int32(env, args[0], &pid);
    const auto snapshot = ohoswayland::PresentStats::Instance().Snapshot(ohoswayland::SteadyNowNs());
    napi_value result;
    napi_create_object(env, &result);
    const auto set = [&](const char* name, double number) {
        napi_value value;
        napi_create_double(env, number, &value);
        napi_set_named_property(env, result, name, value);
    };
    set("fps", snapshot.frames);
    set("presented", static_cast<double>(snapshot.presented));
    set("refreshHz", snapshot.refreshHz);
    set("frameMs", snapshot.meanIntervalMs);
    set("worstFrameMs", snapshot.worstIntervalMs);
    napi_value intervals;
    napi_create_array_with_length(env, snapshot.intervalsMs.size(), &intervals);
    for (size_t i = 0; i < snapshot.intervalsMs.size(); ++i) {
        napi_value interval;
        napi_create_double(env, snapshot.intervalsMs[i], &interval);
        napi_set_element(env, intervals, i, interval);
    }
    napi_set_named_property(env, result, "intervalsMs", intervals);
    set("inputMs", snapshot.inputToFrameMs);
    set("bufferWidth", snapshot.buffer.bufferW);
    set("bufferHeight", snapshot.buffer.bufferH);
    set("clientScale", snapshot.buffer.clientScale);
    set("pixelRatio", snapshot.buffer.pixelRatio);
    set("rssBytes", ResidentBytes(pid));
    const char* path = snapshot.buffer.path == ohoswayland::PresentPath::GpuDirect ? "gpu"
        : snapshot.buffer.path == ohoswayland::PresentPath::Shm ? "shm" : "";
    napi_value pathValue;
    napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &pathValue);
    napi_set_named_property(env, result, "path", pathValue);
    return result;
}

napi_value GetGraphicsCapabilities(napi_env env, napi_callback_info)
{
    const auto caps = EglRenderer::ProbeCapabilities();
    napi_value result;
    napi_create_object(env, &result);
    const auto set = [&](const char* name, bool supported) {
        napi_value value;
        napi_get_boolean(env, supported, &value);
        napi_set_named_property(env, result, name, value);
    };
    set("egl", caps.egl);
    set("gles3", caps.gles3);
    set("nativeVsync", caps.nativeVsync);
    set("zeroCopyPresentation", caps.zeroCopyPresentation);
    set("desktopGl", caps.desktopGl);
    set("vulkan", caps.vulkan);
    set("shmDiagnostic", caps.shmDiagnostic);
    return result;
}

napi_value StartGpuHost(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1) {
        Throw(env, "startGpuHost requires a runtime directory");
        return nullptr;
    }
    auto& broker = ohoswayland::GraphicsBroker::Instance();
    if (!broker.Start(StringArg(env, args[0]))) {
        Throw(env, broker.State().error);
        return nullptr;
    }
    PluginManager::GetInstance()->SetGpuDemoEnabled(true);
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

napi_value StopGpuHost(napi_env env, napi_callback_info)
{
    PluginManager::GetInstance()->SetGpuDemoEnabled(false);
    ohoswayland::GraphicsBroker::Instance().Stop();
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}

napi_value GetGpuGuestEnvironment(napi_env env, napi_callback_info)
{
    const auto lines = ohoswayland::GraphicsBroker::Instance().GuestEnvironment();
    napi_value result;
    napi_create_array_with_length(env, lines.size(), &result);
    for (size_t i = 0; i < lines.size(); ++i) {
        napi_value value;
        napi_create_string_utf8(env, lines[i].c_str(), NAPI_AUTO_LENGTH, &value);
        napi_set_element(env, result, i, value);
    }
    return result;
}

napi_value LaunchChild(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 5) {
        Throw(env, "launchChild requires app id, library, entry point, args, and env");
        return nullptr;
    }
    ohoswayland::AppRecord record;
    record.appId = StringArg(env, args[0]);
    record.packagePath = StringArg(env, args[1]);
    record.entryPoint = StringArg(env, args[2]);
    record.args = StringArray(env, args[3]);
    record.env = StringArray(env, args[4]);
    std::string error;
    const int32_t pid = ohoswayland::NativeChildLauncher::Instance().Launch(std::move(record), error);
    if (pid <= 0) {
        Throw(env, error);
        return nullptr;
    }
    napi_value result;
    napi_create_int32(env, pid, &result);
    return result;
}

napi_value StopChild(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc == 1) {
        std::string error;
        ohoswayland::NativeChildLauncher::Instance().Stop(StringArg(env, args[0]), error);
        if (!error.empty()) Throw(env, error);
    }
    return nullptr;
}

napi_value GetApps(napi_env env, napi_callback_info)
{
    const auto records = ohoswayland::AppRegistry::Instance().List();
    napi_value result;
    napi_create_array_with_length(env, records.size(), &result);
    for (size_t i = 0; i < records.size(); ++i) {
        napi_value record;
        napi_create_object(env, &record);
        napi_value appId;
        napi_value packagePath;
        napi_value entryPoint;
        napi_create_string_utf8(env, records[i].appId.c_str(), NAPI_AUTO_LENGTH, &appId);
        napi_create_string_utf8(env, records[i].packagePath.c_str(), NAPI_AUTO_LENGTH, &packagePath);
        napi_create_string_utf8(env, records[i].entryPoint.c_str(), NAPI_AUTO_LENGTH, &entryPoint);
        napi_set_named_property(env, record, "appId", appId);
        napi_set_named_property(env, record, "packagePath", packagePath);
        napi_set_named_property(env, record, "entryPoint", entryPoint);
        napi_set_element(env, result, i, record);
    }
    return result;
}

napi_value RegisterApp(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 5) {
        Throw(env, "registerApp requires app id, library, entry point, args, and env");
        return nullptr;
    }
    ohoswayland::AppRecord record;
    record.appId = StringArg(env, args[0]);
    record.packagePath = StringArg(env, args[1]);
    record.entryPoint = StringArg(env, args[2]);
    record.args = StringArray(env, args[3]);
    record.env = StringArray(env, args[4]);
    try {
        ohoswayland::AppRegistry::Instance().Register(std::move(record));
    } catch (const std::exception& error) {
        Throw(env, error.what());
    }
    return nullptr;
}

napi_value Init(napi_env env, napi_value exports)
{
    const napi_property_descriptor properties[] = {
        {"startServer", nullptr, StartServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopServer", nullptr, StopServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelCallback", nullptr, SetToplevelCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startSessionBus", nullptr, StartSessionBus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"portalRespond", nullptr, PortalRespond, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPasteboardCallback", nullptr, SetPasteboardCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pasteboardPermission", nullptr, PasteboardPermission, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"dragEvent", nullptr, DragEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentedFps", nullptr, PresentedFps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createRenderer", nullptr, CreateRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeRenderer", nullptr, ResizeRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyRenderer", nullptr, DestroyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"bindToplevel", nullptr, BindToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOutputScale", nullptr, SetOutputScale, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOutputSize", nullptr, SetOutputSize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setContentInsets", nullptr, SetContentInsets, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendPointerEvent", nullptr, SendPointerEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKeyEvent", nullptr, SendKeyEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendScrollEvent", nullptr, SendScrollEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"watchCoasting", nullptr, WatchCoasting, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getGraphicsCapabilities", nullptr, GetGraphicsCapabilities, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDiagnostics", nullptr, GetDiagnostics, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startGpuHost", nullptr, StartGpuHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopGpuHost", nullptr, StopGpuHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getGpuGuestEnvironment", nullptr, GetGpuGuestEnvironment, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchChild", nullptr, LaunchChild, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopChild", nullptr, StopChild, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getApps", nullptr, GetApps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerApp", nullptr, RegisterApp, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probePty", nullptr, ProbePty, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ensureSshKey", nullptr, EnsureSshKey, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendText", nullptr, SendText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"extractTar", nullptr, ExtractTar, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelMaximized", nullptr, SetToplevelMaximized, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"closeToplevel", nullptr, CloseToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"activateTitlebar", nullptr, ActivateTitlebarItem, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"captureFrame", nullptr, CaptureFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}
}

napi_module gModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {nullptr},
};

extern "C" __attribute__((constructor)) void RegisterOhosWaylandModule()
{
    napi_module_register(&gModule);
}
