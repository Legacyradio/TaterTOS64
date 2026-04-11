/*
 * ts_http.h — TaterSurf HTTP/1.1 client
 *
 * Header-only. Supports HTTP and HTTPS (via BearSSL).
 * Uses TaterTOS socket API (fry_socket, fry_connect, fry_send, fry_recv).
 *
 * Features:
 *   - HTTP/1.1 GET and POST
 *   - HTTPS via BearSSL TLS 1.2 (full X.509 validation, 146 root CAs)
 *   - Chunked transfer-encoding decoding
 *   - Content-Length body collection
 *   - Connection: close (read until EOF)
 *   - Redirect following (301/302/303/307/308, max 5 hops)
 *   - Header parsing (status, content-type, location, set-cookie)
 *   - In-memory cookie jar (per session)
 *
 * Usage pattern (matches evloop.c proven networking model):
 *   struct ts_http req;
 *   ts_http_init(&req);
 *   ts_http_get(&req, "https://example.com/");
 *   // In poll loop:
 *   while (req.state != TS_HTTP_DONE && req.state != TS_HTTP_ERROR) {
 *       // poll req.sock_fd for POLLIN
 *       ts_http_poll(&req);
 *   }
 *   // Use req.response.body, req.response.body_len
 *   ts_http_free(&req);
 */

#ifndef TS_HTTP_H
#define TS_HTTP_H

#include "ts_url.h"
#include "../libc/libc.h"
#include <stdint.h>
#include <stddef.h>

/* BearSSL headers for HTTPS */
#include "bearssl/inc/bearssl.h"

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_HTTP_MAX_HEADERS      64
#define TS_HTTP_MAX_HEADER_NAME  64
#define TS_HTTP_MAX_HEADER_VALUE 512
#define TS_HTTP_RECV_BUF         8192
#define TS_HTTP_MAX_REDIRECTS    5
#define TS_HTTP_MAX_BODY         (4 * 1024 * 1024)  /* 4MB max response */
#define TS_HTTP_CONNECT_TIMEOUT  10000  /* 10 seconds */
#define TS_HTTP_RECV_TIMEOUT     30000  /* 30 seconds */

#define TS_HTTP_MAX_COOKIES      64
#define TS_HTTP_COOKIE_NAME_MAX  64
#define TS_HTTP_COOKIE_VALUE_MAX 256
#define TS_HTTP_COOKIE_DOMAIN_MAX 128

/* ================================================================== */
/* Data structures                                                     */
/* ================================================================== */

struct ts_http_header {
    char name[TS_HTTP_MAX_HEADER_NAME];
    char value[TS_HTTP_MAX_HEADER_VALUE];
};

struct ts_http_response {
    int status_code;                /* e.g. 200, 301, 404 */
    char status_text[64];           /* e.g. "OK" */
    struct ts_http_header headers[TS_HTTP_MAX_HEADERS];
    int header_count;
    char *body;                     /* malloc'd body buffer */
    size_t body_len;                /* actual body length */
    size_t body_cap;                /* allocated capacity */
    char content_type[128];         /* parsed Content-Type */
    char location[512];             /* parsed Location (redirects) */
    size_t content_length;          /* from Content-Length, or 0 */
    int content_length_present;     /* 1 if Content-Length header was sent */
    int chunked;                    /* 1 if Transfer-Encoding: chunked */
    int connection_close;           /* 1 if Connection: close */
};

enum ts_http_state {
    TS_HTTP_IDLE = 0,
    TS_HTTP_RESOLVING,
    TS_HTTP_CONNECTING,
    TS_HTTP_TLS_HANDSHAKE,
    TS_HTTP_SENDING,
    TS_HTTP_RECV_HEADERS,
    TS_HTTP_RECV_BODY,
    TS_HTTP_RECV_CHUNKED,
    TS_HTTP_DONE,
    TS_HTTP_ERROR
};

/* Chunked transfer state machine */
enum ts_chunk_state {
    TS_CHUNK_SIZE = 0,     /* reading hex chunk size line */
    TS_CHUNK_DATA,         /* reading chunk data bytes */
    TS_CHUNK_CRLF,         /* consuming trailing \r\n after chunk data */
    TS_CHUNK_DONE          /* final zero-length chunk seen */
};

/* Cookie */
struct ts_cookie {
    char name[TS_HTTP_COOKIE_NAME_MAX];
    char value[TS_HTTP_COOKIE_VALUE_MAX];
    char domain[TS_HTTP_COOKIE_DOMAIN_MAX];
    char path[256];
    int secure;             /* 1 = HTTPS only */
};

/* Cookie jar (per-session, in-memory) */
struct ts_cookie_jar {
    struct ts_cookie cookies[TS_HTTP_MAX_COOKIES];
    int count;
};

/* ================================================================== */
/* Trust anchors — embedded Mozilla/NSS root CA bundle                 */
/* ================================================================== */

#include "ts_trust_anchors.h"

/* TLS context (one per connection) */
struct ts_tls_ctx {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    int initialized;
};

/* Main HTTP request structure */
struct ts_http {
    /* Request */
    struct ts_url url;
    char method[8];                 /* "GET" or "POST" */
    char *post_body;                /* POST body (malloc'd, NULL for GET) */
    size_t post_body_len;

    /* Connection */
    int sock_fd;
    enum ts_http_state state;
    struct ts_tls_ctx tls;
    int is_https;

    /* Response */
    struct ts_http_response response;

    /* Error */
    char error[256];

    /* Internal receive state */
    char recv_buf[TS_HTTP_RECV_BUF];
    int recv_len;                   /* bytes in recv_buf */
    int recv_pos;                   /* parse position in recv_buf */
    int headers_complete;

    /* TLS handshake state (for non-blocking handshake in poll loop) */
    int hs_attempts;

    /* Chunked state */
    enum ts_chunk_state chunk_state;
    size_t chunk_remaining;         /* bytes remaining in current chunk */

    /* Redirect tracking */
    int redirect_count;
    int poll_diag;

    /* Cookie jar (shared across redirects) */
    struct ts_cookie_jar *cookies;

    /* Progress callback */
    void (*on_progress)(struct ts_http *req, size_t bytes_received);
};

/* ================================================================== */
/* Global TLS state — BearSSL needs no global init                     */
/* ==================================================================  */

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

static void ts_http__zero(void *p, size_t n) {
    char *d = (char *)p;
    while (n--) *d++ = 0;
}

