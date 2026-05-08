/*
 * smoketest.c — New syscall smoke test for TaterTOS64v3
 *
 * Exercises: pipe2, dup3, socketpair, getcwd, chdir, accept4,
 * timerfd, signalfd, inotify, memfd_create, sendfile
 */

#include "libc.h"
#include "fry.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/memfd.h>
#include <sys/sendfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) do { printf("  %-32s ... ", name); fflush(stdout); } while(0)
#define PASS() do { puts("PASS"); g_pass++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_fail++; } while(0)
#define FAIL_ERRNO(msg) do { printf("FAIL: %s (errno=%d)\n", msg, errno); g_fail++; } while(0)

/* -----------------------------------------------------------------------
 * pipe2
 * ----------------------------------------------------------------------- */
static void test_pipe2(void) {
    TEST("pipe2(O_NONBLOCK)");
    int fds[2];
    int rc = pipe2(fds, O_NONBLOCK);
    if (rc < 0) { FAIL_ERRNO("pipe2"); return; }
    /* Should be non-blocking: read on empty pipe returns EAGAIN */
    char buf[4];
    int nr = read(fds[0], buf, sizeof(buf));
    if (nr >= 0) { FAIL("non-blocking read should have returned -1/EAGAIN"); close(fds[0]); close(fds[1]); return; }
    close(fds[0]); close(fds[1]);
    PASS();
}

static void test_pipe2_cloexec(void) {
    TEST("pipe2(O_CLOEXEC)");
    int fds[2];
    int rc = pipe2(fds, O_CLOEXEC);
    if (rc < 0) { FAIL_ERRNO("pipe2"); return; }
    int fl = fcntl(fds[0], F_GETFD, 0);
    if (fl < 0 || !(fl & FD_CLOEXEC)) { FAIL("FD_CLOEXEC not set on read end"); close(fds[0]); close(fds[1]); return; }
    fl = fcntl(fds[1], F_GETFD, 0);
    if (fl < 0 || !(fl & FD_CLOEXEC)) { FAIL("FD_CLOEXEC not set on write end"); close(fds[0]); close(fds[1]); return; }
    close(fds[0]); close(fds[1]);
    PASS();
}

