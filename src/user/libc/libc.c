// Minimal userspace libc

#include "libc.h"
#include <fry_time.h>
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

/* ------------------------------------------------------------------ */
/* assert support                                                      */
/* ------------------------------------------------------------------ */

void abort(void) {
    printf("ABORT called\n");
    fry_exit(134);
    __builtin_unreachable();
}

void __assert_fail(const char *expr, const char *file,
                   unsigned int line, const char *func) {
    printf("ASSERTION FAILED: %s at %s:%u (%s)\n", expr, file, line, func);
    fry_exit(127);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* qsort — heapsort (in-place, no malloc, O(n log n) worst case)      */
/* bsearch — binary search on sorted array                            */
/* ------------------------------------------------------------------ */

static void qs_swap(uint8_t *a, uint8_t *b, size_t size) {
    uint8_t tmp;
    while (size--) {
        tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

static void qs_sift_down(uint8_t *base, size_t size,
                         int (*cmp)(const void *, const void *),
                         size_t start, size_t end) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        size_t swap_idx = root;
        if (cmp(base + swap_idx * size, base + child * size) < 0)
            swap_idx = child;
        if (child + 1 <= end &&
            cmp(base + swap_idx * size, base + (child + 1) * size) < 0)
            swap_idx = child + 1;
        if (swap_idx == root)
            return;
        qs_swap(base + root * size, base + swap_idx * size, size);
        root = swap_idx;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    uint8_t *b;
    size_t end;
    size_t start;
    if (!base || nmemb < 2 || !size || !compar) return;
    b = (uint8_t *)base;
    /* Build max-heap */
    start = (nmemb - 2) / 2;
    for (;;) {
        qs_sift_down(b, size, compar, start, nmemb - 1);
        if (start == 0) break;
        start--;
    }
    /* Extract elements from heap */
    end = nmemb - 1;
    while (end > 0) {
        qs_swap(b, b + end * size, size);
        end--;
        qs_sift_down(b, size, compar, 0, end);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const uint8_t *b = (const uint8_t *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, b + mid * size);
        if (cmp < 0)
            hi = mid;
        else if (cmp > 0)
            lo = mid + 1;
        else
            return (void *)(b + mid * size);
    }
    return NULL;
}

char *itoa(int value, char *buf, int base) {
    if (base < 2 || base > 16) {
        buf[0] = 0;
        return buf;
    }
    if (value < 0 && base == 10) {
        buf[0] = '-';
        unsigned int mag = (unsigned int)(-(value + 1)) + 1U;
        utoa(mag, buf + 1, base);
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

static inline long syscall4(long n, long a, long b, long c, long d) {
    long ret;
    register long ra __asm__("rdi") = a;
    register long rb __asm__("rsi") = b;
    register long rc __asm__("rdx") = c;
    register long rd __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+D"(ra), "+S"(rb), "+d"(rc), "+r"(rd)
                     : "a"(n)
                     : "rcx", "r11", "r8", "r9", "memory");
    return ret;
}

static inline long syscall5(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long ra __asm__("rdi") = a;
    register long rb __asm__("rsi") = b;
    register long rc __asm__("rdx") = c;
    register long rd __asm__("r10") = d;
    register long re __asm__("r8") = e;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+D"(ra), "+S"(rb), "+d"(rc), "+r"(rd), "+r"(re)
                     : "a"(n)
                     : "rcx", "r11", "r9", "memory");
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
    SYS_READDIR_EX = 35,
    SYS_MOUNTS_DEBUG = 36,
    SYS_WIFI_STATUS = 37,
    SYS_WIFI_SCAN = 38,
    SYS_WIFI_CONNECT = 39,
    SYS_WIFI_DEBUG = 40,
    SYS_WIFI_CPU_STATUS = 41,
    SYS_WIFI_INIT_LOG = 42,
    SYS_WIFI_DEBUG2 = 43,
    SYS_WIFI_HANDOFF = 44,
    SYS_WIFI_DEBUG3 = 45,
    SYS_WIFI_REINIT = 46,
    SYS_WIFI_CMD_TRACE = 47,
    SYS_WIFI_SRAM = 48,
    SYS_WIFI_DEEP_DIAG = 49,
    SYS_WIFI_VERIFY = 50,
    SYS_ETH_DIAG = 51,
    SYS_MMAP = 52,
    SYS_MUNMAP = 53,
    SYS_MPROTECT = 54,
    SYS_PIPE = 55,
    SYS_DUP = 56,
    SYS_DUP2 = 57,
    SYS_POLL = 58,
    SYS_FCNTL = 59,
    SYS_SPAWN_ARGS = 60,
    SYS_GET_ARGC = 61,
    SYS_GET_ARGV = 62,
    SYS_GETENV_SYS = 63,
    SYS_THREAD_CREATE = 64,
    SYS_THREAD_EXIT = 65,
    SYS_THREAD_JOIN = 66,
    SYS_GETTID = 67,
    SYS_FUTEX_WAIT = 68,
    SYS_FUTEX_WAKE = 69,
    SYS_SET_TLS_BASE = 70,
    SYS_GET_TLS_BASE = 71,
    SYS_SOCKET = 72,
    SYS_CONNECT = 73,
    SYS_BIND = 74,
    SYS_LISTEN = 75,
    SYS_ACCEPT = 76,
    SYS_SEND = 77,
    SYS_RECV = 78,
    SYS_SHUTDOWN_SOCK = 79,
    SYS_GETSOCKOPT = 80,
    SYS_SETSOCKOPT = 81,
    SYS_SENDTO = 82,
    SYS_RECVFROM = 83,
    SYS_DNS_RESOLVE = 84,
    SYS_GETRANDOM = 85,
    SYS_CLOCK_GETTIME = 86,
    SYS_NANOSLEEP = 87,
    SYS_LSEEK = 88,
    SYS_FTRUNCATE = 89,
    SYS_RENAME = 90,
    SYS_FSTAT = 91,
    SYS_KBD_EVENT = 92,
    SYS_MOUSE_GET_EXT = 93,
    SYS_CLIPBOARD_GET = 94,
    SYS_CLIPBOARD_SET = 95,
    SYS_AUDIO_OPEN  = 96,
    SYS_AUDIO_WRITE = 97,
    SYS_AUDIO_CLOSE = 98,
    SYS_AUDIO_INFO  = 99
};

#define FRY_THREAD_STACK_SIZE (2u * 1024u * 1024u)
#define FRY_TLS_KEYS_MAX 64u

struct fry_tls_block {
    void *slots[FRY_TLS_KEYS_MAX];
    uint32_t thread_tid;
    void *stack_base;
    size_t stack_len;
    void *tls_base;
};

struct fry_thread_bootstrap {
    fry_thread_start_t start;
    void *arg;
    void *tls_base;
};

static volatile uint32_t g_fry_tls_keys_used;

static struct fry_tls_block *fry_tls_block_current(void) {
    return (struct fry_tls_block *)fry_tls_get_base();
}

static struct fry_tls_block *fry_tls_block_ensure(void) {
    struct fry_tls_block *block = fry_tls_block_current();
    long tid;
    if (block) return block;

    block = (struct fry_tls_block *)calloc(1, sizeof(*block));
    if (!block) return 0;
    block->tls_base = block;
    tid = syscall0(SYS_GETTID);
    if (tid > 0) block->thread_tid = (uint32_t)tid;
    if (fry_tls_set_base(block) < 0) {
        free(block);
        return 0;
    }
    return block;
}

__attribute__((noreturn))
static void fry_thread_trampoline(void *opaque) {
    struct fry_thread_bootstrap ctx = *(struct fry_thread_bootstrap *)opaque;
    struct fry_tls_block *block = (struct fry_tls_block *)ctx.tls_base;
    if (fry_tls_set_base(ctx.tls_base) < 0) {
        fry_thread_exit(1);
    }
    if (block) {
        block->tls_base = block;
        if (block->thread_tid == 0) {
            long tid = syscall0(SYS_GETTID);
            if (tid > 0) block->thread_tid = (uint32_t)tid;
        }
    }
    ctx.start(ctx.arg);
    fry_thread_exit(0);
}

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

long fry_gettid(void) {
    return syscall0(SYS_GETTID);
}

int fry_thread_current(struct fry_thread *thr) {
    struct fry_tls_block *block;
    long tid;

    if (!thr) return -EINVAL;

    memset(thr, 0, sizeof(*thr));
    block = fry_tls_block_current();
    if (!block) block = fry_tls_block_ensure();
    if (!block) return -ENOMEM;

    thr->tls_base = block;
    if (!block->tls_base) block->tls_base = block;

    tid = block->thread_tid ? (long)block->thread_tid : fry_gettid();
    if (tid < 0) return (int)tid;

    block->thread_tid = (uint32_t)tid;
    thr->tid = (uint32_t)tid;
    thr->stack_base = block->stack_base;
    thr->stack_len = block->stack_len;
    return 0;
}

long fry_futex_wait(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ms) {
    return syscall3(SYS_FUTEX_WAIT, (long)addr, (long)expected, (long)timeout_ms);
}

long fry_futex_wake(volatile uint32_t *addr, uint32_t count) {
    return syscall2(SYS_FUTEX_WAKE, (long)addr, (long)count);
}

long fry_tls_set_base(void *base) {
    return syscall1(SYS_SET_TLS_BASE, (long)base);
}

void *fry_tls_get_base(void) {
    return (void *)(uintptr_t)syscall0(SYS_GET_TLS_BASE);
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

long fry_pipe(int fds[2]) {
    return syscall1(SYS_PIPE, (long)fds);
}

long fry_dup(int oldfd) {
    return syscall1(SYS_DUP, (long)oldfd);
}

long fry_dup2(int oldfd, int newfd) {
    return syscall2(SYS_DUP2, (long)oldfd, (long)newfd);
}

long fry_poll(struct fry_pollfd *fds, uint32_t nfds, uint64_t timeout_ms) {
    return syscall3(SYS_POLL, (long)fds, (long)nfds, (long)timeout_ms);
}

long fry_fcntl(int fd, int cmd, long arg) {
    return syscall3(SYS_FCNTL, (long)fd, (long)cmd, arg);
}

long fry_spawn_args(const char *path, const char **argv, uint32_t argc,
                    const char **envp, uint32_t envc) {
    return syscall5(SYS_SPAWN_ARGS, (long)path, (long)argv, (long)argc,
                    (long)envp, (long)envc);
}

long fry_get_argc(void) {
    return syscall0(SYS_GET_ARGC);
}

long fry_get_argv(uint32_t index, char *buf, size_t len) {
    return syscall3(SYS_GET_ARGV, (long)index, (long)buf, (long)len);
}

long fry_getenv(const char *name, char *buf, size_t len) {
    return syscall3(SYS_GETENV_SYS, (long)name, (long)buf, (long)len);
}

/* Phase 4: Socket ABI wrappers */
long fry_socket(int domain, int type, int protocol) {
    return syscall3(SYS_SOCKET, (long)domain, (long)type, (long)protocol);
}

long fry_connect(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen) {
    return syscall3(SYS_CONNECT, (long)fd, (long)addr, (long)addrlen);
}

long fry_bind(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen) {
    return syscall3(SYS_BIND, (long)fd, (long)addr, (long)addrlen);
}

long fry_listen(int fd, int backlog) {
    return syscall2(SYS_LISTEN, (long)fd, (long)backlog);
}

long fry_accept(int fd, struct fry_sockaddr_in *addr, uint32_t *addrlen) {
    return syscall3(SYS_ACCEPT, (long)fd, (long)addr, (long)addrlen);
}

long fry_send(int fd, const void *buf, size_t len, int flags) {
    return syscall4(SYS_SEND, (long)fd, (long)buf, (long)len, (long)flags);
}

long fry_recv(int fd, void *buf, size_t len, int flags) {
    return syscall4(SYS_RECV, (long)fd, (long)buf, (long)len, (long)flags);
}

long fry_shutdown_sock(int fd, int how) {
    return syscall2(SYS_SHUTDOWN_SOCK, (long)fd, (long)how);
}

long fry_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen) {
    return syscall5(SYS_GETSOCKOPT, (long)fd, (long)level, (long)optname,
                    (long)optval, (long)optlen);
}

long fry_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    return syscall5(SYS_SETSOCKOPT, (long)fd, (long)level, (long)optname,
                    (long)optval, (long)optlen);
}

long fry_sendto(int fd, const void *buf, size_t len, int flags,
                const struct fry_sockaddr_in *dest_addr) {
    return syscall5(SYS_SENDTO, (long)fd, (long)buf, (long)len,
                    (long)flags, (long)dest_addr);
}

long fry_recvfrom(int fd, void *buf, size_t len, int flags,
                  struct fry_sockaddr_in *src_addr) {
    return syscall5(SYS_RECVFROM, (long)fd, (long)buf, (long)len,
                    (long)flags, (long)src_addr);
}

long fry_dns_resolve(const char *hostname, uint32_t *ip_out) {
    return syscall2(SYS_DNS_RESOLVE, (long)hostname, (long)ip_out);
}

long fry_getrandom(void *buf, unsigned long len, unsigned int flags) {
    return syscall3(SYS_GETRANDOM, (long)buf, (long)len, (long)flags);
}

long fry_clock_gettime(int clock_id, struct fry_timespec *ts) {
    return syscall2(SYS_CLOCK_GETTIME, (long)clock_id, (long)ts);
}

long fry_nanosleep(const struct fry_timespec *req, struct fry_timespec *rem) {
    return syscall2(SYS_NANOSLEEP, (long)req, (long)rem);
}

/* Phase 6: Filesystem expansion */
long fry_lseek(int fd, int64_t offset, int whence) {
    return syscall3(SYS_LSEEK, (long)fd, (long)offset, (long)whence);
}

long fry_ftruncate(int fd, uint64_t length) {
    return syscall2(SYS_FTRUNCATE, (long)fd, (long)length);
}

long fry_rename(const char *old_path, const char *new_path) {
    return syscall2(SYS_RENAME, (long)old_path, (long)new_path);
}

long fry_fstat(int fd, struct fry_stat *st) {
    return syscall2(SYS_FSTAT, (long)fd, (long)st);
}

/* Phase 7: GUI/Input expansion */
long fry_kbd_event(struct fry_key_event *out) {
    return syscall1(SYS_KBD_EVENT, (long)out);
}

long fry_mouse_get_ext(struct fry_mouse_state *ms) {
    return syscall1(SYS_MOUSE_GET_EXT, (long)ms);
}

long fry_clipboard_get(char *buf, size_t maxlen) {
    return syscall2(SYS_CLIPBOARD_GET, (long)buf, (long)maxlen);
}

long fry_clipboard_set(const char *buf, size_t len) {
    return syscall2(SYS_CLIPBOARD_SET, (long)buf, (long)len);
}

/* Audio syscalls */
long fry_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    return syscall3(SYS_AUDIO_OPEN, (long)sample_rate, (long)channels, (long)bits);
}