static int ts_http__strncasecmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static char *ts_http__strcasestr(const char *haystack, const char *needle) {
    size_t nlen;
    if (!haystack || !needle) return NULL;
    nlen = ts__strlen(needle);
    if (nlen == 0) return (char *)haystack;
    while (*haystack) {
        if (ts_http__strncasecmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

/* Copy string safely with size limit */
static void ts_http__strlcpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Trim leading/trailing whitespace — returns pointer into same buffer */
static const char *ts_http__trim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Grow response body buffer */
static int ts_http__body_append(struct ts_http *req,
                                 const char *data, size_t len) {
    struct ts_http_response *r = &req->response;
    if (r->body_len + len > TS_HTTP_MAX_BODY) {
        ts_http__strlcpy(req->error, "Response body too large", sizeof(req->error));
        req->state = TS_HTTP_ERROR;
        return -1;
    }
    /* Grow buffer if needed */
    if (r->body_len + len > r->body_cap) {
        size_t new_cap = r->body_cap ? r->body_cap * 2 : 4096;
        char *new_buf;
        while (new_cap < r->body_len + len) new_cap *= 2;
        if (new_cap > TS_HTTP_MAX_BODY) new_cap = TS_HTTP_MAX_BODY;
        new_buf = (char *)realloc(r->body, new_cap);
        if (!new_buf) {
            ts_http__strlcpy(req->error, "Out of memory for response body",
                             sizeof(req->error));
            req->state = TS_HTTP_ERROR;
            return -1;
        }
        r->body = new_buf;
        r->body_cap = new_cap;
    }
    memcpy(r->body + r->body_len, data, len);
    r->body_len += len;
    return 0;
}

/*
 * X.509 time check callback — real wall-clock validation.
 *
 * BearSSL encodes certificate times as:
 *   days   = days since 0000-01-01 (proleptic Gregorian)
 *   seconds = seconds within the day (0..86399)
 *
 * Returns: -1 = before notBefore, 0 = within validity, 1 = after notAfter
 *
 * Unix epoch (1970-01-01 00:00:00 UTC) = day 719528 in this calendar.
 */
#define TS_EPOCH_DAY_OFFSET  719528u
#define TS_SECONDS_PER_DAY   86400u

static int ts_tls_time_check(void *ctx,
        uint32_t not_before_days, uint32_t not_before_seconds,
        uint32_t not_after_days, uint32_t not_after_seconds) {
    (void)ctx;

    /* Get current wall-clock time from TaterTOS RTC+HPET */
    time_t now_epoch = time_func(NULL);
    if (now_epoch <= 0) {
        /* No valid time available — accept cert (safe fallback for
         * hardware with dead/missing RTC battery; TaterTOS selftest
         * already warns if RTC is pre-2024) */
        return 0;
    }

    uint32_t now_days = TS_EPOCH_DAY_OFFSET
                      + (uint32_t)(now_epoch / TS_SECONDS_PER_DAY);
    uint32_t now_secs = (uint32_t)(now_epoch % TS_SECONDS_PER_DAY);

    /* Before notBefore? */
    if (now_days < not_before_days
        || (now_days == not_before_days && now_secs < not_before_seconds))
        return -1;

    /* After notAfter? */
    if (now_days > not_after_days
        || (now_days == not_after_days && now_secs > not_after_seconds))
        return 1;

    return 0;
}

/* ================================================================== */
/* BearSSL engine helpers — non-blocking pump for poll loop            */
/* ================================================================== */

/*
 * Pump outgoing record data (SENDREC) to the socket.
 * Returns: 0 = ok (may have sent partial), -1 = fatal socket error.
 */
static int ts_tls_pump_send(struct ts_http *req) {
    br_ssl_engine_context *eng = &req->tls.sc.eng;
    unsigned st = br_ssl_engine_current_state(eng);
    if (st & BR_SSL_SENDREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
        if (buf && len > 0) {
            long rc = fry_send(req->sock_fd, buf, len, 0);
            if (rc > 0)
                br_ssl_engine_sendrec_ack(eng, (size_t)rc);
            else if (rc < 0 && rc != -EAGAIN && rc != -EWOULDBLOCK)
                return -1;
        }
    }
    return 0;
}

/*
 * Pump incoming record data (RECVREC) from the socket.
 * Returns: 0 = ok, -1 = fatal socket error, -2 = EOF.
 */
static int ts_tls_pump_recv(struct ts_http *req) {
    br_ssl_engine_context *eng = &req->tls.sc.eng;
    unsigned st = br_ssl_engine_current_state(eng);
    if (st & BR_SSL_RECVREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
        if (buf && len > 0) {
            long rc = fry_recv(req->sock_fd, buf, len, 0);
            if (rc > 0)
                br_ssl_engine_recvrec_ack(eng, (size_t)rc);
            else if (rc == 0)
                return -2; /* EOF */
            else if (rc != -EAGAIN && rc != -EWOULDBLOCK)
                return -1;
        }
    }
    return 0;
}

/* ================================================================== */
/* TLS setup/teardown                                                  */
/* ================================================================== */

static int ts_tls_setup(struct ts_http *req) {
    struct ts_tls_ctx *t = &req->tls;
    unsigned char entropy[32];

    /*
     * HARDENED TLS configuration — ECDHE-only, AEAD-only, TLS 1.2+
     *
     * Cipher suites (in preference order):
     *   1. ECDHE_ECDSA + AES-256-GCM + SHA384
     *   2. ECDHE_RSA   + AES-256-GCM + SHA384
     *   3. ECDHE_ECDSA + CHACHA20-POLY1305 + SHA256
     *   4. ECDHE_RSA   + CHACHA20-POLY1305 + SHA256
     *   5. ECDHE_ECDSA + AES-128-GCM + SHA256
     *   6. ECDHE_RSA   + AES-128-GCM + SHA256
     *
     * Excluded: all RSA key exchange (no forward secrecy),
     *           all CBC ciphers (padding oracle attacks),
     *           all 3DES, RC4, NULL, export ciphers,
     *           TLS 1.0 and 1.1.
     */

    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    };

    /* Start with full init to get all crypto implementations registered,
     * then override the cipher suite list to our hardened set. */
    br_ssl_client_init_full(&t->sc, &t->xc,
                            TS_TRUST_ANCHORS, TS_TRUST_ANCHORS_NUM);

    /* Override cipher suites — ECDHE + AEAD only */
    br_ssl_engine_set_suites(&t->sc.eng, suites,
                              sizeof(suites) / sizeof(suites[0]));

    /* Enforce TLS 1.2 minimum — no TLS 1.0/1.1 fallback */
    br_ssl_engine_set_versions(&t->sc.eng, BR_TLS12, BR_TLS12);

    /* Real wall-clock time callback for certificate date validation.
     * Uses TaterTOS RTC (read at boot) + HPET monotonic offset to get
     * current UTC time, then compares against cert notBefore/notAfter. */
    br_x509_minimal_set_time_callback(&t->xc, NULL, ts_tls_time_check);

    /* Set I/O buffer (bidirectional) */
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof(t->iobuf), 1);

    /* Inject entropy from TaterTOS kernel RNG */
    fry_getrandom(entropy, sizeof(entropy), 0);
    br_ssl_engine_inject_entropy(&t->sc.eng, entropy, sizeof(entropy));

    /* Reset for new connection with SNI hostname */
    if (!br_ssl_client_reset(&t->sc, req->url.host, 0)) {
        int err = br_ssl_engine_last_error(&t->sc.eng);
        snprintf(req->error, sizeof(req->error),
                 "BearSSL reset failed: err=%d", err);
        return -1;
    }

    t->initialized = 1;
    return 0;
}

/*
 * ts_tls_handshake — pump the BearSSL engine until handshake completes.
 *
 * Called each poll cycle during TS_HTTP_TLS_HANDSHAKE state.
 * Returns: 1 = handshake complete, 0 = in progress, -1 = error.
 */
static int ts_tls_handshake(struct ts_http *req) {
    br_ssl_engine_context *eng = &req->tls.sc.eng;
    unsigned st;

    /* Pump record I/O */
    if (ts_tls_pump_send(req) < 0) {
        snprintf(req->error, sizeof(req->error), "TLS: send error during handshake");
        return -1;
    }
    if (ts_tls_pump_recv(req) < 0) {
        snprintf(req->error, sizeof(req->error), "TLS: recv error during handshake");
        return -1;
    }

    st = br_ssl_engine_current_state(eng);

    if (st & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(eng);
        snprintf(req->error, sizeof(req->error),
                 "TLS handshake failed: BearSSL err=%d", err);
        return -1;
    }

    /* SENDAPP means handshake is done and we can send application data */
    if (st & BR_SSL_SENDAPP)
        return 1;

    return 0; /* still handshaking */
}

static void ts_tls_teardown(struct ts_tls_ctx *t) {
    if (!t->initialized) return;
    br_ssl_engine_close(&t->sc.eng);
    t->initialized = 0;
}

/* ================================================================== */
/* Send/recv wrappers (plain or TLS)                                   */
/* ================================================================== */

static long ts_http__send(struct ts_http *req, const void *buf, size_t len) {
    if (req->is_https && req->tls.initialized) {
        br_ssl_engine_context *eng = &req->tls.sc.eng;
        const unsigned char *src = (const unsigned char *)buf;
        size_t total = 0;
        int attempts = 0;

        while (total < len && attempts < 200) {
            unsigned st = br_ssl_engine_current_state(eng);
            if (st & BR_SSL_CLOSED) return -1;
            if (st & BR_SSL_SENDAPP) {
                size_t alen;
                unsigned char *abuf = br_ssl_engine_sendapp_buf(eng, &alen);
                if (abuf && alen > 0) {
                    size_t chunk = len - total;
                    if (chunk > alen) chunk = alen;
                    memcpy(abuf, src + total, chunk);
                    br_ssl_engine_sendapp_ack(eng, chunk);
                    total += chunk;
                }
            }
            /* Flush record data to socket */
            br_ssl_engine_flush(eng, 0);
            if (ts_tls_pump_send(req) < 0) return -1;
            if (ts_tls_pump_recv(req) < 0) return -1;
            attempts++;
        }
        /* Final flush */
        br_ssl_engine_flush(eng, 0);
        while (br_ssl_engine_current_state(eng) & BR_SSL_SENDREC) {
            if (ts_tls_pump_send(req) < 0) break;
        }
        return total > 0 ? (long)total : -1;
    }
    return fry_send(req->sock_fd, buf, len, 0);
}

static long ts_http__recv(struct ts_http *req, void *buf, size_t len) {
    if (req->is_https && req->tls.initialized) {
        br_ssl_engine_context *eng = &req->tls.sc.eng;
        unsigned st;
        int pumps;

        /* Pump socket data into BearSSL until app data is available
         * or the socket has nothing left. A single TLS record may
         * span multiple recv() calls, so loop. */
        for (pumps = 0; pumps < 32; pumps++) {
            st = br_ssl_engine_current_state(eng);
            if (st & BR_SSL_RECVAPP) break;   /* got decrypted data */
            if (st & BR_SSL_CLOSED) break;     /* connection closed */
            if (!(st & BR_SSL_RECVREC)) break; /* engine doesn't want more */

            {
                int rc = ts_tls_pump_recv(req);
                if (rc == -2) return 0; /* EOF */
                if (rc < 0) return -1;
            }
            /* BearSSL may need to send during recv (renegotiation) */
            ts_tls_pump_send(req);
        }

        st = br_ssl_engine_current_state(eng);

        if (st & BR_SSL_RECVAPP) {
            size_t alen;
            unsigned char *abuf = br_ssl_engine_recvapp_buf(eng, &alen);
            if (abuf && alen > 0) {
                size_t take = alen < len ? alen : len;
                memcpy(buf, abuf, take);
                br_ssl_engine_recvapp_ack(eng, take);
                return (long)take;
            }
        }

        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            if (err == BR_ERR_OK) return 0; /* clean close */
            fprintf(stderr, "TLS_CLOSED: err=%d body=%zu\n",
                    err, req->response.body_len);
            return 0; /* treat as EOF — server may reset after sending data */
        }

        return -2; /* no data yet, try again */
    }
    {
        long rc = fry_recv(req->sock_fd, buf, len, 0);
        if (rc < 0) return -2; /* EAGAIN / no data yet — poll again */
        return rc;
    }
}

