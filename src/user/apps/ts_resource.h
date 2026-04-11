/*
 * ts_resource.h — TaterSurf external resource loader
 *
 * Header-only. Manages concurrent HTTP fetches for external resources
 * (stylesheets, scripts, images) referenced by <link>, <script src>,
 * and <img src> tags.
 *
 * Architecture:
 *   - Active slots: up to TS_RES_MAX_ACTIVE concurrent HTTP requests
 *   - Cache: completed resources stored by URL
 *   - Queue: overflow URLs waiting for a free active slot
 *
 * Usage:
 *   struct ts_resource_mgr rmgr;
 *   ts_resource_mgr_init(&rmgr, &cookie_jar);
 *   ts_resource_enqueue(&rmgr, "https://site.com/style.css", TS_RES_CSS, base_url);
 *   // In poll loop:
 *   int nres = ts_resource_get_pollfds(&rmgr, pfds + base_nfds);
 *   fry_poll(pfds, base_nfds + nres, 33);
 *   ts_resource_tick(&rmgr);
 *   // Check for completions:
 *   struct ts_resource_result *r;
 *   while ((r = ts_resource_next_complete(&rmgr)) != NULL) { ... }
 */

#ifndef TS_RESOURCE_H
#define TS_RESOURCE_H

#include "ts_http.h"
#include "ts_url.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_RES_MAX_ACTIVE     4   /* concurrent HTTP fetches */
#define TS_RES_MAX_CACHE     96   /* completed resource cache */
#define TS_RES_MAX_QUEUE     64   /* pending queue depth */

/* ================================================================== */
/* Types                                                               */
/* ================================================================== */

enum ts_resource_type {
    TS_RES_CSS = 0,
    TS_RES_JS,
    TS_RES_IMAGE
};

enum ts_resource_state {
    TS_RES_IDLE = 0,
    TS_RES_LOADING,
    TS_RES_DONE,
    TS_RES_FAILED
};

/* ================================================================== */
/* Cached resource entry                                               */
/* ================================================================== */

struct ts_resource_entry {
    char url[512];
    enum ts_resource_type type;
    char *data;
    size_t data_len;
    /* For decoded images */
    uint32_t *pixels;
    int img_w, img_h;
    int used;
};

/* ================================================================== */
/* Active fetch slot                                                   */
/* ================================================================== */

struct ts_resource_slot {
    struct ts_http http;
    struct ts_cookie_jar cookies;
    char url[512];
    enum ts_resource_type type;
    int active;             /* 1 = fetch in progress */
    int cache_idx;          /* destination cache entry */
};

/* ================================================================== */
/* Queue entry                                                         */
/* ================================================================== */

struct ts_resource_queue_entry {
    char url[512];
    enum ts_resource_type type;
};

/* ================================================================== */
/* Completion result (for consumer to process)                         */
/* ================================================================== */

struct ts_resource_result {
    int cache_idx;
    enum ts_resource_type type;
    char *data;
    size_t data_len;
    char url[512];
};

/* ================================================================== */
/* Resource manager                                                    */
/* ================================================================== */

struct ts_resource_mgr {
    /* Active fetch slots */
    struct ts_resource_slot slots[TS_RES_MAX_ACTIVE];

    /* Cache of completed resources */
    struct ts_resource_entry cache[TS_RES_MAX_CACHE];
    int cache_count;

    /* Pending queue */
    struct ts_resource_queue_entry queue[TS_RES_MAX_QUEUE];
    int queue_head;
    int queue_tail;
    int queue_count;

    /* Shared cookie jar (from main browser) */
    struct ts_cookie_jar *cookies;

    /* Completion buffer */
    struct ts_resource_result completions[16];
    int completion_count;
    int completion_read;

    /* Base URL for resolving relative URLs */
    struct ts_url base_url;
};

/* ================================================================== */
/* Initialization                                                      */
/* ================================================================== */

static void ts_resource_mgr_init(struct ts_resource_mgr *mgr,
                                  struct ts_cookie_jar *cookies) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->cookies = cookies;
    {
        int i;
        for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
            mgr->slots[i].http.sock_fd = -1;
            mgr->slots[i].active = 0;
        }
    }
}

static void ts_resource_mgr_set_base(struct ts_resource_mgr *mgr,
                                      const char *base_url_str) {
    ts_url_parse(base_url_str, &mgr->base_url);
}

/* ================================================================== */
/* URL deduplication — check if URL is already cached or loading        */
/* ================================================================== */

