/*
 * abitest.c — TaterTOS ABI Stability Test
 */

#include "libc.h"
#include "fry.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * ABI Tests
 * ----------------------------------------------------------------------- */

static void test_syscall_wrappers(void) {
    puts("Testing syscall wrappers...");

    long pid = fry_getpid();
    if (pid <= 0) printf("FAIL: getpid -> %ld\n", pid);

    long tid = fry_gettid();
    if (tid <= 0) printf("FAIL: gettid -> %ld\n", tid);

    puts("Syscall wrappers OK.");
}

static void test_unknown_syscall(void) {
    puts("Testing unknown syscall behavior...");
    long r = fry_syscall_raw(9999, 0);
    if (r != -ENOSYS) {
        printf("FAIL: unknown syscall returned %ld, expected -ENOSYS (%d)\n", r, -ENOSYS);
    } else {
        puts("Unknown syscall OK.");
    }
}

static void test_invalid_mmap(void) {
    puts("Testing invalid mmap parameters...");
    void *p = fry_mmap(0, 0, 0, 0, -1, 0);
    if (!FRY_IS_ERR(p)) {
        printf("FAIL: mmap(size=0) returned valid ptr %p\n", p);
    } else {
        puts("Invalid mmap OK.");
    }
}

int main(void) {
    puts("TaterTOS ABI Stability Test Suite\n");

    test_syscall_wrappers();
    test_unknown_syscall();
    test_invalid_mmap();

    puts("\nAll tests passed.");
    return 0;
}
