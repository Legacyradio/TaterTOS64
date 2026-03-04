// Minimal userspace libc

#include "libc.h"
#include <stdarg.h>

// String/memory
size_t strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0 || b[i] == 0) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    size_t i = 0;
    while (src[i] && i < n) {
        *d++ = src[i++];
    }
    *d = 0;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = (const unsigned char *)a;
    const unsigned char *bb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    }
    return 0;
}

char *utoa(unsigned int value, char *buf, int base);

char *itoa(int value, char *buf, int base) {
    if (base < 2 || base > 16) {
        buf[0] = 0;
        return buf;
    }
    if (value < 0 && base == 10) {
        buf[0] = '-';
        utoa((unsigned int)(-value), buf + 1, base);
        return buf;
    }
    return utoa((unsigned int)value, buf, base);
}

char *utoa(unsigned int value, char *buf, int base) {
    if (base < 2 || base > 16) {
        buf[0] = 0;
        return buf;
    }
    char tmp[32];
    int i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value) {
            unsigned int d = value % (unsigned int)base;
            tmp[i++] = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
            value /= (unsigned int)base;
        }
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

int atoi(const char *s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val * sign;
}

// Syscalls (SYSCALL instruction)
/*
 * TaterTOS syscall entry does not preserve all caller-saved GPRs on return.
 * Model that explicitly in clobbers/constraints so inlined call sites stay
 * correct (e.g., malloc keeping live state across fry_sbrk).
 */
