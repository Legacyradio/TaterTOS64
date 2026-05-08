#define _LIBC_SOURCE_FILE
#include "libc.h"
#include "fry.h"
#include <stdlib.h>


static int g_errno_fallback = 0;
static fry_tls_key_t g_errno_key;
static int g_errno_key_inited = 0;

static void errno_init_key(void) {
    if (!g_errno_key_inited) {
        if (fry_tls_key_create(&g_errno_key) == 0) {
            g_errno_key_inited = 1;
        }
    }
}

int *__errno_location(void) {
    errno_init_key();
    if (g_errno_key_inited) {
        int *p = (int *)fry_tls_get(g_errno_key);
        if (!p) {
            p = (int *)malloc(sizeof(int));
            if (p) {
                *p = 0;
                fry_tls_set(g_errno_key, p);
                return p;
            }
        }
        if (p) return p;
    }
    return &g_errno_fallback;
}
