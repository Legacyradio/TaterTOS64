/*
 * lfutex.c - dynamically linked Linux x86_64 probe for contended futex paths.
 *
 * Built by build_iso.sh with the host Linux toolchain:
 *   gcc -no-pie -pthread -o lfutex.lxe tools/host/lfutex.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define FUTEX_OP_SET 0
#define FUTEX_OP_ADD 1
#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    ((((op) & 0xf) << 28) | (((cmp) & 0xf) << 24) | \
     (((oparg) & 0xfff) << 12) | ((cmparg) & 0xfff))
#endif

static volatile uint32_t g_bitset_word;
static volatile uint32_t g_requeue_a;
static volatile uint32_t g_requeue_b;
static volatile uint32_t g_wakeop_a;
static volatile uint32_t g_wakeop_b;
static volatile int g_bitset_entered;
static volatile int g_requeue_entered;
static volatile int g_wakeop_entered;

static int futex_call(volatile uint32_t *uaddr, int op, uint32_t val,
                      const struct timespec *ts, volatile uint32_t *uaddr2,
                      uint32_t val3) {
    return (int)syscall(SYS_futex, uaddr, op, val, ts, uaddr2, val3);
}

static void abs_deadline_ms(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_nsec += (ms % 1000) * 1000000L;
    ts->tv_sec += ms / 1000 + ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

static int wait_until_entered(volatile int *counter, int want, const char *name) {
    for (int i = 0; i < 100000 && *counter < want; i++)
        sched_yield();
    usleep(50000);
    if (*counter < want) {
        printf("[FAIL] %s waiter did not enter futex wait (got %d want %d)\n",
               name, *counter, want);
        return 1;
    }
    return 0;
}

static void *wait_bitset_thread(void *arg) {
    (void)arg;
    struct timespec ts;
    abs_deadline_ms(&ts, 1000);
    __sync_fetch_and_add(&g_bitset_entered, 1);
    int rc = futex_call(&g_bitset_word,
                        FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                        0, &ts, NULL, 0x1u);
    if (rc != 0)
        printf("[FAIL] wait_bitset thread: errno=%d (%s)\n", errno, strerror(errno));
    return (void *)(intptr_t)rc;
}

static void *wait_requeue_a_thread(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&g_requeue_entered, 1);
    int rc = futex_call(&g_requeue_a, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        0, NULL, NULL, 0);
    if (rc != 0)
        printf("[FAIL] requeue waiter: errno=%d (%s)\n", errno, strerror(errno));
    return (void *)(intptr_t)rc;
}

static void *wait_wakeop_a_thread(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&g_wakeop_entered, 1);
    int rc = futex_call(&g_wakeop_a, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        0, NULL, NULL, 0);
    if (rc != 0)
        printf("[FAIL] wake_op uaddr1 waiter: errno=%d (%s)\n", errno, strerror(errno));
    return (void *)(intptr_t)rc;
}

static void *wait_wakeop_b_thread(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&g_wakeop_entered, 1);
    int rc = futex_call(&g_wakeop_b, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        0, NULL, NULL, 0);
    if (rc != 0)
        printf("[FAIL] wake_op uaddr2 waiter: errno=%d (%s)\n", errno, strerror(errno));
    return (void *)(intptr_t)rc;
}

static int join_zero(pthread_t t, const char *name) {
    void *ret = NULL;
    int rc = pthread_join(t, &ret);
    if (rc != 0) {
        printf("[FAIL] pthread_join(%s): %s\n", name, strerror(rc));
        return 1;
    }
    if ((intptr_t)ret != 0) {
        printf("[FAIL] %s returned %ld\n", name, (long)(intptr_t)ret);
        return 1;
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== TaterTOS linuxulator: futex probe ====\n");

    volatile uint32_t mismatch = 1;
    errno = 0;
    struct timespec zero = {0, 0};
    int rc = futex_call(&mismatch, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        0, &zero, NULL, 0);
    if (rc != -1 || errno != EAGAIN) {
        printf("[FAIL] FUTEX_WAIT mismatch rc=%d errno=%d\n", rc, errno);
        return 1;
    }
    printf("[OK] FUTEX_WAIT mismatch returned EAGAIN\n");

    volatile uint32_t timeout_word = 0;
    errno = 0;
    rc = futex_call(&timeout_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                    0, &zero, NULL, 0);
    if (rc != -1 || errno != ETIMEDOUT) {
        printf("[FAIL] FUTEX_WAIT zero timeout rc=%d errno=%d\n", rc, errno);
        return 1;
    }
    printf("[OK] FUTEX_WAIT zero timeout returned ETIMEDOUT\n");

    pthread_t tb;
    g_bitset_word = 0;
    g_bitset_entered = 0;
    if (pthread_create(&tb, NULL, wait_bitset_thread, NULL) != 0) {
        printf("[FAIL] pthread_create(bitset)\n");
        return 1;
    }
    if (wait_until_entered(&g_bitset_entered, 1, "bitset") != 0)
        return 1;
    rc = futex_call(&g_bitset_word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG,
                    1, NULL, NULL, 0x2u);
    if (rc != 0) {
        printf("[FAIL] FUTEX_WAKE_BITSET wrong mask woke %d\n", rc);
        return 1;
    }
    g_bitset_word = 1;
    rc = futex_call(&g_bitset_word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG,
                    1, NULL, NULL, 0x1u);
    if (rc != 1 || join_zero(tb, "bitset") != 0) {
        printf("[FAIL] FUTEX_WAKE_BITSET matching mask woke %d\n", rc);
        return 1;
    }
    printf("[OK] FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET passed\n");

    pthread_t tr1, tr2;
    g_requeue_a = 0;
    g_requeue_b = 0;
    g_requeue_entered = 0;
    pthread_create(&tr1, NULL, wait_requeue_a_thread, NULL);
    pthread_create(&tr2, NULL, wait_requeue_a_thread, NULL);
    if (wait_until_entered(&g_requeue_entered, 2, "requeue") != 0)
        return 1;
    rc = futex_call(&g_requeue_a, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG,
                    1, (const struct timespec *)(uintptr_t)1,
                    &g_requeue_b, 0);
    if (rc != 2) {
        printf("[FAIL] FUTEX_CMP_REQUEUE rc=%d errno=%d\n", rc, errno);
        return 1;
    }
    rc = futex_call(&g_requeue_b, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                    1, NULL, NULL, 0);
    if (rc != 1 || join_zero(tr1, "requeue-1") != 0 ||
        join_zero(tr2, "requeue-2") != 0) {
        printf("[FAIL] FUTEX_CMP_REQUEUE final wake rc=%d\n", rc);
        return 1;
    }
    printf("[OK] FUTEX_CMP_REQUEUE passed\n");

    pthread_t two1, two2;
    g_wakeop_a = 0;
    g_wakeop_b = 0;
    g_wakeop_entered = 0;
    pthread_create(&two1, NULL, wait_wakeop_a_thread, NULL);
    pthread_create(&two2, NULL, wait_wakeop_b_thread, NULL);
    if (wait_until_entered(&g_wakeop_entered, 2, "wakeop") != 0)
        return 1;
    rc = futex_call(&g_wakeop_a, FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG,
                    1, (const struct timespec *)(uintptr_t)1,
                    &g_wakeop_b, FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0));
    if (rc != 2 || g_wakeop_b != 1 || join_zero(two1, "wakeop-1") != 0 ||
        join_zero(two2, "wakeop-2") != 0) {
        printf("[FAIL] FUTEX_WAKE_OP rc=%d word2=%u\n", rc, g_wakeop_b);
        return 1;
    }
    printf("[OK] FUTEX_WAKE_OP passed\n");

    printf("[OK] futex probe complete\n");
    return 0;
}
