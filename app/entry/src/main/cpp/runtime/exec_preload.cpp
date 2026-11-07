// LD_PRELOADed into every program the direct child execs from the app's HNP
// (direct_child.cpp). The in-process path patches a dlopen'ed program's
// imports instead (elf_entry.cpp); an exec'd program gets the same
// workarounds by symbol interposition, plus the window lookup Mesa expects
// from the host library it can no longer see.
//
// These run inside the dynamic loader's startup and in every libc user, so
// they make raw syscalls and use no dlsym, locks, or C++ runtime.

#include <native_window/external_window.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "runtime/remote_wsi_protocol.h"
#include "runtime/sandbox_syscalls.h"
#include "runtime/user_folders.h"

namespace {
using ohoswayland::remote_wsi::RemoteWindow;

long RawSyscall(long number, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

long Result(long value)
{
    if (value < 0 && value > -4096) {
        errno = static_cast<int>(-value);
        return -1;
    }
    return value;
}

// fstat on a memfd fails with EACCES in the sandbox; Wayland clients size
// their shm pools with it.
int StatFallback(int fd, struct stat* st)
{
    const off_t current = lseek(fd, 0, SEEK_CUR);
    const off_t end = lseek(fd, 0, SEEK_END);
    if (current < 0 || end < 0) {
        errno = EACCES;
        return -1;
    }
    lseek(fd, current, SEEK_SET);
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_size = end;
    st->st_blksize = 4096;
    st->st_blocks = (end + 511) / 512;
    st->st_nlink = 1;
    return 0;
}

// Windows handed out by OhosDirectWindowAcquire. Mesa asks once per surface,
// so a small table that is never freed is enough.
constexpr int kMaxWindows = 64;
RemoteWindow gWindows[kMaxWindows];
int gWindowCount = 0;

bool IsRemoteWindow(const void* window)
{
    return window >= static_cast<const void*>(gWindows) &&
           window < static_cast<const void*>(gWindows + kMaxWindows);
}
struct passwd* SelfPasswd(struct passwd* entry)
{
    static char name[] = "ohos";
    static char password[] = "x";
    static char shell[] = "/system/bin/sh";
    const char* home = getenv("HOME");
    entry->pw_name = name;
    entry->pw_passwd = password;
    entry->pw_uid = getuid();
    entry->pw_gid = getgid();
    entry->pw_gecos = name;
    entry->pw_dir = const_cast<char*>(home ? home : "/");
    entry->pw_shell = shell;
    return entry;
}

template <typename Fn> Fn Libc(const char* name)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

// The user's Documents, Desktop and Download folders open once the user
// grants the app the matching permission. The first refused access to one
// asks the app, which shows the HarmonyOS permission dialog
// (folder_access.cpp), and is retried if the user allows it. The launcher
// lists the folders in OHOS_USER_FOLDERS as name=path pairs.
constexpr int kFolders = 3;
struct Folder {
    char name[16];
    char path[128];
    int answer;    // 0 not asked, 1 granted, -1 refused
};
Folder gFolders[kFolders];
bool gFoldersRead = false;

bool HasPrefix(const char* path, const char* prefix)
{
    const size_t length = std::strlen(prefix);
    return length > 0 && std::strncmp(path, prefix, length) == 0 && (path[length] == '/' || path[length] == '\0');
}

void ReadFolders()
{
    gFoldersRead = true;
    const char* list = getenv("OHOS_USER_FOLDERS");
    for (int i = 0; list && *list && i < kFolders; ++i) {
        const char* end = std::strchr(list, ';');
        const size_t length = end ? static_cast<size_t>(end - list) : std::strlen(list);
        char entry[160];
        snprintf(entry, sizeof(entry), "%.*s", static_cast<int>(length), list);
        char* path = std::strchr(entry, '=');
        if (path) {
            *path++ = '\0';
            snprintf(gFolders[i].name, sizeof(gFolders[i].name), "%s", entry);
            snprintf(gFolders[i].path, sizeof(gFolders[i].path), "%s", path);
        }
        list = end ? end + 1 : "";
    }
}

Folder* FolderOf(const char* path)
{
    if (!path || path[0] != '/') return nullptr;
    if (!gFoldersRead) ReadFolders();
    for (Folder& folder : gFolders) {
        if (HasPrefix(path, folder.path)) return &folder;
    }
    return nullptr;
}

// Asks the app for the folder and waits for the user's answer.
bool RequestFolder(Folder* folder)
{
    if (folder->answer != 0) return folder->answer > 0;
    folder->answer = -1;
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (!runtimeDir) return false;
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/%s", runtimeDir, ohoswayland::kFolderSocket);
    const int fd = static_cast<int>(RawSyscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, 0, 0, 0));
    if (fd < 0) return false;
    char request[32];
    const int length = snprintf(request, sizeof(request), "%s\n", folder->name);
    char reply = '0';
    if (RawSyscall(SYS_connect, fd, reinterpret_cast<long>(&address), sizeof(address), 0, 0, 0) == 0 &&
        RawSyscall(SYS_write, fd, reinterpret_cast<long>(request), length, 0, 0, 0) == length) {
        while (RawSyscall(SYS_read, fd, reinterpret_cast<long>(&reply), 1, 0, 0, 0) == -EINTR) {
        }
    }
    RawSyscall(SYS_close, fd, 0, 0, 0, 0, 0);
    folder->answer = reply == '1' ? 1 : -1;
    return folder->answer > 0;
}

// Whether a failed call on path should be retried after asking for its folder.
bool RetryAfterRequest(long result, const char* path)
{
    if (result >= 0 || (errno != EPERM && errno != EACCES)) return false;
    const int saved = errno;
    Folder* folder = FolderOf(path);
    const bool granted = folder && RequestFolder(folder);
    errno = saved;
    return granted;
}

} // namespace

