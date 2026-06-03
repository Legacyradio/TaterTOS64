/*
 * lthr.c — dynamically linked Linux x86_64 probe exercising threading + gaps.
 *
 * Targets the syscalls Claude Code / Node.js use beyond the libc baseline:
 *   - clone (56)          → pthread_create
 *   - futex (202)         → pthread_mutex_lock/unlock, pthread_join
 *   - nanosleep (35)      → sleep/usleep
 *   - fcntl (72)          → fcntl(F_GETFL / F_SETFD)
 *   - dup / dup2 (32/33)  → dup2
 *   - tgkill (234)        → will come with real signals later
 *   - clock_getres (229)  → often paired with clock_gettime
 *
 * Built: gcc -no-pie -pthread -o lthr.lxe tools/host/lthr.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_counter = 0;

static void *thread_fn(void *arg) {
    int id = (int)(intptr_t)arg;
    printf("[OK] thread %d started\n", id);

    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&g_mtx);
        g_counter++;
        pthread_mutex_unlock(&g_mtx);
    }
    printf("[OK] thread %d done, counter=%d\n", id, g_counter);
    return (void *)(intptr_t)g_counter;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Force unbuffered output so we see diagnostics even if we crash */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("==== TaterTOS linuxulator: thread + gap probe ====\n");

    /* 1. pthread_create → clone syscall */
    pthread_t t1, t2;
    int rc = pthread_create(&t1, NULL, thread_fn, (void *)1);
    if (rc != 0) {
        printf("[FAIL] pthread_create #1: %s\n", strerror(rc));
        return 1;
    }
    printf("[OK] pthread_create #1 returned\n");

    rc = pthread_create(&t2, NULL, thread_fn, (void *)2);
    if (rc != 0) {
        printf("[FAIL] pthread_create #2: %s\n", strerror(rc));
        return 1;
    }
    printf("[OK] pthread_create #2 returned\n");

    /* 2. nanosleep → should trigger nanosleep syscall */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };  /* 50 ms */
    if (nanosleep(&ts, NULL) == 0) {
        printf("[OK] nanosleep(50ms) passed\n");
    } else {
        printf("[INFO] nanosleep failed: %s (non-fatal)\n", strerror(errno));
    }

    /* 3. fcntl */
    int fd = open("/LXTEST.TXT", O_RDONLY);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            printf("[OK] fcntl(F_GETFL)=0x%x\n", flags);
        } else {
            printf("[INFO] fcntl(F_GETFL) failed: %s\n", strerror(errno));
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) {
            printf("[OK] fcntl(F_SETFD, FD_CLOEXEC) passed\n");
        } else {
            printf("[INFO] fcntl(F_SETFD) failed: %s\n", strerror(errno));
        }
        close(fd);
    } else {
        printf("[FAIL] open for fcntl test: %s\n", strerror(errno));
    }

    /* 4. dup2 */
    int null_fd = open("/LXTEST.TXT", O_RDONLY);
    if (null_fd >= 0) {
        int dup_fd = dup2(null_fd, 100);
        if (dup_fd >= 0) {
            printf("[OK] dup2(%d, 100)=%d\n", null_fd, dup_fd);
            close(dup_fd);
        } else {
            printf("[INFO] dup2 failed: %s\n", strerror(errno));
        }
        close(null_fd);
    }

    /* 5. pthread_join → futex syscall */
    void *ret1, *ret2;
    pthread_join(t1, &ret1);
    printf("[OK] pthread_join #1, ret=%ld\n", (long)ret1);
    pthread_join(t2, &ret2);
    printf("[OK] pthread_join #2, ret=%ld\n", (long)ret2);

    printf("[OK] thread+gap probe complete, counter=%d, calling exit(0)\n", g_counter);
    return 0;
}
