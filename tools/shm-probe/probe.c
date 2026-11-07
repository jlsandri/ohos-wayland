// Reports which shared-memory primitives the direct-window host process allows.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    int fd = memfd_create("probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    printf("SHM_PROBE memfd_create fd=%d errno=%d (%s)\n", fd, fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    // rustix's linux_raw backend issues the syscall directly.
    fd = (int)syscall(SYS_memfd_create, "smithay-client-toolkit", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    printf("SHM_PROBE raw memfd_create fd=%d errno=%d (%s)\n", fd, fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    fd = (int)syscall(SYS_memfd_create, "smithay-client-toolkit", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0) {
        int seal = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
        int sized = ftruncate(fd, 4096);
        int sizeErr = sized < 0 ? errno : 0;
        void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        printf("SHM_PROBE seal=%d ftruncate=%d errno=%d mmap=%p errno=%d\n", seal, sized, sizeErr,
               map, map == MAP_FAILED ? errno : 0);
        if (map != MAP_FAILED) munmap(map, 4096);
        close(fd);
    }
    fd = (int)syscall(SYS_memfd_create, "probe", MFD_CLOEXEC | MFD_ALLOW_SEALING | 0x8U);
    printf("SHM_PROBE raw memfd_create NOEXEC_SEAL fd=%d errno=%d (%s)\n", fd, fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) close(fd);
    fd = (int)syscall(SYS_memfd_create, "probe", MFD_CLOEXEC | 0x10U);
    printf("SHM_PROBE raw memfd_create EXEC fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0) close(fd);
    fd = (int)syscall(SYS_memfd_create, "probe", MFD_CLOEXEC);
    printf("SHM_PROBE raw memfd_create nosealing fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0) close(fd);
    fd = shm_open("/shm-probe", O_RDWR | O_CREAT | O_EXCL, 0600);
    printf("SHM_PROBE shm_open fd=%d errno=%d (%s)\n", fd, fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0) { shm_unlink("/shm-probe"); close(fd); }
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/shm-probe-XXXXXX", dir ? dir : "/data/storage/el2/base/cache");
    fd = mkostemp(path, O_CLOEXEC);
    printf("SHM_PROBE mkostemp %s fd=%d errno=%d\n", path, fd, fd < 0 ? errno : 0);
    if (fd >= 0) { unlink(path); close(fd); }
    {
        // Rust std implements File::metadata with statx and falls back to
        // fstat only on ENOSYS or EPERM.
        int mfd = (int)syscall(SYS_memfd_create, "statx", MFD_CLOEXEC);
        unsigned char buf[256];
        long r = syscall(SYS_statx, mfd, "", 0x1000 /* AT_EMPTY_PATH */, 0xfff, buf);
        printf("SHM_PROBE statx(memfd)=%ld errno=%d (%s)\n", r, r < 0 ? errno : 0, r < 0 ? strerror(errno) : "ok");
        r = syscall(SYS_statx, -100 /* AT_FDCWD */, "/system/bin/sh", 0, 0xfff, buf);
        printf("SHM_PROBE statx(path)=%ld errno=%d\n", r, r < 0 ? errno : 0);
        r = syscall(SYS_statx, 0, NULL, 0, 0xfff, NULL);
        printf("SHM_PROBE statx(null probe)=%ld errno=%d\n", r, r < 0 ? errno : 0);
        struct stat st;
        printf("SHM_PROBE fstat(memfd)=%d errno=%d\n", fstat(mfd, &st), errno);
        close(mfd);
    }
    printf("SHM_PROBE access /dev/shm=%d errno=%d\n", access("/dev/shm", W_OK), errno);
    fflush(stdout);
    return 0;
}
