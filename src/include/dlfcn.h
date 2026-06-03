/* TaterTOS64 dlfcn.h — POSIX dynamic linking
 *
 * TaterTOS64 has no dynamic linking. All dl* functions return NULL/error.
 * Chromium native_library_posix.cc handles NULL returns gracefully.
 */
#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x00001
#define RTLD_NOW    0x00002
#define RTLD_NOLOAD 0x00004
#define RTLD_GLOBAL 0x00100
#define RTLD_LOCAL  0x00000

#define RTLD_DEFAULT ((void *) 0)
#define RTLD_NEXT    ((void *) -1L)

void *dlopen(const char *filename, int flags);
int   dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);

/* GNU extension: Dl_info and dladdr */
typedef struct {
    const char *dli_fname;  /* Pathname of shared object */
    void       *dli_fbase;  /* Base address at which shared object is loaded */
    const char *dli_sname;  /* Name of nearest symbol */
    void       *dli_saddr;  /* Address of nearest symbol */
} Dl_info;

int dladdr(const void *addr, Dl_info *info);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */
