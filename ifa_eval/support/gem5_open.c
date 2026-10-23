/* newlib's RISC-V libgloss opens files with the proxy kernel's SYS_open
 * (1024), which gem5's SE mode (Linux system calls) does not implement.
 * This _open uses Linux openat(AT_FDCWD, ...) instead, translating the
 * newlib open flags to Linux ones. */
#include <errno.h>
#include <fcntl.h>

#define LINUX_SYS_openat 56
#define LINUX_AT_FDCWD -100
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000

int
_open(const char *name, int flags, int mode)
{
    long linux_flags = flags & O_ACCMODE;

    if (flags & O_CREAT)
        linux_flags |= LINUX_O_CREAT;
    if (flags & O_EXCL)
        linux_flags |= LINUX_O_EXCL;
    if (flags & O_TRUNC)
        linux_flags |= LINUX_O_TRUNC;
    if (flags & O_APPEND)
        linux_flags |= LINUX_O_APPEND;

    register long a0 asm("a0") = LINUX_AT_FDCWD;
    register long a1 asm("a1") = (long)name;
    register long a2 asm("a2") = linux_flags;
    register long a3 asm("a3") = mode;
    register long a7 asm("a7") = LINUX_SYS_openat;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
                 : "memory");
    if (a0 < 0) {
        errno = -a0;
        return -1;
    }
    return a0;
}
