// Generic host for native clients that present through a compositor-owned
// OHNativeWindow. The app process starts this library with
// OH_Ability_CreateNativeChildProcess, sends the normal encoded launch record,
// and later attaches one window per toplevel surface key. The client's EGL
// stack fetches its window with OhosDirectWindowAcquire.

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

#include <chrono>
#include <condition_variable>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <link.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "graphics/virgl_ipc_protocol.h"
#include "runtime/elf_entry.h"
#include "runtime/launch_params.h"
#include "runtime/launch_protocol.h"
#include "runtime/remote_wsi_host.h"
#include "runtime/remote_wsi_protocol.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Direct"

extern char** environ;

namespace {
using EntryFn = void (*)(NativeChildProcess_Args);
using MainFn = int (*)(int, char**, char**);

std::mutex gMutex;
std::condition_variable gCondition;
OHIPCRemoteStub* gStub = nullptr;
std::string gLaunchRecord;
bool gLaunched = false;
std::map<uint64_t, OHNativeWindow*> gWindows;

int WriteResult(OHIPCParcel* reply, int32_t result)
{
    return reply ? OH_IPCParcel_WriteInt32(reply, result) : OH_IPC_CHECK_PARAM_ERROR;
}

int32_t AttachWindow(const OHIPCParcel* data)
{
    int64_t key = 0;
    int64_t period = 0;
    int32_t flags = 0;
    OHNativeWindow* window = nullptr;
    if (OH_IPCParcel_ReadInt64(data, &key) != OH_IPC_SUCCESS || key <= 0 ||
        OH_IPCParcel_ReadInt64(data, &period) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(data, &flags) != OH_IPC_SUCCESS ||
        OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(data), &window) != 0 || !window) {
        if (window) OH_NativeWindow_DestroyNativeWindow(window);
        return -4;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto [it, inserted] = gWindows.emplace(static_cast<uint64_t>(key), window);
        if (!inserted) {
            // Kopper keeps using the first window for this surface. A later
            // attach for the same key is a consumer restart and is dropped.
            OH_NativeWindow_DestroyNativeWindow(window);
            window = it->second;
        }
    }
    gCondition.notify_all();
    OH_LOG_INFO(LOG_APP, "DIRECT_WINDOW_ATTACH key=%{public}lld window=%{public}p",
                static_cast<long long>(key), window);
    return 0;
}

int OnRequest(uint32_t code, const OHIPCParcel* data, OHIPCParcel* reply, void*)
{
    int32_t version = 0;
    if (!data || OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS ||
        version != winehua::virgl_ipc::kProtocolVersion) {
        return WriteResult(reply, -1);
    }
    switch (code) {
    case winehua::virgl_ipc::kLaunchRequest: {
        const char* record = OH_IPCParcel_ReadString(data);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!record || gLaunched) return WriteResult(reply, -2);
            gLaunchRecord = record;
            gLaunched = true;
        }
        gCondition.notify_all();
        const int result = WriteResult(reply, 0);
        return result == OH_IPC_SUCCESS ? OH_IPCParcel_WriteInt32(reply, getpid()) : result;
    }
    case winehua::virgl_ipc::kAttachSurfaceRequest:
        return WriteResult(reply, AttachWindow(data));
    case winehua::virgl_ipc::kDetachSurfaceRequest:
    case winehua::virgl_ipc::kSetFramePeriodRequest:
        return WriteResult(reply, 0);
    case winehua::virgl_ipc::kShutdownRequest:
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            _exit(0);
        }).detach();
        return WriteResult(reply, 0);
    default:
        return WriteResult(reply, -6);
    }
}

// Diagnostic fault reporter for the forked child: logs the fault and the
// program counter as module+offset, which crash dumps miss once a runtime
// such as Rust's replaces the system handler.
struct FaultModule {
    uintptr_t pc;
    const char* name;
    uintptr_t base;
};

