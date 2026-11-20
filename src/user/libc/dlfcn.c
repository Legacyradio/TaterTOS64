/*
 * dlfcn.c — Dynamic loading stubs for POSIX compatibility
 *
 * Phase 8: NSPR expects dlopen/dlsym/dlclose/dlerror to exist.
 * TaterTOS uses static linking only, so these are stubs that
 * return sensible errors. This allows NSPR to compile and
 * gracefully handle the lack of dynamic loading.
 *
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>
#include <dlfcn.h>

/* -----------------------------------------------------------------------
 * dlerror state
 * ----------------------------------------------------------------------- */

static const char *g_dlerror_msg = 0;

/* Special handle for RTLD_DEFAULT */
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)
#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_GLOBAL  0x00100
#define RTLD_LOCAL   0x00000

/* -----------------------------------------------------------------------
 * dlopen — always fails (no dynamic loading)
 * ----------------------------------------------------------------------- */

void *dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename) {
        /* NULL filename means "return handle to main program" */
        /* Return a non-null sentinel so NSPR can call dlsym on it */
        g_dlerror_msg = 0;
        return (void *)1;
    }
    g_dlerror_msg = "dlopen: dynamic loading not supported on TaterTOS";
    return 0;
}

/* -----------------------------------------------------------------------
 * dlsym — always returns NULL (no dynamic symbols)
 * ----------------------------------------------------------------------- */

void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    (void)symbol;
    g_dlerror_msg = "dlsym: dynamic symbols not available on TaterTOS";
    return 0;
}

/* -----------------------------------------------------------------------
 * dlclose — no-op
 * ----------------------------------------------------------------------- */

int dlclose(void *handle) {
    (void)handle;
    g_dlerror_msg = 0;
    return 0;
}

/* -----------------------------------------------------------------------
 * dlerror — return last error message
 * ----------------------------------------------------------------------- */

char *dlerror(void) {
    const char *msg = g_dlerror_msg;
    g_dlerror_msg = 0;
    return (char *)msg;
}

/* -----------------------------------------------------------------------
 * dladdr / dladdr1 — symbol lookup from address (stubs)
 * TaterTOS has no dynamic symbol table. Return 0 (failure).
 * CPython uses these for traceback symbol resolution — gracefully
 * degrades to raw addresses when they fail.
 * ----------------------------------------------------------------------- */

int dladdr(const void *addr, Dl_info *info) {
    (void)addr;
    if (info) {
        info->dli_fname = NULL;
        info->dli_fbase = NULL;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
    }
    return 0;
}

int dladdr1(const void *addr, Dl_info *info, void **extra_info, int flags) {
    (void)flags;
    (void)extra_info;
    return dladdr(addr, info);
}
