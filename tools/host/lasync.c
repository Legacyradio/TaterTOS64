/*
 * lasync.c - dynamically linked Linux x86_64 probe for async-I/O syscalls.
 *
 * Built by build_iso.sh with the host Linux toolchain:
 *   gcc -no-pie -o lasync.lxe tools/host/lasync.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what) {
    printf("[FAIL] %s: errno=%d (%s)\n", what, errno, strerror(errno));
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== TaterTOS linuxulator: async I/O probe ====\n");

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) return fail("eventfd2");

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) return fail("epoll_create1");

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0xEFDULL;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0)
        return fail("epoll_ctl(eventfd)");

    struct epoll_event out[4];
    int n = epoll_wait(ep, out, 4, 0);
    if (n < 0) return fail("epoll_wait(initial)");
    if (n != 0) {
        printf("[FAIL] epoll_wait(initial) returned %d, expected 0\n", n);
        return 1;
    }

    uint64_t one = 1;
    if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one))
        return fail("write(eventfd)");

    n = epoll_wait(ep, out, 4, 50);
    if (n != 1 || out[0].data.u64 != 0xEFDULL || !(out[0].events & EPOLLIN)) {
        printf("[FAIL] epoll_wait(eventfd) n=%d events=0x%x data=0x%llx\n",
               n, n > 0 ? out[0].events : 0,
               n > 0 ? (unsigned long long)out[0].data.u64 : 0ULL);
        return 1;
    }

    uint64_t got = 0;
    if (read(efd, &got, sizeof(got)) != (ssize_t)sizeof(got) || got != 1)
        return fail("read(eventfd)");
    printf("[OK] eventfd + epoll readiness passed\n");

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) return fail("timerfd_create");

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x7FDULL;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) < 0)
        return fail("epoll_ctl(timerfd)");

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 1000000; /* 1ms */
    if (timerfd_settime(tfd, 0, &its, NULL) < 0)
        return fail("timerfd_settime");

    struct timespec nap = {0, 10000000}; /* 10ms */
    nanosleep(&nap, NULL);

    n = epoll_wait(ep, out, 4, 50);
    int saw_timer = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].data.u64 == 0x7FDULL && (out[i].events & EPOLLIN))
            saw_timer = 1;
    }
    if (!saw_timer) {
        printf("[FAIL] epoll_wait(timerfd) n=%d\n", n);
        return 1;
    }
    got = 0;
    if (read(tfd, &got, sizeof(got)) != (ssize_t)sizeof(got) || got < 1)
        return fail("read(timerfd)");
    printf("[OK] timerfd + epoll readiness passed\n");

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    int sfd = (int)syscall(SYS_signalfd4, -1, &mask, 8,
                           SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) return fail("signalfd4");
    struct signalfd_siginfo si;
    ssize_t sr = read(sfd, &si, sizeof(si));
    if (sr != -1 || errno != EAGAIN) {
        printf("[FAIL] signalfd empty read sr=%ld errno=%d\n", (long)sr, errno);
        return 1;
    }
    printf("[OK] signalfd4 create + empty nonblock read passed\n");

    close(sfd);
    close(tfd);
    close(efd);
    close(ep);
    printf("[OK] async I/O probe complete\n");
    return 0;
}
