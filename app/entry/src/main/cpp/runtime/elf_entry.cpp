#include "runtime/elf_entry.h"
#include "runtime/sandbox_syscalls.h"

#include <hilog/log.h>

#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <cerrno>
#include <cstdlib>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Direct"

namespace ohoswayland {
namespace {

using MainFn = int (*)(int, char**, char**);

struct EntryState {
    const char* path = nullptr;
    uintptr_t base = 0;
    const ElfW(Dyn)* dynamic = nullptr;
    char** envp = nullptr;
    int status = 0;
    jmp_buf done;
};
EntryState gEntry;

int FindObject(dl_phdr_info* info, size_t, void*)
{
    if (!info->dlpi_name || std::strcmp(info->dlpi_name, gEntry.path) != 0) return 0;
    gEntry.base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            gEntry.dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    }
    return 1;
}

// musl leaves d_ptr values unrelocated; glibc-style loaders relocate them.
uintptr_t DynamicPointer(uintptr_t value)
{
    return value < gEntry.base ? gEntry.base + value : value;
}

// HongMeng returns EACCES for fstat on memfds in app processes, and Rust's
// std sizes memfd mappings with fstat (smithay-client-toolkit shm pools).
// Fall back to a regular-file answer sized with lseek.
int FstatShim(int fd, struct stat* st)
{
    const int result = fstat(fd, st);
    if (result == 0 || errno != EACCES) return result;
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

long SyscallShim(long number, long a, long b, long c, long d, long e, long f)
{
    const int killed = SandboxKilledIndex(number);
    if (killed >= 0) {
        static uint64_t reported = 0;
        const uint64_t bit = 1ull << killed;
        if (!(reported & bit)) {
            reported |= bit;
            OH_LOG_INFO(LOG_APP, "DIRECT_SYSCALL_ENOSYS nr=%{public}ld", number);
        }
        errno = ENOSYS;
        return -1;
    }
    return syscall(number, a, b, c, d, e, f);
}

// Installed apps that end themselves leave no crash record; log who ended
// them and with what status before doing it.
uintptr_t CallerOffset(void* caller)
{
    return reinterpret_cast<uintptr_t>(caller) - gEntry.base;
}

[[noreturn]] void ExitShim(int status)
{
    OH_LOG_INFO(LOG_APP, "DIRECT_APP_EXIT exit(%{public}d) from +0x%{public}lx", status,
                static_cast<unsigned long>(CallerOffset(__builtin_return_address(0))));
    exit(status);
}

[[noreturn]] void UnderscoreExitShim(int status)
{
    OH_LOG_INFO(LOG_APP, "DIRECT_APP_EXIT _exit(%{public}d) from +0x%{public}lx", status,
                static_cast<unsigned long>(CallerOffset(__builtin_return_address(0))));
    _exit(status);
}

[[noreturn]] void AbortShim()
{
    OH_LOG_ERROR(LOG_APP, "DIRECT_APP_EXIT abort() from +0x%{public}lx",
                 static_cast<unsigned long>(CallerOffset(__builtin_return_address(0))));
    abort();
}

// Called by the executable's startup code in place of libc's version.
int StartMainHook(MainFn main, int argc, char** argv, void (*)(), void (*)(), void (*)())
{
    gEntry.status = main(argc, argv, gEntry.envp);
    longjmp(gEntry.done, 1);
}

// Points the object's imports of the named symbols at replacements.
struct Import {
    const char* name;
    uintptr_t replacement;
    bool patched;
};

void PatchImports(Import* imports, size_t count)
{
    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rela = 0;
    size_t jmprelSize = 0, relaSize = 0;
    for (const ElfW(Dyn)* d = gEntry.dynamic; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = DynamicPointer(d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = DynamicPointer(d->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = DynamicPointer(d->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmprelSize = d->d_un.d_val; break;
        case DT_RELA: rela = DynamicPointer(d->d_un.d_ptr); break;
        case DT_RELASZ: relaSize = d->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab) return;
    const auto* symbols = reinterpret_cast<const ElfW(Sym)*>(symtab);
    const auto* names = reinterpret_cast<const char*>(strtab);
    const struct { uintptr_t table; size_t size; } tables[] = {{jmprel, jmprelSize}, {rela, relaSize}};
    for (const auto& table : tables) {
        if (!table.table) continue;
        const auto* entries = reinterpret_cast<const ElfW(Rela)*>(table.table);
        for (size_t i = 0; i < table.size / sizeof(ElfW(Rela)); ++i) {
            const uint32_t type = ELF64_R_TYPE(entries[i].r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
            const ElfW(Sym)& symbol = symbols[ELF64_R_SYM(entries[i].r_info)];
            for (size_t k = 0; k < count; ++k) {
                if (std::strcmp(names + symbol.st_name, imports[k].name) != 0) continue;
                auto* slot = reinterpret_cast<uintptr_t*>(gEntry.base + entries[i].r_offset);
                const long page = sysconf(_SC_PAGESIZE);
                auto* pageStart = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~(page - 1));
                if (mprotect(pageStart, page, PROT_READ | PROT_WRITE) != 0) continue;
                *slot = imports[k].replacement;
                imports[k].patched = true;
            }
        }
    }
}

bool LocateObject(const char* path)
{
    gEntry.path = path;
    gEntry.base = 0;
    gEntry.dynamic = nullptr;
    dl_iterate_phdr(FindObject, nullptr);
    return gEntry.dynamic != nullptr;
}

void ApplyShims()
{
    Import shims[] = {
        {"fstat", reinterpret_cast<uintptr_t>(&FstatShim), false},
        {"fstat64", reinterpret_cast<uintptr_t>(&FstatShim), false},
        {"syscall", reinterpret_cast<uintptr_t>(&SyscallShim), false},
        {"exit", reinterpret_cast<uintptr_t>(&ExitShim), false},
        {"_exit", reinterpret_cast<uintptr_t>(&UnderscoreExitShim), false},
        {"_Exit", reinterpret_cast<uintptr_t>(&UnderscoreExitShim), false},
        {"abort", reinterpret_cast<uintptr_t>(&AbortShim), false},
    };
    PatchImports(shims, sizeof(shims) / sizeof(shims[0]));
}

uintptr_t EntryAddress()
{
    const int fd = open(gEntry.path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ElfW(Ehdr) header = {};
    const bool read = pread(fd, &header, sizeof(header), 0) == sizeof(header);
    close(fd);
    if (!read || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_type != ET_DYN) return 0;
    return gEntry.base + header.e_entry;
}

// Enters the program's _start with sp at an initial process stack: argc,
// argv, envp, and an empty auxv. _start derives argc and argv from sp.
[[noreturn]] void EnterAt(uintptr_t entry, uintptr_t stackPointer)
{
    asm volatile(
        "mov sp, %1\n"
        "mov x29, #0\n"
        "mov x30, #0\n"
        "br %0\n"
        :
        : "r"(entry), "r"(stackPointer)
        : "memory");
    __builtin_unreachable();
}

} // namespace

void ApplyCompatibilityShims(const char* path)
{
    if (LocateObject(path)) ApplyShims();
}

int RunExecutableEntry(const char* path, int argc, char** argv, char** envp, bool* ran)
{
    *ran = false;
    gEntry = {};
    gEntry.envp = envp;
    const uintptr_t entry = LocateObject(path) ? EntryAddress() : 0;
    Import startMain = {"__libc_start_main", reinterpret_cast<uintptr_t>(&StartMainHook), false};
    if (entry) PatchImports(&startMain, 1);
    if (!entry || !startMain.patched) {
        OH_LOG_ERROR(LOG_APP, "DIRECT_ENTRY_ERROR %{public}s base=%{public}p dynamic=%{public}p entry=%{public}p",
                     path, reinterpret_cast<void*>(gEntry.base), gEntry.dynamic, reinterpret_cast<void*>(entry));
        return -1;
    }

    // Programs get a full-size stack; the initial frame sits at its top.
    constexpr size_t kStackSize = 8 << 20;
    void* stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) return -1;
    size_t envc = 0;
    while (envp && envp[envc]) ++envc;
    const size_t words = 1 + argc + 1 + envc + 1 + 2;
    auto* frame = reinterpret_cast<uintptr_t*>(
        (reinterpret_cast<uintptr_t>(stack) + kStackSize - words * sizeof(uintptr_t) - 64) & ~uintptr_t(15));
    size_t w = 0;
    frame[w++] = static_cast<uintptr_t>(argc);
    for (int i = 0; i < argc; ++i) frame[w++] = reinterpret_cast<uintptr_t>(argv[i]);
    frame[w++] = 0;
    for (size_t i = 0; i < envc; ++i) frame[w++] = reinterpret_cast<uintptr_t>(envp[i]);
    frame[w++] = 0;
    frame[w++] = AT_NULL;
    frame[w++] = 0;

    OH_LOG_INFO(LOG_APP, "DIRECT_ENTRY %{public}s entry=%{public}p", path, reinterpret_cast<void*>(entry));
    *ran = true;
    if (setjmp(gEntry.done) == 0) EnterAt(entry, reinterpret_cast<uintptr_t>(frame));
    munmap(stack, kStackSize);
    return gEntry.status;
}

} // namespace ohoswayland
