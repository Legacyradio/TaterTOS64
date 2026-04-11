#include "../libc/libc.h"
#include <stdint.h>
#include <errno.h>

#define SMOKE_WORKERS 4
#define SMOKE_LOOPS 500

#define CONTEND_WORKERS 8
#define CONTEND_LOOPS 4000

#define COND_WORKERS 6
#define COND_ROUNDS 24

#define CHURN_WORKERS 6
#define CHURN_ROUNDS 12

#define FUTEX_TIMEOUT_MS 25
#define FUTEX_WAKE_DELAY_MS 10

static void fail(const char *msg, int code) {
    printf("thtest: %s\n", msg);
    fry_exit(code);
}

static void check(int ok, const char *msg, int code) {
    if (!ok) fail(msg, code);
}

static fry_mutex_t g_smoke_mutex = FRY_MUTEX_INIT;
static fry_cond_t g_smoke_cond = FRY_COND_INIT;
static fry_sem_t g_smoke_sem;
static fry_once_t g_smoke_once = FRY_ONCE_INIT;
static fry_tls_key_t g_smoke_tls_key;
static volatile int g_smoke_once_count;
static volatile int g_smoke_ready_count;
static volatile int g_smoke_go_flag;
static volatile int g_smoke_counter;
static volatile uintptr_t g_smoke_tls_seen[SMOKE_WORKERS];
static volatile uintptr_t g_smoke_tls_base_seen[SMOKE_WORKERS];
static volatile long g_smoke_worker_pid[SMOKE_WORKERS];
static volatile long g_smoke_worker_tid[SMOKE_WORKERS];

static void smoke_reset(void) {
    g_smoke_mutex = (fry_mutex_t)FRY_MUTEX_INIT;
    g_smoke_cond = (fry_cond_t)FRY_COND_INIT;
    g_smoke_once = (fry_once_t)FRY_ONCE_INIT;
    g_smoke_once_count = 0;
    g_smoke_ready_count = 0;
    g_smoke_go_flag = 0;
    g_smoke_counter = 0;
    for (int i = 0; i < SMOKE_WORKERS; i++) {
        g_smoke_tls_seen[i] = 0;
        g_smoke_tls_base_seen[i] = 0;
        g_smoke_worker_pid[i] = 0;
        g_smoke_worker_tid[i] = 0;
    }
}

static void smoke_once_init(void) {
    g_smoke_once_count++;
}

static void smoke_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    int rc;

    rc = fry_once(&g_smoke_once, smoke_once_init);
    if (rc < 0) fry_thread_exit(10 + id);

    rc = fry_tls_set(g_smoke_tls_key, (void *)(uintptr_t)(0x1000u + (uint32_t)id));
    if (rc < 0) fry_thread_exit(20 + id);

    g_smoke_tls_seen[id] = (uintptr_t)fry_tls_get(g_smoke_tls_key);
    g_smoke_tls_base_seen[id] = (uintptr_t)fry_tls_get_base();
    g_smoke_worker_pid[id] = fry_getpid();
    g_smoke_worker_tid[id] = fry_gettid();

    rc = fry_mutex_lock(&g_smoke_mutex);
    if (rc < 0) fry_thread_exit(30 + id);
    g_smoke_ready_count++;
    rc = fry_cond_signal(&g_smoke_cond);
    if (rc < 0) {
        fry_mutex_unlock(&g_smoke_mutex);
        fry_thread_exit(40 + id);
    }
    while (!g_smoke_go_flag) {
        rc = fry_cond_wait(&g_smoke_cond, &g_smoke_mutex);
        if (rc < 0) {
            fry_mutex_unlock(&g_smoke_mutex);
            fry_thread_exit(50 + id);
        }
    }
    rc = fry_mutex_unlock(&g_smoke_mutex);
    if (rc < 0) fry_thread_exit(60 + id);

    for (int i = 0; i < SMOKE_LOOPS; i++) {
        rc = fry_mutex_lock(&g_smoke_mutex);
        if (rc < 0) fry_thread_exit(70 + id);
        g_smoke_counter++;
        rc = fry_mutex_unlock(&g_smoke_mutex);
        if (rc < 0) fry_thread_exit(80 + id);
    }

    rc = fry_sem_post(&g_smoke_sem);
    if (rc < 0) fry_thread_exit(90 + id);

    fry_thread_exit(100 + id);
}

