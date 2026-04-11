/*
 * string_ext.c — Extended string/memory functions for POSIX compatibility
 *
 * Phase 8: Functions needed by NSPR, NSS, and browser runtime.
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>

typedef struct {
    int __count;
    union {
        unsigned int __wch;
        char __wchb[4];
    } __value;
} mbstate_t;

typedef unsigned int wint_t;

#ifndef EILSEQ
#define EILSEQ 84
#endif

/* -----------------------------------------------------------------------
 * String search
 * ----------------------------------------------------------------------- */

char *strchr(const char *s, int c) {
    if (!s) return 0;
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    if (!s) return 0;
    const char *last = 0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    if (!s || !accept) return 0;
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
        s++;
    }
    return 0;
}

size_t strspn(const char *s, const char *accept) {
    if (!s || !accept) return 0;
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    if (!s || !reject) return 0;
    size_t count = 0;
    while (*s) {
        for (const char *r = reject; *r; r++) {
            if (*s == *r) return count;
        }
        count++;
        s++;
    }
    return count;
}

/* -----------------------------------------------------------------------
 * strtok — NOT thread-safe (uses static state, per POSIX spec)
 * ----------------------------------------------------------------------- */
static char *strtok_state;

char *strtok(char *str, const char *delim) {
    if (str) strtok_state = str;
    if (!strtok_state || !*strtok_state) return 0;
    /* skip leading delimiters */
    strtok_state += strspn(strtok_state, delim);
    if (!*strtok_state) return 0;
    char *start = strtok_state;
    strtok_state += strcspn(strtok_state, delim);
    if (*strtok_state) {
        *strtok_state = '\0';
        strtok_state++;
    }
    return start;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!saveptr) return 0;
    if (str) *saveptr = str;
    if (!*saveptr || !**saveptr) return 0;
    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr) return 0;
    char *start = *saveptr;
    *saveptr += strcspn(*saveptr, delim);
    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    }
    return start;
}

/* -----------------------------------------------------------------------
 * strdup / strndup
 * ----------------------------------------------------------------------- */

char *strdup(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

char *strndup(const char *s, size_t n) {
    if (!s) return 0;
    size_t len = strlen(s);
    if (len > n) len = n;
    char *d = (char *)malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

/* -----------------------------------------------------------------------
 * memchr
 * ----------------------------------------------------------------------- */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Number parsing: strtol, strtoul, strtoll, strtoull
 * ----------------------------------------------------------------------- */

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int digit_val(char c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;
    return (v < base) ? v : -1;
}

long strtol(const char *nptr, char **endptr, int base) {
    if (!nptr) { if (endptr) *endptr = (char *)nptr; return 0; }

    const char *s = nptr;
    while (is_space(*s)) s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    /* auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long result = 0;
    int any = 0;
    while (*s) {
        int d = digit_val(*s, base);
        if (d < 0) break;
        result = result * (unsigned long)base + (unsigned long)d;
        any = 1;
        s++;
    }

    if (endptr) *endptr = any ? (char *)s : (char *)nptr;
    return neg ? -(long)result : (long)result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    if (!nptr) { if (endptr) *endptr = (char *)nptr; return 0; }

    const char *s = nptr;
    while (is_space(*s)) s++;

    if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long result = 0;
    int any = 0;
    while (*s) {
        int d = digit_val(*s, base);
        if (d < 0) break;
        result = result * (unsigned long)base + (unsigned long)d;
        any = 1;
        s++;
    }

    if (endptr) *endptr = any ? (char *)s : (char *)nptr;
    return result;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtoul(nptr, endptr, base);
}

/* -----------------------------------------------------------------------
 * strtod — basic floating-point parser (integer + fractional, no exponent)
 * Sufficient for config file parsing. No IEEE edge-case handling.
 * ----------------------------------------------------------------------- */

double strtod(const char *nptr, char **endptr) {
    if (!nptr) { if (endptr) *endptr = (char *)nptr; return 0.0; }

    const char *s = nptr;
    while (is_space(*s)) s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    double result = 0.0;
    int any = 0;

    /* integer part */
    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        any = 1;
        s++;
    }

    /* fractional part */
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s - '0') * frac;
            frac *= 0.1;
            any = 1;
            s++;
        }
    }

    /* exponent (e/E) */
    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double mult = 1.0;
        for (int i = 0; i < exp; i++) mult *= 10.0;
        if (eneg) result /= mult;
        else result *= mult;
    }

    if (endptr) *endptr = any ? (char *)s : (char *)nptr;
    return neg ? -result : result;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

