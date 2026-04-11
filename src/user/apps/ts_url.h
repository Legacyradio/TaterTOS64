/*
 * ts_url.h — TaterSurf URL parser
 *
 * Header-only. Parses URLs into components, resolves relative URLs,
 * formats URLs back to strings.
 *
 * Handles:
 *   http://host:port/path?query#fragment
 *   https://host/path
 *   //host/path  (protocol-relative)
 *   /path        (host-relative)
 *   path         (path-relative)
 *   bare hostname (assumes http://)
 */

#ifndef TS_URL_H
#define TS_URL_H

#include <stddef.h>
#include <stdint.h>

/* ----- Data structures ----- */

struct ts_url {
    char scheme[16];       /* "http" or "https", lowercase */
    char host[256];        /* hostname (no port) */
    uint16_t port;         /* 0 = use default (80 for http, 443 for https) */
    char path[1024];       /* path including leading /, empty = "/" */
    char query[512];       /* query string after ?, without the ? */
    char fragment[128];    /* fragment after #, without the # */
};

/* ----- Helper declarations (static, internal) ----- */

static int ts__is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ts__is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int ts__is_scheme_char(char c) {
    return ts__is_alpha(c) || ts__is_digit(c) || c == '+' || c == '-' || c == '.';
}

static char ts__tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static size_t ts__strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void ts__strncpy_lower(char *dst, const char *src, size_t n, size_t max) {
    size_t i;
    if (n >= max) n = max - 1;
    for (i = 0; i < n; i++)
        dst[i] = ts__tolower(src[i]);
    dst[i] = '\0';
}

static void ts__strncpy_safe(char *dst, const char *src, size_t n, size_t max) {
    size_t i;
    if (n >= max) n = max - 1;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int ts__strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int ts__strncasecmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = ts__tolower(a[i]);
        char cb = ts__tolower(b[i]);
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

/* ----- URL Parsing ----- */

/*
 * ts_url_parse — parse a raw URL string into components.
 *
 * Returns 0 on success, -1 on malformed URL.
 *
 * Accepts:
 *   "http://example.com/path?q=1#top"
 *   "https://example.com:8443/api"
 *   "//example.com/path"         (protocol-relative, defaults to http)
 *   "/path/to/page"              (host-relative)
 *   "example.com"                (bare hostname, assumes http)
 *   "example.com:8080/path"      (bare hostname with port)
 */
static int ts_url_parse(const char *raw, struct ts_url *out) {
    const char *p = raw;
    const char *end;
    size_t len;

    if (!raw || !out) return -1;

    /* Zero-initialize output */
    {
        char *d = (char *)out;
        size_t i;
        for (i = 0; i < sizeof(*out); i++) d[i] = 0;
    }

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    /* Remove trailing whitespace */
    len = ts__strlen(p);
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t' ||
                        p[len-1] == '\n' || p[len-1] == '\r'))
        len--;

    if (len == 0) return -1;

    end = p + len;

    /* ---- Detect scheme ---- */
    /* Look for "scheme://" pattern */
    {
        const char *colon = p;
        while (colon < end && ts__is_scheme_char(*colon)) colon++;
        if (colon < end - 2 && colon[0] == ':' && colon[1] == '/' && colon[2] == '/') {
            /* Found scheme */
            ts__strncpy_lower(out->scheme, p, (size_t)(colon - p), sizeof(out->scheme));
            p = colon + 3; /* skip "://" */
        } else if (p[0] == '/' && p[1] == '/') {
            /* Protocol-relative "//host/path" */
            out->scheme[0] = 'h'; out->scheme[1] = 't'; out->scheme[2] = 't';
            out->scheme[3] = 'p'; out->scheme[4] = '\0';
            p += 2; /* skip "//" */
        } else if (p[0] == '/') {
            /* Host-relative "/path" — no scheme, no host */
            out->scheme[0] = '\0';
            goto parse_path;
        } else {
            /* Could be bare hostname or relative path */
            /* Heuristic: if it contains a dot before any / and no spaces,
               treat as hostname. Otherwise treat as relative path. */
            const char *dot = p;
            const char *slash = p;
            int has_dot = 0;
            while (dot < end && *dot != '/') {
                if (*dot == '.') has_dot = 1;
                dot++;
            }
            slash = dot; /* first / or end */
            if (has_dot) {
                /* Bare hostname: "example.com" or "example.com:8080/path" */
                out->scheme[0] = 'h'; out->scheme[1] = 't'; out->scheme[2] = 't';
                out->scheme[3] = 'p'; out->scheme[4] = '\0';
                /* Fall through to host parsing */
            } else {
                /* Relative path */
                (void)slash;
                out->scheme[0] = '\0';
                goto parse_path;
            }
        }
    }

    /* ---- Parse host and optional port ---- */
    {
        const char *host_start = p;
        const char *host_end;
        const char *port_start = NULL;

        /* Find end of authority (host:port) — delimited by /, ?, #, or end */
        host_end = p;
        while (host_end < end && *host_end != '/' && *host_end != '?' && *host_end != '#')
            host_end++;

        /* Check for port separator (last ':' in authority) */
        {
            const char *c = host_end - 1;
            while (c > host_start) {
                if (*c == ':') {
                    /* Verify everything after ':' is digits */
                    const char *d = c + 1;
                    int all_digits = 1;
                    while (d < host_end) {
                        if (!ts__is_digit(*d)) { all_digits = 0; break; }
                        d++;
                    }
                    if (all_digits && d > c + 1) {
                        port_start = c + 1;
                        host_end = c; /* host ends before ':' */
                    }
                    break;
                }
                c--;
            }
        }

        /* Copy host (lowercase) */
        ts__strncpy_lower(out->host, host_start, (size_t)(host_end - host_start),
                          sizeof(out->host));

        /* Parse port */
        if (port_start) {
            uint32_t port_val = 0;
            const char *d = port_start;
            while (d < end && ts__is_digit(*d) && *d != '/' && *d != '?' && *d != '#') {
                port_val = port_val * 10 + (uint32_t)(*d - '0');
                d++;
            }
            if (port_val > 65535) port_val = 0;
            out->port = (uint16_t)port_val;
            p = d;
        } else {
            p = host_end + (size_t)(host_end - host_start); /* skip past authority */
            /* Recalculate p — point to first char after authority */
            p = host_start;
            while (p < end && *p != '/' && *p != '?' && *p != '#') p++;
        }
    }