static int ts_resource__is_known(struct ts_resource_mgr *mgr,
                                  const char *url) {
    int i;
    /* Check cache */
    for (i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].used && strcmp(mgr->cache[i].url, url) == 0)
            return 1;
    }
    /* Check active slots */
    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        if (mgr->slots[i].active && strcmp(mgr->slots[i].url, url) == 0)
            return 1;
    }
    /* Check queue */
    {
        int j;
        for (j = 0; j < mgr->queue_count; j++) {
            int idx = (mgr->queue_head + j) % TS_RES_MAX_QUEUE;
            if (strcmp(mgr->queue[idx].url, url) == 0)
                return 1;
        }
    }
    return 0;
}

/* ================================================================== */
/* Start a fetch in a free slot                                        */
/* ================================================================== */

static int ts_resource__start_fetch(struct ts_resource_mgr *mgr,
                                     const char *url,
                                     enum ts_resource_type type) {
    int i;
    struct ts_resource_slot *slot = NULL;

    /* Find a free slot */
    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        if (!mgr->slots[i].active) {
            slot = &mgr->slots[i];
            break;
        }
    }
    if (!slot) return -1; /* all slots busy */

    /* Allocate cache entry */
    if (mgr->cache_count >= TS_RES_MAX_CACHE) return -1;
    {
        int ci = mgr->cache_count;
        struct ts_resource_entry *ce = &mgr->cache[ci];
        memset(ce, 0, sizeof(*ce));
        strncpy(ce->url, url, sizeof(ce->url) - 1);
        ce->type = type;
        ce->used = 1;
        mgr->cache_count++;
        slot->cache_idx = ci;
    }

    /* Resolve relative URL */
    {
        struct ts_url resolved;
        char resolved_str[2048];
        ts_url_resolve(&mgr->base_url, url, &resolved);
        ts_url_to_string(&resolved, resolved_str, sizeof(resolved_str));
        strncpy(slot->url, resolved_str, sizeof(slot->url) - 1);
    }

    /* Init HTTP request */
    ts_http_free(&slot->http);
    ts_http_init(&slot->http);
    ts_cookie_jar_init(&slot->cookies);
    /* Copy cookies from shared jar */
    if (mgr->cookies) {
        memcpy(&slot->cookies, mgr->cookies, sizeof(slot->cookies));
    }
    slot->http.cookies = &slot->cookies;
    slot->type = type;

    /* Start fetch */
    fprintf(stderr, "RES_FETCH: start url=%.80s\n", slot->url);
    if (ts_http_get(&slot->http, slot->url) < 0) {
        fprintf(stderr, "RES_FETCH: FAILED err=%s\n", slot->http.error);
        slot->active = 0;
        return -1;
    }
    fprintf(stderr, "RES_FETCH: OK state=%d fd=%d\n",
            slot->http.state, slot->http.sock_fd);

    slot->active = 1;
    return 0;
}

/* ================================================================== */
/* Enqueue a resource for fetching                                     */
/* ================================================================== */

static void ts_resource_enqueue(struct ts_resource_mgr *mgr,
                                 const char *url,
                                 enum ts_resource_type type) {
    char resolved_str[2048];

    if (!url || !url[0]) return;

    /* Resolve to absolute URL for dedup */
    {
        struct ts_url resolved;
        ts_url_resolve(&mgr->base_url, url, &resolved);
        ts_url_to_string(&resolved, resolved_str, sizeof(resolved_str));
    }

    /* Skip if already known */
    if (ts_resource__is_known(mgr, resolved_str)) return;

    /* Try to start immediately in a free slot */
    if (ts_resource__start_fetch(mgr, resolved_str, type) == 0)
        return;

    /* Queue for later */
    if (mgr->queue_count < TS_RES_MAX_QUEUE) {
        int idx = (mgr->queue_head + mgr->queue_count) % TS_RES_MAX_QUEUE;
        strncpy(mgr->queue[idx].url, resolved_str,
                sizeof(mgr->queue[idx].url) - 1);
        mgr->queue[idx].type = type;
        mgr->queue_count++;
    }
}

/* ================================================================== */
/* Get poll file descriptors for active fetches                        */
/* ================================================================== */

static int ts_resource_get_pollfds(struct ts_resource_mgr *mgr,
                                    struct fry_pollfd *pfds) {
    int count = 0;
    int i;
    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        struct ts_resource_slot *s = &mgr->slots[i];
        if (s->active && s->http.sock_fd >= 0 &&
            s->http.state >= TS_HTTP_TLS_HANDSHAKE &&
            s->http.state <= TS_HTTP_RECV_CHUNKED) {
            pfds[count].fd = s->http.sock_fd;
            pfds[count].events = FRY_POLLIN |
                (s->http.state == TS_HTTP_TLS_HANDSHAKE ? FRY_POLLOUT : 0);
            pfds[count].revents = 0;
            count++;
        }
    }
    return count;
}

/* ================================================================== */
/* Tick — poll active fetches, complete finished ones, start queued     */
/* ================================================================== */

static void ts_resource_tick(struct ts_resource_mgr *mgr) {
    int i;

    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        struct ts_resource_slot *s = &mgr->slots[i];
        if (!s->active) continue;
        if (s->http.state == TS_HTTP_DONE || s->http.state == TS_HTTP_ERROR)
            goto complete_slot;
        if (s->http.sock_fd < 0 && s->http.state != TS_HTTP_IDLE) {
            goto complete_slot; /* socket closed unexpectedly */
        }
        if (s->http.sock_fd < 0) continue;

        /* Poll the HTTP request (including TLS handshake) */
        {
            int rc = ts_http_poll(&s->http);
            if (rc == 1) {
                /* Check for redirect */
                if (ts_http_is_redirect(&s->http)) {
                    ts_http_follow_redirect(&s->http);
                    continue;
                }
                goto complete_slot;
            } else if (rc < 0) {
                goto complete_slot;
            }
        }
        continue;

    complete_slot:
        /* Transfer response data to cache */
        {
            struct ts_resource_entry *ce = &mgr->cache[s->cache_idx];
            if (s->http.state == TS_HTTP_DONE &&
                s->http.response.body &&
                s->http.response.body_len > 0) {
                ce->data = s->http.response.body;
                ce->data_len = s->http.response.body_len;
                /* Detach body from HTTP response so it's not freed */
                s->http.response.body = NULL;
                s->http.response.body_len = 0;
                s->http.response.body_cap = 0;
            }

            /* Push completion notification */
            if (mgr->completion_count < 16) {
                struct ts_resource_result *r =
                    &mgr->completions[mgr->completion_count++];
                r->cache_idx = s->cache_idx;
                r->type = s->type;
                r->data = ce->data;
                r->data_len = ce->data_len;
                strncpy(r->url, ce->url, sizeof(r->url) - 1);
            }
        }

        /* Free slot */
        ts_http_free(&s->http);
        ts_http_init(&s->http);
        s->http.sock_fd = -1;
        s->active = 0;

        /* Start next queued resource */
        if (mgr->queue_count > 0) {
            struct ts_resource_queue_entry *qe =
                &mgr->queue[mgr->queue_head];
            ts_resource__start_fetch(mgr, qe->url, qe->type);
            mgr->queue_head = (mgr->queue_head + 1) % TS_RES_MAX_QUEUE;
            mgr->queue_count--;
        }
    }
}

/* ================================================================== */
/* Read next completed resource (returns NULL when no more)            */
/* ================================================================== */

static struct ts_resource_result *ts_resource_next_complete(
    struct ts_resource_mgr *mgr) {
    if (mgr->completion_read >= mgr->completion_count)
        return NULL;
    return &mgr->completions[mgr->completion_read++];
}

/* Reset completion buffer (call after processing all completions) */
static void ts_resource_completions_clear(struct ts_resource_mgr *mgr) {
    mgr->completion_count = 0;
    mgr->completion_read = 0;
}

/* ================================================================== */
/* Cache lookup                                                        */
/* ================================================================== */

static struct ts_resource_entry *ts_resource_cache_get(
    struct ts_resource_mgr *mgr, const char *url) {
    int i;
    for (i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].used && strcmp(mgr->cache[i].url, url) == 0)
            return &mgr->cache[i];
    }
    return NULL;
}

/* ================================================================== */
/* Check if any active fetches remain                                  */
/* ================================================================== */

static int ts_resource_is_busy(struct ts_resource_mgr *mgr) {
    int i;
    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        if (mgr->slots[i].active) return 1;
    }
    return mgr->queue_count > 0;
}

/* ================================================================== */
/* Teardown — free all cached data                                     */
/* ================================================================== */

static void ts_resource_mgr_destroy(struct ts_resource_mgr *mgr) {
    int i;
    for (i = 0; i < TS_RES_MAX_ACTIVE; i++) {
        if (mgr->slots[i].active) {
            ts_http_free(&mgr->slots[i].http);
            mgr->slots[i].active = 0;
        }
    }
    for (i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].data) {
            free(mgr->cache[i].data);
            mgr->cache[i].data = NULL;
        }
        if (mgr->cache[i].pixels) {
            free(mgr->cache[i].pixels);
            mgr->cache[i].pixels = NULL;
        }
    }
    mgr->cache_count = 0;
    mgr->queue_count = 0;
}

#endif /* TS_RESOURCE_H */