long fry_audio_write(const void *pcm_data, size_t len) {
    return syscall2(SYS_AUDIO_WRITE, (long)pcm_data, (long)len);
}

long fry_audio_close(void) {
    return syscall0(SYS_AUDIO_CLOSE);
}

long fry_audio_info(void *info_buf) {
    return syscall1(SYS_AUDIO_INFO, (long)info_buf);
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

void *fry_mmap(void *addr, size_t len, uint32_t prot, uint32_t flags) {
    return fry_mmap_fd(addr, len, prot, flags, -1);
}

void *fry_mmap_fd(void *addr, size_t len, uint32_t prot, uint32_t flags, int fd) {
    return (void *)(uintptr_t)syscall5(SYS_MMAP, (long)addr, (long)len,
                                       (long)prot, (long)flags, (long)fd);
}

void *fry_mreserve(void *addr, size_t len, uint32_t flags) {
    return fry_mmap(addr, len, 0, flags | FRY_MAP_ANON | FRY_MAP_RESERVE);
}

void *fry_mguard(void *addr, size_t len) {
    return fry_mmap(addr, len, 0, FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_GUARD);
}

long fry_mcommit(void *addr, size_t len, uint32_t prot) {
    return fry_mprotect(addr, len, prot);
}

long fry_munmap(void *addr, size_t len) {
    return syscall2(SYS_MUNMAP, (long)addr, (long)len);
}

long fry_mprotect(void *addr, size_t len, uint32_t prot) {
    return syscall3(SYS_MPROTECT, (long)addr, (long)len, (long)prot);
}

long fry_syscall_raw(long num, long a1) {
    return syscall1(num, a1);
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

long fry_thread_create(struct fry_thread *thr, fry_thread_start_t start, void *arg) {
    uint8_t *stack;
    uint64_t stack_top;
    uint64_t ctx_addr;
    struct fry_thread_bootstrap *ctx;
    struct fry_tls_block *tls_block;
    long tid;

    if (!thr || !start) return -EINVAL;
    if (thr->tid != 0) return -EINVAL;

    stack = (uint8_t *)fry_mmap(0, FRY_THREAD_STACK_SIZE,
                                FRY_PROT_READ | FRY_PROT_WRITE,
                                FRY_MAP_PRIVATE | FRY_MAP_ANON);
    if (FRY_IS_ERR(stack)) return (long)(intptr_t)stack;

    tls_block = (struct fry_tls_block *)calloc(1, sizeof(*tls_block));
    if (!tls_block) {
        fry_munmap(stack, FRY_THREAD_STACK_SIZE);
        return -ENOMEM;
    }
    tls_block->stack_base = stack;
    tls_block->stack_len = FRY_THREAD_STACK_SIZE;
    tls_block->tls_base = tls_block;

    stack_top = (uint64_t)(uintptr_t)(stack + FRY_THREAD_STACK_SIZE);
    /* Place bootstrap context 256 bytes below stack_top to leave room for
     * the trampoline's function prologue (callee-saved register pushes +
     * local variables).  Previously the context was at stack_top-32 and
     * RSP at stack_top-8, so the prologue's push rbp/rbx/r12 overwrote
     * the context before the trampoline could copy it. */
    ctx_addr = (stack_top - 256u - sizeof(*ctx)) & ~0xFULL;
    ctx = (struct fry_thread_bootstrap *)(uintptr_t)ctx_addr;
    ctx->start = start;
    ctx->arg = arg;
    ctx->tls_base = tls_block;

    tid = syscall3(SYS_THREAD_CREATE,
                   (long)(uintptr_t)fry_thread_trampoline,
                   (long)(uintptr_t)ctx,
                   (long)(stack_top - 8u));
    if (tid < 0) {
        free(tls_block);
        fry_munmap(stack, FRY_THREAD_STACK_SIZE);
        return tid;
    }

    tls_block->thread_tid = (uint32_t)tid;
    thr->tid = (uint32_t)tid;
    thr->stack_base = stack;
    thr->stack_len = FRY_THREAD_STACK_SIZE;
    thr->tls_base = tls_block;
    return tid;
}

long fry_thread_join(struct fry_thread *thr, int *exit_code) {
    long rc;
    long unmap_rc;

    if (!thr || thr->tid == 0 || !thr->stack_base || thr->stack_len == 0) return -EINVAL;

    rc = syscall1(SYS_THREAD_JOIN, (long)thr->tid);
    if (rc < 0) return rc;

    if (exit_code) *exit_code = (int)rc;
    unmap_rc = fry_munmap(thr->stack_base, thr->stack_len);
    if (thr->tls_base) free(thr->tls_base);
    thr->tid = 0;
    thr->stack_base = 0;
    thr->stack_len = 0;
    thr->tls_base = 0;
    if (unmap_rc < 0) return unmap_rc;
    return 0;
}

__attribute__((noreturn))
void fry_thread_exit(int code) {
    (void)syscall1(SYS_THREAD_EXIT, (long)code);
    for (;;) {}
}

int fry_tls_key_create(fry_tls_key_t *out_key) {
    uint32_t key;
    if (!out_key) return -EINVAL;
    key = (uint32_t)__sync_fetch_and_add(&g_fry_tls_keys_used, 1u);
    if (key >= FRY_TLS_KEYS_MAX) return -ENOSPC;
    *out_key = key;
    return 0;
}

void *fry_tls_get(fry_tls_key_t key) {
    struct fry_tls_block *block;
    if (key >= FRY_TLS_KEYS_MAX) return 0;
    block = fry_tls_block_ensure();
    if (!block) return 0;
    return block->slots[key];
}

int fry_tls_set(fry_tls_key_t key, void *value) {
    struct fry_tls_block *block;
    if (key >= FRY_TLS_KEYS_MAX) return -EINVAL;
    block = fry_tls_block_ensure();
    if (!block) return -ENOMEM;
    block->slots[key] = value;
    return 0;
}

int fry_mutex_trylock(fry_mutex_t *mutex) {
    if (!mutex) return -EINVAL;
    return __sync_bool_compare_and_swap(&mutex->state, 0u, 1u) ? 0 : -EBUSY;
}

int fry_mutex_lock(fry_mutex_t *mutex) {
    if (!mutex) return -EINVAL;
    if (__sync_bool_compare_and_swap(&mutex->state, 0u, 1u)) return 0;

    for (;;) {
        uint32_t prev = __sync_lock_test_and_set(&mutex->state, 2u);
        if (prev == 0u) return 0;
        long rc = fry_futex_wait(&mutex->state, 2u, 0);
        if (rc < 0 && rc != -EAGAIN) return (int)rc;
    }
}

int fry_mutex_unlock(fry_mutex_t *mutex) {
    uint32_t prev;
    if (!mutex) return -EINVAL;
    prev = __sync_lock_test_and_set(&mutex->state, 0u);
    if (prev == 0u) return -EPERM;
    if (prev == 2u) (void)fry_futex_wake(&mutex->state, 1u);
    return 0;
}

int fry_cond_wait(fry_cond_t *cond, fry_mutex_t *mutex) {
    uint32_t seq;
    long rc;
    int lock_rc;
    if (!cond || !mutex) return -EINVAL;
    seq = cond->seq;
    lock_rc = fry_mutex_unlock(mutex);
    if (lock_rc < 0) return lock_rc;
    rc = fry_futex_wait(&cond->seq, seq, 0);
    lock_rc = fry_mutex_lock(mutex);
    if (lock_rc < 0) return lock_rc;
    if (rc < 0 && rc != -EAGAIN) return (int)rc;
    return 0;
}

int fry_cond_signal(fry_cond_t *cond) {
    if (!cond) return -EINVAL;
    (void)__sync_fetch_and_add(&cond->seq, 1u);
    (void)fry_futex_wake(&cond->seq, 1u);
    return 0;
}

int fry_cond_broadcast(fry_cond_t *cond) {
    if (!cond) return -EINVAL;
    (void)__sync_fetch_and_add(&cond->seq, 1u);
    (void)fry_futex_wake(&cond->seq, UINT32_MAX);
    return 0;
}

int fry_sem_init(fry_sem_t *sem, uint32_t value) {
    if (!sem) return -EINVAL;
    sem->count = value;
    return 0;
}

int fry_sem_wait(fry_sem_t *sem) {
    if (!sem) return -EINVAL;
    for (;;) {
        uint32_t count = sem->count;
        while (count != 0u) {
            if (__sync_bool_compare_and_swap(&sem->count, count, count - 1u)) return 0;
            count = sem->count;
        }
        {
            long rc = fry_futex_wait(&sem->count, 0u, 0);
            if (rc < 0 && rc != -EAGAIN) return (int)rc;
        }
    }
}

int fry_sem_post(fry_sem_t *sem) {
    if (!sem) return -EINVAL;
    (void)__sync_fetch_and_add(&sem->count, 1u);
    (void)fry_futex_wake(&sem->count, 1u);
    return 0;
}

int fry_once(fry_once_t *once, void (*init_fn)(void)) {
    if (!once || !init_fn) return -EINVAL;
    for (;;) {
        uint32_t state = once->state;
        if (state == 2u) return 0;
        if (state == 0u && __sync_bool_compare_and_swap(&once->state, 0u, 1u)) {
            init_fn();
            __sync_synchronize();
            once->state = 2u;
            (void)fry_futex_wake(&once->state, UINT32_MAX);
            return 0;
        }
        {
            long rc = fry_futex_wait(&once->state, 1u, 0);
            if (rc < 0 && rc != -EAGAIN) return (int)rc;
        }
    }
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

long fry_mounts_dbg(struct fry_mounts_dbg *out) {
    return syscall1(SYS_MOUNTS_DEBUG, (long)out);
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

long fry_wifi_status(struct fry_wifi_status *out) {
    return syscall1(SYS_WIFI_STATUS, (long)out);
}

long fry_wifi_scan(struct fry_wifi_scan_entry *out, uint32_t max_entries, uint32_t *out_count) {
    return syscall3(SYS_WIFI_SCAN, (long)out, (long)max_entries, (long)out_count);
}

long fry_wifi_connect(const char *ssid, const char *passphrase) {
    return syscall2(SYS_WIFI_CONNECT, (long)ssid, (long)passphrase);
}

long fry_wifi_debug(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_DEBUG, (long)buf, (long)bufsz);
}

long fry_wifi_cpu_status(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_CPU_STATUS, (long)buf, (long)bufsz);
}

long fry_wifi_init_log(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_INIT_LOG, (long)buf, (long)bufsz);
}

long fry_wifi_debug2(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_DEBUG2, (long)buf, (long)bufsz);
}

