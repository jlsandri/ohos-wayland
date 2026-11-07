#include "native_child_launcher.h"

#include "graphics/graphics_broker.h"
#include "runtime/launch_protocol.h"
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <algorithm>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Child"

namespace ohoswayland {
namespace {
NativeChildLauncher* gLauncher = nullptr;

void ChildExited(int32_t pid, int32_t signal)
{
    OH_LOG_INFO(LOG_APP, "[NCP] exited pid=%{public}d signal=%{public}d", pid, signal);
    GraphicsBroker::Instance().OnClientExit(pid);
    if (gLauncher) gLauncher->OnExit(pid);
}
}

NativeChildLauncher& NativeChildLauncher::Instance()
{
    static NativeChildLauncher launcher;
    return launcher;
}

int32_t NativeChildLauncher::Launch(AppRecord record, std::string& error)
{
    if (record.appId.empty() || record.packagePath.empty() || record.entryPoint.empty()) {
        error = "app id, package path, and entry point are required";
        return -1;
    }
    {
        // Relaunching an app id replaces it; the exit callback may not have
        // reported the old process yet.
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = apps_.find(record.appId);
        if (it != apps_.end()) {
            for (const int32_t pid : it->second.pids) OH_Ability_KillChildProcess(pid);
            apps_.erase(it);
        }
    }

    std::string params;
    AppendLaunchField(params, "OWP1");
    AppendLaunchField(params, record.appId);
    AppendLaunchField(params, record.packagePath);
    AppendLaunchField(params, record.entryPoint);
    AppendLaunchField(params, std::to_string(record.args.size()));
    for (const auto& arg : record.args) AppendLaunchField(params, arg);
    AppendLaunchField(params, std::to_string(record.env.size()));
    for (const auto& env : record.env) AppendLaunchField(params, env);

    // Direct-window clients need an IPC channel to receive their OHNativeWindow,
    // and only the direct host can exec a program from the app's HNP.
    const bool direct = record.entryPoint == "exec" ||
                        std::find(record.env.begin(), record.env.end(), "OHOS_WAYLAND_DIRECT_WINDOW=1") !=
                            record.env.end();
    int32_t pid = -1;
    if (direct) {
        pid = GraphicsBroker::Instance().LaunchDirect(params, error);
        if (pid <= 0) return -1;
    } else {
        NativeChildProcess_Args args = {};
        args.entryParams = params.data();
        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        const std::string entry = record.packagePath + ":" + record.entryPoint;
        const auto result = OH_Ability_StartNativeChildProcess(entry.c_str(), args, options, &pid);
        if (result != NCP_NO_ERROR || pid <= 0) {
            error = "OH_Ability_StartNativeChildProcess returned " + std::to_string(result);
            return -1;
        }
    }

    record.pids.push_back(pid);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        apps_.emplace(record.appId, std::move(record));
        if (!gLauncher) {
            gLauncher = this;
            OH_Ability_RegisterNativeChildProcessExitCallback(ChildExited);
        }
    }
    OH_LOG_INFO(LOG_APP, "[NCP] launched pid=%{public}d", pid);
    return pid;
}

void NativeChildLauncher::Stop(const std::string& appId, std::string& error)
{
    std::vector<int32_t> pids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = apps_.find(appId);
        if (it == apps_.end()) return;
        pids = it->second.pids;
        apps_.erase(it);
    }
    // Children started through the IPC variant never report their exit, so
    // a pid may already be gone. Stopping is idempotent; failures are logged.
    for (const int32_t pid : pids) {
        const auto result = OH_Ability_KillChildProcess(pid);
        if (result != NCP_NO_ERROR)
            OH_LOG_WARN(LOG_APP, "[NCP] stop pid=%{public}d result=%{public}d", pid, result);
        GraphicsBroker::Instance().OnClientExit(pid);
    }
    static_cast<void>(error);
}

void NativeChildLauncher::OnExit(int32_t pid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = apps_.begin(); it != apps_.end();) {
        auto& pids = it->second.pids;
        pids.erase(std::remove(pids.begin(), pids.end(), pid), pids.end());
        if (pids.empty()) it = apps_.erase(it);
        else ++it;
    }
}

} // namespace ohoswayland
