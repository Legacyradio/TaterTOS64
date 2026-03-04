#ifndef TATER_SPINLOCK_H
#define TATER_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

void spinlock_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
uint64_t spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);

#endif