/* -----------------------------------------------------------------------
 * Error handling: strerror
 * ----------------------------------------------------------------------- */

char *strerror(int errnum) {
    switch (errnum) {
        case 0:     return "Success";
        case EPERM:  return "Operation not permitted";
        case ENOENT: return "No such file or directory";
        case ESRCH:  return "No such process";
        case EINTR:  return "Interrupted system call";
        case EIO:    return "I/O error";
        case ENXIO:  return "No such device or address";
        case E2BIG:  return "Argument list too long";
        case EBADF:  return "Bad file descriptor";
        case ECHILD: return "No child processes";
        case EAGAIN: return "Resource temporarily unavailable";
        case ENOMEM: return "Out of memory";
        case EACCES: return "Permission denied";
        case EFAULT: return "Bad address";
        case EEXIST: return "File exists";
        case ENOTDIR:return "Not a directory";
        case EISDIR: return "Is a directory";
        case EINVAL: return "Invalid argument";
        case ENFILE: return "Too many open files in system";
        case EMFILE: return "Too many open files";
        case ENOSPC: return "No space left on device";
        case EPIPE:  return "Broken pipe";
        case ESPIPE: return "Illegal seek";
        case ERANGE: return "Result too large";
        case ENOSYS: return "Function not implemented";
        case EBUSY:  return "Device or resource busy";
        case ETIMEDOUT: return "Connection timed out";
        case ECONNREFUSED: return "Connection refused";
        case ECONNRESET:   return "Connection reset by peer";
        case ENOTCONN:     return "Not connected";
        case EADDRINUSE:   return "Address already in use";
        case EINPROGRESS:  return "Operation in progress";
        default:     return "Unknown error";
    }
}

void perror(const char *s) {
    /* No thread-local errno yet; just print the prefix */
    if (s && *s) {
        printf("%s: (errno unavailable)\n", s);
    }
}

/* -----------------------------------------------------------------------
 * Character classification (ctype.h equivalents)
 * ----------------------------------------------------------------------- */

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c <= 0x7E; }
int isgraph(int c) { return c > 0x20 && c <= 0x7E; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* -----------------------------------------------------------------------
 * abs / labs / llabs
 * ----------------------------------------------------------------------- */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

/* -----------------------------------------------------------------------
 * libgen — basename / dirname
 * ----------------------------------------------------------------------- */

char *basename(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    char *end;
    char *start;

    if (!path || !*path) return dot;

    end = path + strlen(path);
    while (end > path && end[-1] == '/') end--;
    if (end == path) return slash;

    *end = '\0';
    start = end;
    while (start > path && start[-1] != '/') start--;
    return start;
}

char *dirname(char *path) {
    static char dot[] = ".";
    static char slash[] = "/";
    char *end;

    if (!path || !*path) return dot;

    end = path + strlen(path);
    while (end > path && end[-1] == '/') end--;
    if (end == path) return slash;

    while (end > path && end[-1] != '/') end--;
    if (end == path) {
        path[0] = '.';
        path[1] = '\0';
        return path;
    }

    while (end > path && end[-1] == '/') end--;
    if (end == path) return slash;

    *end = '\0';
    return path;
}

/* -----------------------------------------------------------------------
 * wchar / multibyte conversion
 * ----------------------------------------------------------------------- */

size_t wcslen(const wchar_t *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len] != 0) len++;
    return len;
}