int FindFaultModule(dl_phdr_info* info, size_t, void* data)
{
    auto* module = static_cast<FaultModule*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        if (ph.p_type == PT_LOAD && module->pc >= start && module->pc < start + ph.p_memsz) {
            module->name = info->dlpi_name;
            module->base = info->dlpi_addr;
            return 1;
        }
    }
    return 0;
}

void ReportFault(int signal, siginfo_t* info, void* context)
{
    const auto* uc = static_cast<ucontext_t*>(context);
    FaultModule pc = {uc->uc_mcontext.pc, "?", 0};
    FaultModule lr = {uc->uc_mcontext.regs[30], "?", 0};
    dl_iterate_phdr(FindFaultModule, &pc);
    dl_iterate_phdr(FindFaultModule, &lr);
    OH_LOG_ERROR(LOG_APP, "DIRECT_DIAG_FAULT signal=%{public}d addr=%{public}p pc=%{public}s+0x%{public}lx lr=%{public}s+0x%{public}lx",
                 signal, info->si_addr, pc.name, static_cast<unsigned long>(pc.pc - pc.base),
                 lr.name, static_cast<unsigned long>(lr.pc - lr.base));
    _exit(128 + signal);
}

// Ordinary executables write to stdout and stderr; forward both to HiLog.
void ForwardOutput(const std::string& appId)
{
    const std::string path = "/data/storage/el2/base/cache/ohos_wl_" + appId + ".log";
    // Both streams append so interleaved writes keep their order.
    if (FILE* truncate = fopen(path.c_str(), "w")) fclose(truncate);
    if (!freopen(path.c_str(), "a", stdout) || !freopen(path.c_str(), "a", stderr)) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([path]() {
        FILE* file = nullptr;
        char line[2048];
        while (true) {
            if (!file) file = fopen(path.c_str(), "r");
            if (file) {
                while (fgets(line, sizeof(line), file)) OH_LOG_INFO(LOG_APP, "APP %{public}s", line);
                clearerr(file);
            }
            usleep(20000);
        }
    }).detach();
}

// Runs an executable that exports main. Brew and similar PIE builds do.
void RunMain(const std::string& record, std::string_view packagePath, void* library)
{
    // Record layout: magic, app id, package path, entry point, argc, args..., envc, env...
    std::string_view input = record;
    std::string_view field;
    for (int i = 0; i < 5; ++i) ohoswayland::TakeLaunchField(input, field);
    const size_t count = std::strtoul(std::string(field).c_str(), nullptr, 10);
    std::vector<std::string> argv;
    argv.emplace_back(packagePath);
    for (size_t i = 0; i < count && ohoswayland::TakeLaunchField(input, field); ++i) argv.emplace_back(field);
    if (!ohoswayland::ApplyLaunchParams(record.c_str())) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR invalid environment");
        return;
    }
    std::vector<char*> args;
    for (auto& arg : argv) args.push_back(arg.data());
    args.push_back(nullptr);
    ohoswayland::ApplyCompatibilityShims(std::string(packagePath).c_str());
    // Diagnostic: run the app in a forked child and report how it ended, for
    // deaths that leave no crash record (seccomp kills). Windows do not work
    // in the child; use only to find early failures.
    if (getenv("OHOS_WL_DIAG_FORK")) {
        const pid_t child = fork();
        if (child > 0) {
            int wstatus = 0;
            waitpid(child, &wstatus, 0);
            OH_LOG_INFO(LOG_APP, "DIRECT_DIAG child=%{public}d exited=%{public}d status=%{public}d signal=%{public}d",
                        child, WIFEXITED(wstatus), WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 0,
                        WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0);
            return;
        }
        struct sigaction action = {};
        action.sa_sigaction = ReportFault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
        sigaction(SIGILL, &action, nullptr);
    }
    auto entry = reinterpret_cast<MainFn>(dlsym(library, "main"));
    int status = 0;
    if (entry) {
        status = entry(static_cast<int>(argv.size()), args.data(), environ);
    } else {
        // Distribution builds rarely export main; enter through _start.
        bool ran = false;
        status = ohoswayland::RunExecutableEntry(std::string(packagePath).c_str(),
                                                 static_cast<int>(argv.size()), args.data(), environ, &ran);
        if (!ran) return;
    }
    fflush(stdout);
    OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH_MAIN_EXIT status=%{public}d", status);
}