static void run_smoke_test(void) {
    struct fry_thread threads[SMOKE_WORKERS] = {{0}};
    long main_pid = fry_getpid();
    long main_tid = fry_gettid();
    uintptr_t main_tls_value = 0xABC0u;
    int rc;

    smoke_reset();

    rc = fry_sem_init(&g_smoke_sem, 0);
    check(rc == 0, "smoke sem init failed", 1);

    rc = fry_tls_key_create(&g_smoke_tls_key);
    check(rc == 0, "smoke tls key create failed", 2);

    rc = fry_tls_set(g_smoke_tls_key, (void *)main_tls_value);
    check(rc == 0, "smoke main tls set failed", 3);
    check((uintptr_t)fry_tls_get(g_smoke_tls_key) == main_tls_value,
          "smoke main tls get failed", 4);
    check((uintptr_t)fry_tls_get_base() != 0u, "smoke main tls base missing", 5);

    for (int i = 0; i < SMOKE_WORKERS; i++) {
        rc = (int)fry_thread_create(&threads[i], smoke_worker, (void *)(intptr_t)i);
        check(rc >= 0, "smoke thread create failed", 6);
    }

    rc = fry_mutex_lock(&g_smoke_mutex);
    check(rc == 0, "smoke main mutex lock failed", 7);
    while (g_smoke_ready_count < SMOKE_WORKERS) {
        rc = fry_cond_wait(&g_smoke_cond, &g_smoke_mutex);
        if (rc < 0) {
            fry_mutex_unlock(&g_smoke_mutex);
            fail("smoke main cond wait failed", 8);
        }
    }
    g_smoke_go_flag = 1;
    rc = fry_cond_broadcast(&g_smoke_cond);
    if (rc < 0) {
        fry_mutex_unlock(&g_smoke_mutex);
        fail("smoke main cond broadcast failed", 9);
    }
    rc = fry_mutex_unlock(&g_smoke_mutex);
    check(rc == 0, "smoke main mutex unlock failed", 10);

    for (int i = 0; i < SMOKE_WORKERS; i++) {
        rc = fry_sem_wait(&g_smoke_sem);
        check(rc == 0, "smoke sem wait failed", 11);
    }

    for (int i = 0; i < SMOKE_WORKERS; i++) {
        int exit_code = -1;
        rc = (int)fry_thread_join(&threads[i], &exit_code);
        check(rc == 0, "smoke thread join failed", 12);
        check(exit_code == 100 + i, "smoke thread exit code mismatch", 13);
    }

    check(g_smoke_once_count == 1, "smoke once count mismatch", 14);
    check(g_smoke_counter == SMOKE_WORKERS * SMOKE_LOOPS,
          "smoke counter mismatch", 15);
    check((uintptr_t)fry_tls_get(g_smoke_tls_key) == main_tls_value,
          "smoke main tls changed", 16);

    for (int i = 0; i < SMOKE_WORKERS; i++) {
        check(g_smoke_worker_pid[i] == main_pid, "smoke worker pid mismatch", 17);
        check(g_smoke_worker_tid[i] > 0 && g_smoke_worker_tid[i] != main_tid,
              "smoke worker tid mismatch", 18);
        check(g_smoke_tls_seen[i] == (uintptr_t)(0x1000u + (uint32_t)i),
              "smoke worker tls value mismatch", 19);
        check(g_smoke_tls_base_seen[i] != 0u, "smoke worker tls base missing", 20);
        for (int j = i + 1; j < SMOKE_WORKERS; j++) {
            check(g_smoke_worker_tid[i] != g_smoke_worker_tid[j],
                  "smoke duplicate worker tid", 21);
            check(g_smoke_tls_base_seen[i] != g_smoke_tls_base_seen[j],
                  "smoke tls bases not unique", 22);
        }
    }
}

static volatile uint32_t g_futex_word;
static volatile int g_futex_wait_rc;
static fry_sem_t g_futex_ready_sem;
static fry_sem_t g_futex_done_sem;

static void futex_waiter(void *arg) {
    int rc;
    (void)arg;
    rc = fry_sem_post(&g_futex_ready_sem);
    if (rc < 0) fry_thread_exit(1);
    g_futex_wait_rc = (int)fry_futex_wait(&g_futex_word, 0u, 0);
    rc = fry_sem_post(&g_futex_done_sem);
    if (rc < 0) fry_thread_exit(2);
    fry_thread_exit(0);
}