// The host sends the program's output to a log it forwards to HiLog line by
// line; a file would otherwise make stdout fully buffered.
__attribute__((constructor)) void LineBufferStdout()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
}

// A GTK 4 program gets native HarmonyOS title bars from the bridge, loaded
// once its GTK is mapped and before main, as the in-process host does
// (direct_child.cpp LoadTitlebarBridge). Programs without GTK, such as the
// shells and ssh a terminal starts, skip it.
__attribute__((constructor)) void LoadTitlebarBridge()
{
    const char* bridge = getenv(ohoswayland::remote_wsi::kTitlebarBridgeEnv);
    if (bridge && dlopen("libgtk-4.so.1", RTLD_NOW | RTLD_NOLOAD)) dlopen(bridge, RTLD_NOW | RTLD_GLOBAL);
}

extern "C" {

__attribute__((visibility("default"))) long syscall(long number, ...)
{
    va_list args;
    va_start(args, number);
    long a[6];
    for (long& value : a) value = va_arg(args, long);
    va_end(args);
    if (ohoswayland::SandboxKilledIndex(number) >= 0) {
        errno = ENOSYS;
        return -1;
    }
    long result = Result(RawSyscall(number, a[0], a[1], a[2], a[3], a[4], a[5]));
    const bool pathCall = number == SYS_openat || number == SYS_newfstatat || number == SYS_statx ||
                          number == SYS_faccessat;
    if (pathCall && RetryAfterRequest(result, reinterpret_cast<const char*>(a[1]))) {
        result = Result(RawSyscall(number, a[0], a[1], a[2], a[3], a[4], a[5]));
    }
    return result;
}

// The calls programs make on a user folder before anything else, for the
// folder permission request above.
__attribute__((visibility("default"))) int openat(int dirfd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    long result = Result(RawSyscall(SYS_openat, dirfd, reinterpret_cast<long>(path), flags | O_LARGEFILE, mode, 0, 0));
    if (RetryAfterRequest(result, path)) {
        result = Result(RawSyscall(SYS_openat, dirfd, reinterpret_cast<long>(path), flags | O_LARGEFILE, mode, 0, 0));
    }
    return static_cast<int>(result);
}

__attribute__((visibility("default"))) int open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return openat(AT_FDCWD, path, flags, mode);
}

__attribute__((visibility("default"))) int fstatat(int dirfd, const char* path, struct stat* st, int flags)
{
    long result = Result(RawSyscall(SYS_newfstatat, dirfd, reinterpret_cast<long>(path), reinterpret_cast<long>(st),
                                    flags, 0, 0));
    if (RetryAfterRequest(result, path)) {
        result = Result(RawSyscall(SYS_newfstatat, dirfd, reinterpret_cast<long>(path), reinterpret_cast<long>(st),
                                   flags, 0, 0));
    }
    return static_cast<int>(result);
}

__attribute__((visibility("default"))) int stat(const char* path, struct stat* st)
{
    return fstatat(AT_FDCWD, path, st, 0);
}