static size_t utf8_decode_one(const char *s, size_t n, uint32_t *out) {
    unsigned char c0;

    if (!s || n == 0) return (size_t)-2;
    c0 = (unsigned char)s[0];

    if (c0 < 0x80) {
        *out = c0;
        return 1;
    }

    if ((c0 & 0xE0u) == 0xC0u) {
        if (n < 2) return (size_t)-2;
        if (((unsigned char)s[1] & 0xC0u) != 0x80u) return (size_t)-1;
        *out = ((uint32_t)(c0 & 0x1Fu) << 6) |
               (uint32_t)((unsigned char)s[1] & 0x3Fu);
        if (*out < 0x80u) return (size_t)-1;
        return 2;
    }

    if ((c0 & 0xF0u) == 0xE0u) {
        if (n < 3) return (size_t)-2;
        if ((((unsigned char)s[1] & 0xC0u) != 0x80u) ||
            (((unsigned char)s[2] & 0xC0u) != 0x80u)) {
            return (size_t)-1;
        }
        *out = ((uint32_t)(c0 & 0x0Fu) << 12) |
               ((uint32_t)((unsigned char)s[1] & 0x3Fu) << 6) |
               (uint32_t)((unsigned char)s[2] & 0x3Fu);
        if (*out < 0x800u) return (size_t)-1;
        if (*out >= 0xD800u && *out <= 0xDFFFu) return (size_t)-1;
        return 3;
    }

    if ((c0 & 0xF8u) == 0xF0u) {
        if (n < 4) return (size_t)-2;
        if ((((unsigned char)s[1] & 0xC0u) != 0x80u) ||
            (((unsigned char)s[2] & 0xC0u) != 0x80u) ||
            (((unsigned char)s[3] & 0xC0u) != 0x80u)) {
            return (size_t)-1;
        }
        *out = ((uint32_t)(c0 & 0x07u) << 18) |
               ((uint32_t)((unsigned char)s[1] & 0x3Fu) << 12) |
               ((uint32_t)((unsigned char)s[2] & 0x3Fu) << 6) |
               (uint32_t)((unsigned char)s[3] & 0x3Fu);
        if (*out < 0x10000u || *out > 0x10FFFFu) return (size_t)-1;
        return 4;
    }

    return (size_t)-1;
}

static size_t utf8_encode_one(uint32_t wc, char *out) {
    if (wc <= 0x7Fu) {
        if (out) out[0] = (char)wc;
        return 1;
    }
    if (wc <= 0x7FFu) {
        if (out) {
            out[0] = (char)(0xC0u | (wc >> 6));
            out[1] = (char)(0x80u | (wc & 0x3Fu));
        }
        return 2;
    }
    if (wc <= 0xFFFFu) {
        if (wc >= 0xD800u && wc <= 0xDFFFu) return 0;
        if (out) {
            out[0] = (char)(0xE0u | (wc >> 12));
            out[1] = (char)(0x80u | ((wc >> 6) & 0x3Fu));
            out[2] = (char)(0x80u | (wc & 0x3Fu));
        }
        return 3;
    }
    if (wc <= 0x10FFFFu) {
        if (out) {
            out[0] = (char)(0xF0u | (wc >> 18));
            out[1] = (char)(0x80u | ((wc >> 12) & 0x3Fu));
            out[2] = (char)(0x80u | ((wc >> 6) & 0x3Fu));
            out[3] = (char)(0x80u | (wc & 0x3Fu));
        }
        return 4;
    }
    return 0;
}

int mbsinit(const mbstate_t *ps) {
    return !ps || ps->__count == 0;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    size_t rc = mbrtowc(0, s, n, ps);
    return rc;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    uint32_t codepoint = 0;
    size_t rc;

    if (ps) ps->__count = 0;
    if (!s) return 0;

    rc = utf8_decode_one(s, n, &codepoint);
    if (rc == (size_t)-1) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (rc == (size_t)-2) return (size_t)-2;
    if (pwc) *pwc = (wchar_t)codepoint;
    return codepoint == 0 ? 0 : rc;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
    size_t rc;

    if (ps) ps->__count = 0;
    if (!s) return 1;

    rc = utf8_encode_one((uint32_t)wc, s);
    if (rc == 0) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return rc;
}

size_t mbsrtowcs(wchar_t *dest, const char **src, size_t len, mbstate_t *ps) {
    const char *s;
    size_t out_count = 0;

    if (!src || !*src) return 0;
    if (ps) ps->__count = 0;

    s = *src;
    while (*s) {
        wchar_t wc;
        size_t rc;

        rc = mbrtowc(&wc, s, strlen(s), ps);
        if (rc == (size_t)-1 || rc == (size_t)-2) return (size_t)-1;

        if (dest) {
            if (out_count >= len) {
                *src = s;
                return out_count;
            }
            dest[out_count] = wc;
        }

        out_count++;
        s += rc;
    }

    if (dest && out_count < len) dest[out_count] = 0;
    *src = 0;
    return out_count;
}

