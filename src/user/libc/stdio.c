/*
 * stdio.c — FILE stream implementation for POSIX compatibility
 *
 * Phase 8: Provides fopen/fclose/fread/fwrite/fgets/fputs/fprintf/fseek/ftell
 * and related FILE-based I/O. Built on top of fry_open/fry_read/fry_write/
 * fry_close/fry_lseek syscall wrappers.
 *
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * FILE structure and static pool
 * ----------------------------------------------------------------------- */

#define STDIO_BUFSZ  4096
#define STDIO_MAX_FILES 32

#define _FILE_READ   0x01
#define _FILE_WRITE  0x02
#define _FILE_APPEND 0x04
#define _FILE_EOF    0x08
#define _FILE_ERR    0x10
#define _FILE_OPEN   0x20
#define _FILE_UNBUF  0x40

struct _FILE {
    int      fd;
    uint32_t flags;
    uint8_t  buf[STDIO_BUFSZ];
    size_t   buf_pos;    /* current read position in buffer */
    size_t   buf_len;    /* valid bytes in buffer */
    size_t   wbuf_pos;   /* bytes in write buffer not yet flushed */
};

static struct _FILE g_file_pool[STDIO_MAX_FILES];

/* Pre-opened stdin/stdout/stderr */
static struct _FILE g_stdin  = { .fd = 0, .flags = _FILE_READ  | _FILE_OPEN | _FILE_UNBUF };
static struct _FILE g_stdout = { .fd = 1, .flags = _FILE_WRITE | _FILE_OPEN };
static struct _FILE g_stderr = { .fd = 2, .flags = _FILE_WRITE | _FILE_OPEN | _FILE_UNBUF };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

