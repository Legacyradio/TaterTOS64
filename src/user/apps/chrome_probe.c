/*
 * chrome_probe.c — Chrome/GN compatibility probe for TaterTOS64v3
 *
 * Tests that Chromium-facing libc headers/symbols compile, link, and
 * return plausible values. Run before dragging in a 30GB Chromium checkout.
 *
 * Target syscalls (next likely Chromium symbols):
 *   uname, sysinfo, getrusage, getpriority/setpriority,
 *   fsync/fdatasync, sched_getaffinity/sched_setaffinity,
 *   pthread_barrier_*, pthread_spin_*, splice, tee
 */

#include "libc.h"
#include "fry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Test framework
 * ----------------------------------------------------------------------- */
static int g_pass = 0;
static int g_fail = 0;

#define TEST(name)   do { printf("  %-36s ... ", name); fflush(stdout); } while(0)
#define PASS()       do { puts("PASS"); g_pass++; } while(0)
#define FAIL(msg)    do { printf("FAIL: %s\n", msg); g_fail++; } while(0)
#define FAIL_ERRNO(msg) do { printf("FAIL: %s (errno=%d)\n", msg, errno); g_fail++; } while(0)

/* -----------------------------------------------------------------------
 * uname
 * ----------------------------------------------------------------------- */
static void test_uname(void) {
    TEST("uname()");
    struct utsname buf;
    int rc = uname(&buf);
    if (rc < 0) { FAIL_ERRNO("uname"); return; }
    printf("sysname=%s nodename=%s release=%s version=%s machine=%s ",
           buf.sysname, buf.nodename, buf.release, buf.version, buf.machine);
    PASS();
}

/* -----------------------------------------------------------------------
 * sysinfo
 * ----------------------------------------------------------------------- */
static void test_sysinfo(void) {
    TEST("sysinfo()");
    struct sysinfo info;
    int rc = sysinfo(&info);
    if (rc < 0) { FAIL_ERRNO("sysinfo"); return; }
    printf("uptime=%lu procs=%u ",
           (unsigned long)info.uptime, info.procs);
    PASS();
}

/* -----------------------------------------------------------------------
 * getrusage
 * ----------------------------------------------------------------------- */
static void test_getrusage(void) {
    TEST("getrusage(SELF)");
    struct rusage usage;
    int rc = getrusage(RUSAGE_SELF, &usage);
    if (rc < 0) { FAIL_ERRNO("getrusage"); return; }
    printf("utime=%lld.%06ld ",
           (long long)usage.ru_utime.tv_sec,
           (long)usage.ru_utime.tv_usec);
    PASS();
}

/* -----------------------------------------------------------------------
 * getpriority / setpriority
 * ----------------------------------------------------------------------- */