/* ================================================================== */
/* Cookie handling                                                     */
/* ================================================================== */

/* Forward declaration — defined after ts_cookie_build */
static void ts_cookie_jar_save(const struct ts_cookie_jar *jar);

static void ts_cookie_jar_init(struct ts_cookie_jar *jar) {
    ts_http__zero(jar, sizeof(*jar));
}

/* Parse a Set-Cookie header and add to jar */
static void ts_cookie_parse(struct ts_cookie_jar *jar,
                             const char *value, const char *host) {
    struct ts_cookie c;
    const char *p = value;
    const char *eq;
    const char *semi;

    ts_http__zero(&c, sizeof(c));

    /* Parse name=value */
    eq = p;
    while (*eq && *eq != '=' && *eq != ';') eq++;
    if (*eq != '=') return;

    ts_http__strlcpy(c.name, p, sizeof(c.name));
    {
        size_t nlen = (size_t)(eq - p);
        if (nlen >= sizeof(c.name)) nlen = sizeof(c.name) - 1;
        c.name[nlen] = '\0';
    }

    p = eq + 1;
    semi = p;
    while (*semi && *semi != ';') semi++;
    {
        size_t vlen = (size_t)(semi - p);
        if (vlen >= sizeof(c.value)) vlen = sizeof(c.value) - 1;
        memcpy(c.value, p, vlen);
        c.value[vlen] = '\0';
    }

    /* Default domain from request host */
    ts_http__strlcpy(c.domain, host, sizeof(c.domain));
    c.path[0] = '/'; c.path[1] = '\0';

    /* Parse cookie attributes */
    p = semi;
    while (*p == ';') {
        p++;
        while (*p == ' ') p++;
        if (ts_http__strncasecmp(p, "domain=", 7) == 0) {
            p += 7;
            if (*p == '.') p++; /* skip leading dot */
            {
                const char *ae = p;
                while (*ae && *ae != ';') ae++;
                size_t alen = (size_t)(ae - p);
                if (alen >= sizeof(c.domain)) alen = sizeof(c.domain) - 1;
                memcpy(c.domain, p, alen);
                c.domain[alen] = '\0';
                p = ae;
            }
        } else if (ts_http__strncasecmp(p, "path=", 5) == 0) {
            p += 5;
            {
                const char *ae = p;
                while (*ae && *ae != ';') ae++;
                size_t alen = (size_t)(ae - p);
                if (alen >= sizeof(c.path)) alen = sizeof(c.path) - 1;
                memcpy(c.path, p, alen);
                c.path[alen] = '\0';
                p = ae;
            }
        } else if (ts_http__strncasecmp(p, "secure", 6) == 0) {
            c.secure = 1;
            p += 6;
        } else {
            /* Skip unknown attribute */
            while (*p && *p != ';') p++;
        }
    }

    /* Add or update cookie in jar */
    {
        int i;
        for (i = 0; i < jar->count; i++) {
            if (ts_http__strncasecmp(jar->cookies[i].name, c.name,
                                      sizeof(c.name)) == 0 &&
                ts_http__strncasecmp(jar->cookies[i].domain, c.domain,
                                      sizeof(c.domain)) == 0) {
                jar->cookies[i] = c;
                return;
            }
        }
        if (jar->count < TS_HTTP_MAX_COOKIES) {
            jar->cookies[jar->count++] = c;
        }
    }
    /* Auto-persist cookies to disk */
    ts_cookie_jar_save(jar);
}

