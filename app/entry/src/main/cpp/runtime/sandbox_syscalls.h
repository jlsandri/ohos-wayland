#pragma once

#include <cstddef>

namespace ohoswayland {

// The app sandbox's seccomp policy kills the process on these syscalls
// instead of failing them (verification/ghostty/syscall-probe output).
// Programs probe for them and fall back on ENOSYS, so answer that.
constexpr long kSandboxKilledSyscalls[] = {
    293 /* rseq */, 435 /* clone3 */, 436 /* close_range */, 425 /* io_uring_setup */,
    426 /* io_uring_enter */, 427 /* io_uring_register */, 444 /* landlock_create_ruleset */,
    445 /* landlock_add_rule */, 446 /* landlock_restrict_self */, 447 /* memfd_secret */,
    441 /* epoll_pwait2 */, 437 /* openat2 */, 449 /* futex_waitv */, 236 /* get_mempolicy */,
    272 /* kcmp */, 97 /* unshare */, 289 /* pkey_alloc */, 264 /* name_to_handle_at */,
};

// Index of number in kSandboxKilledSyscalls, or -1.
constexpr int SandboxKilledIndex(long number)
{
    for (size_t i = 0; i < sizeof(kSandboxKilledSyscalls) / sizeof(kSandboxKilledSyscalls[0]); ++i) {
        if (kSandboxKilledSyscalls[i] == number) return static_cast<int>(i);
    }
    return -1;
}

} // namespace ohoswayland
