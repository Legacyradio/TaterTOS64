/*
 * lgap.c — dynamically linked Linux x86_64 probe targeting non-thread syscall gaps.
 *
 * Skips threading (clone3/pthread) — those come in a separate rung.
 * This probe flushes: clock_nanosleep (230), fcntl (72), dup/dup2 (32/33),
 * and whatever else a real libc binary triggers beyond the LLIBC baseline.
 *
 * Built: gcc -no-pie -o lgap.lxe tools/host/lgap.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("==== Tater Bridge: syscall gap probe ====\n");

    /* 1. nanosleep → clock_nanosleep (230) on modern glibc */
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        if (nanosleep(&ts, NULL) == 0) {
            printf("[OK] nanosleep(50ms) passed\n");
        } else {
            printf("[INFO] nanosleep failed: %s (errno=%d)\n",
                   strerror(errno), errno);
        }
    }

    /* 2. fcntl (72) → F_GETFL, F_SETFD */
    {
        int fd = open("/LXTEST.TXT", O_RDONLY);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                printf("[OK] fcntl(F_GETFL)=0x%x\n", flags);
            } else {
                printf("[INFO] fcntl(F_GETFL) failed: %s (errno=%d)\n",
                       strerror(errno), errno);
            }
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) {
                printf("[OK] fcntl(F_SETFD, FD_CLOEXEC) passed\n");
            } else {
                printf("[INFO] fcntl(F_SETFD) failed: %s (errno=%d)\n",
                       strerror(errno), errno);
            }
            close(fd);
        } else {
            printf("[FAIL] open for fcntl test: %s (errno=%d)\n",
                   strerror(errno), errno);
        }
    }

    /* 3. dup2 (33) */
    {
        int fd = open("/LXTEST.TXT", O_RDONLY);
        if (fd >= 0) {
            int dup_fd = dup2(fd, 10);
            if (dup_fd >= 0) {
                printf("[OK] dup2(%d, 10)=%d\n", fd, dup_fd);
                close(dup_fd);
            } else {
                printf("[INFO] dup2 failed: %s (errno=%d)\n",
                       strerror(errno), errno);
            }
            close(fd);
        }
    }

    /* 4. clock_getres (229) — often paired with clock_gettime */
    {
        struct timespec res;
        if (clock_getres(CLOCK_REALTIME, &res) == 0) {
            printf("[OK] clock_getres(CLOCK_REALTIME)=%ld.%09ld\n",
                   (long)res.tv_sec, (long)res.tv_nsec);
        } else {
            printf("[INFO] clock_getres failed: %s (errno=%d)\n",
                   strerror(errno), errno);
        }
    }

    printf("[OK] gap probe complete, calling exit(0)\n");
    return 0;
}
