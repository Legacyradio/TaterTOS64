// Spinlock

#include "spinlock.h"

void spinlock_init(spinlock_t *l) {
    l->lock = 0;
}

void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->lock, 1)) {
        while (l->lock) {
            __asm__ volatile("pause");
        }
    }
}

void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->lock);
}

static inline uint64_t irq_save(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags = irq_save();
    spin_lock(l);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    irq_restore(flags);
}