__attribute__((visibility("default"))) int lstat(const char* path, struct stat* st)
{
    return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

__attribute__((visibility("default"))) int statx(int dirfd, const char* path, int flags, unsigned mask,
                                                 struct statx* st)
{
    long result = Result(RawSyscall(SYS_statx, dirfd, reinterpret_cast<long>(path), flags, mask,
                                    reinterpret_cast<long>(st), 0));
    if (RetryAfterRequest(result, path)) {
        result = Result(RawSyscall(SYS_statx, dirfd, reinterpret_cast<long>(path), flags, mask,
                                   reinterpret_cast<long>(st), 0));
    }
    return static_cast<int>(result);
}

__attribute__((visibility("default"))) int access(const char* path, int mode)
{
    long result = Result(RawSyscall(SYS_faccessat, AT_FDCWD, reinterpret_cast<long>(path), mode, 0, 0, 0));
    if (RetryAfterRequest(result, path)) {
        result = Result(RawSyscall(SYS_faccessat, AT_FDCWD, reinterpret_cast<long>(path), mode, 0, 0, 0));
    }
    return static_cast<int>(result);
}

__attribute__((visibility("default"))) int chdir(const char* path)
{
    long result = Result(RawSyscall(SYS_chdir, reinterpret_cast<long>(path), 0, 0, 0, 0, 0));
    if (RetryAfterRequest(result, path)) result = Result(RawSyscall(SYS_chdir, reinterpret_cast<long>(path), 0, 0, 0, 0, 0));
    return static_cast<int>(result);
}

__attribute__((visibility("default"))) DIR* opendir(const char* path)
{
    static auto next = Libc<DIR* (*)(const char*)>("opendir");
    DIR* dir = next(path);
    if (!dir && RetryAfterRequest(-1, path)) dir = next(path);
    return dir;
}

// The large-file names older binaries import; musl's headers map them to
// the names above.
__attribute__((visibility("default"))) int Open64(const char* path, int flags, ...) __asm__("open64");
int Open64(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return openat(AT_FDCWD, path, flags, mode);
}

__attribute__((visibility("default"))) int Stat64(const char* path, struct stat* st) __asm__("stat64");
int Stat64(const char* path, struct stat* st)
{
    return fstatat(AT_FDCWD, path, st, 0);
}

__attribute__((visibility("default"))) int Lstat64(const char* path, struct stat* st) __asm__("lstat64");
int Lstat64(const char* path, struct stat* st)
{
    return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

__attribute__((visibility("default"))) int fstat(int fd, struct stat* st)
{
    const long result = Result(RawSyscall(SYS_fstat, fd, reinterpret_cast<long>(st), 0, 0, 0, 0));
    return result == 0 || errno != EACCES ? static_cast<int>(result) : StatFallback(fd, st);
}

// musl's headers define fstat64 as fstat; older binaries still import it.
__attribute__((visibility("default"))) int Fstat64(int fd, struct stat* st) __asm__("fstat64");
int Fstat64(int fd, struct stat* st)
{
    return fstat(fd, st);
}

// Newer musl entry points distribution programs link that the HarmonyOS libc
// lacks. Found by comparing every hnp object's undefined symbols against the
// system libc.

// The resolver state behind _res. musl keeps it only for API compatibility;
// a zeroed block larger than its struct __res_state is what it would return.
__attribute__((visibility("default"))) void* __res_state()
{
    alignas(16) static unsigned char state[1024];
    return state;
}

__attribute__((visibility("default"))) int pidfd_getpid(int fd)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE* file = fopen(path, "re");
    if (!file) return -1;
    char line[128];
    int pid = -1;
    while (fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Pid:", 4) == 0) pid = atoi(line + 4);
    }
    fclose(file);
    if (pid <= 0) errno = pid == 0 ? EREMOTE : EBADF;
    return pid > 0 ? pid : -1;
}

// Callers fall back to posix_spawnp when this reports no support.
__attribute__((visibility("default"))) int pidfd_spawnp(int*, const char*, const void*, const void*, char* const*,
                                                       char* const*)
{
    return ENOSYS;
}

// Seccomp kills the set*id calls. Programs that drop privileges they never
// had (busybox does on every applet) ask for the ids they already hold.
__attribute__((visibility("default"))) int setuid(uid_t uid)
{
    if (uid == getuid() && uid == geteuid()) return 0;
    errno = EPERM;
    return -1;
}

__attribute__((visibility("default"))) int setgid(gid_t gid)
{
    if (gid == getgid() && gid == getegid()) return 0;
    errno = EPERM;
    return -1;
}

// The sandbox has no shadow file, so no user has a shadow entry.
__attribute__((visibility("default"))) int getspnam_r(const char*, struct spwd*, char*, size_t, struct spwd** result)
{
    *result = nullptr;
    return ENOENT;
}

// The sandbox has no passwd entry for the app's uid, so programs that look up
// their own user (ssh, shells, Ghostty's default shell) fail. Answer for that
// uid from the environment; other users go to libc.
__attribute__((visibility("default"))) struct passwd* getpwuid(uid_t uid)
{
    static struct passwd self;
    if (uid == getuid()) return SelfPasswd(&self);
    static auto next = Libc<struct passwd* (*)(uid_t)>("getpwuid");
    return next ? next(uid) : nullptr;
}

__attribute__((visibility("default"))) struct passwd* getpwnam(const char* name)
{
    static struct passwd self;
    if (name && std::strcmp(name, "ohos") == 0) return SelfPasswd(&self);
    static auto next = Libc<struct passwd* (*)(const char*)>("getpwnam");
    return next ? next(name) : nullptr;
}

__attribute__((visibility("default"))) int getpwuid_r(uid_t uid, struct passwd* entry, char* buffer, size_t size,
                                                     struct passwd** result)
{
    if (uid == getuid()) {
        *result = SelfPasswd(entry);
        return 0;
    }
    static auto next = Libc<int (*)(uid_t, struct passwd*, char*, size_t, struct passwd**)>("getpwuid_r");
    if (next) return next(uid, entry, buffer, size, result);
    *result = nullptr;
    return 0;
}

__attribute__((visibility("default"))) int getpwnam_r(const char* name, struct passwd* entry, char* buffer,
                                                     size_t size, struct passwd** result)
{
    if (name && std::strcmp(name, "ohos") == 0) {
        *result = SelfPasswd(entry);
        return 0;
    }
    static auto next = Libc<int (*)(const char*, struct passwd*, char*, size_t, struct passwd**)>("getpwnam_r");
    if (next) return next(name, entry, buffer, size, result);
    *result = nullptr;
    return 0;
}

// The system Vulkan loader, the one that knows the GPU, has no Wayland or X11
// surfaces, but distribution GTK links their entry points. Such programs are
// mapped to it at bottle time (ohos_wl.py fix_object), so answer here that
// the extensions are absent.
constexpr int32_t kVkErrorExtensionNotPresent = -7;

__attribute__((visibility("default"))) int32_t vkCreateWaylandSurfaceKHR(void*, const void*, const void*, void*)
{
    return kVkErrorExtensionNotPresent;
}

__attribute__((visibility("default"))) int32_t vkCreateXlibSurfaceKHR(void*, const void*, const void*, void*)
{
    return kVkErrorExtensionNotPresent;
}

__attribute__((visibility("default"))) int32_t vkCreateXcbSurfaceKHR(void*, const void*, const void*, void*)
{
    return kVkErrorExtensionNotPresent;
}

__attribute__((visibility("default"))) uint32_t vkGetPhysicalDeviceWaylandPresentationSupportKHR(void*, uint32_t, void*)
{
    return 0;
}

__attribute__((visibility("default"))) uint32_t vkGetPhysicalDeviceXlibPresentationSupportKHR(void*, uint32_t, void*,
                                                                                            unsigned long)
{
    return 0;
}

__attribute__((visibility("default"))) uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(void*, uint32_t, void*,
                                                                                           uint32_t)
{
    return 0;
}

// Mesa's OHOS direct path looks this up to get the window for a surface. In
// an exec'd client the host keeps the real window, so return a token that
// libohos_vkwsi.so turns into a remote swapchain.
__attribute__((visibility("default"))) OHNativeWindow* OhosDirectWindowAcquire(uint64_t key, int32_t)
{
    const int count = __atomic_load_n(&gWindowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; ++i) {
        if (gWindows[i].surfaceKey == key) return reinterpret_cast<OHNativeWindow*>(&gWindows[i]);
    }
    const int index = __atomic_fetch_add(&gWindowCount, 1, __ATOMIC_ACQ_REL);
    if (index >= kMaxWindows) return nullptr;
    gWindows[index] = {ohoswayland::remote_wsi::kRemoteWindowMagic, key};
    return reinterpret_cast<OHNativeWindow*>(&gWindows[index]);
}

// Mesa sets the buffer geometry on the window it acquired. The host sets it
// on the real window when it creates the swapchain.
__attribute__((visibility("default"))) int32_t OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow* window,
                                                                                    int code, ...)
{
    if (IsRemoteWindow(window)) return 0;
    va_list args;
    va_start(args, code);
    long a[4];
    for (long& value : a) value = va_arg(args, long);
    va_end(args);
    using Fn = int32_t (*)(OHNativeWindow*, int, ...);
    static Fn next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "OH_NativeWindow_NativeWindowHandleOpt"));
    return next ? next(window, code, a[0], a[1], a[2], a[3]) : -1;
}
}