static void test_priority(void) {
    TEST("getpriority(PRIO_PROCESS)");
    errno = 0;
    int prio = getpriority(PRIO_PROCESS, getpid());
    if (prio == -1 && errno != 0) { FAIL_ERRNO("getpriority"); return; }
    printf("prio=%d ", prio);

    int rc = setpriority(PRIO_PROCESS, getpid(), prio);
    if (rc < 0) { FAIL_ERRNO("setpriority"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * fsync / fdatasync (on /dev/null equivalent — try stdin/pipe)
 * ----------------------------------------------------------------------- */
static void test_fsync(void) {
    TEST("fsync() on pipe");
    int p[2];
    if (pipe(p) < 0) { FAIL_ERRNO("pipe"); return; }
    int rc = fsync(p[1]);
    /* fsync on a pipe returns -EINVAL normally, but should not crash */
    if (rc != 0 && errno != EINVAL && errno != EROFS && errno != EINVAL) {
        close(p[0]); close(p[1]);
        FAIL_ERRNO("fsync unexpected errno");
        return;
    }
    close(p[0]); close(p[1]);
    PASS();
}

static void test_fdatasync(void) {
    TEST("fdatasync() on pipe");
    int p[2];
    if (pipe(p) < 0) { FAIL_ERRNO("pipe"); return; }
    int rc = fdatasync(p[1]);
    if (rc != 0 && errno != EINVAL) {
        close(p[0]); close(p[1]);
        FAIL_ERRNO("fdatasync unexpected errno");
        return;
    }
    close(p[0]); close(p[1]);
    PASS();
}

/* -----------------------------------------------------------------------
 * sched_getaffinity / sched_setaffinity
 * ----------------------------------------------------------------------- */
static void test_affinity(void) {
    TEST("sched_getaffinity()");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    int rc = sched_getaffinity(0, sizeof(mask), &mask);
    if (rc < 0) { FAIL_ERRNO("sched_getaffinity"); return; }
    int ncpus = 0;
    for (int i = 0; i < (int)(sizeof(mask)*8); i++) {
        if (CPU_ISSET(i, &mask)) ncpus++;
    }
    printf("cpus=%d ", ncpus);

    /* set back the same mask */
    rc = sched_setaffinity(0, sizeof(mask), &mask);
    if (rc < 0) { FAIL_ERRNO("sched_setaffinity"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * pthread_barrier (link-only check via create+wait+destroy)
 * ----------------------------------------------------------------------- */
static void test_pthread_barrier(void) {
    TEST("pthread_barrier_init/wait/destroy");
    pthread_barrier_t barrier;
    int rc = pthread_barrier_init(&barrier, NULL, 1);
    if (rc != 0) { FAIL("pthread_barrier_init failed"); return; }
    rc = pthread_barrier_wait(&barrier);
    if (rc != PTHREAD_BARRIER_SERIAL_THREAD && rc != 0) {
        FAIL("pthread_barrier_wait failed");
        pthread_barrier_destroy(&barrier);
        return;
    }
    rc = pthread_barrier_destroy(&barrier);
    if (rc != 0) { FAIL("pthread_barrier_destroy failed"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * pthread_spinlock (link-only check via lock/unlock/destroy)
 * ----------------------------------------------------------------------- */
static void test_pthread_spinlock(void) {
    TEST("pthread_spin_init/lock/unlock/destroy");
    pthread_spinlock_t spin;
    int rc = pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
    if (rc != 0) { FAIL("pthread_spin_init failed"); return; }
    rc = pthread_spin_lock(&spin);
    if (rc != 0) { FAIL("pthread_spin_lock failed"); goto out; }
    rc = pthread_spin_unlock(&spin);
    if (rc != 0) { FAIL("pthread_spin_unlock failed"); goto out; }
    rc = pthread_spin_destroy(&spin);
    if (rc != 0) { FAIL("pthread_spin_destroy failed"); goto out; }
    PASS();
    return;
out:
    pthread_spin_destroy(&spin);
}

/* -----------------------------------------------------------------------
 * splice / tee (link-only — syscall wrappers)
 * ----------------------------------------------------------------------- */
static void test_splice(void) {
    TEST("splice() link check");
    int p[2];
    if (pipe(p) < 0) { FAIL_ERRNO("pipe"); return; }
    /* splice(fd_in, NULL, fd_out, NULL, 0, 0) should return 0 or -EINVAL */
    errno = 0;
    ssize_t rc = splice(p[0], NULL, p[1], NULL, 0, 0);
    int saved = errno;
    close(p[0]); close(p[1]);
    if (rc < 0 && saved != EINVAL && saved != ENOSYS) {
        FAIL_ERRNO("splice unexpected errno");
        return;
    }
    PASS();
}

static void test_tee(void) {
    TEST("tee() link check");
    int p[2];
    if (pipe(p) < 0) { FAIL_ERRNO("pipe"); return; }
    errno = 0;
    ssize_t rc = tee(p[0], p[1], 0, 0);
    int saved = errno;
    close(p[0]); close(p[1]);
    if (rc < 0 && saved != EINVAL && saved != ENOSYS) {
        FAIL_ERRNO("tee unexpected errno");
        return;
    }
    PASS();
}

/* -----------------------------------------------------------------------
 * mlock / munlock
 * ----------------------------------------------------------------------- */
static void test_mlock(void) {
    TEST("mlock/munlock()");
    void *page = (void*)(uintptr_t)fry_mmap(NULL, 4096, 3, 0x22, -1, 0);
    if (!page || page == (void*)-1) { FAIL("mmap for test page failed"); return; }
    int rc = mlock(page, 4096);
    if (rc < 0 && errno != ENOSYS) { FAIL_ERRNO("mlock"); goto out; }
    rc = munlock(page, 4096);
    if (rc < 0 && errno != ENOSYS) { FAIL_ERRNO("munlock"); goto out; }
    PASS();
out:
    fry_munmap(page, 4096);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    puts("Chrome/GN compatibility probe for TaterTOS64v3\n");

    test_uname();
    test_sysinfo();
    test_getrusage();
    test_priority();
    test_fsync();
    test_fdatasync();
    test_affinity();
    test_pthread_barrier();
    test_pthread_spinlock();
    test_splice();
    test_tee();
    test_mlock();

    printf("\n--- chrome_probe: %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