static inline long syscall3(long n, long a, long b, long c) {
    long ret;
    register long ra __asm__("rdi") = a;
    register long rb __asm__("rsi") = b;
    register long rc __asm__("rdx") = c;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+D"(ra), "+S"(rb), "+d"(rc)
                     : "a"(n)
                     : "rcx", "r11", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long syscall2(long n, long a, long b) {
    long ret;
    register long ra __asm__("rdi") = a;
    register long rb __asm__("rsi") = b;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+D"(ra), "+S"(rb)
                     : "a"(n)
                     : "rcx", "r11", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long syscall1(long n, long a) {
    long ret;
    register long ra __asm__("rdi") = a;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+D"(ra)
                     : "a"(n)
                     : "rcx", "r11", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long syscall0(long n) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n)
                     : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

enum {
    SYS_WRITE = 0,
    SYS_READ = 1,
    SYS_EXIT = 2,
    SYS_SPAWN = 3,
    SYS_SLEEP = 4,
    SYS_OPEN = 5,
    SYS_CLOSE = 6,
    SYS_GETPID = 7,
    SYS_STAT = 8,
    SYS_READDIR = 9,
    SYS_GETTIME = 10,
    SYS_REBOOT = 11,
    SYS_SHUTDOWN = 12,
    SYS_WAIT = 13,
    SYS_PROCCOUNT = 14,
    SYS_SETBRIGHT = 15,
    SYS_GETBRIGHT = 16,
    SYS_GETBATTERY = 17,
    SYS_FB_INFO = 18,
    SYS_FB_MAP = 19,
    SYS_MOUSE_GET = 20,
    SYS_PROC_OUTPUT = 21,
    SYS_SBRK = 22,
    SYS_SHM_ALLOC = 23,
    SYS_SHM_MAP = 24,
    SYS_PROC_INPUT = 25,
    SYS_KILL = 26,
    SYS_SHM_FREE = 27,
    SYS_ACPI_DIAG = 28,
    SYS_CREATE = 29,
    SYS_MKDIR = 30,
    SYS_UNLINK = 31,
    SYS_STORAGE_INFO = 32,
    SYS_PATH_FS_INFO = 33,
    SYS_MOUNTS_INFO = 34,
    SYS_READDIR_EX = 35
};

long fry_write(int fd, const void *buf, size_t len) {
    return syscall3(SYS_WRITE, fd, (long)buf, len);
}

long fry_read(int fd, void *buf, size_t len) {
    return syscall3(SYS_READ, fd, (long)buf, len);
}

long fry_spawn(const char *path) {
    return syscall1(SYS_SPAWN, (long)path);
}

long fry_exit(int code) {
    return syscall1(SYS_EXIT, (long)code);
}

long fry_sleep(uint64_t ms) {
    return syscall1(SYS_SLEEP, (long)ms);
}

long fry_open(const char *path, int flags) {
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

long fry_close(int fd) {
    return syscall1(SYS_CLOSE, (long)fd);
}

long fry_getpid(void) {
    return syscall0(SYS_GETPID);
}

long fry_gettime(void) {
    return syscall0(SYS_GETTIME);
}

long fry_sbrk(intptr_t increment) {
    return syscall1(SYS_SBRK, (long)increment);
}

long fry_shm_alloc(size_t size) {
    return syscall1(SYS_SHM_ALLOC, (long)size);
}

long fry_shm_map(int shm_id) {
    return syscall1(SYS_SHM_MAP, (long)shm_id);
}

long fry_shm_free(int shm_id) {
    return syscall1(SYS_SHM_FREE, (long)shm_id);
}

long fry_stat(const char *path, struct fry_stat *st) {
    return syscall2(SYS_STAT, (long)path, (long)st);
}

long fry_readdir(const char *path, void *buf, size_t len) {
    return syscall3(SYS_READDIR, (long)path, (long)buf, (long)len);
}
long fry_readdir_ex(const char *path, void *buf, size_t len) {
    return syscall3(SYS_READDIR_EX, (long)path, (long)buf, (long)len);
}

long fry_reboot(void) {
    return syscall1(SYS_REBOOT, 0);
}

long fry_shutdown(void) {
    return syscall1(SYS_SHUTDOWN, 0);
}

long fry_wait(uint32_t pid) {
    return syscall1(SYS_WAIT, (long)pid);
}

long fry_proc_count(void) {
    return syscall0(SYS_PROCCOUNT);
}

long fry_setbrightness(uint32_t percent) {
    return syscall1(SYS_SETBRIGHT, (long)percent);
}

long fry_getbrightness(void) {
    return syscall0(SYS_GETBRIGHT);
}

long fry_getbattery(struct fry_battery_status *out) {
    return syscall1(SYS_GETBATTERY, (long)out);
}

long fry_fb_info(struct fry_fb_info *info) {
    return syscall1(SYS_FB_INFO, (long)info);
}

long fry_fb_map(void) {
    return syscall0(SYS_FB_MAP);
}

long fry_mouse_get(struct fry_mouse_state *ms) {
    return syscall1(SYS_MOUSE_GET, (long)ms);
}

long fry_proc_output(uint32_t pid, void *buf, size_t len) {
    return syscall3(SYS_PROC_OUTPUT, (long)pid, (long)buf, (long)len);
}

long fry_proc_input(uint32_t pid, const void *buf, size_t len) {
    return syscall3(SYS_PROC_INPUT, (long)pid, (long)buf, len);
}

long fry_kill(long pid) {
    return syscall1(SYS_KILL, pid);
}

long fry_acpi_diag(struct fry_acpi_diag *out) {
    return syscall1(SYS_ACPI_DIAG, (long)out);
}

long fry_storage_info(struct fry_storage_info *out) {
    return syscall1(SYS_STORAGE_INFO, (long)out);
}

long fry_path_fs_info(const char *path, struct fry_path_fs_info *out) {
    return syscall2(SYS_PATH_FS_INFO, (long)path, (long)out);
}

long fry_mounts_info(struct fry_mounts_info *out) {
    return syscall1(SYS_MOUNTS_INFO, (long)out);
}

long fry_create(const char *path, uint16_t type) {
    return syscall2(SYS_CREATE, (long)path, (long)type);
}

long fry_mkdir(const char *path) {
    return syscall1(SYS_MKDIR, (long)path);
}

long fry_unlink(const char *path) {
    return syscall1(SYS_UNLINK, (long)path);
}

int getchar_nb(void) {
    char c = 0;
    long n = fry_read(0, &c, 1);
    if (n <= 0) return -1;
    return (int)(unsigned char)c;
}

// printf subset
static void u64_to_str(uint64_t val, char *buf, int base, int uppercase) {
    char tmp[32];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            uint64_t digit = val % (uint64_t)base;
            if (digit < 10) tmp[i++] = (char)('0' + digit);
            else tmp[i++] = (char)((uppercase ? 'A' : 'a') + (digit - 10));
            val /= (uint64_t)base;
        }
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
}

static void append_char(char *buf, size_t n, size_t *pos, char c) {
    if (*pos + 1 < n) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void append_str(char *buf, size_t n, size_t *pos, const char *s) {
    if (!s) s = "(null)";
    while (*s) {
        append_char(buf, n, pos, *s++);
    }
}

static void append_padded(char *buf, size_t n, size_t *pos,
                          const char *s, int width, char pad) {
    int len = 0;
    for (const char *t = s; *t; t++) len++;
    for (int i = len; i < width; i++)
        append_char(buf, n, pos, pad);
    append_str(buf, n, pos, s);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            append_char(buf, n, &pos, *p);
            continue;
        }
        p++;
        /* parse flags */
        char pad = ' ';
        int left_align = 0;
        for (;;) {
            if (*p == '0')      { pad = '0'; p++; }
            else if (*p == '-') { left_align = 1; p++; }
            else break;
        }
        if (left_align) pad = ' ';
        /* parse width */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        /* parse length modifier */
        int longlong = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                longlong = 1;
                p++;
            }
        }
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            if (left_align) {
                int len = 0;
                for (const char *t = s; *t; t++) len++;
                append_str(buf, n, &pos, s);
                for (int i = len; i < width; i++)
                    append_char(buf, n, &pos, ' ');
            } else {
                append_padded(buf, n, &pos, s, width, ' ');
            }
        } else if (*p == 'c') {
            append_char(buf, n, &pos, (char)va_arg(ap, int));
        } else if (*p == 'd' || *p == 'u') {
            long long v = longlong ? va_arg(ap, long long) : va_arg(ap, int);
            int neg = 0;
            if (*p == 'd' && v < 0) {
                neg = 1;
                v = -v;
            }
            char num[32];
            u64_to_str((uint64_t)v, num, 10, 0);
            if (neg && pad == '0' && width > 0) {
                append_char(buf, n, &pos, '-');
                append_padded(buf, n, &pos, num, width - 1, '0');
            } else {
                char full[34];
                int fi = 0;
                if (neg) full[fi++] = '-';
                for (int i = 0; num[i]; i++) full[fi++] = num[i];
                full[fi] = 0;
                if (left_align) {
                    int len = fi;
                    append_str(buf, n, &pos, full);
                    for (int i = len; i < width; i++)
                        append_char(buf, n, &pos, ' ');
                } else {
                    append_padded(buf, n, &pos, full, width, pad);
                }
            }
        } else if (*p == 'x' || *p == 'X') {
            unsigned long long v = longlong ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            char num[32];
            u64_to_str((uint64_t)v, num, 16, (*p == 'X'));
            if (left_align) {
                int len = 0;
                for (const char *t = num; *t; t++) len++;
                append_str(buf, n, &pos, num);
                for (int i = len; i < width; i++)
                    append_char(buf, n, &pos, ' ');
            } else {
                append_padded(buf, n, &pos, num, width, pad);
            }
        } else if (*p == 'p') {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            append_str(buf, n, &pos, "0x");
            char num[32];
            u64_to_str(v, num, 16, 0);
            append_str(buf, n, &pos, num);
        } else if (*p == '%') {
            append_char(buf, n, &pos, '%');
        }
    }
    if (n > 0) {
        size_t term = (pos < n) ? pos : (n - 1);
        buf[term] = 0;
    }
    return (int)pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return len;
    size_t out = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;
    fry_write(1, buf, out);
    return len;
}