parse_path:
    /* ---- Parse path ---- */
    {
        const char *path_start = p;
        const char *path_end = p;
        while (path_end < end && *path_end != '?' && *path_end != '#')
            path_end++;
        if (path_end > path_start) {
            ts__strncpy_safe(out->path, path_start, (size_t)(path_end - path_start),
                             sizeof(out->path));
        }
        p = path_end;
    }

    /* Default path to "/" if we have a host but no path */
    if (out->host[0] && !out->path[0]) {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    /* ---- Parse query ---- */
    if (p < end && *p == '?') {
        p++; /* skip '?' */
        {
            const char *q_start = p;
            const char *q_end = p;
            while (q_end < end && *q_end != '#') q_end++;
            ts__strncpy_safe(out->query, q_start, (size_t)(q_end - q_start),
                             sizeof(out->query));
            p = q_end;
        }
    }

    /* ---- Parse fragment ---- */
    if (p < end && *p == '#') {
        p++; /* skip '#' */
        ts__strncpy_safe(out->fragment, p, (size_t)(end - p), sizeof(out->fragment));
    }

    /* ---- Set default port ---- */
    if (out->port == 0) {
        if (ts__strcmp(out->scheme, "https") == 0)
            out->port = 443;
        else if (ts__strcmp(out->scheme, "http") == 0)
            out->port = 80;
    }

    return 0;
}

/* ----- URL Resolution ----- */

/*
 * ts_url_resolve — resolve a relative URL against a base URL.
 *
 * RFC 3986 Section 5 algorithm (simplified).
 *
 * If 'relative' is an absolute URL (has scheme), it is returned as-is.
 * If protocol-relative (//), inherits scheme from base.
 * If host-relative (/), inherits scheme+host from base.
 * If path-relative, merges with base path.
 */
