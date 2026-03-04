#ifndef TATER_HEAP_H
#define TATER_HEAP_H

#include <stdint.h>

void heap_init(void);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, uint64_t new_size);

#endif