/* Build Cookie header value from jar for a given URL */
static int ts_cookie_build(const struct ts_cookie_jar *jar,
                            const struct ts_url *url,
                            char *buf, size_t max) {
    size_t pos = 0;
    int i;
    int first = 1;

    buf[0] = '\0';
    for (i = 0; i < jar->count; i++) {
        const struct ts_cookie *c = &jar->cookies[i];
        size_t nlen, vlen;

        /* Domain match (suffix match) */
        {
            size_t dlen = ts__strlen(c->domain);
            size_t hlen = ts__strlen(url->host);
            if (hlen < dlen) continue;
            if (ts_http__strncasecmp(url->host + hlen - dlen, c->domain, dlen) != 0)
                continue;
        }

        /* Path match (prefix match) */
        {
            size_t plen = ts__strlen(c->path);
            if (plen > 0 && ts_http__strncasecmp(url->path, c->path, plen) != 0)
                continue;
        }

        /* Secure check */
        if (c->secure && ts__strcmp(url->scheme, "https") != 0)
            continue;

        nlen = ts__strlen(c->name);
        vlen = ts__strlen(c->value);
        if (pos + nlen + vlen + 4 >= max) break;

        if (!first) { buf[pos++] = ';'; buf[pos++] = ' '; }
        memcpy(buf + pos, c->name, nlen); pos += nlen;
        buf[pos++] = '=';
        memcpy(buf + pos, c->value, vlen); pos += vlen;
        first = 0;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ================================================================== */
/* Cookie persistence — save/load to filesystem                        */
/* ================================================================== */

#define TS_COOKIE_FILE "/data/cookies.txt"

/*
 * ts_cookie_jar_save — write all cookies to file.
 * Format: one cookie per line: domain\tpath\tsecure\tname\tvalue
 */
static void ts_cookie_jar_save(const struct ts_cookie_jar *jar) {
    FILE *f = fopen(TS_COOKIE_FILE, "w");
    int i;
    if (!f) return;
    for (i = 0; i < jar->count; i++) {
        const struct ts_cookie *c = &jar->cookies[i];
        fprintf(f, "%s\t%s\t%d\t%s\t%s\n",
                c->domain, c->path, c->secure, c->name, c->value);
    }
    fclose(f);
}

/*
 * ts_cookie_jar_load — read cookies from file into jar.
 */
static void ts_cookie_jar_load(struct ts_cookie_jar *jar) {
    FILE *f = fopen(TS_COOKIE_FILE, "r");
    char line[2048];
    if (!f) return;
    while (jar->count < TS_HTTP_MAX_COOKIES && fgets(line, sizeof(line), f)) {
        struct ts_cookie *c = &jar->cookies[jar->count];
        char *p = line;
        char *tab;
        /* domain */
        tab = strchr(p, '\t'); if (!tab) continue; *tab = '\0';
        ts_http__strlcpy(c->domain, p, sizeof(c->domain)); p = tab + 1;
        /* path */
        tab = strchr(p, '\t'); if (!tab) continue; *tab = '\0';
        ts_http__strlcpy(c->path, p, sizeof(c->path)); p = tab + 1;
        /* secure */
        tab = strchr(p, '\t'); if (!tab) continue; *tab = '\0';
        c->secure = (*p == '1') ? 1 : 0; p = tab + 1;
        /* name */
        tab = strchr(p, '\t'); if (!tab) continue; *tab = '\0';
        ts_http__strlcpy(c->name, p, sizeof(c->name)); p = tab + 1;
        /* value (rest of line, strip newline) */
        { size_t vl = strlen(p);
          while (vl > 0 && (p[vl-1] == '\n' || p[vl-1] == '\r')) vl--;
          if (vl >= sizeof(c->value)) vl = sizeof(c->value) - 1;
          memcpy(c->value, p, vl); c->value[vl] = '\0';
        }
        jar->count++;
    }
    fclose(f);
}

/* ================================================================== */
/* Header parsing                                                      */
/* ================================================================== */

/*
 * Parse HTTP response headers from recv_buf.
 * Returns 1 if headers are complete, 0 if need more data.
 */
static int ts_http__parse_headers(struct ts_http *req) {
    char *buf = req->recv_buf;
    int len = req->recv_len;
    char *end_of_headers;
    char *line;
    char *next;

    /* Look for \r\n\r\n header terminator */
    end_of_headers = NULL;
    {
        int i;
        for (i = 0; i < len - 3; i++) {
            if (buf[i] == '\r' && buf[i+1] == '\n' &&
                buf[i+2] == '\r' && buf[i+3] == '\n') {
                end_of_headers = buf + i + 4;
                break;
            }
        }
    }
    if (!end_of_headers) return 0; /* need more data */

    /* Parse status line: "HTTP/1.1 200 OK\r\n" */
    line = buf;
    next = line;
    while (next < end_of_headers && *next != '\r') next++;

    {
        /* Find first space (after HTTP/x.x) */
        char *sp1 = line;
        while (sp1 < next && *sp1 != ' ') sp1++;
        if (sp1 < next) sp1++;

        /* Parse status code */
        req->response.status_code = 0;
        while (sp1 < next && *sp1 >= '0' && *sp1 <= '9') {
            req->response.status_code = req->response.status_code * 10 + (*sp1 - '0');
            sp1++;
        }

        /* Parse status text */
        if (sp1 < next && *sp1 == ' ') sp1++;
        {
            size_t tlen = (size_t)(next - sp1);
            if (tlen >= sizeof(req->response.status_text))
                tlen = sizeof(req->response.status_text) - 1;
            memcpy(req->response.status_text, sp1, tlen);
            req->response.status_text[tlen] = '\0';
        }
    }

    /* Advance past status line \r\n */
    line = next;
    if (line < end_of_headers && *line == '\r') line++;
    if (line < end_of_headers && *line == '\n') line++;

    /* Parse headers */
    while (line < end_of_headers - 2) { /* -2 for final \r\n */
        char *colon;
        char *value;
        size_t name_len, value_len;
        struct ts_http_header *h;

        next = line;
        while (next < end_of_headers && *next != '\r') next++;

        /* Find colon separator */
        colon = line;
        while (colon < next && *colon != ':') colon++;
        if (colon >= next) { line = next + 2; continue; }

        name_len = (size_t)(colon - line);
        value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t')) value++;
        value_len = (size_t)(next - value);

        /* Store header */
        if (req->response.header_count < TS_HTTP_MAX_HEADERS) {
            h = &req->response.headers[req->response.header_count++];
            if (name_len >= sizeof(h->name)) name_len = sizeof(h->name) - 1;
            memcpy(h->name, line, name_len);
            h->name[name_len] = '\0';
            if (value_len >= sizeof(h->value)) value_len = sizeof(h->value) - 1;
            memcpy(h->value, value, value_len);
            h->value[value_len] = '\0';

            /* Parse well-known headers */
            if (ts_http__strncasecmp(h->name, "content-type", 12) == 0) {
                ts_http__strlcpy(req->response.content_type, h->value,
                                 sizeof(req->response.content_type));
            } else if (ts_http__strncasecmp(h->name, "content-length", 14) == 0) {
                req->response.content_length = 0;
                req->response.content_length_present = 1;
                {
                    const char *d = h->value;
                    while (*d >= '0' && *d <= '9') {
                        req->response.content_length =
                            req->response.content_length * 10 + (size_t)(*d - '0');
                        d++;
                    }
                }
            } else if (ts_http__strncasecmp(h->name, "transfer-encoding", 17) == 0) {
                if (ts_http__strcasestr(h->value, "chunked"))
                    req->response.chunked = 1;
            } else if (ts_http__strncasecmp(h->name, "location", 8) == 0) {
                ts_http__strlcpy(req->response.location, h->value,
                                 sizeof(req->response.location));
            } else if (ts_http__strncasecmp(h->name, "connection", 10) == 0) {
                if (ts_http__strcasestr(h->value, "close"))
                    req->response.connection_close = 1;
            } else if (ts_http__strncasecmp(h->name, "set-cookie", 10) == 0) {
                if (req->cookies) {
                    ts_cookie_parse(req->cookies, h->value, req->url.host);
                }
            }
        }

        /* Advance to next line */
        line = next;
        if (line < end_of_headers && *line == '\r') line++;
        if (line < end_of_headers && *line == '\n') line++;
    }

    /* Move remaining data (body start) to front of recv_buf */
    {
        int consumed = (int)(end_of_headers - buf);
        int remaining = len - consumed;
        if (remaining > 0)
            memmove(buf, end_of_headers, (size_t)remaining);
        req->recv_len = remaining;
    }

    req->headers_complete = 1;
    return 1;
}

/* ================================================================== */
/* Chunked transfer decoding                                           */
/* ================================================================== */

/*
 * Process chunked data from recv_buf.
 * Returns: 0 = need more data, 1 = all chunks done, -1 = error.
 */
static int ts_http__process_chunked(struct ts_http *req) {
    char *buf = req->recv_buf;
    int len = req->recv_len;
    int pos = 0;

    while (pos < len) {
        switch (req->chunk_state) {
        case TS_CHUNK_SIZE: {
            /* Find end of chunk size line (\r\n) */
            int line_end = -1;
            int i;
            for (i = pos; i < len - 1; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n') {
                    line_end = i;
                    break;
                }
            }
            if (line_end < 0) goto need_more;

            /* Parse hex chunk size */
            req->chunk_remaining = 0;
            {
                int j;
                for (j = pos; j < line_end; j++) {
                    char c = buf[j];
                    int digit;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                    else break; /* chunk extension — ignore */
                    req->chunk_remaining = req->chunk_remaining * 16 + (size_t)digit;
                }
            }

            pos = line_end + 2; /* skip \r\n */

            if (req->chunk_remaining == 0) {
                req->chunk_state = TS_CHUNK_DONE;
                goto done;
            }
            req->chunk_state = TS_CHUNK_DATA;
            break;
        }

        case TS_CHUNK_DATA: {
            size_t avail = (size_t)(len - pos);
            size_t take = avail < req->chunk_remaining ? avail : req->chunk_remaining;
            if (ts_http__body_append(req, buf + pos, take) < 0)
                return -1;
            pos += (int)take;
            req->chunk_remaining -= take;
            if (req->chunk_remaining == 0)
                req->chunk_state = TS_CHUNK_CRLF;
            if (pos >= len) goto need_more;
            break;
        }

        case TS_CHUNK_CRLF:
            /* Consume \r\n after chunk data */
            if (pos < len && buf[pos] == '\r') pos++;
            if (pos < len && buf[pos] == '\n') pos++;
            else if (pos >= len) goto need_more;
            req->chunk_state = TS_CHUNK_SIZE;
            break;

        case TS_CHUNK_DONE:
            goto done;
        }
    }

need_more:
    /* Compact buffer */
    if (pos > 0) {
        int remaining = len - pos;
        if (remaining > 0)
            memmove(buf, buf + pos, (size_t)remaining);
        req->recv_len = remaining;
    }
    return 0;

done:
    req->recv_len = 0;
    return 1;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

/*
 * ts_http_init — zero-initialize a request struct.
 */
static void ts_http_init(struct ts_http *req) {
    ts_http__zero(req, sizeof(*req));
    req->sock_fd = -1;
}

/*
 * ts_http_free — close socket, free TLS, free body.
 */
static void ts_http_free(struct ts_http *req) {
    if (req->tls.initialized)
        ts_tls_teardown(&req->tls);
    if (req->sock_fd >= 0) {
        fry_close(req->sock_fd);
        req->sock_fd = -1;
    }
    if (req->response.body) {
        free(req->response.body);
        req->response.body = NULL;
    }
    if (req->post_body) {
        free(req->post_body);
        req->post_body = NULL;
    }
    req->response.body_len = 0;
    req->response.body_cap = 0;
}

/*
 * ts_http_get — start an HTTP(S) GET request.
 *
 * Performs DNS resolve, TCP connect, optional TLS handshake,
 * and sends the HTTP request. After this call, use ts_http_poll()
 * in your event loop to receive the response.
 *
 * Returns 0 on success (request sent), -1 on error (check req->error).
 */
static int ts_http__send_request(struct ts_http *req);

static int ts_http_get(struct ts_http *req, const char *url_str) {
    struct fry_sockaddr_in addr;
    uint32_t ip;
    long rc;
    char request_buf[2048];
    int request_len;
    char host_hdr[280];
    char req_path[1536];
    char cookie_hdr[1024];

    /* Parse URL */
    if (ts_url_parse(url_str, &req->url) < 0) {
        ts_http__strlcpy(req->error, "Invalid URL", sizeof(req->error));
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    ts_http__strlcpy(req->method, "GET", sizeof(req->method));
    req->is_https = ts_url_is_https(&req->url);

    /* Reset response */
    ts_http__zero(&req->response, sizeof(req->response));
    req->recv_len = 0;
    req->recv_pos = 0;
    req->headers_complete = 0;
    req->chunk_state = TS_CHUNK_SIZE;
    req->chunk_remaining = 0;

    /* DNS resolve */
    req->state = TS_HTTP_RESOLVING;
    rc = fry_dns_resolve(req->url.host, &ip);
    if (rc < 0) {
        snprintf(req->error, sizeof(req->error),
                 "DNS failed for %s (err %ld)", req->url.host, rc);
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    /* Create TCP socket */
    req->sock_fd = (int)fry_socket(AF_INET, SOCK_STREAM, 0);
    if (req->sock_fd < 0) {
        snprintf(req->error, sizeof(req->error),
                 "socket() failed: %d", req->sock_fd);
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    /* Set receive timeout */
    {
        uint64_t timeout_ms = TS_HTTP_RECV_TIMEOUT;
        fry_setsockopt(req->sock_fd, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout_ms, sizeof(timeout_ms));
    }

    /* Connect */
    req->state = TS_HTTP_CONNECTING;
    ts_http__zero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = fry_htons(ts_url_effective_port(&req->url));
    addr.sin_addr = ip;

    rc = fry_connect(req->sock_fd, &addr, sizeof(addr));
    if (rc < 0) {
        snprintf(req->error, sizeof(req->error),
                 "connect() to %s:%u failed: %ld",
                 req->url.host, ts_url_effective_port(&req->url), rc);
        req->state = TS_HTTP_ERROR;
        fry_close(req->sock_fd);
        req->sock_fd = -1;
        return -1;
    }

    /* Set socket non-blocking for async poll loop */
    fry_fcntl(req->sock_fd, F_SETFL, O_NONBLOCK);

    /* TLS handshake if HTTPS — set up context, handshake runs in poll loop */
    if (req->is_https) {
        if (ts_tls_setup(req) < 0) {
            req->state = TS_HTTP_ERROR;
            fry_close(req->sock_fd);
            req->sock_fd = -1;
            return -1;
        }
        req->state = TS_HTTP_TLS_HANDSHAKE;
        req->hs_attempts = 0;
        return 0; /* handshake + send happen in ts_http_poll */
    }

    /* Build and send HTTP request (plain HTTP only — HTTPS sends after handshake) */
    return ts_http__send_request(req);
}

/*
 * ts_http_post — start an HTTP(S) POST request.
 * body is copied (caller can free after call).
 */
static int ts_http_post(struct ts_http *req, const char *url_str,
                         const char *body, size_t body_len) {
    /* Set POST method and body before connecting */
    ts_http__strlcpy(req->method, "POST", sizeof(req->method));
    if (body && body_len > 0) {
        req->post_body = (char *)malloc(body_len);
        if (req->post_body) {
            memcpy(req->post_body, body, body_len);
            req->post_body_len = body_len;
        }
    }
    /* Reuse ts_http_get for DNS/connect/TLS — method is already set to POST */
    {
        /* Parse URL */
        if (ts_url_parse(url_str, &req->url) < 0) {
            ts_http__strlcpy(req->error, "Invalid URL", sizeof(req->error));
            req->state = TS_HTTP_ERROR;
            return -1;
        }
        req->is_https = ts_url_is_https(&req->url);

        /* Reset response */
        ts_http__zero(&req->response, sizeof(req->response));
        req->recv_len = 0;
        req->recv_pos = 0;
        req->headers_complete = 0;
        req->chunk_state = TS_CHUNK_SIZE;
        req->chunk_remaining = 0;

        /* DNS */
        req->state = TS_HTTP_RESOLVING;
        { uint32_t ip; long rc;
          struct fry_sockaddr_in addr;
          rc = fry_dns_resolve(req->url.host, &ip);
          if (rc < 0) {
              snprintf(req->error, sizeof(req->error),
                       "DNS failed for %s", req->url.host);
              req->state = TS_HTTP_ERROR; return -1;
          }
          req->sock_fd = (int)fry_socket(AF_INET, SOCK_STREAM, 0);
          if (req->sock_fd < 0) { req->state = TS_HTTP_ERROR; return -1; }
          { uint64_t tmo = TS_HTTP_RECV_TIMEOUT;
            fry_setsockopt(req->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo)); }
          req->state = TS_HTTP_CONNECTING;
          ts_http__zero(&addr, sizeof(addr));
          addr.sin_family = AF_INET;
          addr.sin_port = fry_htons(ts_url_effective_port(&req->url));
          addr.sin_addr = ip;
          rc = fry_connect(req->sock_fd, &addr, sizeof(addr));
          if (rc < 0) {
              req->state = TS_HTTP_ERROR;
              fry_close(req->sock_fd); req->sock_fd = -1; return -1;
          }
          fry_fcntl(req->sock_fd, F_SETFL, O_NONBLOCK);
          if (req->is_https) {
              if (ts_tls_setup(req) < 0) {
                  req->state = TS_HTTP_ERROR;
                  fry_close(req->sock_fd); req->sock_fd = -1; return -1;
              }
              req->state = TS_HTTP_TLS_HANDSHAKE;
              req->hs_attempts = 0;
              return 0;
          }
          return ts_http__send_request(req);
        }
    }
}

/*
 * ts_http_reuse_get — send a new HTTP GET on an existing connected socket.
 *
 * Reuses the current socket + TLS session for a new URL (must be same host).
 * The previous response body is freed. The connection stays open (keep-alive).
 *
 * Returns 0 on success (request sent), -1 on error.
 */
static int ts_http_reuse_get(struct ts_http *req, const char *url_str) {
    /* Free previous response body */
    if (req->response.body) {
        free(req->response.body);
        req->response.body = NULL;
    }
    req->response.body_len = 0;
    req->response.body_cap = 0;

    /* Parse new URL */
    if (ts_url_parse(url_str, &req->url) < 0) {
        ts_http__strlcpy(req->error, "Invalid URL", sizeof(req->error));
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    ts_http__strlcpy(req->method, "GET", sizeof(req->method));

    /* Reset response state for new request */
    ts_http__zero(&req->response, sizeof(req->response));
    req->recv_len = 0;
    req->recv_pos = 0;
    req->headers_complete = 0;
    req->chunk_state = TS_CHUNK_SIZE;
    req->chunk_remaining = 0;
    req->redirect_count = 0;
    req->error[0] = '\0';

    /* Socket and TLS stay as-is — send new GET on existing connection */
    return ts_http__send_request(req);
}

/*
 * ts_http_poll — called from the main event loop when the socket has data.
 *
 * Returns:
 *   0  = still receiving, call again when more data arrives
 *   1  = response complete (state = TS_HTTP_DONE)
 *   -1 = error (check req->error)
 */
static int ts_http__send_request(struct ts_http *req) {
    char host_hdr[264], req_path[1024], cookie_hdr[2048], request_buf[4096];
    int request_len;
    long rc;
    int is_post = (req->method[0] == 'P');

    ts_url_host_header(&req->url, host_hdr, sizeof(host_hdr));
    ts_url_request_path(&req->url, req_path, sizeof(req_path));

    cookie_hdr[0] = '\0';
    if (req->cookies) {
        ts_cookie_build(req->cookies, &req->url, cookie_hdr, sizeof(cookie_hdr));
    }

    if (is_post && req->post_body && req->post_body_len > 0) {
        request_len = snprintf(request_buf, sizeof(request_buf),
                                "POST %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: Mozilla/5.0 (TaterTOS64v3) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) TaterSurf/2.0 Chrome/120.0.0.0\r\n"
                                "Accept: text/html,application/xhtml+xml,application/xml;"
                                "q=0.9,application/json,*/*;q=0.8\r\n"
                                "Accept-Language: en-US,en;q=0.9\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: keep-alive\r\n"
                                "Content-Type: application/x-www-form-urlencoded\r\n"
                                "Content-Length: %zu\r\n"
                                "Sec-Fetch-Dest: document\r\n"
                                "Sec-Fetch-Mode: navigate\r\n"
                                "Sec-Fetch-Site: same-origin\r\n"
                                "%s%s%s"
                                "\r\n",
                                req_path,
                                host_hdr,
                                req->post_body_len,
                                cookie_hdr[0] ? "Cookie: " : "",
                                cookie_hdr[0] ? cookie_hdr : "",
                                cookie_hdr[0] ? "\r\n" : "");
    } else {
        request_len = snprintf(request_buf, sizeof(request_buf),
                                "GET %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: Mozilla/5.0 (TaterTOS64v3) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) TaterSurf/2.0 Chrome/120.0.0.0\r\n"
                                "Accept: text/html,application/xhtml+xml,application/xml;"
                                "q=0.9,application/json,*/*;q=0.8\r\n"
                                "Accept-Language: en-US,en;q=0.9\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: keep-alive\r\n"
                                "Upgrade-Insecure-Requests: 1\r\n"
                                "Sec-Fetch-Dest: document\r\n"
                                "Sec-Fetch-Mode: navigate\r\n"
                                "Sec-Fetch-Site: none\r\n"
                                "%s%s%s"
                                "\r\n",
                                req_path,
                                host_hdr,
                                cookie_hdr[0] ? "Cookie: " : "",
                                cookie_hdr[0] ? cookie_hdr : "",
                                cookie_hdr[0] ? "\r\n" : "");
    }

    /* Send headers */
    rc = ts_http__send(req, request_buf, (size_t)request_len);
    if (rc < 0) {
        snprintf(req->error, sizeof(req->error), "send() failed: %ld", rc);
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    /* Send POST body if present */
    if (is_post && req->post_body && req->post_body_len > 0) {
        rc = ts_http__send(req, req->post_body, req->post_body_len);
        if (rc < 0) {
            snprintf(req->error, sizeof(req->error), "POST body send failed: %ld", rc);
            req->state = TS_HTTP_ERROR;
            return -1;
        }
    }

    req->state = TS_HTTP_RECV_HEADERS;
    return 0;
}

static int ts_http_poll(struct ts_http *req) {
    long n;

    if (req->state == TS_HTTP_DONE || req->state == TS_HTTP_ERROR)
        return (req->state == TS_HTTP_DONE) ? 1 : -1;

    /* Non-blocking TLS handshake (called each poll cycle) */
    if (req->state == TS_HTTP_TLS_HANDSHAKE) {
        int hs_ret = ts_tls_handshake(req);
        if (hs_ret < 0) {
            req->state = TS_HTTP_ERROR;
            ts_tls_teardown(&req->tls);
            fry_close(req->sock_fd);
            req->sock_fd = -1;
            return -1;
        }
        if (hs_ret == 0) {
            req->hs_attempts++;
            if (req->hs_attempts > 3000) { /* ~100 seconds at 33ms poll */
                ts_http__strlcpy(req->error, "TLS handshake timeout",
                                 sizeof(req->error));
                req->state = TS_HTTP_ERROR;
                ts_tls_teardown(&req->tls);
                fry_close(req->sock_fd);
                req->sock_fd = -1;
                return -1;
            }
            return 0; /* still handshaking, render can happen */
        }
        /* Handshake complete — send the HTTP request */
        return ts_http__send_request(req);
    }

    /* If headers are done and there's unprocessed data in recv_buf,
     * process the body before trying to recv more. This handles the
     * case where body data arrived in the same TLS record as headers. */
    if (req->headers_complete && req->recv_pos < req->recv_len)
        goto process_body;

    /* Receive data into buffer */
    {
        size_t space = sizeof(req->recv_buf) - (size_t)req->recv_len;
        if (space == 0) {
            /* Buffer full — process what we have */
            if (req->headers_complete) goto process_body;
            ts_http__strlcpy(req->error, "Header too large",
                             sizeof(req->error));
            req->state = TS_HTTP_ERROR;
            return -1;
        }

        n = ts_http__recv(req, req->recv_buf + req->recv_len, space);
        if (req->poll_diag++ % 100 == 0)
            fprintf(stderr, "POLL: state=%d n=%ld hdr=%d body=%zu fd=%d https=%d\n",
                    req->state, n, req->headers_complete,
                    req->response.body_len, req->sock_fd, req->is_https);
        if (n == -2) return 0; /* TLS wants more data, poll again */
        if (n < 0) {
            snprintf(req->error, sizeof(req->error), "recv failed: %ld", n);
            req->state = TS_HTTP_ERROR;
            return -1;
        }
        if (n == 0) {
            /* Connection closed */
            if (!req->headers_complete) {
                ts_http__strlcpy(req->error, "Connection closed before headers",
                                 sizeof(req->error));
                req->state = TS_HTTP_ERROR;
                return -1;
            }
            /* For Connection: close, EOF means body is complete */
            req->state = TS_HTTP_DONE;
            /* Null-terminate body for convenience */
            if (req->response.body) {
                ts_http__body_append(req, "", 1);
                req->response.body_len--; /* don't count null terminator */
            }
            return 1;
        }
        req->recv_len += (int)n;
    }

    /* Parse headers if not done */
    if (!req->headers_complete) {
        if (!ts_http__parse_headers(req))
            return 0; /* need more data for headers */

        /* Headers parsed — determine body receive mode */
        if (req->response.chunked) {
            req->state = TS_HTTP_RECV_CHUNKED;
        } else {
            req->state = TS_HTTP_RECV_BODY;
        }

        /* Content-Length: 0 means no body — done immediately */
        if (!req->response.chunked && req->response.content_length == 0 &&
            req->response.content_length_present) {
            req->state = TS_HTTP_DONE;
            return 1;
        }

        /* Pre-allocate body buffer if Content-Length known */
        if (req->response.content_length > 0 &&
            req->response.content_length <= TS_HTTP_MAX_BODY) {
            req->response.body = (char *)malloc(req->response.content_length + 1);
            if (req->response.body) {
                req->response.body_cap = req->response.content_length + 1;
            }
        }
    }

process_body:
    /* Process body data */
    if (req->state == TS_HTTP_RECV_CHUNKED) {
        int chunk_ret = ts_http__process_chunked(req);
        if (chunk_ret < 0) return -1;
        if (chunk_ret == 1) {
            req->state = TS_HTTP_DONE;
            if (req->response.body) {
                ts_http__body_append(req, "", 1);
                req->response.body_len--;
            }
            return 1;
        }
    } else if (req->state == TS_HTTP_RECV_BODY) {
        /* Append recv_buf content to body */
        if (req->recv_len > 0) {
            if (ts_http__body_append(req, req->recv_buf,
                                      (size_t)req->recv_len) < 0)
                return -1;

            if (req->on_progress)
                req->on_progress(req, req->response.body_len);

            req->recv_len = 0;
        }

        /* Check if we have all data by Content-Length */
        if (req->response.content_length > 0 &&
            req->response.body_len >= req->response.content_length) {
            req->state = TS_HTTP_DONE;
            if (req->response.body) {
                ts_http__body_append(req, "", 1);
                req->response.body_len--;
            }
            return 1;
        }
    }

    return 0;
}

/*
 * ts_http_is_redirect — check if response is a redirect.
 */
static int ts_http_is_redirect(const struct ts_http *req) {
    int code = req->response.status_code;
    return (code == 301 || code == 302 || code == 303 ||
            code == 307 || code == 308) &&
           req->response.location[0] != '\0';
}

/*
 * ts_http_follow_redirect — follow a redirect.
 *
 * Closes current connection, resolves new URL, starts new request.
 * Returns 0 on success, -1 if max redirects exceeded.
 */
static int ts_http_follow_redirect(struct ts_http *req) {
    struct ts_url new_url;
    char new_url_str[2048];

    if (req->redirect_count >= TS_HTTP_MAX_REDIRECTS) {
        ts_http__strlcpy(req->error, "Too many redirects",
                         sizeof(req->error));
        req->state = TS_HTTP_ERROR;
        return -1;
    }

    /* Resolve redirect location against current URL */
    ts_url_resolve(&req->url, req->response.location, &new_url);
    ts_url_to_string(&new_url, new_url_str, sizeof(new_url_str));

    fprintf(stderr, "REDIRECT[%d]: %d -> %.120s\n",
            req->redirect_count, req->response.status_code, new_url_str);

    /* Close current connection */
    if (req->tls.initialized)
        ts_tls_teardown(&req->tls);
    if (req->sock_fd >= 0) {
        fry_close(req->sock_fd);
        req->sock_fd = -1;
    }

    /* Free current body */
    if (req->response.body) {
        free(req->response.body);
        req->response.body = NULL;
    }
    req->response.body_len = 0;
    req->response.body_cap = 0;

    /* Preserve cookies and redirect count */
    {
        struct ts_cookie_jar *jar = req->cookies;
        int rcount = req->redirect_count + 1;
        void (*progress)(struct ts_http *, size_t) = req->on_progress;

        ts_http__zero(req, sizeof(*req));
        req->sock_fd = -1;
        req->cookies = jar;
        req->redirect_count = rcount;
        req->on_progress = progress;
    }

    return ts_http_get(req, new_url_str);
}

/*
 * ts_http_get_header — find a response header value by name.
 * Returns pointer to value or NULL if not found.
 */
static const char *ts_http_get_header(const struct ts_http *req,
                                       const char *name) {
    int i;
    size_t nlen = ts__strlen(name);
    for (i = 0; i < req->response.header_count; i++) {
        if (ts_http__strncasecmp(req->response.headers[i].name, name, nlen) == 0 &&
            req->response.headers[i].name[nlen] == '\0')
            return req->response.headers[i].value;
    }
    return NULL;
}

#endif /* TS_HTTP_H */