size_t wcsrtombs(char *dest, const wchar_t **src, size_t len, mbstate_t *ps) {
    const wchar_t *s;
    size_t out_count = 0;

    if (!src || !*src) return 0;
    if (ps) ps->__count = 0;

    s = *src;
    while (*s) {
        char tmp[4];
        size_t rc = utf8_encode_one((uint32_t)*s, tmp);

        if (rc == 0) {
            errno = EILSEQ;
            return (size_t)-1;
        }

        if (dest) {
            if (out_count + rc > len) {
                *src = s;
                return out_count;
            }
            memcpy(dest + out_count, tmp, rc);
        }

        out_count += rc;
        s++;
    }

    if (dest && out_count < len) dest[out_count] = '\0';
    *src = 0;
    return out_count;
}

/* -----------------------------------------------------------------------
 * wctype / wide numeric conversion
 * ----------------------------------------------------------------------- */

int iswalpha(wint_t wc) { return (wc <= 0x7Fu) ? isalpha((int)wc) : 0; }
int iswdigit(wint_t wc) { return (wc <= 0x7Fu) ? isdigit((int)wc) : 0; }
int iswalnum(wint_t wc) { return (wc <= 0x7Fu) ? isalnum((int)wc) : 0; }
int iswspace(wint_t wc) { return (wc <= 0x7Fu) ? isspace((int)wc) : 0; }
int iswupper(wint_t wc) { return (wc <= 0x7Fu) ? isupper((int)wc) : 0; }
int iswlower(wint_t wc) { return (wc <= 0x7Fu) ? islower((int)wc) : 0; }
int iswprint(wint_t wc) { return (wc <= 0x7Fu) ? isprint((int)wc) : 0; }
int iswgraph(wint_t wc) { return (wc <= 0x7Fu) ? isgraph((int)wc) : 0; }
int iswcntrl(wint_t wc) { return (wc <= 0x7Fu) ? iscntrl((int)wc) : 0; }
int iswpunct(wint_t wc) { return (wc <= 0x7Fu) ? ispunct((int)wc) : 0; }
int iswxdigit(wint_t wc) { return (wc <= 0x7Fu) ? isxdigit((int)wc) : 0; }
int iswblank(wint_t wc) { return wc == (wint_t)' ' || wc == (wint_t)'\t'; }
wint_t towupper(wint_t wc) { return (wc <= 0x7Fu) ? (wint_t)toupper((int)wc) : wc; }
wint_t towlower(wint_t wc) { return (wc <= 0x7Fu) ? (wint_t)tolower((int)wc) : wc; }

typedef unsigned long wctype_t;
typedef const int *wctrans_t;

enum {
    WCTYPE_ALNUM = 1,
    WCTYPE_ALPHA,
    WCTYPE_BLANK,
    WCTYPE_CNTRL,
    WCTYPE_DIGIT,
    WCTYPE_GRAPH,
    WCTYPE_LOWER,
    WCTYPE_PRINT,
    WCTYPE_PUNCT,
    WCTYPE_SPACE,
    WCTYPE_UPPER,
    WCTYPE_XDIGIT
};

static const int g_wctrans_tolower = 1;
static const int g_wctrans_toupper = 2;

wctype_t wctype(const char *property) {
    if (!property) return 0;
    if (strcmp(property, "alnum") == 0) return WCTYPE_ALNUM;
    if (strcmp(property, "alpha") == 0) return WCTYPE_ALPHA;
    if (strcmp(property, "blank") == 0) return WCTYPE_BLANK;
    if (strcmp(property, "cntrl") == 0) return WCTYPE_CNTRL;
    if (strcmp(property, "digit") == 0) return WCTYPE_DIGIT;
    if (strcmp(property, "graph") == 0) return WCTYPE_GRAPH;
    if (strcmp(property, "lower") == 0) return WCTYPE_LOWER;
    if (strcmp(property, "print") == 0) return WCTYPE_PRINT;
    if (strcmp(property, "punct") == 0) return WCTYPE_PUNCT;
    if (strcmp(property, "space") == 0) return WCTYPE_SPACE;
    if (strcmp(property, "upper") == 0) return WCTYPE_UPPER;
    if (strcmp(property, "xdigit") == 0) return WCTYPE_XDIGIT;
    return 0;
}

