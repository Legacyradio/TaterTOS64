#include "../libc/libc.h"
#include <stdint.h>

#define PAGE_SIZE 4096u

int main(void) {
    uint8_t *p = (uint8_t *)fry_mreserve(0, PAGE_SIZE * 3u, FRY_MAP_PRIVATE);
    if (FRY_IS_ERR(p)) {
        printf("vmfault: reserve failed\n");
        fry_exit(1);
    }
    if (fry_mcommit(p + PAGE_SIZE, PAGE_SIZE,
                    FRY_PROT_READ | FRY_PROT_WRITE) != 0) {
        printf("vmfault: commit failed\n");
        fry_exit(2);
    }

    p[PAGE_SIZE] = 0x5Au;
    printf("vmfault: provoking reserved-page fault\n");
    {
        volatile uint8_t *guard = p;
        volatile uint8_t sink = guard[0];
        (void)sink;
    }

    printf("vmfault: unexpected survival\n");
    fry_exit(3);
}
