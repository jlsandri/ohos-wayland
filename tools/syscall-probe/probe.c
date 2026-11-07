// Runs each syscall in its own forked child and reports how the child ended.
// A seccomp kill shows up as a signal (SIGSYS) instead of an error return.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct probe { const char *name; long nr; long a, b, c, d, e; };

int main(void)
{
    static char buf[4096];
    const struct probe probes[] = {
        {"setuid", 146, -1, 0, 0, 0, 0},
        {"setgid", 144, -1, 0, 0, 0, 0},
        {"statx", 291, -100, (long)"/", 0, 0xfff, (long)buf},
        {"prlimit64", 261, 0, 7, 0, (long)buf, 0},
        {"getrlimit", 163, 7, (long)buf, 0, 0, 0},
        {"setrlimit", 164, 7, (long)buf, 0, 0, 0},
        {"sched_getaffinity", 123, 0, 128, (long)buf, 0, 0},
        {"rseq", 293, 0, 0, 0, 0, 0},
        {"clone3", 435, 0, 0, 0, 0, 0},
        {"pidfd_open", 434, 1, 0, 0, 0, 0},
        {"close_range", 436, 1000, 1001, 0, 0, 0},
        {"membarrier", 283, 0, 0, 0, 0, 0},
        {"io_uring_setup", 425, 1, (long)buf, 0, 0, 0},
        {"landlock_create_ruleset", 444, 0, 0, 1, 0, 0},
        {"memfd_secret", 447, 0, 0, 0, 0, 0},
        {"userfaultfd", 282, 0, 0, 0, 0, 0},
        {"perf_event_open", 241, 0, 0, -1, -1, 0},
        {"epoll_pwait2", 441, -1, 0, 0, 0, 0},
        {"faccessat2", 439, -100, (long)"/", 0, 0, 0},
        {"openat2", 437, -100, (long)"/", 0, 0, 0},
        {"inotify_init1", 26, 0, 0, 0, 0, 0},
        {"signalfd4", 74, -1, (long)buf, 8, 0, 0},
        {"timerfd_create", 85, 1, 0, 0, 0, 0},
        {"eventfd2", 19, 0, 0, 0, 0, 0},
        {"getrandom", 278, (long)buf, 16, 0, 0, 0},
        {"copy_file_range", 285, -1, 0, -1, 0, 0},
        {"setsid", 157, 0, 0, 0, 0, 0},
        {"sched_setscheduler", 119, 0, 0, (long)buf, 0, 0},
        {"ioprio_set", 30, 1, 0, 0, 0, 0},
        {"personality", 92, 0xffffffff, 0, 0, 0, 0},
        {"mlock2", 284, (long)buf, 16, 0, 0, 0},
        {"futex_waitv", 449, 0, 0, 0, 0, 0},
        {"get_mempolicy", 236, 0, 0, 0, 0, 0},
        {"process_vm_readv", 270, 0, 0, 0, 0, 0},
        {"kcmp", 272, 0, 0, 0, 0, 0},
        {"unshare", 97, 0, 0, 0, 0, 0},
        {"pkey_alloc", 289, 0, 0, 0, 0, 0},
        {"mremap", 216, 0, 0, 0, 0, 0},
        {"msync", 227, 0, 0, 0, 0, 0},
        {"process_madvise", 440, -1, 0, 0, 0, 0},
        {"set_robust_list", 99, 0, 0, 0, 0, 0},
        {"sigaltstack", 132, 0, (long)buf, 0, 0, 0},
        {"getcpu", 168, (long)buf, 0, 0, 0, 0},
        {"sched_getattr", 275, 0, (long)buf, 56, 0, 0},
        {"name_to_handle_at", 264, -100, (long)"/", (long)buf, (long)buf, 0},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        const struct probe *p = &probes[i];
        pid_t child = fork();
        if (child == 0) {
            long r = syscall(p->nr, p->a, p->b, p->c, p->d, p->e);
            _exit(r < 0 ? (errno & 0x7f) : 0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        if (WIFSIGNALED(status))
            printf("SYSCALL_PROBE %s KILLED signal=%d\n", p->name, WTERMSIG(status));
        else
            printf("SYSCALL_PROBE %s returned errno=%d\n", p->name, WEXITSTATUS(status));
        fflush(stdout);
    }
    return 0;
}
