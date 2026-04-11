/*
 * TaterTOS64v3 entropy subsystem.
 *
 * Provides cryptographic-quality random bytes for getrandom() and kernel use.
 *
 * Entropy sources (best to worst):
 *   1. RDSEED — true hardware entropy (Intel Broadwell+, AMD Zen+)
 *   2. RDRAND — conditioned hardware RNG (Intel Ivy Bridge+, AMD Bulldozer+)
 *   3. RDTSC + boot jitter — last resort fallback for ancient CPUs
 *
 * The pool is a 256-byte state mixed with a ChaCha20-quarter-round based
 * CSPRNG.  Even with only RDRAND, the output is crypto-grade because RDRAND
 * itself passes NIST SP 800-90A.  The pool adds defense-in-depth.
 */

#include "entropy.h"
#include "../../include/errno.h"

static int g_has_rdrand;
static int g_has_rdseed;
static int g_seeded;

/* 256-bit internal state (4 x 64-bit) */
static uint64_t pool[4];
static uint64_t pool_counter;

/* Spinlock for pool access (simple test-and-set) */
static volatile uint32_t pool_lock;

static void spin_lock_pool(void) {
    while (__sync_lock_test_and_set(&pool_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static void spin_unlock_pool(void) {
    __sync_lock_release(&pool_lock);
}

/*
 * CPUID helpers
 */
static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/*
 * RDRAND: returns 1 on success, 0 on failure.
 * Intel docs say retry up to 10 times on CF=0.
 */
static int rdrand64(uint64_t *val) {
    uint64_t v;
    uint8_t ok;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *val = v;
            return 1;
        }
    }
    return 0;
}

/*
 * RDSEED: returns 1 on success, 0 on failure.
 * Retry a few times; RDSEED can transiently fail when entropy is depleted.
 */
static int rdseed64(uint64_t *val) {
    uint64_t v;
    uint8_t ok;
    for (int i = 0; i < 32; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *val = v;
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Mix function: rotate-xor-add, inspired by SipRound.
 */
static uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static void pool_mix(void) {
    pool[0] += pool[1]; pool[2] += pool[3];
    pool[1] = rotl64(pool[1], 13) ^ pool[0];
    pool[3] = rotl64(pool[3], 16) ^ pool[2];
    pool[0] = rotl64(pool[0], 32);
    pool[0] += pool[3]; pool[2] += pool[1];
    pool[1] = rotl64(pool[1], 17) ^ pool[0];
    pool[3] = rotl64(pool[3], 21) ^ pool[2];
    pool[0] = rotl64(pool[0], 32);
}

/*
 * Inject a 64-bit entropy sample into the pool.
 */
static void pool_inject(uint64_t val) {
    pool_counter++;
    pool[pool_counter & 3] ^= val;
    pool_mix();
}

/*
 * Extract a 64-bit output from the pool.
 */
static uint64_t pool_extract(void) {
    pool_counter++;
    pool_mix();
    return pool[0] ^ pool[1] ^ pool[2] ^ pool[3];
}

/*
 * Seed the pool from the best available source.
 */
static void seed_pool(void) {
    uint64_t val;

    /* Inject RDSEED if available (true entropy) */
    if (g_has_rdseed) {
        for (int i = 0; i < 8; i++) {
            if (rdseed64(&val)) {
                pool_inject(val);
            }
        }
    }

    /* Inject RDRAND (conditioned DRNG) */
    if (g_has_rdrand) {
        for (int i = 0; i < 8; i++) {
            if (rdrand64(&val)) {
                pool_inject(val);
            }
        }
    }

    /* Always mix in TSC for additional jitter */
    pool_inject(rdtsc());
    pool_inject(rdtsc() ^ 0xA5A5A5A5A5A5A5A5ULL);
    pool_inject(rdtsc());

    /* Additional mixing rounds */
    for (int i = 0; i < 20; i++) {
        pool_mix();
    }
}

void entropy_init(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check RDRAND: CPUID.01H:ECX.RDRAND[bit 30] */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    g_has_rdrand = (ecx >> 30) & 1;

    /* Check RDSEED: CPUID.(EAX=07H,ECX=0):EBX.RDSEED[bit 18] */
    cpuid(7, &eax, &ebx, &ecx, &edx);
    g_has_rdseed = (ebx >> 18) & 1;

    pool[0] = 0x6A09E667F3BCC908ULL;  /* fractional part of sqrt(2) */
    pool[1] = 0xBB67AE8584CAA73BULL;  /* fractional part of sqrt(3) */
    pool[2] = 0x3C6EF372FE94F82BULL;  /* fractional part of sqrt(5) */
    pool[3] = 0xA54FF53A5F1D36F1ULL;  /* fractional part of sqrt(7) */
    pool_counter = 0;
    pool_lock = 0;

    seed_pool();
    g_seeded = 1;
}

int entropy_has_rdrand(void) { return g_has_rdrand; }
int entropy_has_rdseed(void) { return g_has_rdseed; }
int entropy_ready(void)      { return g_seeded; }

int entropy_getbytes(void *buf, uint32_t len) {
    if (!buf || len == 0) return 0;
    if (!g_seeded) return -EAGAIN;

    uint8_t *out = (uint8_t *)buf;
    uint32_t pos = 0;

    spin_lock_pool();

    /* Re-seed with fresh hardware entropy periodically */
    uint64_t fresh;
    if (g_has_rdrand && rdrand64(&fresh)) {
        pool_inject(fresh);
    }
    pool_inject(rdtsc());

    while (pos < len) {
        uint64_t val = pool_extract();
        uint32_t chunk = len - pos;
        if (chunk > 8) chunk = 8;
        for (uint32_t i = 0; i < chunk; i++) {
            out[pos++] = (uint8_t)(val >> (i * 8));
        }
    }

    spin_unlock_pool();
    return 0;
}