int iswctype(wint_t wc, wctype_t desc) {
    switch (desc) {
        case WCTYPE_ALNUM: return iswalnum(wc);
        case WCTYPE_ALPHA: return iswalpha(wc);
        case WCTYPE_BLANK: return iswblank(wc);
        case WCTYPE_CNTRL: return iswcntrl(wc);
        case WCTYPE_DIGIT: return iswdigit(wc);
        case WCTYPE_GRAPH: return iswgraph(wc);
        case WCTYPE_LOWER: return iswlower(wc);
        case WCTYPE_PRINT: return iswprint(wc);
        case WCTYPE_PUNCT: return iswpunct(wc);
        case WCTYPE_SPACE: return iswspace(wc);
        case WCTYPE_UPPER: return iswupper(wc);
        case WCTYPE_XDIGIT: return iswxdigit(wc);
        default: return 0;
    }
}

wctrans_t wctrans(const char *property) {
    if (!property) return 0;
    if (strcmp(property, "tolower") == 0) return &g_wctrans_tolower;
    if (strcmp(property, "toupper") == 0) return &g_wctrans_toupper;
    return 0;
}

wint_t towctrans(wint_t wc, wctrans_t desc) {
    if (!desc) return wc;
    if (*desc == g_wctrans_tolower) return towlower(wc);
    if (*desc == g_wctrans_toupper) return towupper(wc);
    return wc;
}

static char *wide_to_ascii_dup(const wchar_t *src, size_t *out_len) {
    size_t len;
    char *buf;

    if (!src) return 0;

    len = wcslen(src);
    buf = (char *)malloc(len + 1);
    if (!buf) return 0;

    for (size_t i = 0; i < len; i++) {
        if ((unsigned int)src[i] > 0x7Fu) {
            buf[i] = '\0';
            if (out_len) *out_len = i;
            return buf;
        }
        buf[i] = (char)src[i];
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static wchar_t *wide_end_from_ascii(wchar_t *base, const char *ascii, const char *ascii_end) {
    if (!base || !ascii || !ascii_end) return base;
    return base + (ascii_end - ascii);
}

double wcstod(const wchar_t *nptr, wchar_t **endptr) {
    char *ascii;
    char *ascii_end = 0;
    double result;

    ascii = wide_to_ascii_dup(nptr, 0);
    if (!ascii) {
        if (endptr) *endptr = (wchar_t *)nptr;
        return 0.0;
    }

    result = strtod(ascii, &ascii_end);
    if (endptr) *endptr = wide_end_from_ascii((wchar_t *)nptr, ascii, ascii_end);
    free(ascii);
    return result;
}

float wcstof(const wchar_t *nptr, wchar_t **endptr) {
    return (float)wcstod(nptr, endptr);
}

long double wcstold(const wchar_t *nptr, wchar_t **endptr) {
    return (long double)wcstod(nptr, endptr);
}

long wcstol(const wchar_t *nptr, wchar_t **endptr, int base) {
    char *ascii;
    char *ascii_end = 0;
    long result;

    ascii = wide_to_ascii_dup(nptr, 0);
    if (!ascii) {
        if (endptr) *endptr = (wchar_t *)nptr;
        return 0;
    }

    result = strtol(ascii, &ascii_end, base);
    if (endptr) *endptr = wide_end_from_ascii((wchar_t *)nptr, ascii, ascii_end);
    free(ascii);
    return result;
}

unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base) {
    char *ascii;
    char *ascii_end = 0;
    unsigned long result;

    ascii = wide_to_ascii_dup(nptr, 0);
    if (!ascii) {
        if (endptr) *endptr = (wchar_t *)nptr;
        return 0;
    }

    result = strtoul(ascii, &ascii_end, base);
    if (endptr) *endptr = wide_end_from_ascii((wchar_t *)nptr, ascii, ascii_end);
    free(ascii);
    return result;
}

long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base) {
    return (long long)wcstol(nptr, endptr, base);
}

unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base) {
    return (unsigned long long)wcstoul(nptr, endptr, base);
}
