/*
 * abitest.c — Phase 0 ABI discipline test suite
 *
 * Exercises negative paths: invalid pointers, bad FDs, short buffers,
 * permission violations, and unknown syscalls. Verifies that each
 * returns the correct errno value.
 */
#include "../libc/libc.h"
#include <stdint.h>

static int passed;
static int failed;

static void pass(const char *name) {
    passed++;
    printf("  PASS: %s\n", name);
}

static void fail_msg(const char *name, long got, long want) {
    failed++;
    printf("  FAIL: %s (got %ld, want %ld)\n", name, got, want);
}

static void check(const char *name, long result, long expected) {
    if (result == expected) {
        pass(name);
    } else {
        fail_msg(name, result, expected);
    }
}

/* ---- Test groups ---- */

static void test_unknown_syscall(void) {
    printf("[unknown syscall]\n");
    long r = fry_syscall_raw(9999, 0);
    check("syscall 9999 returns -ENOSYS", r, -ENOSYS);
}

static void test_bad_fd_write(void) {
    printf("[bad fd write]\n");
    char buf[4] = "hi\n";

    /* FD out of range */
    long r = fry_write(99, buf, 3);
    check("write fd=99 returns -EBADF", r, -EBADF);

    /* FD in range but not open */
    r = fry_write(63, buf, 3);
    check("write fd=63 (not open) returns -EBADF", r, -EBADF);

    /* Negative FD (cast to int) */
    r = fry_write(-5, buf, 3);
    check("write fd=-5 returns -EBADF", r, -EBADF);
}

static void test_bad_fd_read(void) {
    printf("[bad fd read]\n");
    char buf[64];

    long r = fry_read(99, buf, sizeof(buf));
    check("read fd=99 returns -EBADF", r, -EBADF);

    r = fry_read(63, buf, sizeof(buf));
    check("read fd=63 (not open) returns -EBADF", r, -EBADF);
}

static void test_bad_fd_close(void) {
    printf("[bad fd close]\n");

    long r = fry_close(99);
    check("close fd=99 returns -EBADF", r, -EBADF);

    r = fry_close(63);
    check("close fd=63 (not open) returns -EBADF", r, -EBADF);

    /* stdin/stdout/stderr are special; close on fd=0 should fail */
    r = fry_close(0);
    check("close fd=0 (stdin) returns -EBADF", r, -EBADF);
}

static void test_bad_pointer_write(void) {
    printf("[bad pointer write]\n");

    /* NULL pointer */
    long r = fry_write(1, (void *)0, 10);
    check("write NULL buf returns -EFAULT", r, -EFAULT);

    /* Kernel-space pointer */
    r = fry_write(1, (void *)0xFFFF800000000000ULL, 10);
    check("write kernel ptr returns -EFAULT", r, -EFAULT);
}

static void test_bad_pointer_read(void) {
    printf("[bad pointer read]\n");

    long r = fry_read(0, (void *)0, 64);
    check("read into NULL returns -EFAULT", r, -EFAULT);

    r = fry_read(0, (void *)0xFFFF800000000000ULL, 64);
    check("read into kernel ptr returns -EFAULT", r, -EFAULT);
}

static void test_open_nonexistent(void) {
    printf("[open nonexistent]\n");

    long r = fry_open("/this/path/does/not/exist", 0);
    check("open nonexistent returns -ENOENT", r, -ENOENT);
}

static void test_stat_nonexistent(void) {
    printf("[stat nonexistent]\n");
    struct fry_stat st;

    long r = fry_stat("/no/such/file", &st);
    check("stat nonexistent returns -ENOENT", r, -ENOENT);
}

static void test_stat_bad_pointer(void) {
    printf("[stat bad pointer]\n");

    long r = fry_stat("/apps", (struct fry_stat *)0);
    check("stat NULL buf returns -EFAULT", r, -EFAULT);
}

static void test_mmap_invalid_args(void) {
    printf("[mmap invalid args]\n");

    /* Zero length */
    void *p = fry_mmap(0, 0, FRY_PROT_READ | FRY_PROT_WRITE,
                       FRY_MAP_PRIVATE | FRY_MAP_ANON);
    check("mmap len=0 returns -EINVAL", (long)(intptr_t)p, -EINVAL);

    /* W^X violation: WRITE|EXEC */
    p = fry_mmap(0, 4096, FRY_PROT_READ | FRY_PROT_WRITE | FRY_PROT_EXEC,
                 FRY_MAP_PRIVATE | FRY_MAP_ANON);
    check("mmap W^X returns -EINVAL", (long)(intptr_t)p, -EINVAL);

    /* Misaligned hint with FIXED */
    p = fry_mmap((void *)0x0000100000000001ULL, 4096,
                 FRY_PROT_READ | FRY_PROT_WRITE,
                 FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_FIXED);
    check("mmap misaligned FIXED returns -EINVAL", (long)(intptr_t)p, -EINVAL);

    /* Conflicting flags: SHARED|PRIVATE */
    p = fry_mmap(0, 4096, FRY_PROT_READ,
                 FRY_MAP_SHARED | FRY_MAP_PRIVATE | FRY_MAP_ANON);
    check("mmap SHARED|PRIVATE returns -EINVAL", (long)(intptr_t)p, -EINVAL);
}

static void test_munmap_invalid(void) {
    printf("[munmap invalid]\n");

    /* Misaligned base */
    long r = fry_munmap((void *)0x0000100000000001ULL, 4096);
    check("munmap misaligned returns -EINVAL", r, -EINVAL);

    /* Zero length */
    r = fry_munmap((void *)0x0000100000000000ULL, 0);
    check("munmap len=0 returns -EINVAL", r, -EINVAL);
}

static void test_kill_protected(void) {
    printf("[kill protected pids]\n");

    /* Kill pid 0 */
    long r = fry_kill(0);
    check("kill pid=0 returns -EPERM", r, -EPERM);

    /* Kill pid 1 (init) */
    r = fry_kill(1);
    check("kill pid=1 returns -EPERM", r, -EPERM);

    /* Kill nonexistent pid */
    r = fry_kill(60000);
    check("kill pid=60000 returns -ESRCH", r, -ESRCH);
}

static void test_shm_invalid(void) {
    printf("[shm invalid]\n");

    /* Zero-size alloc */
    long r = fry_shm_alloc(0);
    check("shm_alloc size=0 returns -EINVAL", r, -EINVAL);

    /* Free invalid ID */
    r = fry_shm_free(999);
    check("shm_free id=999 returns -EINVAL", r, -EINVAL);

    /* Map invalid ID */
    r = fry_shm_map(999);
    check("shm_map id=999 returns -EINVAL", r, -EINVAL);
}

static void test_sbrk_invalid(void) {
    printf("[sbrk invalid]\n");

    /* Negative increment */
    long r = fry_sbrk(-4096);
    check("sbrk negative returns -EINVAL", r, -EINVAL);
}

int main(void) {
    printf("abitest: starting phase-0 ABI discipline tests\n");

    test_unknown_syscall();
    test_bad_fd_write();
    test_bad_fd_read();
    test_bad_fd_close();
    test_bad_pointer_write();
    test_bad_pointer_read();
    test_open_nonexistent();
    test_stat_nonexistent();
    test_stat_bad_pointer();
    test_mmap_invalid_args();
    test_munmap_invalid();
    test_kill_protected();
    test_shm_invalid();
    test_sbrk_invalid();

    printf("abitest: %d passed, %d failed\n", passed, failed);
    fry_exit(failed > 0 ? 1 : 0);
}