static void run_futex_test(void) {
    struct fry_thread waiter = {0};
    long rc;
    long start_ms;
    long end_ms;
    int exit_code = -1;

    g_futex_word = 1u;
    rc = fry_futex_wait(&g_futex_word, 0u, FUTEX_TIMEOUT_MS);
    check(rc == -EAGAIN, "futex mismatch path failed", 30);

    g_futex_word = 0u;
    start_ms = fry_gettime();
    rc = fry_futex_wait(&g_futex_word, 0u, FUTEX_TIMEOUT_MS);
    end_ms = fry_gettime();
    check(rc == -ETIMEDOUT, "futex timeout path failed", 31);
    check(end_ms >= start_ms, "futex timeout clock moved backward", 32);
    check((end_ms - start_ms) >= (FUTEX_TIMEOUT_MS / 2),
          "futex timeout returned too early", 33);

    rc = fry_sem_init(&g_futex_ready_sem, 0u);
    check(rc == 0, "futex ready sem init failed", 34);
    rc = fry_sem_init(&g_futex_done_sem, 0u);
    check(rc == 0, "futex done sem init failed", 35);

    g_futex_wait_rc = -9999;
    g_futex_word = 0u;
    rc = fry_thread_create(&waiter, futex_waiter, 0);
    check(rc >= 0, "futex waiter create failed", 36);

    rc = fry_sem_wait(&g_futex_ready_sem);
    check(rc == 0, "futex waiter ready wait failed", 37);
    fry_sleep(FUTEX_WAKE_DELAY_MS);

    g_futex_word = 1u;
    rc = fry_futex_wake(&g_futex_word, 1u);
    check(rc == 1, "futex wake-one count mismatch", 38);

    rc = fry_sem_wait(&g_futex_done_sem);
    check(rc == 0, "futex waiter done wait failed", 39);
    rc = fry_thread_join(&waiter, &exit_code);
    check(rc == 0, "futex waiter join failed", 40);
    check(exit_code == 0, "futex waiter exit code mismatch", 41);
    check(g_futex_wait_rc == 0, "futex waiter did not wake cleanly", 42);

    rc = fry_futex_wake(&g_futex_word, 1u);
    check(rc == 0, "futex empty wake count mismatch", 43);
}

static fry_mutex_t g_contend_mutex = FRY_MUTEX_INIT;
static fry_cond_t g_contend_cond = FRY_COND_INIT;
static fry_sem_t g_contend_done_sem;
static fry_tls_key_t g_contend_tls_key;
static volatile int g_contend_ready;
static volatile int g_contend_go;
static volatile int g_contend_counter;
static volatile uintptr_t g_contend_tls_seen[CONTEND_WORKERS];

static void contend_reset(void) {
    g_contend_mutex = (fry_mutex_t)FRY_MUTEX_INIT;
    g_contend_cond = (fry_cond_t)FRY_COND_INIT;
    g_contend_ready = 0;
    g_contend_go = 0;
    g_contend_counter = 0;
    for (int i = 0; i < CONTEND_WORKERS; i++) {
        g_contend_tls_seen[i] = 0;
    }
}

static void contend_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    int rc;

    rc = fry_tls_set(g_contend_tls_key, (void *)(uintptr_t)(0x2000u + (uint32_t)id));
    if (rc < 0) fry_thread_exit(10 + id);
    g_contend_tls_seen[id] = (uintptr_t)fry_tls_get(g_contend_tls_key);

    rc = fry_mutex_lock(&g_contend_mutex);
    if (rc < 0) fry_thread_exit(20 + id);
    g_contend_ready++;
    rc = fry_cond_signal(&g_contend_cond);
    if (rc < 0) {
        fry_mutex_unlock(&g_contend_mutex);
        fry_thread_exit(30 + id);
    }
    while (!g_contend_go) {
        rc = fry_cond_wait(&g_contend_cond, &g_contend_mutex);
        if (rc < 0) {
            fry_mutex_unlock(&g_contend_mutex);
            fry_thread_exit(40 + id);
        }
    }
    rc = fry_mutex_unlock(&g_contend_mutex);
    if (rc < 0) fry_thread_exit(50 + id);

    for (int i = 0; i < CONTEND_LOOPS; i++) {
        rc = fry_mutex_lock(&g_contend_mutex);
        if (rc < 0) fry_thread_exit(60 + id);
        g_contend_counter++;
        rc = fry_mutex_unlock(&g_contend_mutex);
        if (rc < 0) fry_thread_exit(70 + id);
    }

    rc = fry_sem_post(&g_contend_done_sem);
    if (rc < 0) fry_thread_exit(80 + id);
    fry_thread_exit(200 + id);
}

