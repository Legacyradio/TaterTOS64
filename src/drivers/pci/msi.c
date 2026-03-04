// MSI/MSI-X vector allocator

#include <stdint.h>
#include "msi.h"

#define MSI_VEC_BASE 0x50
#define MSI_VEC_MAX  0xEF
#define MSI_VEC_COUNT (MSI_VEC_MAX - MSI_VEC_BASE + 1)

static uint8_t vec_bitmap[(MSI_VEC_COUNT + 7) / 8];

static inline int bit_test(uint32_t idx) {
    return (vec_bitmap[idx / 8] >> (idx % 8)) & 1u;
}

static inline void bit_set(uint32_t idx) {
    vec_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static inline void bit_clear(uint32_t idx) {
    vec_bitmap[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

int msi_alloc_vector(void) {
    for (uint32_t i = 0; i < MSI_VEC_COUNT; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            return (int)(MSI_VEC_BASE + i);
        }
    }
    return -1;
}

void msi_free_vector(int vector) {
    if (vector < MSI_VEC_BASE || vector > MSI_VEC_MAX) return;
    uint32_t idx = (uint32_t)(vector - MSI_VEC_BASE);
    bit_clear(idx);
}