long fry_wifi_handoff(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_HANDOFF, (long)buf, (long)bufsz);
}

long fry_wifi_debug3(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_DEBUG3, (long)buf, (long)bufsz);
}

long fry_wifi_reinit(void) {
    return syscall0(SYS_WIFI_REINIT);
}

long fry_wifi_cmd_trace(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_CMD_TRACE, (long)buf, (long)bufsz);
}

long fry_wifi_sram(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_SRAM, (long)buf, (long)bufsz);
}

long fry_wifi_deep_diag(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_DEEP_DIAG, (long)buf, (long)bufsz);
}

long fry_wifi_verify(char *buf, uint32_t bufsz) {
    return syscall2(SYS_WIFI_VERIFY, (long)buf, (long)bufsz);
}

long fry_eth_diag(char *buf, uint32_t bufsz) {
    return syscall2(SYS_ETH_DIAG, (long)buf, (long)bufsz);
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
        /* parse precision */
        int has_prec = 0;
        int prec = 6; /* default precision for %f */
        if (*p == '.') {
            has_prec = 1;
            p++;
            prec = 0;
            while (*p >= '0' && *p <= '9') {
                prec = prec * 10 + (*p - '0');
                p++;
            }
        }
        /* parse length modifier */
        int longlong = 0;
        int is_long_double = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                longlong = 1;
                p++;
            }
        } else if (*p == 'L') {
            is_long_double = 1;
            p++;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++; /* hh — still int via va_arg promotion */
        } else if (*p == 'z' || *p == 'j' || *p == 't') {
            longlong = 1; /* size_t/intmax_t/ptrdiff_t — treat as 64-bit */
            p++;
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
        } else if (*p == 'o') {
            unsigned long long v = longlong ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            char num[32];
            u64_to_str((uint64_t)v, num, 8, 0);
            if (left_align) {
                int len = 0;
                for (const char *t = num; *t; t++) len++;
                append_str(buf, n, &pos, num);
                for (int i = len; i < width; i++)
                    append_char(buf, n, &pos, ' ');
            } else {
                append_padded(buf, n, &pos, num, width, pad);
            }
        } else if (*p == 'f' || *p == 'F' || *p == 'e' || *p == 'E' ||
                   *p == 'g' || *p == 'G' || *p == 'a' || *p == 'A') {
            /*
             * Float formatting: %f (fixed), %e (scientific), %g (auto).
             * Uses integer decomposition — no fenv dependency.
             */
            double val = va_arg(ap, double);
            (void)is_long_double; /* promoted to double by va_arg anyway */
            char fbuf[80];
            int fi = 0;
            char spec = *p;
            int upper = (spec == 'F' || spec == 'E' || spec == 'G' || spec == 'A');

            /* Handle special values */
            union { double d; uint64_t u; } du;
            du.d = val;
            int is_neg = (du.u >> 63) != 0;
            uint64_t exp_bits = (du.u >> 52) & 0x7FF;
            uint64_t frac_bits = du.u & 0x000FFFFFFFFFFFFFull;

            if (exp_bits == 0x7FF) {
                if (is_neg) fbuf[fi++] = '-';
                if (frac_bits != 0) {
                    const char *ns = upper ? "NAN" : "nan";
                    while (*ns) fbuf[fi++] = *ns++;
                } else {
                    const char *is = upper ? "INF" : "inf";
                    while (*is) fbuf[fi++] = *is++;
                }
                fbuf[fi] = 0;
            } else {
                if (is_neg) { fbuf[fi++] = '-'; val = -val; }

                /* Convert %g to %f or %e based on magnitude */
                char effective = spec;
                if (spec == 'g' || spec == 'G') {
                    int g_prec = prec > 0 ? prec : 1;
                    if (val == 0.0) {
                        effective = (spec == 'g') ? 'f' : 'F';
                        prec = g_prec > 1 ? g_prec - 1 : 0;
                    } else {
                        /* Compute exponent for %g decision */
                        double tmp = val;
                        int e10 = 0;
                        if (tmp >= 10.0) {
                            while (tmp >= 10.0) { tmp /= 10.0; e10++; }
                        } else if (tmp > 0.0 && tmp < 1.0) {
                            while (tmp < 1.0) { tmp *= 10.0; e10--; }
                        }
                        if (e10 >= -4 && e10 < g_prec) {
                            effective = (spec == 'g') ? 'f' : 'F';
                            prec = g_prec - 1 - e10;
                            if (prec < 0) prec = 0;
                        } else {
                            effective = (spec == 'g') ? 'e' : 'E';
                            prec = g_prec - 1;
                            if (prec < 0) prec = 0;
                        }
                    }
                }

                if (effective == 'f' || effective == 'F') {
                    /* Fixed-point: integer part + '.' + fractional digits */
                    uint64_t int_part = (uint64_t)val;
                    double frac = val - (double)int_part;
                    /* Integer part */
                    char ibuf[24];
                    u64_to_str(int_part, ibuf, 10, 0);
                    for (int i = 0; ibuf[i]; i++) fbuf[fi++] = ibuf[i];
                    if (prec > 0 || has_prec) {
                        fbuf[fi++] = '.';
                        for (int d = 0; d < prec && d < 20; d++) {
                            frac *= 10.0;
                            int digit = (int)frac;
                            if (digit > 9) digit = 9;
                            fbuf[fi++] = '0' + (char)digit;
                            frac -= (double)digit;
                        }
                        /* Round last digit */
                        if (prec > 0 && prec <= 20) {
                            frac *= 10.0;
                            if ((int)frac >= 5) {
                                /* Propagate rounding */
                                int carry = 1;
                                for (int k = fi - 1; k >= 0 && carry; k--) {
                                    if (fbuf[k] == '.') continue;
                                    if (fbuf[k] == '-') break;
                                    int d = fbuf[k] - '0' + carry;
                                    if (d > 9) { fbuf[k] = '0'; carry = 1; }
                                    else { fbuf[k] = '0' + (char)d; carry = 0; }
                                }
                                if (carry) {
                                    /* Need to insert a '1' at front */
                                    int start = is_neg ? 1 : 0;
                                    for (int k = fi; k > start; k--)
                                        fbuf[k] = fbuf[k - 1];
                                    fbuf[start] = '1';
                                    fi++;
                                }
                            }
                        }
                    }
                    fbuf[fi] = 0;
                    /* Strip trailing zeros for %g */
                    if ((spec == 'g' || spec == 'G') && fi > 0) {
                        while (fi > 1 && fbuf[fi - 1] == '0') fi--;
                        if (fi > 0 && fbuf[fi - 1] == '.') fi--;
                        fbuf[fi] = 0;
                    }
                } else if (effective == 'e' || effective == 'E') {
                    /* Scientific notation: d.ddddddde+NN */
                    int e10 = 0;
                    double mant = val;
                    if (mant > 0.0) {
                        while (mant >= 10.0) { mant /= 10.0; e10++; }
                        while (mant < 1.0 && mant > 0.0) { mant *= 10.0; e10--; }
                    }
                    /* Leading digit */
                    int lead = (int)mant;
                    if (lead > 9) lead = 9;
                    fbuf[fi++] = '0' + (char)lead;
                    mant -= (double)lead;
                    if (prec > 0) {
                        fbuf[fi++] = '.';
                        for (int d = 0; d < prec && d < 20; d++) {
                            mant *= 10.0;
                            int digit = (int)mant;
                            if (digit > 9) digit = 9;
                            fbuf[fi++] = '0' + (char)digit;
                            mant -= (double)digit;
                        }
                        /* Round */
                        if (prec <= 20) {
                            mant *= 10.0;
                            if ((int)mant >= 5) {
                                int carry = 1;
                                for (int k = fi - 1; k >= 0 && carry; k--) {
                                    if (fbuf[k] == '.' || fbuf[k] == '-') continue;
                                    int d = fbuf[k] - '0' + carry;
                                    if (d > 9) { fbuf[k] = '0'; carry = 1; }
                                    else { fbuf[k] = '0' + (char)d; carry = 0; }
                                }
                                if (carry) e10++;
                            }
                        }
                    }
                    /* Strip trailing zeros for %g */
                    if ((spec == 'g' || spec == 'G') && fi > 0) {
                        while (fi > 1 && fbuf[fi - 1] == '0') fi--;
                        if (fi > 0 && fbuf[fi - 1] == '.') fi--;
                    }
                    /* Exponent */
                    fbuf[fi++] = upper ? 'E' : 'e';
                    fbuf[fi++] = (e10 >= 0) ? '+' : '-';
                    if (e10 < 0) e10 = -e10;
                    if (e10 >= 100) {
                        fbuf[fi++] = '0' + (char)(e10 / 100);
                        fbuf[fi++] = '0' + (char)((e10 / 10) % 10);
                        fbuf[fi++] = '0' + (char)(e10 % 10);
                    } else {
                        fbuf[fi++] = '0' + (char)(e10 / 10);
                        fbuf[fi++] = '0' + (char)(e10 % 10);
                    }
                    fbuf[fi] = 0;
                } else {
                    /* %a/%A hex float — rare, stub it */
                    const char *stub = upper ? "0X0P+0" : "0x0p+0";
                    while (*stub) fbuf[fi++] = *stub++;
                    fbuf[fi] = 0;
                }
            }

            /* Output with width/alignment */
            if (left_align) {
                int len = fi;
                append_str(buf, n, &pos, fbuf);
                for (int i = len; i < width; i++)
                    append_char(buf, n, &pos, ' ');
            } else {
                append_padded(buf, n, &pos, fbuf, width, pad);
            }
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
    size_t map_len;
    struct malloc_block *next;
    uint32_t magic;
    uint32_t flags;
} __attribute__((aligned(16)));