static struct _FILE *alloc_file(void) {
    for (int i = 0; i < STDIO_MAX_FILES; i++) {
        if (!(g_file_pool[i].flags & _FILE_OPEN)) {
            memset(&g_file_pool[i], 0, sizeof(struct _FILE));
            return &g_file_pool[i];
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Mode parsing
 * ----------------------------------------------------------------------- */

static int parse_mode(const char *mode, int *fry_flags, uint32_t *file_flags) {
    if (!mode || !mode[0]) return -1;

    *fry_flags = 0;
    *file_flags = 0;

    switch (mode[0]) {
    case 'r':
        *file_flags = _FILE_READ;
        *fry_flags = O_RDONLY;
        break;
    case 'w':
        *file_flags = _FILE_WRITE;
        *fry_flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case 'a':
        *file_flags = _FILE_WRITE | _FILE_APPEND;
        *fry_flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    default:
        return -1;
    }

    /* Check for '+' (read+write) and 'b' (binary, ignored) */
    for (int i = 1; mode[i]; i++) {
        if (mode[i] == '+') {
            *file_flags |= _FILE_READ | _FILE_WRITE;
            *fry_flags = O_RDWR;
            if (mode[0] == 'w') *fry_flags |= O_CREAT | O_TRUNC;
            else if (mode[0] == 'a') *fry_flags |= O_CREAT | O_APPEND;
        }
        /* 'b' for binary mode is accepted but a no-op */
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * fopen / fclose / fflush
 * ----------------------------------------------------------------------- */

FILE *fopen(const char *path, const char *mode) {
    int fry_flags;
    uint32_t file_flags;
    struct _FILE *f;
    long fd;

    if (!path || !mode) return 0;
    if (parse_mode(mode, &fry_flags, &file_flags) < 0) return 0;

    /* For write/append, ensure file exists first if creating */
    if (fry_flags & O_CREAT) {
        /* Try to create; ignore EEXIST */
        long rc = fry_create(path, 0);
        (void)rc;
    }

    fd = fry_open(path, fry_flags);
    if (fd < 0) return 0;

    f = alloc_file();
    if (!f) {
        fry_close((int)fd);
        return 0;
    }

    f->fd = (int)fd;
    f->flags = file_flags | _FILE_OPEN;
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    int fry_flags;
    uint32_t file_flags;
    struct _FILE *f;

    if (fd < 0 || !mode) return 0;
    if (parse_mode(mode, &fry_flags, &file_flags) < 0) return 0;
    (void)fry_flags; /* fd is already open */

    f = alloc_file();
    if (!f) return 0;

    f->fd = fd;
    f->flags = file_flags | _FILE_OPEN;
    return f;
}

int fflush(FILE *stream) {
    struct _FILE *f = stream;
    if (!f) {
        /* Flush all open write streams */
        fflush(stdout);
        fflush(stderr);
        for (int i = 0; i < STDIO_MAX_FILES; i++) {
            if ((g_file_pool[i].flags & (_FILE_OPEN | _FILE_WRITE)) == (_FILE_OPEN | _FILE_WRITE)) {
                fflush(&g_file_pool[i]);
            }
        }
        return 0;
    }

    if (!(f->flags & _FILE_OPEN)) return -1;
    if (f->wbuf_pos > 0 && (f->flags & _FILE_WRITE)) {
        long written = fry_write(f->fd, f->buf, f->wbuf_pos);
        if (written < 0) {
            f->flags |= _FILE_ERR;
            return -1;
        }
        f->wbuf_pos = 0;
    }
    return 0;
}

int fclose(FILE *stream) {
    struct _FILE *f = stream;
    if (!f || !(f->flags & _FILE_OPEN)) return -1;

    fflush(stream);
    long rc = fry_close(f->fd);
    f->flags = 0;
    f->fd = -1;
    return (rc < 0) ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * fread / fwrite
 * ----------------------------------------------------------------------- */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    struct _FILE *f = stream;
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    if (!(f->flags & (_FILE_READ | _FILE_OPEN))) return 0;

    size_t total = size * nmemb;
    uint8_t *dst = (uint8_t *)ptr;
    size_t got = 0;

    /* Drain buffered data first */
    while (got < total && f->buf_pos < f->buf_len) {
        dst[got++] = f->buf[f->buf_pos++];
    }

    /* Read remaining directly if large */
    while (got < total) {
        size_t remain = total - got;
        if (remain >= STDIO_BUFSZ) {
            long n = fry_read(f->fd, dst + got, remain);
            if (n <= 0) {
                if (n == 0) f->flags |= _FILE_EOF;
                else f->flags |= _FILE_ERR;
                break;
            }
            got += (size_t)n;
        } else {
            /* Refill buffer */
            long n = fry_read(f->fd, f->buf, STDIO_BUFSZ);
            if (n <= 0) {
                if (n == 0) f->flags |= _FILE_EOF;
                else f->flags |= _FILE_ERR;
                break;
            }
            f->buf_pos = 0;
            f->buf_len = (size_t)n;
            while (got < total && f->buf_pos < f->buf_len) {
                dst[got++] = f->buf[f->buf_pos++];
            }
        }
    }

    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    struct _FILE *f = stream;
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    if (!(f->flags & (_FILE_WRITE | _FILE_OPEN))) return 0;

    size_t total = size * nmemb;
    const uint8_t *src = (const uint8_t *)ptr;
    size_t written = 0;

    if (f->flags & _FILE_UNBUF) {
        /* Unbuffered: write directly */
        while (written < total) {
            long n = fry_write(f->fd, src + written, total - written);
            if (n <= 0) {
                f->flags |= _FILE_ERR;
                break;
            }
            written += (size_t)n;
        }
    } else {
        /* Buffered: accumulate in write buffer */
        while (written < total) {
            size_t space = STDIO_BUFSZ - f->wbuf_pos;
            size_t chunk = total - written;
            if (chunk > space) chunk = space;
            memcpy(f->buf + f->wbuf_pos, src + written, chunk);
            f->wbuf_pos += chunk;
            written += chunk;
            if (f->wbuf_pos >= STDIO_BUFSZ) {
                if (fflush(stream) != 0) break;
            }
        }
    }

    return written / size;
}

/* -----------------------------------------------------------------------
 * fgets / fputs / fgetc / fputc / ungetc
 * ----------------------------------------------------------------------- */

int fgetc(FILE *stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1) return -1;
    return (int)c;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, stream) != 1) return -1;
    return (int)ch;
}

int ungetc(int c, FILE *stream) {
    struct _FILE *f = stream;
    if (!f || c == -1) return -1;
    if (f->buf_pos > 0) {
        f->buf_pos--;
        f->buf[f->buf_pos] = (uint8_t)c;
    } else if (f->buf_len < STDIO_BUFSZ) {
        /* Shift buffer right by 1 */
        memmove(f->buf + 1, f->buf, f->buf_len);
        f->buf[0] = (uint8_t)c;
        f->buf_len++;
    } else {
        return -1;
    }
    f->flags &= ~_FILE_EOF;
    return c;
}

char *fgets(char *s, int n, FILE *stream) {
    if (!s || n <= 0 || !stream) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(stream);
        if (c == -1) {
            if (i == 0) return 0;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return -1;
    size_t len = strlen(s);
    return (fwrite(s, 1, len, stream) == len) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * fprintf / vfprintf
 * ----------------------------------------------------------------------- */

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char buf[2048];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len < 0) return len;
    size_t out = ((size_t)len < sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;
    size_t written = fwrite(buf, 1, out, stream);
    return (int)written;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
    va_end(ap);
    return ret;
}

/* -----------------------------------------------------------------------
 * fseek / ftell / rewind / fsetpos / fgetpos
 * ----------------------------------------------------------------------- */

int fseek(FILE *stream, long offset, int whence) {
    struct _FILE *f = stream;
    if (!f || !(f->flags & _FILE_OPEN)) return -1;

    /* Flush write buffer before seeking */
    if (f->flags & _FILE_WRITE) fflush(stream);

    /* Invalidate read buffer */
    f->buf_pos = 0;
    f->buf_len = 0;
    f->flags &= ~_FILE_EOF;

    long rc = fry_lseek(f->fd, (int64_t)offset, whence);
    return (rc < 0) ? -1 : 0;
}

long ftell(FILE *stream) {
    struct _FILE *f = stream;
    if (!f || !(f->flags & _FILE_OPEN)) return -1;

    long pos = fry_lseek(f->fd, 0, 1 /* SEEK_CUR */);
    if (pos < 0) return -1;

    /* Adjust for buffered but unconsumed read data */
    if (f->buf_len > f->buf_pos) {
        pos -= (long)(f->buf_len - f->buf_pos);
    }
    /* Adjust for buffered but unflushed write data */
    if (f->wbuf_pos > 0) {
        pos += (long)f->wbuf_pos;
    }

    return pos;
}

void rewind(FILE *stream) {
    fseek(stream, 0, 0 /* SEEK_SET */);
    if (stream) {
        struct _FILE *f = stream;
        f->flags &= ~(_FILE_EOF | _FILE_ERR);
    }
}

/* -----------------------------------------------------------------------
 * feof / ferror / clearerr / fileno
 * ----------------------------------------------------------------------- */

int feof(FILE *stream) {
    struct _FILE *f = stream;
    return (f && (f->flags & _FILE_EOF)) ? 1 : 0;
}

int ferror(FILE *stream) {
    struct _FILE *f = stream;
    return (f && (f->flags & _FILE_ERR)) ? 1 : 0;
}

void clearerr(FILE *stream) {
    struct _FILE *f = stream;
    if (f) f->flags &= ~(_FILE_EOF | _FILE_ERR);
}

int fileno(FILE *stream) {
    struct _FILE *f = stream;
    return f ? f->fd : -1;
}

/* -----------------------------------------------------------------------
 * setvbuf / setbuf
 * ----------------------------------------------------------------------- */

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    struct _FILE *f = stream;
    (void)buf; (void)size; /* We use internal buffer only */
    if (!f) return -1;
    if (mode == 2 /* _IONBF */) {
        f->flags |= _FILE_UNBUF;
    } else {
        f->flags &= ~_FILE_UNBUF;
    }
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    setvbuf(stream, buf, buf ? 0 /* _IOFBF */ : 2 /* _IONBF */, STDIO_BUFSZ);
}

/* -----------------------------------------------------------------------
 * remove / rename (stdio-level wrappers)
 * ----------------------------------------------------------------------- */

int remove_file(const char *path) {
    return (fry_unlink(path) < 0) ? -1 : 0;
}

int rename_file(const char *oldpath, const char *newpath) {
    return (fry_rename(oldpath, newpath) < 0) ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * sscanf — minimal subset (%d, %u, %x, %s, %c, %ld, %lu, %lx)
 * ----------------------------------------------------------------------- */

int vsscanf(const char *str, const char *fmt, va_list ap) {
    if (!str || !fmt) return -1;
    int count = 0;
    const char *s = str;

    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++;
            fmt++;
            continue;
        }
        fmt++; /* skip '%' */

        /* Optional '*' for suppression */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }

        if (*fmt == 'd' || *fmt == 'i') {
            while (*s == ' ' || *s == '\t') s++;
            char *end = 0;
            long val = strtol(s, &end, 0);
            if (end == s) break;
            if (!suppress) {
                if (is_long >= 2) *va_arg(ap, long long *) = (long long)val;
                else if (is_long) *va_arg(ap, long *) = val;
                else *va_arg(ap, int *) = (int)val;
                count++;
            }
            s = end;
        } else if (*fmt == 'u') {
            while (*s == ' ' || *s == '\t') s++;
            char *end = 0;
            unsigned long val = strtoul(s, &end, 10);
            if (end == s) break;
            if (!suppress) {
                if (is_long >= 2) *va_arg(ap, unsigned long long *) = (unsigned long long)val;
                else if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                count++;
            }
            s = end;
        } else if (*fmt == 'x' || *fmt == 'X') {
            while (*s == ' ' || *s == '\t') s++;
            char *end = 0;
            unsigned long val = strtoul(s, &end, 16);
            if (end == s) break;
            if (!suppress) {
                if (is_long >= 2) *va_arg(ap, unsigned long long *) = (unsigned long long)val;
                else if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                count++;
            }
            s = end;
        } else if (*fmt == 's') {
            while (*s == ' ' || *s == '\t') s++;
            if (!*s) break;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int n = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
                if (width > 0 && n >= width) break;
                if (out) out[n] = *s;
                n++;
                s++;
            }
            if (out) out[n] = '\0';
            if (!suppress) count++;
        } else if (*fmt == 'c') {
            if (!*s) break;
            if (!suppress) {
                *va_arg(ap, char *) = *s;
                count++;
            }
            s++;
        } else if (*fmt == 'n') {
            if (!suppress) *va_arg(ap, int *) = (int)(s - str);
        } else if (*fmt == '%') {
            if (*s != '%') break;
            s++;
        }

        fmt++;
    }

    return count;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsscanf(str, fmt, ap);
    va_end(ap);
    return ret;
}

int fscanf(FILE *stream, const char *fmt, ...) {
    /* Read a line, then sscanf it */
    char buf[2048];
    if (!fgets(buf, (int)sizeof(buf), stream)) return -1;
    va_list ap;
    va_start(ap, fmt);
    int ret = vsscanf(buf, fmt, ap);
    va_end(ap);
    return ret;
}