std::string LibraryDir()
{
    Dl_info info = {};
    dladdr(reinterpret_cast<void*>(&LibraryDir), &info);
    const std::string path = info.dli_fname ? info.dli_fname : "";
    return path.substr(0, path.rfind('/'));
}

std::string Prepend(const char* name, const std::string& value)
{
    const char* current = getenv(name);
    return current && *current ? value + ":" + current : value;
}

// Runs a program installed from the app's HNP as its own process. The kernel
// loads it, so non-PIE executables and static TLS work as on Linux. The host
// stays as its parent to keep the window: libohos_vkwsi.so in the program
// presents through this process (remote_wsi_host.cpp).
void RunExec(const std::string& record, std::string_view programPath)
{
    std::string_view input = record;
    std::string_view field;
    for (int i = 0; i < 5; ++i) ohoswayland::TakeLaunchField(input, field);
    const size_t count = std::strtoul(std::string(field).c_str(), nullptr, 10);
    std::vector<std::string> argv;
    argv.emplace_back(programPath.substr(programPath.rfind('/') + 1));
    for (size_t i = 0; i < count && ohoswayland::TakeLaunchField(input, field); ++i) argv.emplace_back(field);
    if (!ohoswayland::ApplyLaunchParams(record.c_str())) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR invalid environment");
        return;
    }
    const std::string libs = LibraryDir();
    const std::string program(programPath);
    const std::string programDir = program.substr(0, program.rfind('/'));
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    const std::string socket = std::string(runtimeDir ? runtimeDir : "/data/storage/el2/base/cache") +
                               "/ohos-wsi-" + std::to_string(getpid());
    if (ohoswayland::StartRemoteWsiServer(socket)) setenv(ohoswayland::remote_wsi::kSocketEnv, socket.c_str(), 1);
    // The program's own libraries first, then the runtime's (Wayland, Mesa,
    // the exec shims), which every program may need, GPU or not.
    setenv("LD_LIBRARY_PATH", Prepend("LD_LIBRARY_PATH", programDir + ":" + libs).c_str(), 1);
    setenv("LD_PRELOAD", Prepend("LD_PRELOAD", libs + "/libohos_exec.so").c_str(), 1);
    setenv("ZINK_VULKAN_LIBRARY", (libs + "/libohos_vkwsi.so").c_str(), 1);
    const std::string bridge = libs + "/libohos_titlebar.so";
    if (access(bridge.c_str(), R_OK) == 0) setenv(ohoswayland::remote_wsi::kTitlebarBridgeEnv, bridge.c_str(), 1);
    std::vector<char*> args;
    for (auto& arg : argv) args.push_back(arg.data());
    args.push_back(nullptr);
    fflush(stdout);
    const pid_t child = fork();
    if (child == 0) {
        // The program ends with its host, as an in-process one would.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        execve(program.c_str(), args.data(), environ);
        fprintf(stderr, "DIRECT_EXEC_FAILED %s errno=%d\n", program.c_str(), errno);
        _exit(127);
    }
    if (child < 0) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR fork errno=%{public}d", errno);
        return;
    }
    OH_LOG_INFO(LOG_APP, "DIRECT_EXEC pid=%{public}d program=%{public}s", child, program.c_str());
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH_MAIN_EXIT status=%{public}d signal=%{public}d",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