struct malloc_aligned_header {
    uint64_t magic;
    void *base;
    size_t size;
} __attribute__((aligned(16)));

static struct malloc_block *free_list = NULL;

#define MALLOC_MAGIC 0x4D414C4Cu
#define MALLOC_ALIGNED_MAGIC 0x5441544552414C4Cu
#define MALLOC_FLAG_FREE 0x01u
#define MALLOC_FLAG_MMAP 0x02u
#define MALLOC_MMAP_THRESHOLD (64u * 1024u)
#define MALLOC_PAGE_SIZE 4096u

static size_t malloc_align_up(size_t value, size_t align) {
    size_t mask = align - 1;
    return (value + mask) & ~mask;
}

static int malloc_is_mmap_block(const struct malloc_block *block) {
    return block && block->magic == MALLOC_MAGIC &&
           (block->flags & MALLOC_FLAG_MMAP) != 0;
}

static int malloc_valid_alignment(size_t alignment) {
    return alignment >= sizeof(void*) &&
           (alignment & (alignment - 1)) == 0;
}

static struct malloc_aligned_header *malloc_aligned_header_from_ptr(const void *ptr) {
    struct malloc_aligned_header *header;
    struct malloc_block *block;

    if (!ptr) return NULL;
    header = (struct malloc_aligned_header *)ptr - 1;
    if (header->magic != MALLOC_ALIGNED_MAGIC || !header->base) return NULL;