static void ts_url_resolve(const struct ts_url *base,
                            const char *relative,
                            struct ts_url *out) {
    struct ts_url rel;

    if (!base || !relative || !out) return;

    /* Parse the relative URL */
    ts_url_parse(relative, &rel);

    /* If relative has a scheme + host, it's absolute — use as-is */
    if (rel.scheme[0] && rel.host[0]) {
        *out = rel;
        return;
    }

    /* If relative has a host but no scheme (protocol-relative) */
    if (rel.host[0]) {
        *out = rel;
        ts__strncpy_safe(out->scheme, base->scheme, ts__strlen(base->scheme),
                         sizeof(out->scheme));
        if (out->port == 0) {
            if (ts__strcmp(out->scheme, "https") == 0) out->port = 443;
            else out->port = 80;
        }
        return;
    }

    /* Inherit scheme, host, port from base */
    ts__strncpy_safe(out->scheme, base->scheme, ts__strlen(base->scheme),
                     sizeof(out->scheme));
    ts__strncpy_safe(out->host, base->host, ts__strlen(base->host),
                     sizeof(out->host));
    out->port = base->port;

    if (rel.path[0] == '/') {
        /* Host-relative: use rel path directly */
        ts__strncpy_safe(out->path, rel.path, ts__strlen(rel.path),
                         sizeof(out->path));
    } else if (rel.path[0]) {
        /* Path-relative: merge with base path */
        /* Find last '/' in base path */
        size_t base_dir_len = 0;
        {
            size_t i;
            size_t blen = ts__strlen(base->path);
            for (i = blen; i > 0; i--) {
                if (base->path[i - 1] == '/') {
                    base_dir_len = i;
                    break;
                }
            }
        }

        /* Concatenate base directory + relative path */
        {
            size_t rlen = ts__strlen(rel.path);
            size_t total = base_dir_len + rlen;
            if (total >= sizeof(out->path)) total = sizeof(out->path) - 1;
            {
                size_t i;
                for (i = 0; i < base_dir_len && i < sizeof(out->path) - 1; i++)
                    out->path[i] = base->path[i];
                {
                    size_t j;
                    for (j = 0; j < rlen && i < sizeof(out->path) - 1; j++, i++)
                        out->path[i] = rel.path[j];
                }
                out->path[i] = '\0';
            }
        }

        /* Normalize . and .. segments */
        {
            char *path = out->path;
            char normalized[1024];
            char *segments[128];
            int seg_count = 0;
            char *tok = path;
            char *next;
            size_t i, pos;

            /* Split on '/' */
            while (*tok) {
                next = tok;
                while (*next && *next != '/') next++;
                if (next > tok) {
                    size_t slen = (size_t)(next - tok);
                    if (slen == 1 && tok[0] == '.') {
                        /* Current dir — skip */
                    } else if (slen == 2 && tok[0] == '.' && tok[1] == '.') {
                        /* Parent dir — pop */
                        if (seg_count > 0) seg_count--;
                    } else {
                        if (seg_count < 128) {
                            segments[seg_count] = tok;
                            seg_count++;
                        }
                    }
                }
                if (*next == '/') next++;
                tok = next;
            }

            /* Rebuild path */
            normalized[0] = '/';
            pos = 1;
            for (i = 0; i < (size_t)seg_count; i++) {
                char *seg = segments[i];
                char *seg_end = seg;
                while (*seg_end && *seg_end != '/') seg_end++;
                {
                    size_t slen = (size_t)(seg_end - seg);
                    size_t j;
                    if (pos + slen + 1 >= sizeof(normalized)) break;
                    for (j = 0; j < slen; j++)
                        normalized[pos++] = seg[j];
                    if (i + 1 < (size_t)seg_count)
                        normalized[pos++] = '/';
                }
            }
            normalized[pos] = '\0';
            ts__strncpy_safe(out->path, normalized, pos, sizeof(out->path));
        }
    } else {
        /* No path in relative — use base path */
        ts__strncpy_safe(out->path, base->path, ts__strlen(base->path),
                         sizeof(out->path));
    }

    /* Query and fragment come from relative if present, else base */
    if (rel.query[0]) {
        ts__strncpy_safe(out->query, rel.query, ts__strlen(rel.query),
                         sizeof(out->query));
    } else if (!rel.path[0] && !rel.query[0]) {
        ts__strncpy_safe(out->query, base->query, ts__strlen(base->query),
                         sizeof(out->query));
    }

    if (rel.fragment[0]) {
        ts__strncpy_safe(out->fragment, rel.fragment, ts__strlen(rel.fragment),
                         sizeof(out->fragment));
    }
}

/* ----- URL Formatting ----- */

/*
 * ts_url_to_string — format a ts_url back into a URL string.
 */
