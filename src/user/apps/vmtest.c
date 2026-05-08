/*
 * vmtest.c — Virtual Memory System Test
 */

#include "libc.h"
#include "fry.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096

int main(void) {
    puts("TaterTOS VM System Test\n");

    /* Test 1: Anonymous mapping */
    puts("Testing mmap(MAP_ANON)...");
    uint8_t *p = (uint8_t *)mmap(0, PAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap anon");
        return 1;
    }
    p[0] = 0xAA;
    p[PAGE_SIZE] = 0xBB;
    if (p[0] != 0xAA || p[PAGE_SIZE] != 0xBB) {
        puts("FAIL: memory content mismatch");
        return 1;
    }
    puts("mmap anon OK.");

    /* Test 2: Memory reserve/commit (TaterTOS specialized) */
    puts("Testing fry_mreserve/mcommit...");
    uint8_t *r = (uint8_t *)fry_mreserve(0, PAGE_SIZE * 10, 0);
    if (FRY_IS_ERR(r)) {
        printf("FAIL: mreserve -> %d\n", FRY_PTR_ERR(r));
        return 1;
    }
    if (fry_mcommit(r, PAGE_SIZE, FRY_PROT_READ | FRY_PROT_WRITE) < 0) {
        puts("FAIL: mcommit");
        return 1;
    }
    r[0] = 0xCC;
    puts("mreserve/mcommit OK.");

    /* Cleanup */
    munmap(p, PAGE_SIZE * 2);
    fry_munmap(r, PAGE_SIZE * 10);

    puts("\nAll VM tests passed.");
    return 0;
}