    block = (struct malloc_block *)header->base - 1;
    if (block->magic != MALLOC_MAGIC) return NULL;
    return header;
}

static void *malloc_aligned_alloc_impl(size_t alignment, size_t size,
                                       int require_size_multiple) {
    size_t max_size_t = (size_t)-1;
    size_t request = size ? size : 1;
    void *base;
    uintptr_t aligned_addr;
    struct malloc_aligned_header *header;

    if (!malloc_valid_alignment(alignment)) {
        errno = EINVAL;
        return NULL;
    }
    if (require_size_multiple && (size % alignment) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment <= 16) return malloc(request);
    if (request > max_size_t - alignment - sizeof(*header)) {
        errno = ENOMEM;
        return NULL;
    }

    base = malloc(request + alignment - 1 + sizeof(*header));
    if (!base) {
        errno = ENOMEM;
        return NULL;
    }

    aligned_addr = ((uintptr_t)base + sizeof(*header) + alignment - 1) &
                   ~((uintptr_t)alignment - 1);
    header = (struct malloc_aligned_header *)aligned_addr - 1;
    header->magic = MALLOC_ALIGNED_MAGIC;
    header->base = base;
    header->size = size;
    return (void *)aligned_addr;
}

void *malloc(size_t size) {
    size_t max_size_t = (size_t)-1;
    if (size == 0) return NULL;
    if (size > max_size_t - 15) return NULL;
    size = malloc_align_up(size, 16); // 16-byte alignment
    if (size > max_size_t - sizeof(struct malloc_block)) return NULL;

    size_t total_size = size + sizeof(struct malloc_block);
    if (total_size >= MALLOC_MMAP_THRESHOLD) {
        size_t map_len = malloc_align_up(total_size, MALLOC_PAGE_SIZE);
        struct malloc_block *block = (struct malloc_block *)fry_mmap(
            0, map_len, FRY_PROT_READ | FRY_PROT_WRITE,
            FRY_MAP_PRIVATE | FRY_MAP_ANON);
        if (!FRY_IS_ERR(block)) {
            block->size = size;
            block->map_len = map_len;
            block->next = NULL;
            block->magic = MALLOC_MAGIC;
            block->flags = MALLOC_FLAG_MMAP;
            return (void *)(block + 1);
        }
    }

    struct malloc_block *prev = NULL;
    struct malloc_block *curr = free_list;
    while (curr) {
        if ((curr->flags & MALLOC_FLAG_FREE) != 0 && curr->size >= size) {
            curr->flags &= ~MALLOC_FLAG_FREE;
            return (void *)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }
    long res = fry_sbrk((intptr_t)total_size);
    if (res < 0) return NULL;
    struct malloc_block *block = (struct malloc_block *)res;
    block->size = size;
    block->map_len = 0;
    block->next = NULL;
    block->magic = MALLOC_MAGIC;
    block->flags = 0;
    if (prev) prev->next = block;
    else free_list = block;
    return (void *)(block + 1);
}

void free(void *ptr) {
    struct malloc_aligned_header *aligned;
    if (!ptr) return;
    aligned = malloc_aligned_header_from_ptr(ptr);
    if (aligned) {
        void *base = aligned->base;
        aligned->magic = 0;
        aligned->base = NULL;
        aligned->size = 0;
        free(base);
        return;
    }
    struct malloc_block *block = (struct malloc_block *)ptr - 1;
    if (block->magic != MALLOC_MAGIC) return;
    if (malloc_is_mmap_block(block)) {
        (void)fry_munmap(block, block->map_len);
        return;
    }
    block->flags |= MALLOC_FLAG_FREE;
}

void *calloc(size_t nmemb, size_t size) {
    size_t max_size_t = (size_t)-1;
    if (nmemb != 0 && size > max_size_t / nmemb) return NULL;
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    struct malloc_block *block = (struct malloc_block *)ptr - 1;
    if (block->magic != MALLOC_MAGIC) return NULL;
    if (block->size >= size) {
        block->size = size;
        return ptr;
    }
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

void *aligned_alloc(size_t alignment, size_t size) {
    return malloc_aligned_alloc_impl(alignment, size, 1);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    void *ptr;
    if (!memptr) return EINVAL;
    *memptr = NULL;

    if (!malloc_valid_alignment(alignment)) return EINVAL;
    ptr = malloc_aligned_alloc_impl(alignment, size, 0);
    if (!ptr) return (errno == EINVAL) ? EINVAL : ENOMEM;
    *memptr = ptr;
    return 0;
}

void *memalign(size_t alignment, size_t size) {
    return malloc_aligned_alloc_impl(alignment, size, 0);
}

void *valloc(size_t size) {
    return memalign(MALLOC_PAGE_SIZE, size);
}

void *pvalloc(size_t size) {
    size_t rounded = malloc_align_up(size ? size : 1, MALLOC_PAGE_SIZE);
    return memalign(MALLOC_PAGE_SIZE, rounded);
}

size_t malloc_usable_size(void *ptr) {
    struct malloc_aligned_header *aligned;
    struct malloc_block *block;

    if (!ptr) return 0;
    aligned = malloc_aligned_header_from_ptr(ptr);
    if (aligned) return aligned->size;

    block = (struct malloc_block *)ptr - 1;
    if (block->magic != MALLOC_MAGIC) return 0;
    return block->size;
}