static void ts_url_to_string(const struct ts_url *url, char *buf, size_t len) {
    size_t pos = 0;
    int default_port;

    if (!url || !buf || len == 0) return;
    buf[0] = '\0';

    /* Scheme */
    if (url->scheme[0]) {
        const char *s = url->scheme;
        while (*s && pos + 3 < len) buf[pos++] = *s++;
        if (pos + 3 < len) { buf[pos++] = ':'; buf[pos++] = '/'; buf[pos++] = '/'; }
    }

    /* Host */
    {
        const char *h = url->host;
        while (*h && pos < len - 1) buf[pos++] = *h++;
    }

    /* Port (only if non-default) */
    default_port = 0;
    if (ts__strcmp(url->scheme, "http") == 0 && url->port == 80) default_port = 1;
    if (ts__strcmp(url->scheme, "https") == 0 && url->port == 443) default_port = 1;
    if (url->port > 0 && !default_port && pos + 6 < len) {
        buf[pos++] = ':';
        /* Convert port to string */
        {
            char pbuf[8];
            int pi = 0;
            uint16_t p = url->port;
            char tmp[8];
            int ti = 0;
            if (p == 0) { tmp[ti++] = '0'; }
            else { while (p > 0) { tmp[ti++] = '0' + (char)(p % 10); p /= 10; } }
            while (ti > 0 && pi < 7) pbuf[pi++] = tmp[--ti];
            pbuf[pi] = '\0';
            {
                const char *pp = pbuf;
                while (*pp && pos < len - 1) buf[pos++] = *pp++;
            }
        }
    }

    /* Path */
    if (url->path[0]) {
        const char *pa = url->path;
        while (*pa && pos < len - 1) buf[pos++] = *pa++;
    }

    /* Query */
    if (url->query[0] && pos + 1 < len) {
        buf[pos++] = '?';
        {
            const char *q = url->query;
            while (*q && pos < len - 1) buf[pos++] = *q++;
        }
    }

    /* Fragment */
    if (url->fragment[0] && pos + 1 < len) {
        buf[pos++] = '#';
        {
            const char *f = url->fragment;
            while (*f && pos < len - 1) buf[pos++] = *f++;
        }
    }

    buf[pos] = '\0';
}

/* ----- URL Utility ----- */

/*
 * ts_url_is_https — returns 1 if the URL uses HTTPS.
 */
static int ts_url_is_https(const struct ts_url *url) {
    return url && ts__strcmp(url->scheme, "https") == 0;
}

/*
 * ts_url_effective_port — returns the port to connect to.
 */
static uint16_t ts_url_effective_port(const struct ts_url *url) {
    if (!url) return 0;
    if (url->port > 0) return url->port;
    if (ts__strcmp(url->scheme, "https") == 0) return 443;
    return 80;
}

/*
 * ts_url_host_and_port — format "host:port" for HTTP Host header.
 * Omits port if it's the default for the scheme.
 */
static void ts_url_host_header(const struct ts_url *url, char *buf, size_t len) {
    size_t pos = 0;
    int default_port;
    const char *h;

    if (!url || !buf || len == 0) return;

    h = url->host;
    while (*h && pos < len - 1) buf[pos++] = *h++;

    default_port = 0;
    if (ts__strcmp(url->scheme, "http") == 0 && url->port == 80) default_port = 1;
    if (ts__strcmp(url->scheme, "https") == 0 && url->port == 443) default_port = 1;

    if (url->port > 0 && !default_port && pos + 6 < len) {
        buf[pos++] = ':';
        {
            char pbuf[8];
            int pi = 0;
            uint16_t p = url->port;
            char tmp[8];
            int ti = 0;
            if (p == 0) { tmp[ti++] = '0'; }
            else { while (p > 0) { tmp[ti++] = '0' + (char)(p % 10); p /= 10; } }
            while (ti > 0 && pi < 7) pbuf[pi++] = tmp[--ti];
            pbuf[pi] = '\0';
            {
                const char *pp = pbuf;
                while (*pp && pos < len - 1) buf[pos++] = *pp++;
            }
        }
    }

    buf[pos] = '\0';
}

/*
 * ts_url_path_and_query — format "/path?query" for HTTP request line.
 */
static void ts_url_request_path(const struct ts_url *url, char *buf, size_t len) {
    size_t pos = 0;
    const char *s;

    if (!url || !buf || len == 0) return;

    /* Path (default to "/" if empty) */
    s = url->path[0] ? url->path : "/";
    while (*s && pos < len - 1) buf[pos++] = *s++;

    /* Query */
    if (url->query[0] && pos + 1 < len) {
        buf[pos++] = '?';
        s = url->query;
        while (*s && pos < len - 1) buf[pos++] = *s++;
    }

    buf[pos] = '\0';
}

/*
 * ts_url_percent_decode — decode %XX sequences in-place.
 * Returns decoded length.
 */
static size_t ts_url_percent_decode(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            int hi, lo;
            char c1 = src[1], c2 = src[2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            else { *dst++ = *src++; continue; }
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            else { *dst++ = *src++; continue; }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            /* '+' means space in query strings */
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return (size_t)(dst - str);
}

#endif /* TS_URL_H */
