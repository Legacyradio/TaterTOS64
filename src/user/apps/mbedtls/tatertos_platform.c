/*
 * tatertos_platform.c — mbedTLS platform shim for TaterTOS64v3
 *
 * Provides:
 *   - PSA external RNG via fry_getrandom()
 *   - Calloc/free via TaterTOS libc
 *   - Time stub (returns 0 — we don't validate cert expiry)
 *
 * This file is compiled as part of the TaterSurf build and linked
 * alongside the mbedTLS library objects.
 */

#include "../../libc/libc.h"
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* PSA External RNG                                                    */
/*                                                                     */
/* mbedTLS with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG calls this function   */
/* instead of using its own entropy accumulator + DRBG. We feed it    */
/* directly from the kernel's fry_getrandom() syscall which sources   */
/* entropy from RDRAND/RDSEED + timing jitter.                        */
/* ------------------------------------------------------------------ */

#include "include/psa/crypto.h"

/*
 * mbedtls_psa_external_get_random — called by PSA crypto core
 *
 * Parameters:
 *   context    — typed context pointer (unused, we have no RNG state)
 *   output     — buffer to fill with random bytes
 *   output_size — number of bytes requested
 *   output_length — set to number of bytes actually produced
 *
 * Returns PSA_SUCCESS on success, PSA_ERROR_* on failure.
 */
psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output,
    size_t output_size,
    size_t *output_length)
{
    (void)context;

    if (!output || !output_length)
        return PSA_ERROR_INVALID_ARGUMENT;

    if (output_size == 0) {
        *output_length = 0;
        return PSA_SUCCESS;
    }

    long rc = fry_getrandom(output, (unsigned long)output_size, 0);
    if (rc < 0) {
        *output_length = 0;
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }

    *output_length = output_size;
    return PSA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* mbedtls_time — stub for certificate time checks                     */
/*                                                                     */
/* mbedTLS calls mbedtls_time() during X.509 cert validation to check */
/* notBefore/notAfter. Since we accept all certs anyway (no CA store), */
/* and MBEDTLS_HAVE_TIME is not defined, this should not be called.   */
/* Provide it as a safety net in case any code path reaches it.       */
/* ------------------------------------------------------------------ */

typedef long mbedtls_time_t;

mbedtls_time_t mbedtls_time(mbedtls_time_t *timer) {
    /* Return epoch 0 — effectively "January 1, 1970" */
    /* Any cert check against this will either pass (notBefore <= 0) */
    /* or fail, but we skip validation anyway. */
    if (timer) *timer = 0;
    return 0;
}