// A GTK 4 app gets native HarmonyOS title bars from the bridge bottled next
// to it (gtk_titlebar_bridge.cpp).
void LoadTitlebarBridge(std::string_view packagePath)
{
    if (!dlopen("libgtk-4.so.1", RTLD_NOW | RTLD_NOLOAD)) return;
    const std::string path = std::string(packagePath.substr(0, packagePath.rfind('/') + 1)) + "libohos_titlebar.so";
    if (!dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
        OH_LOG_WARN(LOG_APP, "TITLEBAR_BRIDGE_MISSING %{public}s", dlerror());
    }
}

bool ResolveEntry(const std::string& record, EntryFn& entry)
{
    std::string_view input = record;
    std::string_view magic, appId, packagePath, entryPoint;
    if (!ohoswayland::TakeLaunchField(input, magic) || magic != "OWP1" ||
        !ohoswayland::TakeLaunchField(input, appId) ||
        !ohoswayland::TakeLaunchField(input, packagePath) ||
        !ohoswayland::TakeLaunchField(input, entryPoint)) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR invalid launch record");
        return false;
    }
    void* library = dlopen(std::string(packagePath).c_str(), RTLD_NOW | RTLD_GLOBAL);
    void* symbol = library ? dlsym(library, std::string(entryPoint).c_str()) : nullptr;
    if (!symbol) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR %{public}s:%{public}s %{public}s",
                     std::string(packagePath).c_str(), std::string(entryPoint).c_str(), dlerror());
        return false;
    }
    entry = reinterpret_cast<EntryFn>(symbol);
    LoadTitlebarBridge(packagePath);
    OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH app=%{public}s pid=%{public}d",
                std::string(appId).c_str(), getpid());
    return true;
}
} // namespace

extern "C" __attribute__((visibility("default")))
OHNativeWindow* OhosDirectWindowAcquire(uint64_t key, int32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(gMutex);
    gCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0),
                        [key]() { return gWindows.count(key) != 0; });
    const auto it = gWindows.find(key);
    OHNativeWindow* window = it == gWindows.end() ? nullptr : it->second;
    if (!window) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_WINDOW_MISSING key=%{public}llu",
                     static_cast<unsigned long long>(key));
    }
    return window;
}

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    gStub = OH_IPCRemoteStub_Create("ohoswayland.direct.Host", OnRequest, nullptr, nullptr);
    return gStub;
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    std::string record;
    {
        std::unique_lock<std::mutex> lock(gMutex);
        if (!gCondition.wait_for(lock, std::chrono::seconds(10), []() { return gLaunched; })) {
            OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR launch record timed out");
            return;
        }
        record = gLaunchRecord;
    }
    std::string_view input = record;
    std::string_view magic, appId, packagePath, entryPoint;
    ohoswayland::TakeLaunchField(input, magic);
    ohoswayland::TakeLaunchField(input, appId);
    ohoswayland::TakeLaunchField(input, packagePath);
    ohoswayland::TakeLaunchField(input, entryPoint);
    if (entryPoint == "exec") {
        ForwardOutput(std::string(appId));
        OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH app=%{public}s pid=%{public}d exec",
                    std::string(appId).c_str(), getpid());
        RunExec(record, packagePath);
        return;
    }
    if (entryPoint == "main") {
        ForwardOutput(std::string(appId));
        // Installed apps ship in the HAP's libs directory: the loader maps code
        // only from paths its namespace permits, and app processes may not
        // create namespaces for their data directories.
        void* library = dlopen(std::string(packagePath).c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!library) {
            OH_LOG_ERROR(LOG_APP, "DIRECT_LAUNCH_ERROR %{public}s", dlerror());
            return;
        }
        LoadTitlebarBridge(packagePath);
        OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH app=%{public}s pid=%{public}d main",
                    std::string(appId).c_str(), getpid());
        RunMain(record, packagePath, library);
        return;
    }
    EntryFn entry = nullptr;
    if (!ResolveEntry(record, entry)) return;
    NativeChildProcess_Args args = {};
    args.entryParams = record.data();
    entry(args);
    OH_LOG_INFO(LOG_APP, "DIRECT_LAUNCH_EXIT pid=%{public}d", getpid());
}