static void run_mutex_contention_test(void) {
    struct fry_thread threads[CONTEND_WORKERS] = {{0}};
    int rc;

    contend_reset();
    rc = fry_sem_init(&g_contend_done_sem, 0u);
    check(rc == 0, "contention sem init failed", 50);
    rc = fry_tls_key_create(&g_contend_tls_key);
    check(rc == 0, "contention tls key create failed", 51);

    for (int i = 0; i < CONTEND_WORKERS; i++) {
        rc = (int)fry_thread_create(&threads[i], contend_worker, (void *)(intptr_t)i);
        check(rc >= 0, "contention thread create failed", 52);
    }

    rc = fry_mutex_lock(&g_contend_mutex);
    check(rc == 0, "contention mutex lock failed", 53);
    while (g_contend_ready < CONTEND_WORKERS) {
        rc = fry_cond_wait(&g_contend_cond, &g_contend_mutex);
        if (rc < 0) {
            fry_mutex_unlock(&g_contend_mutex);
            fail("contention cond wait failed", 54);
        }
    }
    g_contend_go = 1;
    rc = fry_cond_broadcast(&g_contend_cond);
    if (rc < 0) {
        fry_mutex_unlock(&g_contend_mutex);
        fail("contention cond broadcast failed", 55);
    }
    rc = fry_mutex_unlock(&g_contend_mutex);
    check(rc == 0, "contention mutex unlock failed", 56);

    for (int i = 0; i < CONTEND_WORKERS; i++) {
        rc = fry_sem_wait(&g_contend_done_sem);
        check(rc == 0, "contention sem wait failed", 57);
    }

    for (int i = 0; i < CONTEND_WORKERS; i++) {
        int exit_code = -1;
        rc = (int)fry_thread_join(&threads[i], &exit_code);
        check(rc == 0, "contention thread join failed", 58);
        check(exit_code == 200 + i, "contention thread exit code mismatch", 59);
        check(g_contend_tls_seen[i] == (uintptr_t)(0x2000u + (uint32_t)i),
              "contention tls mismatch", 60);
    }

    check(g_contend_counter == CONTEND_WORKERS * CONTEND_LOOPS,
          "contention counter mismatch", 61);
}

static fry_mutex_t g_round_mutex = FRY_MUTEX_INIT;
static fry_cond_t g_round_cond = FRY_COND_INIT;
static fry_sem_t g_round_done_sem;
static volatile uint32_t g_round_arrived;
static volatile uint32_t g_round_generation;

static void round_reset(void) {
    g_round_mutex = (fry_mutex_t)FRY_MUTEX_INIT;
    g_round_cond = (fry_cond_t)FRY_COND_INIT;
    g_round_arrived = 0u;
    g_round_generation = 0u;
}

static void round_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    uint32_t seen = 0u;
    int rc;

    for (int round = 0; round < COND_ROUNDS; round++) {
        rc = fry_mutex_lock(&g_round_mutex);
        if (rc < 0) fry_thread_exit(10 + id);
        g_round_arrived++;
        rc = fry_cond_signal(&g_round_cond);
        if (rc < 0) {
            fry_mutex_unlock(&g_round_mutex);
            fry_thread_exit(20 + id);
        }
        while (g_round_generation == seen) {
            rc = fry_cond_wait(&g_round_cond, &g_round_mutex);
            if (rc < 0) {
                fry_mutex_unlock(&g_round_mutex);
                fry_thread_exit(30 + id);
            }
        }
        seen = g_round_generation;
        rc = fry_mutex_unlock(&g_round_mutex);
        if (rc < 0) fry_thread_exit(40 + id);

        rc = fry_sem_post(&g_round_done_sem);
        if (rc < 0) fry_thread_exit(50 + id);
    }

    fry_thread_exit(0);
}

