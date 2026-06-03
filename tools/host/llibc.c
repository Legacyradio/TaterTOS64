/*
 * llibc.c — dynamically linked Linux x86_64 ELF probe exercising libc.
 *
 * Built with the host Linux toolchain (gcc -no-pie), this is a real
 * glibc-dependent binary that calls libc wrappers (printf, fopen, malloc,
 * mmap, etc.) instead of raw syscalls. Its purpose: flush out all the
 * syscalls glibc startup + basic C operations need, so we know exactly
 * what's missing before attempting a Claude Code or Node.js binary.
 *
 * Does NOT use nostdlib — intentionally pulls in full libc init.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <errno.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("==== TaterTOS linuxulator: libc probe ====\n");

    /* 1. printf → write syscall via libc */
    printf("[OK] printf works via libc\n");

    /* 2. malloc → brk */
    void *p = malloc(65536);
    if (p) {
        int n = snprintf(p, 64, "[OK] malloc(64K) returned %p", p);
        printf("%s\n", (char *)p);
        free(p);
    } else {
        printf("[FAIL] malloc(64K) returned NULL\n");
    }

    /* 3. fopen / fread */
    FILE *f = fopen("/LXTEST.TXT", "r");
    if (f) {
        char buf[128];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        if (n > 0) {
            buf[n] = 0;
            printf("[OK] fopen+fread(%zu bytes): '%.40s'\n", n, buf);
        } else {
            printf("[FAIL] fread returned 0\n");
        }
        fclose(f);
    } else {
        printf("[FAIL] fopen(\"/LXTEST.TXT\") failed: %s\n", strerror(errno));
    }

    /* 4. open / fstat / mmap */
    int fd = open("/LXTEST.TXT", O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) {
            void *map = mmap(NULL, (size_t)st.st_size, PROT_READ,
                             MAP_PRIVATE, fd, 0);
            if (map != MAP_FAILED) {
                printf("[OK] mmap(%ld bytes): '%.40s'\n",
                       (long)st.st_size, (char *)map);
                munmap(map, (size_t)st.st_size);
            } else {
                printf("[FAIL] mmap failed: %s\n", strerror(errno));
            }
        } else {
            printf("[FAIL] fstat failed: %s\n", strerror(errno));
        }
        close(fd);
    } else {
        printf("[FAIL] open(\"/LXTEST.TXT\") failed: %s\n", strerror(errno));
    }

    /* 5. getpid */
    printf("[OK] getpid=%d\n", (int)getpid());

    /* 6. brk stress — 10 small allocations */
    for (int i = 0; i < 10; i++) {
        void *x = malloc(4096);
        if (!x) {
            printf("[FAIL] malloc #%d returned NULL\n", i);
            return 1;
        }
        memset(x, 0xCC, 4096);
        free(x);
    }
    printf("[OK] 10x malloc/free loop passed\n");

    /* 7. writev test — printf uses writev internally on some glibc */
    struct iovec iov[2];
    iov[0].iov_base = "[OK] writev";
    iov[0].iov_len  = 12;
    iov[1].iov_base = " reached\n";
    iov[1].iov_len  = 9;
    ssize_t wv = writev(1, iov, 2);
    if (wv < 0) {
        printf("[FAIL] writev: %s\n", strerror(errno));
    }

    /* 8. getcwd */
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("[OK] getcwd=%s\n", cwd);
    } else {
        printf("[OK] getcwd failed (expected): %s\n", strerror(errno));
    }

    printf("[OK] libc probe complete, calling exit(0)\n");
    return 0;
}