static void test_pipe2_invalid(void) {
    TEST("pipe2(invalid flags)");
    int fds[2];
    int rc = pipe2(fds, 0xDEAD);
    if (rc == 0) { FAIL("should have rejected invalid flags"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * dup3
 * ----------------------------------------------------------------------- */
static void test_dup3(void) {
    TEST("dup3(O_CLOEXEC)");
    int fds[2];
    if (pipe(fds) < 0) { FAIL_ERRNO("pipe"); return; }
    int newfd = fds[1] + 5;
    if (newfd >= 64) newfd = 10;
    int rc = dup3(fds[0], newfd, O_CLOEXEC);
    if (rc < 0) { FAIL_ERRNO("dup3"); close(fds[0]); close(fds[1]); return; }
    int fl = fcntl(newfd, F_GETFD, 0);
    if (fl < 0 || !(fl & FD_CLOEXEC)) { FAIL("FD_CLOEXEC not set"); close(fds[0]); close(fds[1]); close(newfd); return; }
    close(fds[0]); close(fds[1]); close(newfd);
    PASS();
}

static void test_dup3_same_fd(void) {
    TEST("dup3(oldfd==newfd)");
    int fds[2];
    if (pipe(fds) < 0) { FAIL_ERRNO("pipe"); return; }
    int rc = dup3(fds[0], fds[0], O_CLOEXEC);
    if (rc < 0) { FAIL_ERRNO("dup3 same fd"); close(fds[0]); close(fds[1]); return; }
    /* Should return newfd without closing it */
    close(fds[0]); close(fds[1]);
    PASS();
}

/* -----------------------------------------------------------------------
 * socketpair
 * ----------------------------------------------------------------------- */
static void test_socketpair(void) {
    TEST("socketpair(AF_UNIX, SOCK_STREAM)");
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (rc < 0) { FAIL_ERRNO("socketpair"); return; }
    /* Write from one end, read from the other */
    const char *msg = "hello";
    int nw = write(sv[0], msg, 6);
    if (nw != 6) { FAIL_ERRNO("write"); close(sv[0]); close(sv[1]); return; }
    char buf[16];
    int nr = read(sv[1], buf, sizeof(buf));
    if (nr != 6 || memcmp(buf, msg, 6) != 0) { FAIL("read mismatch"); close(sv[0]); close(sv[1]); return; }
    /* Write back the other way */
    nw = write(sv[1], "world", 6);
    if (nw != 6) { FAIL_ERRNO("write2"); close(sv[0]); close(sv[1]); return; }
    nr = read(sv[0], buf, sizeof(buf));
    if (nr != 6 || memcmp(buf, "world", 6) != 0) { FAIL("read2 mismatch"); close(sv[0]); close(sv[1]); return; }
    close(sv[0]); close(sv[1]);
    PASS();
}

/* -----------------------------------------------------------------------
 * getcwd / chdir
 * ----------------------------------------------------------------------- */
static void test_getcwd(void) {
    TEST("getcwd()");
    char buf[256];
    char *r = getcwd(buf, sizeof(buf));
    if (!r) { FAIL_ERRNO("getcwd"); return; }
    if (buf[0] != '/') { FAIL("doesn't start with /"); return; }
    PASS();
}

static void test_chdir(void) {
    TEST("chdir('/') + getcwd()");
    if (chdir("/") < 0) { FAIL_ERRNO("chdir(/)"); return; }
    char buf[256];
    if (!getcwd(buf, sizeof(buf))) { FAIL_ERRNO("getcwd after chdir"); return; }
    if (strcmp(buf, "/") != 0) { FAIL("cwd should be /"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * memfd_create
 * ----------------------------------------------------------------------- */
static void test_memfd_basic(void) {
    TEST("memfd_create() + write + read");
    int fd = memfd_create("test", 0);
    if (fd < 0) { FAIL_ERRNO("memfd_create"); return; }
    const char *data = "Hello, memfd!";
    int nw = write(fd, data, strlen(data) + 1);
    if (nw < 0) { FAIL_ERRNO("write"); close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    char buf[64];
    int nr = read(fd, buf, sizeof(buf));
    if (nr < 0) { FAIL_ERRNO("read"); close(fd); return; }
    if (memcmp(buf, data, strlen(data) + 1) != 0) { FAIL("data mismatch"); close(fd); return; }
    close(fd);
    PASS();
}

static void test_memfd_truncate(void) {
    TEST("memfd_create() + ftruncate + lseek");
    int fd = memfd_create("trunc", 0);
    if (fd < 0) { FAIL_ERRNO("memfd_create"); return; }
    if (fry_ftruncate(fd, 4096) < 0) { FAIL_ERRNO("ftruncate"); close(fd); return; }
    off_t pos = lseek(fd, 0, SEEK_END);
    if (pos != 4096) { FAIL("wrong size after ftruncate"); close(fd); return; }
    pos = lseek(fd, 0, SEEK_SET);
    if (pos != 0) { FAIL("lseek back failed"); close(fd); return; }
    /* Write beyond 4096 to trigger growth */
    char big[8192];
    memset(big, 'A', sizeof(big));
    int nw = write(fd, big, sizeof(big));
    if (nw != 8192) { FAIL_ERRNO("write big"); close(fd); return; }
    pos = lseek(fd, 0, SEEK_END);
    if (pos != 8192) { printf("pos=%ld expected 8192", (long)pos); FAIL("wrong size after write"); close(fd); return; }
    close(fd);
    PASS();
}

/* -----------------------------------------------------------------------
 * sendfile (memfd -> memfd)
 * ----------------------------------------------------------------------- */
static void test_sendfile(void) {
    TEST("sendfile(memfd->memfd)");
    int in_fd = memfd_create("src", 0);
    if (in_fd < 0) { FAIL_ERRNO("memfd_create src"); return; }
    const char *payload = "sendfile test data 12345";
    write(in_fd, payload, strlen(payload) + 1);
    lseek(in_fd, 0, SEEK_SET);

    int out_fd = memfd_create("dst", 0);
    if (out_fd < 0) { FAIL_ERRNO("memfd_create dst"); close(in_fd); return; }

    off_t offset = 0;
    ssize_t n = sendfile(out_fd, in_fd, &offset, 64);
    if (n < 0) { FAIL_ERRNO("sendfile"); close(in_fd); close(out_fd); return; }
    if ((size_t)n != strlen(payload) + 1) { printf("n=%ld expected %lu", (long)n, (unsigned long)(strlen(payload) + 1)); FAIL("wrong byte count"); close(in_fd); close(out_fd); return; }
    lseek(out_fd, 0, SEEK_SET);
    char buf[64];
    int nr = read(out_fd, buf, sizeof(buf));
    if (nr < 0 || memcmp(buf, payload, strlen(payload) + 1) != 0) { FAIL("data mismatch after sendfile"); close(in_fd); close(out_fd); return; }
    close(in_fd); close(out_fd);
    PASS();
}

/* -----------------------------------------------------------------------
 * accept4 — just verify the syscall returns something reasonable
 * (we can't fully test without a listening socket and a connection)
 * ----------------------------------------------------------------------- */
static void test_accept4_nofd(void) {
    TEST("accept4(EBADF behavior)");
    int rc = accept4(-1, 0, 0, SOCK_NONBLOCK);
    if (rc >= 0) { FAIL("should have failed with EBADF"); return; }
    PASS();
}

/* -----------------------------------------------------------------------
 * timerfd — basic create + settime + gettime
 * ----------------------------------------------------------------------- */
static void test_timerfd(void) {
    TEST("timerfd_create + settime + gettime");
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) { FAIL_ERRNO("timerfd_create"); return; }

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 50000000; /* 50ms */
    spec.it_interval.tv_sec = 0;
    spec.it_interval.tv_nsec = 10000000; /* 10ms periodic */

    if (timerfd_settime(fd, 0, &spec, 0) < 0) { FAIL_ERRNO("timerfd_settime"); close(fd); return; }

    struct itimerspec curr;
    if (timerfd_gettime(fd, &curr) < 0) { FAIL_ERRNO("timerfd_gettime"); close(fd); return; }

    /* Should have remaining time */
    if (curr.it_value.tv_sec == 0 && curr.it_value.tv_nsec == 0) {
        FAIL("timer already expired");
        close(fd);
        return;
    }

    close(fd);
    PASS();
}

/* -----------------------------------------------------------------------
 * signalfd — basic create
 * ----------------------------------------------------------------------- */
static void test_signalfd(void) {
    TEST("signalfd()");
    uint64_t mask = 0;
    fry_sigemptyset((uint32_t *)&mask);
    fry_sigaddset((uint32_t *)&mask, SIGCHLD);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (fd < 0) { FAIL_ERRNO("signalfd"); return; }

    /* Read should return EAGAIN (no signals pending) */
    struct signalfd_siginfo info;
    int nr = read(fd, &info, sizeof(info));
    if (nr >= 0) { FAIL("should have returned EAGAIN"); close(fd); return; }

    close(fd);
    PASS();
}

/* -----------------------------------------------------------------------
 * inotify — basic init + add_watch + rm_watch
 * ----------------------------------------------------------------------- */
static void test_inotify(void) {
    TEST("inotify_init + add_watch + rm_watch");
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) { FAIL_ERRNO("inotify_init1"); return; }

    int wd = inotify_add_watch(fd, "/", IN_CREATE | IN_DELETE);
    if (wd < 0) { FAIL_ERRNO("inotify_add_watch"); close(fd); return; }

    /* Read should return EAGAIN (no events yet) */
    char evbuf[256];
    int nr = read(fd, evbuf, sizeof(evbuf));
    if (nr >= 0) { FAIL("should have returned EAGAIN"); close(fd); return; }

    /* Remove the watch */
    if (inotify_rm_watch(fd, wd) < 0) { FAIL_ERRNO("inotify_rm_watch"); close(fd); return; }

    close(fd);
    PASS();
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
int main(void) {
    puts("");
    puts("===================================");
    puts("  TaterTOS64v3 syscall smoke test");
    puts("===================================");
    puts("");

    /* Phase 3: IPC / descriptor model */
    test_pipe2();
    test_pipe2_cloexec();
    test_pipe2_invalid();
    test_dup3();
    test_dup3_same_fd();
    test_socketpair();

    /* Phase 8: getcwd/chdir cleanup */
    test_getcwd();
    test_chdir();

    /* Phase 9: accept4, timerfd, signalfd, inotify */
    test_accept4_nofd();
    test_timerfd();
    test_signalfd();
    test_inotify();

    /* Phase 10: memfd_create, sendfile */
    test_memfd_basic();
    test_memfd_truncate();
    test_sendfile();

    puts("");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    puts("===================================");

    return g_fail > 0 ? 1 : 0;
}
