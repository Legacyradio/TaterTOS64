#include "../libc/libc.h"
#include <stdint.h>

#define PAGE_SIZE 4096u
#define MMAP_BASE_MIN 0x0000100000000000ULL

static void fail(const char *msg, int code) {
    printf("vmtest: %s\n", msg);
    fry_exit(code);
}

static void verify_bytes(const uint8_t *buf, size_t len, uint8_t seed, int code) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)((seed + i) & 0xFFu)) {
            printf("vmtest: verify fail at %llu\n", (unsigned long long)i);
            fry_exit(code);
        }
    }
}

static void verify_zero_tail(const uint8_t *buf, size_t start, size_t end, int code) {
    for (size_t i = start; i < end; i++) {
        if (buf[i] != 0) {
            printf("vmtest: zero-tail fail at %llu\n", (unsigned long long)i);
            fry_exit(code);
        }
    }
}

static void run_private_anon_tests(void) {
    const size_t len = PAGE_SIZE * 3u;
    uint8_t *p = (uint8_t *)fry_mmap(0, len,
                                     FRY_PROT_READ | FRY_PROT_WRITE,
                                     FRY_MAP_PRIVATE | FRY_MAP_ANON);
    if (FRY_IS_ERR(p)) fail("anon mmap failed", 1);

    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(i & 0xFFu);
    verify_bytes(p, len, 0, 2);

    if (fry_mprotect(p, len, FRY_PROT_READ) != 0) fail("mprotect read-only failed", 3);
    verify_bytes(p, len, 0, 4);

    if (fry_mprotect(p, len, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("mprotect read-write failed", 5);
    }
    p[0] = 0x5Au;
    p[len - 1] = 0xA5u;
    if (p[0] != 0x5Au || p[len - 1] != 0xA5u) fail("writeback verify failed", 6);

    uint8_t *mid = p + PAGE_SIZE;
    if (fry_mprotect(mid, PAGE_SIZE, FRY_PROT_READ) != 0) {
        fail("partial mprotect read-only failed", 7);
    }
    if (p[0] != 0x5Au || p[len - 1] != 0xA5u || mid[0] != (uint8_t)(PAGE_SIZE & 0xFFu)) {
        fail("partial mprotect verify failed", 8);
    }
    if (fry_mprotect(mid, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("partial mprotect read-write failed", 9);
    }
    mid[0] = 0x33u;
    if (mid[0] != 0x33u) fail("partial mprotect writeback failed", 10);

    if (fry_mprotect(mid, PAGE_SIZE, 0) != 0) fail("partial mprotect prot-none failed", 11);
    if (fry_mprotect(mid, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("partial mprotect restore from prot-none failed", 12);
    }
    if (mid[0] != 0x33u) fail("prot-none restore verify failed", 13);

    if (fry_munmap(mid, PAGE_SIZE) != 0) fail("partial munmap failed", 14);
    uint8_t *q = (uint8_t *)fry_mmap(mid, PAGE_SIZE,
                                     FRY_PROT_READ | FRY_PROT_WRITE,
                                     FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_FIXED);
    if (FRY_IS_ERR(q) || q != mid) fail("fixed remap failed", 15);
    verify_zero_tail(q, 0, PAGE_SIZE, 16);
    if (p[0] != 0x5Au || p[len - 1] != 0xA5u) fail("surrounding pages changed unexpectedly", 17);

    if (fry_munmap(p, PAGE_SIZE) != 0) fail("first-page munmap failed", 18);
    if (fry_munmap(q, PAGE_SIZE) != 0) fail("middle-page munmap failed", 19);
    if (fry_munmap(p + (PAGE_SIZE * 2u), PAGE_SIZE) != 0) fail("last-page munmap failed", 20);
}

static void run_reserve_commit_tests(void) {
    const size_t len = PAGE_SIZE * 3u;
    uint8_t *p = (uint8_t *)fry_mreserve(0, len, FRY_MAP_PRIVATE);
    if (FRY_IS_ERR(p)) fail("reserve mmap failed", 21);

    if (fry_mcommit(p + PAGE_SIZE, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("reserve middle-page commit failed", 22);
    }
    p[PAGE_SIZE] = 0x61u;
    if (p[PAGE_SIZE] != 0x61u) fail("reserve middle-page verify failed", 23);

    if (fry_mcommit(p, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("reserve first-page commit failed", 24);
    }
    if (fry_mcommit(p + (PAGE_SIZE * 2u), PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("reserve last-page commit failed", 25);
    }
    p[0] = 0x41u;
    p[PAGE_SIZE * 2u] = 0x7Eu;
    if (p[0] != 0x41u || p[PAGE_SIZE] != 0x61u || p[PAGE_SIZE * 2u] != 0x7Eu) {
        fail("reserve commit verify failed", 26);
    }
    if (fry_munmap(p, len) != 0) fail("full reserve-backed munmap failed", 27);
}

static void run_shared_anon_test(void) {
    uint8_t *p = (uint8_t *)fry_mmap(0, PAGE_SIZE,
                                     FRY_PROT_READ | FRY_PROT_WRITE,
                                     FRY_MAP_SHARED | FRY_MAP_ANON);
    if (FRY_IS_ERR(p)) fail("shared anon mmap failed", 28);
    p[0] = 0x44u;
    p[PAGE_SIZE - 1u] = 0x99u;
    if (p[0] != 0x44u || p[PAGE_SIZE - 1u] != 0x99u) fail("shared anon verify failed", 29);
    if (fry_munmap(p, PAGE_SIZE) != 0) fail("shared anon munmap failed", 30);
}

static void run_multiregion_unmap_tests(void) {
    const size_t len = PAGE_SIZE * 3u;

    uint8_t *split = (uint8_t *)fry_mmap(0, len,
                                         FRY_PROT_READ | FRY_PROT_WRITE,
                                         FRY_MAP_PRIVATE | FRY_MAP_ANON);
    if (FRY_IS_ERR(split)) fail("split-range mmap failed", 31);
    if (fry_mprotect(split + PAGE_SIZE, PAGE_SIZE, FRY_PROT_READ) != 0) {
        fail("split-range mprotect failed", 32);
    }
    if (fry_munmap(split, len) != 0) fail("split-range full munmap failed", 33);

    uint8_t *reserved = (uint8_t *)fry_mreserve(0, len, FRY_MAP_PRIVATE);
    if (FRY_IS_ERR(reserved)) fail("mixed reserve mmap failed", 34);
    if (fry_mcommit(reserved + PAGE_SIZE, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        fail("mixed reserve commit failed", 35);
    }
    reserved[PAGE_SIZE] = 0x52u;
    if (reserved[PAGE_SIZE] != 0x52u) fail("mixed reserve verify failed", 36);
    if (fry_munmap(reserved, len) != 0) fail("mixed reserve full munmap failed", 37);
}

static void run_guard_page_tests(void) {
    uint8_t *g = (uint8_t *)fry_mguard(0, PAGE_SIZE);
    if (FRY_IS_ERR(g)) fail("guard mmap failed", 55);

    /* commit must be rejected on guard pages */
    if (fry_mcommit(g, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) == 0) {
        fail("guard mcommit should have failed", 56);
    }

    /* guard region can be unmapped */
    if (fry_munmap(g, PAGE_SIZE) != 0) fail("guard munmap failed", 57);

    /* multi-page guard + verify unmap */
    uint8_t *g2 = (uint8_t *)fry_mguard(0, PAGE_SIZE * 3u);
    if (FRY_IS_ERR(g2)) fail("multi guard mmap failed", 58);
    if (fry_munmap(g2, PAGE_SIZE * 3u) != 0) fail("multi guard munmap failed", 59);
}

static void run_file_mapping_tests(void) {
    static const char expected[] =
        "TaterTOS VM fixture\n"
        "Phase one file mapping\n"
        "0123456789abcdef\n";
    const size_t want = sizeof(expected) - 1u;

    long fd = fry_open("/apps/VMTEST.TXT", 0);
    if (fd < 0) fd = fry_open("/VMTEST.TXT", 0);
    if (fd < 0) fail("open vmtest fixture failed", 38);

    uint8_t *ro = (uint8_t *)fry_mmap_fd(0, PAGE_SIZE, FRY_PROT_READ,
                                         FRY_MAP_PRIVATE | FRY_MAP_FILE, (int)fd);
    if (FRY_IS_ERR(ro)) fail("read-only file mmap failed", 39);
    if (memcmp(ro, expected, want) != 0) fail("read-only file mmap content mismatch", 40);
    verify_zero_tail(ro, want, PAGE_SIZE, 41);
    if (fry_munmap(ro, PAGE_SIZE) != 0) fail("read-only file munmap failed", 42);
    fry_close((int)fd);

    fd = fry_open("/apps/VMTEST.TXT", 0);
    if (fd < 0) fd = fry_open("/VMTEST.TXT", 0);
    if (fd < 0) fail("reopen vmtest fixture failed", 43);
    uint8_t *rw = (uint8_t *)fry_mmap_fd(0, PAGE_SIZE,
                                         FRY_PROT_READ | FRY_PROT_WRITE,
                                         FRY_MAP_PRIVATE | FRY_MAP_FILE,
                                         (int)fd);
    if (FRY_IS_ERR(rw)) fail("writable private file mmap failed", 44);
    rw[0] = 'X';
    if (rw[0] != 'X') fail("writable private file mmap verify failed", 45);
    if (fry_munmap(rw, PAGE_SIZE) != 0) fail("writable private file munmap failed", 46);
    fry_close((int)fd);

    fd = fry_open("/apps/VMTEST.TXT", 0);
    if (fd < 0) fd = fry_open("/VMTEST.TXT", 0);
    if (fd < 0) fail("reopen fixture for file verification failed", 47);
    char buf[sizeof(expected)];
    long n = fry_read((int)fd, buf, want);
    fry_close((int)fd);
    if (n != (long)want) fail("fixture reread short", 48);
    if (memcmp(buf, expected, want) != 0) fail("file-backed private write leaked to file", 49);
}

static void run_allocator_test(void) {
    const size_t big_len = 128u * 1024u;
    uint8_t *big = (uint8_t *)malloc(big_len);
    if (!big) fail("large malloc failed", 50);
    if ((uintptr_t)big < MMAP_BASE_MIN) fail("large malloc did not switch to mmap", 51);

    big[0] = 0x12u;
    big[big_len - 1u] = 0x34u;
    uint8_t *bigger = (uint8_t *)realloc(big, big_len + 8192u);
    if (!bigger) fail("large realloc failed", 52);
    if ((uintptr_t)bigger < MMAP_BASE_MIN) fail("large realloc did not stay on mmap path", 53);
    if (bigger[0] != 0x12u || bigger[big_len - 1u] != 0x34u) {
        fail("large realloc content verify failed", 54);
    }
    free(bigger);
}

int main(void) {
    run_private_anon_tests();
    run_reserve_commit_tests();
    run_shared_anon_test();
    run_multiregion_unmap_tests();
    run_guard_page_tests();
    run_file_mapping_tests();
    run_allocator_test();
    printf("vmtest: phase-one ok\n");
    fry_exit(0);
}
