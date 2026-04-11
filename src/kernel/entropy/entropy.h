#ifndef TATER_ENTROPY_H
#define TATER_ENTROPY_H

#include <stdint.h>

/*
 * TaterTOS entropy subsystem.
 *
 * Primary source: RDRAND/RDSEED (Intel Ivy Bridge+, AMD Zen+).
 * Fallback: RDTSC-seeded ChaCha-like CSPRNG for older CPUs.
 *
 * entropy_init() must be called once during boot.
 * entropy_getbytes() is the main interface for kernel and userspace.
 */

/* Initialize entropy subsystem: detect CPU features, seed the pool. */
void entropy_init(void);

/* Fill buf with len cryptographic-quality random bytes.
 * Returns 0 on success, -EAGAIN if not ready and nonblock is set. */
int entropy_getbytes(void *buf, uint32_t len);

/* Returns 1 if RDRAND is available. */
int entropy_has_rdrand(void);

/* Returns 1 if RDSEED is available. */
int entropy_has_rdseed(void);

/* Returns 1 if entropy system is seeded and ready. */
int entropy_ready(void);

#endif
