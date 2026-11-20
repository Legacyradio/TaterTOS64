/*
 * rust_compat.c — libc shims the prebuilt linux-gnu Rust std (pydantic_core,
 * cryptography) references but TaterTOS libc didn't expose. Mostly glibc LFS
 * aliases + a couple of glibc-isms. Keeps the native Rust extensions linkable into
 * python3.fry without a from-scratch Rust std port (fry1730).
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

extern char **environ;

/* environ-backed getenv (CPython never referenced it; Rust std does). */
char *getenv(const char *name) {
    if (!name || !environ) return NULL;
    size_t n = strlen(name);
    for (char **e = environ; *e; ++e)
        if (!strncmp(*e, name, n) && (*e)[n] == '=')
            return *e + n + 1;
    return NULL;
}

/* BSD bcmp == memcmp (LLVM emits bcmp for equality compares). */
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

/* glibc large-file-support aliases -> TaterTOS' already-64-bit base calls. */
off_t lseek64(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
int   fstat64(int fd, struct stat *st) { return fstat(fd, st); }
int   stat64(const char *path, struct stat *st) { return stat(path, st); }
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return mmap(addr, len, prot, flags, fd, off);
}
int open64(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    return open(path, flags, mode);
}

/* XSI strerror_r (returns int). Independent of any GNU-variant in string_ext.c. */
extern char *strerror(int);
int __xpg_strerror_r(int errnum, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return 0;
    const char *s = strerror(errnum);
    size_t i = 0;
    for (; s && s[i] && i + 1 < buflen; ++i) buf[i] = s[i];
    buf[i] = '\0';
    return 0;
}

/* No shared objects in a static .fry image: the unwinder/backtrace finds none. */
int dl_iterate_phdr(void *callback, void *data) { (void)callback; (void)data; return 0; }

/* Generic syscall(): Rust std uses it almost exclusively for getrandom (HashMap
 * RandomState seed). Provide a functional seed so HashMap::new() never fails.
 * (Linux SYS_getrandom == 318. TODO: route to a real TaterTOS entropy source.) */
long syscall(long number, ...) {
    if (number == 318) { /* getrandom(buf, buflen, flags) */
        va_list ap; va_start(ap, number);
        unsigned char *buf = va_arg(ap, unsigned char *);
        size_t len = va_arg(ap, size_t);
        va_end(ap);
        static uint64_t s = 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < len; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = (unsigned char)(s >> 33);
        }
        return (long)len;
    }
    return -1;
}

/* ── Native x86_64-tatertos Rust std/libc bindings (fry1743) ──────────────
 * The native pydantic_core (built for x86_64-tatertos against our own Rust std
 * + libc fork) calls these POSIX-named symbols directly. The glibc/linux shims
 * above are no longer referenced by the native build (linker GC-drops them). */

extern long fry_getrandom(void *buf, unsigned long len, unsigned int flags);
extern int *__errno_location(void);

/* POSIX getrandom() -> TaterTOS SYS_GETRANDOM (85). std's randomness backend
 * (sys/random/getrandom.rs) calls this. Returns count, or -1 w/ errno set. */
long getrandom(void *buf, size_t len, unsigned int flags) {
    long r = fry_getrandom(buf, len, flags);
    if (r < 0) { *__errno_location() = (int)(-r); return -1; }
    return r;
}

/* Unprefixed errno accessor alias (some Rust/libc paths reference this name). */
int *errno_location(void) { return __errno_location(); }