int putchar(int c) {
    char ch = (char)c;
    return (int)fry_write(1, &ch, 1);
}

int puts(const char *s) {
    if (!s) s = "(null)";
    size_t len = strlen(s);
    fry_write(1, s, len);
    fry_write(1, "\n", 1);
    return (int)len;
}

int getchar(void) {
    char c = 0;
    if (fry_read(0, &c, 1) <= 0) return -1;
    return (int)c;
}

char *gets_bounded(char *buf, int max) {
    if (!buf || max <= 0) return 0;
    int pos = 0;
    for (;;) {
        char c = 0;
        long n = fry_read(0, &c, 1);
        if (n <= 0) continue;
        if (c == '\r') c = '\n';
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                fry_write(1, "\b \b", 3);
            }
            continue;
        }
        if (c == '\n') {
            fry_write(1, "\n", 1);
            break;
        }
        if (pos + 1 < max) {
            buf[pos++] = c;
            fry_write(1, &c, 1);
        }
    }
    buf[pos] = 0;
    return buf;
}

struct malloc_block {
    size_t size;
    int free;
    struct malloc_block *next;
};

static struct malloc_block *free_list = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~15; // 16-byte alignment
    struct malloc_block *prev = NULL;
    struct malloc_block *curr = free_list;
    while (curr) {
        if (curr->free && curr->size >= size) {
            curr->free = 0;
            return (void *)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }
    size_t total_size = size + sizeof(struct malloc_block);
    long res = fry_sbrk((intptr_t)total_size);
    if (res == -1) return NULL;
    struct malloc_block *block = (struct malloc_block *)res;
    block->size = size;
    block->free = 0;
    block->next = NULL;
    if (prev) prev->next = block;
    else free_list = block;
    return (void *)(block + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    struct malloc_block *block = (struct malloc_block *)ptr - 1;
    block->free = 1;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    struct malloc_block *block = (struct malloc_block *)ptr - 1;
    if (block->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}