static void run_condvar_rounds_test(void) {
    struct fry_thread threads[COND_WORKERS] = {{0}};
    int rc;

    round_reset();
    rc = fry_sem_init(&g_round_done_sem, 0u);
    check(rc == 0, "round sem init failed", 70);

    for (int i = 0; i < COND_WORKERS; i++) {
        rc = (int)fry_thread_create(&threads[i], round_worker, (void *)(intptr_t)i);
        check(rc >= 0, "round thread create failed", 71);
    }

    for (int round = 0; round < COND_ROUNDS; round++) {
        uint32_t target = (uint32_t)(round + 1) * (uint32_t)COND_WORKERS;
        rc = fry_mutex_lock(&g_round_mutex);
        check(rc == 0, "round mutex lock failed", 72);
        while (g_round_arrived < target) {
            rc = fry_cond_wait(&g_round_cond, &g_round_mutex);
            if (rc < 0) {
                fry_mutex_unlock(&g_round_mutex);
                fail("round cond wait failed", 73);
            }
        }
        g_round_generation++;
        rc = fry_cond_broadcast(&g_round_cond);
        if (rc < 0) {
            fry_mutex_unlock(&g_round_mutex);
            fail("round cond broadcast failed", 74);
        }
        rc = fry_mutex_unlock(&g_round_mutex);
        check(rc == 0, "round mutex unlock failed", 75);

        for (int i = 0; i < COND_WORKERS; i++) {
            rc = fry_sem_wait(&g_round_done_sem);
            check(rc == 0, "round sem wait failed", 76);
        }
    }

    for (int i = 0; i < COND_WORKERS; i++) {
        int exit_code = -1;
        rc = (int)fry_thread_join(&threads[i], &exit_code);
        check(rc == 0, "round thread join failed", 77);
        check(exit_code == 0, "round thread exit code mismatch", 78);
    }
}

static fry_mutex_t g_churn_mutex = FRY_MUTEX_INIT;
static fry_tls_key_t g_churn_tls_key;
static volatile uint32_t g_churn_counter;

static void churn_reset(void) {
    g_churn_mutex = (fry_mutex_t)FRY_MUTEX_INIT;
    g_churn_counter = 0u;
}

static void churn_worker(void *arg) {
    uint32_t token = (uint32_t)(uintptr_t)arg;
    int rc;
    uintptr_t want = (uintptr_t)(0x4000u + token);

    rc = fry_tls_set(g_churn_tls_key, (void *)want);
    if (rc < 0) fry_thread_exit(1);
    if ((uintptr_t)fry_tls_get(g_churn_tls_key) != want) fry_thread_exit(2);

    rc = fry_mutex_lock(&g_churn_mutex);
    if (rc < 0) fry_thread_exit(3);
    g_churn_counter++;
    rc = fry_mutex_unlock(&g_churn_mutex);
    if (rc < 0) fry_thread_exit(4);

    fry_thread_exit((int)token);
}

static void run_thread_churn_test(void) {
    struct fry_thread threads[CHURN_WORKERS];
    uint32_t expected_counter = 0u;
    int rc;

    churn_reset();
    rc = fry_tls_key_create(&g_churn_tls_key);
    check(rc == 0, "churn tls key create failed", 90);

    for (int round = 0; round < CHURN_ROUNDS; round++) {
        for (int i = 0; i < CHURN_WORKERS; i++) {
            uint32_t token = (uint32_t)(round * CHURN_WORKERS + i + 1);
            threads[i].tid = 0;
            threads[i].stack_base = 0;
            threads[i].stack_len = 0;
            threads[i].tls_base = 0;
            rc = (int)fry_thread_create(&threads[i], churn_worker, (void *)(uintptr_t)token);
            check(rc >= 0, "churn thread create failed", 91);
        }
        for (int i = 0; i < CHURN_WORKERS; i++) {
            uint32_t token = (uint32_t)(round * CHURN_WORKERS + i + 1);
            int exit_code = -1;
            rc = (int)fry_thread_join(&threads[i], &exit_code);
            check(rc == 0, "churn thread join failed", 92);
            check(exit_code == (int)token, "churn exit code mismatch", 93);
            expected_counter++;
        }
    }

    check(g_churn_counter == expected_counter, "churn counter mismatch", 94);
}

int main(void) {
    printf("thtest: smoke\n");
    run_smoke_test();

    printf("thtest: futex\n");
    run_futex_test();

    printf("thtest: mutex contention\n");
    run_mutex_contention_test();

    printf("thtest: condvar rounds\n");
    run_condvar_rounds_test();

    printf("thtest: thread churn\n");
    run_thread_churn_test();

    printf("thtest: phase-two closeout ok\n");
    fry_exit(0);
}
