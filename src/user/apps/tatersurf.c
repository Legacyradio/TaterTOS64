/*
 * tatersurf.c — TaterSurf web browser for TaterTOS64v3
 *
 * TaterWin application. Single .fry binary.
 * Fetches web pages over HTTP/HTTPS, parses HTML/CSS, renders to
 * a TaterWin shared-memory window.
 *
 * UI: toolbar (back, forward, reload, URL bar) + viewport + status bar
 * Navigation: type URL + Enter, click links, back/forward buttons
 * Scrolling: mouse wheel, Page Up/Down, Home/End, arrow keys
 */

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"
#include "ts_url.h"
#include "ts_html.h"
#include "ts_css.h"
#include "ts_http.h"
#include "ts_dom.h"
#include "ts_font.h"
#include "ts_layout.h"
#include "ts_dom_bindings.h"
#include "ts_webcomp.h"
#include "ts_resource.h"
#include "ts_image.h"
#include "ts_xml.h"
#include "ts_dash.h"
#include "ts_video.h"
#include "ts_h264_wrap.h"

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include <opus.h>
#include <stdint.h>

/* ================================================================== */
/* Window dimensions                                                   */
/* ================================================================== */

#define INIT_W          900
#define INIT_H          600
#define TOOLBAR_H        32
#define STATUS_H         20
#define URL_BAR_X       120
#define URL_BAR_H        24
#define BTN_W            28
#define BTN_GAP           4

/* ================================================================== */
/* Colors                                                              */
/* ================================================================== */

#define COL_BG          0x0E131B
#define COL_TOOLBAR     0x131A24
#define COL_URL_BG      0x0A0F16
#define COL_URL_TEXT    0xE6ECF8
#define COL_URL_FOCUS   0x29B6F6
#define COL_URL_BORDER  0x2A364A
#define COL_BTN_BG      0x1C2333
#define COL_BTN_FG      0x95A4BC
#define COL_BTN_HOVER   0x29B6F6
#define COL_STATUS_BG   0x131A24
#define COL_STATUS_TEXT 0x95A4BC
#define COL_STATUS_ERR  0xEF5350
#define COL_STATUS_LOAD 0xF4C96A
#define COL_STATUS_OK   0x66BB6A
#define COL_SCROLL_TRACK 0x1A2233
#define COL_SCROLL_THUMB 0x4A5A70
#define COL_TRANS       0xFF000000

/* ================================================================== */
/* History                                                             */
/* ================================================================== */

#define HISTORY_MAX     64

/* ================================================================== */
/* Application state                                                   */
/* ================================================================== */

static int win_w = INIT_W;
static int win_h = INIT_H;

static uint32_t fallback_pixels[INIT_W * INIT_H];
static uint32_t *pixels = fallback_pixels;
static uint32_t *render_buf = NULL;
static gfx_ctx_t g_ctx;
static int g_shm_id = -1;
static tw_msg_header_t g_upd;

/* URL bar */
static char url_bar[2048] = "http://example.com/";
static int url_bar_len = 19;
static int url_bar_cursor = 19;
static int url_bar_focused = 0;
static int url_bar_scroll = 0; /* horizontal scroll offset in chars */

/* Navigation history */
static char history[HISTORY_MAX][512];
static int history_count = 0;
static int history_pos = -1;

/* Page state */
static struct ts_document *g_doc = NULL;
static struct ts_http g_req;
static struct ts_cookie_jar g_cookies;
static struct ts_resource_mgr g_resources;
static int g_resources_loading = 0;

/* ---- TLS connection reuse queue for same-host HTTPS resources ---- */
/* Same-host HTTPS resources reuse the main page's TLS connection via
 * HTTP/1.1 keep-alive (avoids redundant TLS handshakes). Cross-host
 * resources go through ts_resource_mgr with independent BearSSL
 * connections per slot. */
#define REUSE_QUEUE_MAX 32
static struct {
    char url[512];
    enum ts_resource_type type;
} g_reuse_queue[REUSE_QUEUE_MAX];
static int g_reuse_count = 0;
static int g_reuse_idx = 0;      /* next index to fetch */
static int g_reuse_active = 0;   /* 1 = g_req is fetching a reuse resource */
static char *g_page_html = NULL; /* saved copy of main page HTML for relayout */
static size_t g_page_html_len = 0;

static int scroll_x = 0;
static int scroll_y = 0;
static int needs_redraw = 1;

/* Status */
static char status_text[128] = "Ready";
static uint32_t status_color = COL_STATUS_TEXT;
static size_t bytes_loaded = 0;

/* g_dom is declared in ts_dom_bindings.h as a static global */

/* ================================================================== */
/* Media pipeline state                                                */
/* ================================================================== */

enum {
    MEDIA_IDLE = 0,
    MEDIA_FETCH_MPD,
    MEDIA_FETCH_INIT_V,
    MEDIA_FETCH_INIT_A,
    MEDIA_FETCH_SEG_V,
    MEDIA_FETCH_SEG_A,
    MEDIA_PLAYING,
    MEDIA_ERROR
};

static struct {
    int state;

    /* DASH */
    struct ts_dash_manifest manifest;
    struct ts_dash_representation *video_rep;
    struct ts_dash_representation *audio_rep;
    char base_url[2048];

    /* Segment fetching */
    struct ts_http seg_req;
    struct ts_cookie_jar seg_cookies;
    uint32_t video_seg_num;
    uint32_t audio_seg_num;
    uint32_t max_segments;

    /* Video decoder */
    ts_h264_decoder h264;
    uint32_t *rgb_frame;
    int frame_w, frame_h;

    /* Audio decoder */
    OpusDecoder *opus;
    int audio_open;

    /* A/V sync */
    uint64_t start_time_ms;
    uint64_t audio_written_samples;

    /* fMP4 demux read buffer */
    const uint8_t *mp4_buf;
    size_t mp4_buf_len;
} g_media;

/* Input stream for TaterWin messages */
#define INPUT_STREAM_MAX 512
static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len = 0;

/* ================================================================== */
/* Viewport geometry                                                   */
/* ================================================================== */

static int viewport_x(void) { return 0; }
static int viewport_y(void) { return TOOLBAR_H; }
static int viewport_w(void) { return win_w; }
static int viewport_h(void) { return win_h - TOOLBAR_H - STATUS_H; }

/* ================================================================== */
/* History management                                                  */
/* ================================================================== */

static void history_push(const char *url) {
    /* Trim forward history if we navigated back then went somewhere new */
    if (history_pos >= 0 && history_pos < history_count - 1) {
        history_count = history_pos + 1;
    }
    if (history_count >= HISTORY_MAX) {
        /* Shift everything down */
        int i;
        for (i = 0; i < HISTORY_MAX - 1; i++)
            memcpy(history[i], history[i + 1], 512);
        history_count = HISTORY_MAX - 1;
    }
    strncpy(history[history_count], url, 511);
    history[history_count][511] = '\0';
    history_pos = history_count;
    history_count++;
}

static const char *history_back(void) {
    if (history_pos > 0) {
        history_pos--;
        return history[history_pos];
    }
    return NULL;
}

static const char *history_forward(void) {
    if (history_pos < history_count - 1) {
        history_pos++;
        return history[history_pos];
    }
    return NULL;
}

/* ================================================================== */
/* URL bar management                                                  */
/* ================================================================== */

static void url_bar_set(const char *url) {
    url_bar_len = 0;
    while (url[url_bar_len] && url_bar_len < (int)sizeof(url_bar) - 1) {
        url_bar[url_bar_len] = url[url_bar_len];
        url_bar_len++;
    }
    url_bar[url_bar_len] = '\0';
    url_bar_cursor = url_bar_len;
    url_bar_scroll = 0;
}

/* ================================================================== */
/* Navigation                                                          */
/* ================================================================== */

static void set_status(const char *msg, uint32_t color) {
    strncpy(status_text, msg, sizeof(status_text) - 1);
    status_text[sizeof(status_text) - 1] = '\0';
    status_color = color;
    needs_redraw = 1;
}

static void navigate(const char *url_str) {
    char full_url[2048];

    /* If no scheme, default to https://. Explicit http:// is respected. */
    if (url_str[0] != '/' &&
        strncmp(url_str, "http://", 7) != 0 &&
        strncmp(url_str, "https://", 8) != 0) {
        snprintf(full_url, sizeof(full_url), "https://%s", url_str);
    } else {
        strncpy(full_url, url_str, sizeof(full_url) - 1);
        full_url[sizeof(full_url) - 1] = '\0';
    }

    /* Update URL bar */
    url_bar_set(full_url);
    url_bar_focused = 0;

    /* Push to history */
    history_push(full_url);

    /* Reset scroll */
    scroll_x = 0;
    scroll_y = 0;

    /* Free previous request */
    fprintf(stderr, "NAV[1]: start url=%.60s\n", full_url);
    ts_http_free(&g_req);
    fprintf(stderr, "NAV[2]: http_free done\n");

    /* Tear down old DOM (JS timers hold pointers into freed response) */
    if (g_dom) {
        fprintf(stderr, "NAV[3]: dom_destroy start\n");
        ts_dom_destroy(g_dom);
        fprintf(stderr, "NAV[4]: dom_destroy done\n");
        ts_dom_init(g_dom);
        fprintf(stderr, "NAV[5]: dom_init done\n");
    }

    /* Clear previous document */
    if (g_doc) {
        ts_doc_init(g_doc);
    }
    bytes_loaded = 0;
    fprintf(stderr, "NAV[6]: doc cleared, starting HTTP\n");

    /* Start HTTP request */
    set_status("Resolving...", COL_STATUS_LOAD);
    needs_redraw = 1;

    ts_http_init(&g_req);
    g_req.cookies = &g_cookies;
    g_req.on_progress = NULL;

    fprintf(stderr, "NAV[7]: http_get start\n");
    if (ts_http_get(&g_req, full_url) < 0) {
        set_status(g_req.error, COL_STATUS_ERR);
        return;
    }

    if (g_req.is_https)
        set_status("TLS handshake...", COL_STATUS_LOAD);
    else
        set_status("Loading...", COL_STATUS_LOAD);
}

static void navigate_post(const char *url_str, const char *body, size_t body_len) {
    char full_url[2048];

    if (url_str[0] != '/' &&
        strncmp(url_str, "http://", 7) != 0 &&
        strncmp(url_str, "https://", 8) != 0) {
        snprintf(full_url, sizeof(full_url), "https://%s", url_str);
    } else {
        strncpy(full_url, url_str, sizeof(full_url) - 1);
        full_url[sizeof(full_url) - 1] = '\0';
    }

    url_bar_set(full_url);
    url_bar_focused = 0;
    history_push(full_url);
    scroll_x = 0; scroll_y = 0;

    ts_http_free(&g_req);
    if (g_dom) { ts_dom_destroy(g_dom); ts_dom_init(g_dom); }
    if (g_doc) ts_doc_init(g_doc);
    bytes_loaded = 0;

    set_status("Submitting form...", COL_STATUS_LOAD);
    needs_redraw = 1;

    ts_http_init(&g_req);
    g_req.cookies = &g_cookies;
    if (ts_http_post(&g_req, full_url, body, body_len) < 0) {
        set_status(g_req.error, COL_STATUS_ERR);
        return;
    }
    if (g_req.is_https)
        set_status("TLS handshake...", COL_STATUS_LOAD);
    else
        set_status("Posting...", COL_STATUS_LOAD);
}

static void navigate_link(int link_index) {
    if (!g_doc || link_index < 0 || link_index >= g_doc->link_count) return;

    const char *href = g_doc->links[link_index].href;
    if (!href[0]) return;

    /* Resolve relative URL */
    struct ts_url base, resolved;
    ts_url_parse(url_bar, &base);
    ts_url_resolve(&base, href, &resolved);

    char resolved_str[2048];
    ts_url_to_string(&resolved, resolved_str, sizeof(resolved_str));

    navigate(resolved_str);
}

/* ================================================================== */
/* minimp4 read callback — reads from g_media.mp4_buf                  */
/* ================================================================== */

static int mp4_read_cb(int64_t offset, void *buf, size_t size, void *token) {
    (void)token;
    if (offset < 0 || (size_t)offset >= g_media.mp4_buf_len) return 0;
    size_t avail = g_media.mp4_buf_len - (size_t)offset;
    if (size > avail) size = avail;
    memcpy(buf, g_media.mp4_buf + offset, size);
    return (int)size;
}

/* ================================================================== */
/* Media pipeline                                                      */
/* ================================================================== */

static void media_stop(void) {
    if (g_media.h264) {
        ts_h264_destroy(g_media.h264);
        g_media.h264 = NULL;
    }
    if (g_media.opus) {
        opus_decoder_destroy(g_media.opus);
        g_media.opus = NULL;
    }
    if (g_media.audio_open) {
        fry_audio_close();
        g_media.audio_open = 0;
    }
    if (g_media.rgb_frame) {
        free(g_media.rgb_frame);
        g_media.rgb_frame = NULL;
    }
    ts_http_free(&g_media.seg_req);
    g_media.state = MEDIA_IDLE;
}

static void media_start_mpd(const char *mpd_url) {
    media_stop();

    strncpy(g_media.base_url, mpd_url, sizeof(g_media.base_url) - 1);
    /* Strip filename from URL to get base */
    char *last_slash = strrchr(g_media.base_url, '/');
    if (last_slash) last_slash[1] = '\0';

    ts_http_init(&g_media.seg_req);
    ts_cookie_jar_init(&g_media.seg_cookies);
    g_media.seg_req.cookies = &g_media.seg_cookies;

    if (ts_http_get(&g_media.seg_req, mpd_url) < 0) {
        g_media.state = MEDIA_ERROR;
        return;
    }
    g_media.state = MEDIA_FETCH_MPD;
    g_media.video_seg_num = 0;
    g_media.audio_seg_num = 0;
}

static void media_decode_video_segment(const uint8_t *data, size_t len) {
    if (!g_media.h264 || !data || len == 0) return;

    /* Parse fMP4 segment */
    g_media.mp4_buf = data;
    g_media.mp4_buf_len = len;

    MP4D_demux_t mp4;
    if (!MP4D_open(&mp4, mp4_read_cb, NULL, (int64_t)len)) return;

    unsigned i;
    for (i = 0; i < mp4.track_count; i++) {
        if (mp4.track[i].handler_type != MP4D_HANDLER_TYPE_VIDE) continue;

        unsigned s;
        for (s = 0; s < mp4.track[i].sample_count; s++) {
            unsigned frame_bytes, ts, dur;
            int64_t off = MP4D_frame_offset(&mp4, i, s, &frame_bytes, &ts, &dur);
            if (off < 0 || (size_t)(off + frame_bytes) > len) continue;

            const uint8_t *nal = data + off;

            /* Decode NAL unit */
            uint8_t *yp = NULL, *up = NULL, *vp = NULL;
            int w = 0, h = 0, sy = 0, suv = 0;
            int rc = ts_h264_decode(g_media.h264, nal, (int)frame_bytes,
                                     &yp, &up, &vp, &w, &h, &sy, &suv);
            if (rc == 1 && yp && w > 0 && h > 0) {
                /* Allocate or resize RGB frame buffer */
                if (w != g_media.frame_w || h != g_media.frame_h) {
                    free(g_media.rgb_frame);
                    g_media.rgb_frame = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
                    g_media.frame_w = w;
                    g_media.frame_h = h;
                }
                if (g_media.rgb_frame) {
                    ts_video_yuv_to_rgb(yp, up, vp, sy, suv, w, h,
                                         g_media.rgb_frame, w, h, w);
                    needs_redraw = 1;
                }
            }
        }
        break; /* only first video track */
    }

    MP4D_close(&mp4);
}

static void media_decode_audio_segment(const uint8_t *data, size_t len) {
    if (!g_media.opus || !data || len == 0) return;

    g_media.mp4_buf = data;
    g_media.mp4_buf_len = len;

    MP4D_demux_t mp4;
    if (!MP4D_open(&mp4, mp4_read_cb, NULL, (int64_t)len)) return;

    unsigned i;
    for (i = 0; i < mp4.track_count; i++) {
        if (mp4.track[i].handler_type != MP4D_HANDLER_TYPE_SOUN) continue;

        unsigned s;
        for (s = 0; s < mp4.track[i].sample_count; s++) {
            unsigned frame_bytes, ts, dur;
            int64_t off = MP4D_frame_offset(&mp4, i, s, &frame_bytes, &ts, &dur);
            if (off < 0 || (size_t)(off + frame_bytes) > len) continue;

            const uint8_t *frame = data + off;

            /* Decode Opus frame → PCM */
            int16_t pcm[5760 * 2]; /* max frame: 120ms * 48kHz * stereo */
            int samples = opus_decode(g_media.opus, frame, (int)frame_bytes,
                                       pcm, 5760, 0);
            if (samples > 0) {
                /* Open audio output on first decoded frame */
                if (!g_media.audio_open) {
                    if (fry_audio_open(48000, 2, 16) == 0)
                        g_media.audio_open = 1;
                }
                if (g_media.audio_open) {
                    fry_audio_write(pcm, (size_t)samples * 2 * sizeof(int16_t));
                    g_media.audio_written_samples += (uint64_t)samples;
                }
            }
        }
        break; /* only first audio track */
    }

    MP4D_close(&mp4);
}

static void media_fetch_next_segment(void) {
    char seg_url[2048];

    if (g_media.video_rep && g_media.video_seg_num < g_media.max_segments) {
        ts_dash_segment_url(g_media.video_rep, g_media.base_url,
                             g_media.video_seg_num + g_media.video_rep->start_number,
                             seg_url, sizeof(seg_url));
        ts_http_free(&g_media.seg_req);
        ts_http_init(&g_media.seg_req);
        g_media.seg_req.cookies = &g_media.seg_cookies;
        if (ts_http_get(&g_media.seg_req, seg_url) >= 0)
            g_media.state = MEDIA_FETCH_SEG_V;
        else
            g_media.state = MEDIA_ERROR;
    } else {
        g_media.state = MEDIA_PLAYING; /* done fetching */
    }
}

static void media_tick(void) {
    if (g_media.state == MEDIA_IDLE || g_media.state == MEDIA_ERROR) return;
    if (g_media.seg_req.sock_fd < 0 && g_media.state != MEDIA_PLAYING) return;

    /* Poll the segment request if active */
    if (g_media.seg_req.state >= TS_HTTP_RECV_HEADERS &&
        g_media.seg_req.state <= TS_HTTP_RECV_CHUNKED) {
        int rc = ts_http_poll(&g_media.seg_req);
        if (rc == 1) {
            /* Segment complete — handle redirects */
            if (ts_http_is_redirect(&g_media.seg_req)) {
                ts_http_follow_redirect(&g_media.seg_req);
                return;
            }

            switch (g_media.state) {
            case MEDIA_FETCH_MPD: {
                /* Parse MPD */
                if (ts_dash_parse(&g_media.manifest,
                                   g_media.seg_req.response.body,
                                   g_media.seg_req.response.body_len,
                                   g_media.base_url) < 0) {
                    g_media.state = MEDIA_ERROR;
                    return;
                }
                g_media.video_rep = ts_dash_select_video(&g_media.manifest,
                                                          2000000); /* 2 Mbps */
                g_media.audio_rep = ts_dash_select_audio(&g_media.manifest);

                /* Compute max segments from duration */
                if (g_media.video_rep && g_media.video_rep->timescale > 0 &&
                    g_media.video_rep->segment_duration > 0) {
                    uint32_t seg_dur_ms = (g_media.video_rep->segment_duration * 1000)
                                          / g_media.video_rep->timescale;
                    if (seg_dur_ms > 0 && g_media.manifest.duration_ms > 0)
                        g_media.max_segments = g_media.manifest.duration_ms / seg_dur_ms + 1;
                    else
                        g_media.max_segments = 999;
                } else {
                    g_media.max_segments = 999;
                }

                /* Create decoders */
                g_media.h264 = ts_h264_create();
                if (!g_media.h264) {
                    g_media.state = MEDIA_ERROR;
                    return;
                }

                int opus_err = 0;
                g_media.opus = opus_decoder_create(48000, 2, &opus_err);

                /* Fetch video init segment */
                if (g_media.video_rep && g_media.video_rep->init_url[0]) {
                    char init_url[2048];
                    snprintf(init_url, sizeof(init_url), "%s%s",
                             g_media.base_url, g_media.video_rep->init_url);
                    ts_http_free(&g_media.seg_req);
                    ts_http_init(&g_media.seg_req);
                    g_media.seg_req.cookies = &g_media.seg_cookies;
                    if (ts_http_get(&g_media.seg_req, init_url) >= 0)
                        g_media.state = MEDIA_FETCH_INIT_V;
                    else
                        g_media.state = MEDIA_ERROR;
                } else {
                    media_fetch_next_segment();
                }

                set_status("Video: loading...", COL_STATUS_LOAD);
                break;
            }
            case MEDIA_FETCH_INIT_V: {
                /* Feed init segment to H.264 decoder (contains SPS/PPS) */
                if (g_media.seg_req.response.body_len > 0) {
                    media_decode_video_segment(
                        (const uint8_t *)g_media.seg_req.response.body,
                        g_media.seg_req.response.body_len);
                }

                /* Fetch audio init segment if available */
                if (g_media.audio_rep && g_media.audio_rep->init_url[0]) {
                    char init_url[2048];
                    snprintf(init_url, sizeof(init_url), "%s%s",
                             g_media.base_url, g_media.audio_rep->init_url);
                    ts_http_free(&g_media.seg_req);
                    ts_http_init(&g_media.seg_req);
                    g_media.seg_req.cookies = &g_media.seg_cookies;
                    if (ts_http_get(&g_media.seg_req, init_url) >= 0)
                        g_media.state = MEDIA_FETCH_INIT_A;
                    else
                        media_fetch_next_segment();
                } else {
                    media_fetch_next_segment();
                }
                break;
            }
            case MEDIA_FETCH_INIT_A: {
                /* Feed audio init segment */
                if (g_media.seg_req.response.body_len > 0) {
                    media_decode_audio_segment(
                        (const uint8_t *)g_media.seg_req.response.body,
                        g_media.seg_req.response.body_len);
                }
                media_fetch_next_segment();
                break;
            }
            case MEDIA_FETCH_SEG_V: {
                /* Decode video segment */
                if (g_media.seg_req.response.body_len > 0) {
                    media_decode_video_segment(
                        (const uint8_t *)g_media.seg_req.response.body,
                        g_media.seg_req.response.body_len);
                }
                g_media.video_seg_num++;

                /* Fetch matching audio segment if available */
                if (g_media.audio_rep &&
                    g_media.audio_seg_num < g_media.max_segments) {
                    char seg_url[2048];
                    ts_dash_segment_url(g_media.audio_rep, g_media.base_url,
                                         g_media.audio_seg_num + g_media.audio_rep->start_number,
                                         seg_url, sizeof(seg_url));
                    ts_http_free(&g_media.seg_req);
                    ts_http_init(&g_media.seg_req);
                    g_media.seg_req.cookies = &g_media.seg_cookies;
                    if (ts_http_get(&g_media.seg_req, seg_url) >= 0)
                        g_media.state = MEDIA_FETCH_SEG_A;
                    else
                        media_fetch_next_segment();
                } else {
                    media_fetch_next_segment();
                }
                break;
            }
            case MEDIA_FETCH_SEG_A: {
                /* Decode audio segment */
                if (g_media.seg_req.response.body_len > 0) {
                    media_decode_audio_segment(
                        (const uint8_t *)g_media.seg_req.response.body,
                        g_media.seg_req.response.body_len);
                }
                g_media.audio_seg_num++;

                /* Continue with next video segment */
                media_fetch_next_segment();
                break;
            }
            default:
                break;
            }
        } else if (rc < 0) {
            g_media.state = MEDIA_ERROR;
        }
    }
}

/* ================================================================== */
/* Base64 decode (for data: URL inline image handling)                 */
/* ================================================================== */

static int b64_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 string into malloc'd buffer. Returns decoded length, -1 on error. */
static int b64_decode(const char *in, size_t in_len, uint8_t **out, size_t *out_len) {
    size_t alloc = (in_len / 4) * 3 + 4;
    uint8_t *buf = (uint8_t *)malloc(alloc);
    size_t i, o = 0;
    if (!buf) return -1;
    for (i = 0; i + 3 < in_len; i += 4) {
        int a = b64_char_val(in[i]);
        int b = b64_char_val(in[i+1]);
        int c = b64_char_val(in[i+2]);
        int d = b64_char_val(in[i+3]);
        if (a < 0 || b < 0) break;
        buf[o++] = (uint8_t)((a << 2) | (b >> 4));
        if (in[i+2] != '=' && c >= 0) {
            buf[o++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
            if (in[i+3] != '=' && d >= 0)
                buf[o++] = (uint8_t)(((c & 0x03) << 6) | d);
        }
    }
    *out = buf;
    *out_len = o;
    return 0;
}

/* Try to decode a data: URL image inline. Returns 1 if handled, 0 if not a data URL. */
static int try_decode_data_url_image(const char *url, int cache_idx) {
    const char *comma;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    uint32_t *pixels = NULL;
    int w = 0, h = 0;

    if (strncmp(url, "data:image/", 11) != 0) return 0;
    /* Find the base64 data after the comma */
    comma = strchr(url, ',');
    if (!comma) return 0;
    comma++; /* skip the comma */

    if (b64_decode(comma, strlen(comma), &raw, &raw_len) < 0) return 0;
    if (raw_len == 0) { free(raw); return 0; }

    if (ts_image_decode(raw, raw_len, &pixels, &w, &h) == 0) {
        if (g_doc && cache_idx >= 0 && cache_idx < 64) {
            g_doc->img_cache[cache_idx].pixels = pixels;
            g_doc->img_cache[cache_idx].w = w;
            g_doc->img_cache[cache_idx].h = h;
            g_doc->img_cache[cache_idx].used = 1;
            strncpy(g_doc->img_cache[cache_idx].url, url, 511);
            fprintf(stderr, "DATA_URL: decoded %dx%d at cache[%d]\n", w, h, cache_idx);
        } else {
            free(pixels);
        }
    } else {
        fprintf(stderr, "DATA_URL: decode failed len=%zu\n", raw_len);
    }
    free(raw);
    return 1; /* handled, don't HTTP-fetch */
}

/* ================================================================== */
/* Page building (called when HTTP response is complete)               */
/* ================================================================== */

static void build_page(void) {
    if (!g_doc) return;

    ts_doc_init(g_doc);
    /* Verify the zero actually reached rule_count */
    { char *rc_ptr = (char*)&g_doc->stylesheet.rule_count;
      fprintf(stderr, "DOC_INIT: sizeof=%zu rc_off=%zu rc=%d bytes=%02X%02X%02X%02X\n",
            sizeof(*g_doc),
            (size_t)(rc_ptr - (char*)g_doc),
            g_doc->stylesheet.rule_count,
            (unsigned char)rc_ptr[0], (unsigned char)rc_ptr[1],
            (unsigned char)rc_ptr[2], (unsigned char)rc_ptr[3]);
      /* Force it zero as safety belt */
      g_doc->stylesheet.rule_count = 0;
    }

    if (g_req.response.body && g_req.response.body_len > 0) {
        /* Check if response is HTML */
        int is_html = 1;
        if (g_req.response.content_type[0]) {
            if (!strstr(g_req.response.content_type, "html") &&
                !strstr(g_req.response.content_type, "HTML")) {
                is_html = 0;
            }
        }

        if (is_html) {
            /* Parse any <style> blocks into stylesheet first */
            /* (ts_doc_build handles this internally via the tokenizer) */
            ts_doc_build(g_doc, g_req.response.body, g_req.response.body_len);
        } else {
            /* Plain text — wrap in <pre> */
            char *wrapped = malloc(g_req.response.body_len + 32);
            if (wrapped) {
                int wlen = snprintf(wrapped, g_req.response.body_len + 32,
                                    "<pre>%s</pre>",
                                    g_req.response.body);
                ts_doc_build(g_doc, wrapped, (size_t)wlen);
                free(wrapped);
            }
        }

        ts_doc_layout(g_doc, viewport_w());

        fprintf(stderr, "PAGE: nodes=%d links=%d css=%d js=%d img=%d\n",
                g_doc->node_count, g_doc->link_count,
                g_doc->external_css_count, g_doc->external_js_count,
                g_doc->external_img_count);

        /* Update title */
        if (g_doc->title[0]) {
            /* Title could be shown in window title bar — TaterWin doesn't
               support changing title after creation, so show in status */
        }

        /* ---- DOM / JavaScript integration ---- */
        if (g_dom && is_html) {
            /* Rebuild DOM from the same HTML */
            ts_dom_destroy(g_dom);
            ts_dom_init(g_dom);

            /* Set page URL for location object */
            strncpy(g_dom->url, url_bar, sizeof(g_dom->url) - 1);

            /* Load HTML into DOM tree */
            ts_dom_load_html(g_dom, g_req.response.body,
                              g_req.response.body_len);

            /* Register window/document/console globals */
            g_doc_ref = g_doc;
            ts_dom_register_globals(g_dom);

            /* Register Web Components API */
            ts_webcomp_register(g_dom);

            /* Execute <script> tags */
            ts_dom_run_scripts(g_dom);

            /* If scripts modified the DOM, rebuild render tree from DOM */
            if (g_dom->dirty) {
                g_dom->dirty = 0;
                ts_doc_reinit(g_doc);
                ts_doc_build_from_dom(g_doc, g_dom);
                ts_doc_layout(g_doc, viewport_w());
                needs_redraw = 1;

                fprintf(stderr, "DOM_SYNC: rebuilt render tree from DOM, "
                        "nodes=%d links=%d\n",
                        g_doc->node_count, g_doc->link_count);
            }
        }
    }

    fprintf(stderr, "BUILD[1]: post-js\n");
    scroll_x = 0;
    scroll_y = 0;
    needs_redraw = 1;

    fprintf(stderr, "BUILD[2]: saving html copy len=%zu\n",
            g_req.response.body_len);
    /* Save main page HTML for relayout after resource loading.
     * ts_http_reuse_get frees the response body, so we need a copy. */
    if (g_page_html) { free(g_page_html); g_page_html = NULL; }
    if (g_req.response.body && g_req.response.body_len > 0) {
        g_page_html = (char *)malloc(g_req.response.body_len + 1);
        if (g_page_html) {
            memcpy(g_page_html, g_req.response.body, g_req.response.body_len);
            g_page_html[g_req.response.body_len] = '\0';
            g_page_html_len = g_req.response.body_len;
        }
    }
    fprintf(stderr, "BUILD[3]: enqueue resources\n");

    /* ---- Enqueue external resources for loading ---- */
    /* Same-host HTTPS resources reuse the main TLS connection via
     * keep-alive (fast, no extra handshake). Cross-host resources
     * get independent connections through ts_resource_mgr. */
    ts_resource_mgr_destroy(&g_resources);
    ts_resource_mgr_init(&g_resources, &g_cookies);
    ts_resource_mgr_set_base(&g_resources, url_bar);

    /* Reset reuse queue */
    g_reuse_count = 0;
    g_reuse_idx = 0;
    g_reuse_active = 0;

    if (g_doc) {
        int ri;
        int main_is_https = g_req.is_https && g_req.tls.initialized &&
                            g_req.sock_fd >= 0;

        /* Helper: enqueue resource — same-host HTTPS goes to reuse queue
         * (fast keep-alive), cross-host goes to resource manager
         * (independent BearSSL connection per slot). */
        #define ENQUEUE_RES(url_str, rtype) do { \
            if (main_is_https && (url_str) && \
                g_reuse_count < REUSE_QUEUE_MAX) { \
                struct ts_url _ru; \
                ts_url_resolve(&g_resources.base_url, (url_str), &_ru); \
                if (ts__strcmp(_ru.host, g_req.url.host) == 0) { \
                    char _rs[512]; \
                    ts_url_to_string(&_ru, _rs, sizeof(_rs)); \
                    strncpy(g_reuse_queue[g_reuse_count].url, _rs, \
                            sizeof(g_reuse_queue[0].url) - 1); \
                    g_reuse_queue[g_reuse_count].type = (rtype); \
                    g_reuse_count++; \
                } else { \
                    fprintf(stderr, "CROSS-HOST: %s -> %s\n", \
                            g_req.url.host, _ru.host); \
                    ts_resource_enqueue(&g_resources, (url_str), (rtype)); \
                } \
            } else { \
                ts_resource_enqueue(&g_resources, (url_str), (rtype)); \
            } \
        } while (0)

        /* External CSS */
        for (ri = 0; ri < g_doc->external_css_count; ri++)
            ENQUEUE_RES(g_doc->external_css[ri], TS_RES_CSS);
        /* External JS */
        for (ri = 0; ri < g_doc->external_js_count; ri++)
            ENQUEUE_RES(g_doc->external_js[ri], TS_RES_JS);
        /* External images — decode data: URLs inline, HTTP-fetch the rest */
        for (ri = 0; ri < g_doc->external_img_count; ri++) {
            if (!try_decode_data_url_image(g_doc->external_img[ri], ri))
                ENQUEUE_RES(g_doc->external_img[ri], TS_RES_IMAGE);
        }

        #undef ENQUEUE_RES

        g_resources_loading = ts_resource_is_busy(&g_resources) ||
                              g_reuse_count > 0;
        /* If no same-host resources to reuse, tear down main connection */
        if (g_reuse_count == 0 && main_is_https) {
            ts_tls_teardown(&g_req.tls);
            fry_close(g_req.sock_fd);
            g_req.sock_fd = -1;
        }
    } else {
        /* No document — tear down main connection */
        if (g_req.tls.initialized)
            ts_tls_teardown(&g_req.tls);
        if (g_req.sock_fd >= 0) {
            fry_close(g_req.sock_fd);
            g_req.sock_fd = -1;
        }
    }
    fprintf(stderr, "BUILD[5]: build_page done\n");
}

/* ================================================================== */
/* Process completed external resources                                */
/* ================================================================== */

/* ================================================================== */
/* TLS reuse: start next same-host resource on main connection         */
/* ================================================================== */

static void reuse_start_next(void) {
    if (g_reuse_idx >= g_reuse_count) return;
    if (g_reuse_active) return;

    fprintf(stderr, "REUSE: start url=%.80s\n", g_reuse_queue[g_reuse_idx].url);
    if (ts_http_reuse_get(&g_req, g_reuse_queue[g_reuse_idx].url) < 0) {
        fprintf(stderr, "REUSE: FAILED err=%s\n", g_req.error);
        g_reuse_idx++;
        reuse_start_next(); /* try next */
        return;
    }
    g_reuse_active = 1;
}

static void reuse_process_completion(void) {
    if (!g_reuse_active) return;
    if (g_req.state != TS_HTTP_DONE) return;

    /* Process the completed resource */
    int qi = g_reuse_idx;
    enum ts_resource_type rtype = g_reuse_queue[qi].type;
    char *data = g_req.response.body;
    size_t data_len = g_req.response.body_len;

    fprintf(stderr, "REUSE: done url=%.80s len=%u\n",
            g_reuse_queue[qi].url, (unsigned)data_len);

    if (data && data_len > 0) {
        switch (rtype) {
        case TS_RES_CSS:
            if (g_doc) {
                ts_css_parse(&g_doc->stylesheet, data, data_len);
                /* Handle @import rules — enqueue imported CSS */
                {
                    struct ts_css_import_list css_imports;
                    ts_css_extract_imports(data, data_len, &css_imports);
                    if (css_imports.count > 0) {
                        struct ts_url css_base;
                        ts_url_parse(g_reuse_queue[qi].url, &css_base);
                        int ci;
                        for (ci = 0; ci < css_imports.count; ci++) {
                            struct ts_url resolved;
                            char resolved_str[2048];
                            ts_url_resolve(&css_base, css_imports.urls[ci],
                                           &resolved);
                            ts_url_to_string(&resolved, resolved_str,
                                             sizeof(resolved_str));
                            fprintf(stderr, "CSS @import: %s\n", resolved_str);
                            ts_resource_enqueue(&g_resources, resolved_str,
                                                TS_RES_CSS);
                        }
                        g_resources_loading = 1;
                    }
                }
                /* Defer relayout until all reuse resources done */
            }
            break;
        case TS_RES_JS:
            if (g_dom && g_dom->ctx) {
                JSValue result = JS_Eval(g_dom->ctx, data, data_len,
                                          g_reuse_queue[qi].url,
                                          JS_EVAL_TYPE_GLOBAL);
                JS_FreeValue(g_dom->ctx, result);
            }
            break;
        case TS_RES_IMAGE:
            if (g_doc) {
                uint32_t *img_pixels = NULL;
                int img_w = 0, img_h = 0;
                fprintf(stderr, "IMG_DECODE: len=%d magic=%02X%02X%02X%02X%02X%02X\n",
                        (int)data_len,
                        data_len>0?(unsigned)((uint8_t*)data)[0]:0,
                        data_len>1?(unsigned)((uint8_t*)data)[1]:0,
                        data_len>2?(unsigned)((uint8_t*)data)[2]:0,
                        data_len>3?(unsigned)((uint8_t*)data)[3]:0,
                        data_len>4?(unsigned)((uint8_t*)data)[4]:0,
                        data_len>5?(unsigned)((uint8_t*)data)[5]:0);
                if (ts_image_decode((const uint8_t *)data, data_len,
                                     &img_pixels, &img_w, &img_h) == 0) {
                    fprintf(stderr, "IMG_DECODE: OK %dx%d\n", img_w, img_h);
                    /* Find a free image cache slot */
                    int ci;
                    for (ci = 0; ci < 64; ci++) {
                        if (!g_doc->img_cache[ci].used) {
                            g_doc->img_cache[ci].pixels = img_pixels;
                            g_doc->img_cache[ci].w = img_w;
                            g_doc->img_cache[ci].h = img_h;
                            g_doc->img_cache[ci].used = 1;
                            strncpy(g_doc->img_cache[ci].url,
                                    g_reuse_queue[qi].url, 511);
                            needs_redraw = 1;
                            break;
                        }
                    }
                }
            }
            break;
        }
    }

    g_reuse_active = 0;
    g_reuse_idx++;

    /* Start next reuse resource, or tear down connection if done */
    if (g_reuse_idx >= g_reuse_count) {
        /* All reuse resources done — tear down main connection */
        if (g_req.tls.initialized)
            ts_tls_teardown(&g_req.tls);
        if (g_req.sock_fd >= 0) {
            fry_close(g_req.sock_fd);
            g_req.sock_fd = -1;
        }
        g_resources_loading = ts_resource_is_busy(&g_resources);

        if (!g_resources_loading) {
            set_status("Done", COL_STATUS_OK);
            needs_redraw = 1;
        }

        /* Relayout with new images — no full re-parse needed */
        if (g_doc && g_doc->node_count > 0) {
            ts_doc_layout(g_doc, viewport_w());
            needs_redraw = 1;
        }
    } else {
        reuse_start_next();
    }
}

static void process_resource_completions(void) {
    struct ts_resource_result *r;
    int need_relayout = 0;

    while ((r = ts_resource_next_complete(&g_resources)) != NULL) {
        if (!r->data || r->data_len == 0) continue;

        switch (r->type) {
        case TS_RES_CSS:
            /* Parse CSS and merge into document stylesheet */
            if (g_doc) {
                ts_css_parse(&g_doc->stylesheet, r->data, r->data_len);
                /* Handle @import rules — enqueue imported stylesheets */
                {
                    struct ts_css_import_list css_imports;
                    ts_css_extract_imports(r->data, r->data_len, &css_imports);
                    if (css_imports.count > 0) {
                        struct ts_url css_base;
                        ts_url_parse(r->url, &css_base);
                        int ci;
                        for (ci = 0; ci < css_imports.count; ci++) {
                            struct ts_url resolved;
                            char resolved_str[2048];
                            ts_url_resolve(&css_base, css_imports.urls[ci],
                                           &resolved);
                            ts_url_to_string(&resolved, resolved_str,
                                             sizeof(resolved_str));
                            fprintf(stderr, "CSS @import: %s\n", resolved_str);
                            ts_resource_enqueue(&g_resources, resolved_str,
                                                TS_RES_CSS);
                        }
                        g_resources_loading = 1;
                    }
                }
                need_relayout = 1;
            }
            break;

        case TS_RES_JS:
            /* Execute external script via QuickJS */
            if (g_dom && g_dom->ctx) {
                JSValue result = JS_Eval(g_dom->ctx, r->data,
                                          r->data_len, r->url,
                                          JS_EVAL_TYPE_GLOBAL);
                JS_FreeValue(g_dom->ctx, result);
            }
            break;

        case TS_RES_IMAGE:
            /* Decode image and store in document image cache.
             * Match by URL: r->cache_idx is the resource manager's internal
             * index (shared across CSS/JS/images), NOT the external_img index
             * that render nodes reference via img_cache_idx. We must resolve
             * each external_img URL and compare to find the correct slot. */
            if (g_doc) {
                uint32_t *img_pixels = NULL;
                int img_w = 0, img_h = 0;
                fprintf(stderr, "CROSS_IMG: decode len=%u url=%.80s\n",
                        (unsigned)r->data_len, r->url);
                if (ts_image_decode((const uint8_t *)r->data, r->data_len,
                                     &img_pixels, &img_w, &img_h) == 0) {
                    int found = 0;
                    int ei;
                    fprintf(stderr, "CROSS_IMG: decoded %dx%d, matching against %d external_imgs\n",
                            img_w, img_h, g_doc->external_img_count);
                    for (ei = 0; ei < g_doc->external_img_count && !found; ei++) {
                        struct ts_url resolved;
                        char resolved_str[2048];
                        ts_url_resolve(&g_resources.base_url,
                                       g_doc->external_img[ei], &resolved);
                        ts_url_to_string(&resolved, resolved_str,
                                         sizeof(resolved_str));
                        if (strcmp(resolved_str, r->url) == 0) {
                            g_doc->img_cache[ei].pixels = img_pixels;
                            g_doc->img_cache[ei].w = img_w;
                            g_doc->img_cache[ei].h = img_h;
                            g_doc->img_cache[ei].used = 1;
                            strncpy(g_doc->img_cache[ei].url, r->url, 511);
                            needs_redraw = 1;
                            found = 1;
                            fprintf(stderr, "CROSS_IMG: stored at img_cache[%d]\n", ei);
                        }
                    }
                    if (!found) {
                        fprintf(stderr, "CROSS_IMG: NO MATCH for %.80s\n", r->url);
                        free(img_pixels);
                    }
                } else {
                    fprintf(stderr, "CROSS_IMG: decode FAILED (unsupported format?)\n");
                }
            }
            break;
        }
    }
    ts_resource_completions_clear(&g_resources);

    /* If CSS changed, re-parse HTML with updated stylesheet and re-layout */
    if (need_relayout && g_doc && g_page_html) {
        ts_doc_reinit(g_doc);
        ts_doc_build(g_doc, g_page_html, g_page_html_len);
        ts_doc_layout(g_doc, viewport_w());
        needs_redraw = 1;
    }

    /* Update loading state */
    g_resources_loading = ts_resource_is_busy(&g_resources) ||
                          (g_reuse_idx < g_reuse_count);
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

static void render_toolbar(gfx_ctx_t *ctx) {
    int y = 0;
    int btn_y = (TOOLBAR_H - URL_BAR_H) / 2;

    /* Toolbar background */
    gfx_fill(ctx, 0, 0, (uint32_t)win_w, TOOLBAR_H, COL_TOOLBAR);
    /* Bottom border */
    gfx_fill(ctx, 0, TOOLBAR_H - 1, (uint32_t)win_w, 1, COL_URL_BORDER);

    /* Back button */
    {
        int bx = BTN_GAP;
        gfx_fill_rounded(ctx, (uint32_t)bx, (uint32_t)btn_y, BTN_W, URL_BAR_H,
                          COL_BTN_BG, 2);
        gfx_draw_char(ctx, (uint32_t)(bx + 10), (uint32_t)(btn_y + 4),
                       '<', history_pos > 0 ? COL_BTN_FG : COL_URL_BORDER,
                       COL_TRANS);
    }

    /* Forward button */
    {
        int bx = BTN_GAP + BTN_W + BTN_GAP;
        gfx_fill_rounded(ctx, (uint32_t)bx, (uint32_t)btn_y, BTN_W, URL_BAR_H,
                          COL_BTN_BG, 2);
        gfx_draw_char(ctx, (uint32_t)(bx + 10), (uint32_t)(btn_y + 4),
                       '>', history_pos < history_count - 1 ? COL_BTN_FG : COL_URL_BORDER,
                       COL_TRANS);
    }

    /* Reload button */
    {
        int bx = BTN_GAP + (BTN_W + BTN_GAP) * 2;
        gfx_fill_rounded(ctx, (uint32_t)bx, (uint32_t)btn_y, BTN_W, URL_BAR_H,
                          COL_BTN_BG, 2);
        gfx_draw_char(ctx, (uint32_t)(bx + 10), (uint32_t)(btn_y + 4),
                       'R', COL_BTN_FG, COL_TRANS);
    }

    /* URL bar */
    {
        int ubx = URL_BAR_X;
        int ubw = win_w - URL_BAR_X - BTN_GAP;
        if (ubw < 100) ubw = 100;

        gfx_fill(ctx, (uint32_t)ubx, (uint32_t)btn_y,
                 (uint32_t)ubw, URL_BAR_H, COL_URL_BG);
        gfx_rect(ctx, (uint32_t)ubx, (uint32_t)btn_y,
                 (uint32_t)ubw, URL_BAR_H,
                 url_bar_focused ? COL_URL_FOCUS : COL_URL_BORDER);

        /* TLS indicator */
        int text_x = ubx + 6;
        if (g_req.is_https && g_req.state == TS_HTTP_DONE) {
            gfx_draw_char(ctx, (uint32_t)text_x, (uint32_t)(btn_y + 4),
                           'S', COL_STATUS_OK, COL_TRANS);
            text_x += 10;
        }

        /* URL text (with horizontal scroll) */
        {
            int max_chars = (ubw - 12) / 8;
            int display_start = url_bar_scroll;
            int display_len = url_bar_len - display_start;
            if (display_len > max_chars) display_len = max_chars;
            if (display_len < 0) display_len = 0;
            int i;
            for (i = 0; i < display_len; i++) {
                gfx_draw_char(ctx, (uint32_t)(text_x + i * 8),
                              (uint32_t)(btn_y + 4),
                              url_bar[display_start + i],
                              COL_URL_TEXT, COL_TRANS);
            }
            /* Cursor */
            if (url_bar_focused) {
                int cx = text_x + (url_bar_cursor - display_start) * 8;
                if (cx >= ubx + 4 && cx < ubx + ubw - 4)
                    gfx_fill(ctx, (uint32_t)cx, (uint32_t)(btn_y + 3),
                             1, URL_BAR_H - 6, COL_URL_FOCUS);
            }
        }
    }

    (void)y;
}

static void render_status_bar(gfx_ctx_t *ctx) {
    int sy = win_h - STATUS_H;

    gfx_fill(ctx, 0, (uint32_t)sy, (uint32_t)win_w, STATUS_H, COL_STATUS_BG);
    gfx_fill(ctx, 0, (uint32_t)sy, (uint32_t)win_w, 1, COL_URL_BORDER);

    /* Status text (left) */
    gfx_draw_text(ctx, 8, (uint32_t)(sy + 4), status_text,
                   status_color, COL_TRANS);

    /* Page title (center) */
    if (g_doc && g_doc->title[0]) {
        int title_x = win_w / 2 - (int)strlen(g_doc->title) * 4;
        if (title_x < 200) title_x = 200;
        gfx_draw_text(ctx, (uint32_t)title_x, (uint32_t)(sy + 4),
                       g_doc->title, COL_STATUS_TEXT, COL_TRANS);
    }

    /* Bytes loaded (right) */
    {
        char bbuf[32];
        if (bytes_loaded >= 1024 * 1024)
            snprintf(bbuf, sizeof(bbuf), "%u.%u MB",
                     (unsigned)(bytes_loaded / (1024 * 1024)),
                     (unsigned)((bytes_loaded / 102400) % 10));
        else if (bytes_loaded >= 1024)
            snprintf(bbuf, sizeof(bbuf), "%u.%u KB",
                     (unsigned)(bytes_loaded / 1024),
                     (unsigned)((bytes_loaded / 102) % 10));
        else
            snprintf(bbuf, sizeof(bbuf), "%u B", (unsigned)bytes_loaded);
        int bx = win_w - (int)strlen(bbuf) * 8 - 8;
        if (bx < 0) bx = 0;
        gfx_draw_text(ctx, (uint32_t)bx, (uint32_t)(sy + 4),
                       bbuf, COL_STATUS_TEXT, COL_TRANS);
    }
}

static void render_scrollbar(gfx_ctx_t *ctx) {
    if (!g_doc || g_doc->content_height <= viewport_h()) return;

    int sb_x = win_w - 8;
    int sb_y = TOOLBAR_H;
    int sb_h = viewport_h();
    int track_h = sb_h;

    /* Track */
    gfx_fill(ctx, (uint32_t)sb_x, (uint32_t)sb_y, 8, (uint32_t)track_h,
             COL_SCROLL_TRACK);

    /* Thumb */
    int content_h = g_doc->content_height;
    if (content_h < 1) content_h = 1;
    int thumb_h = (viewport_h() * track_h) / content_h;
    if (thumb_h < 20) thumb_h = 20;
    if (thumb_h > track_h) thumb_h = track_h;
    int thumb_y = sb_y + (scroll_y * (track_h - thumb_h)) / (content_h - viewport_h());
    if (thumb_y < sb_y) thumb_y = sb_y;
    if (thumb_y + thumb_h > sb_y + track_h) thumb_y = sb_y + track_h - thumb_h;

    gfx_fill_rounded(ctx, (uint32_t)sb_x, (uint32_t)thumb_y,
                      8, (uint32_t)thumb_h, COL_SCROLL_THUMB, 3);
}

static void render(void) {
    if (!render_buf) return;

    /* Clear */
    gfx_fill(&g_ctx, 0, 0, (uint32_t)win_w, (uint32_t)win_h, COL_BG);

    /* Page content */
    if (g_doc && g_doc->node_count > 0) {
        /* Create a sub-context for the viewport area */
        gfx_ctx_t vp_ctx;
        gfx_init(&vp_ctx, render_buf + TOOLBAR_H * (uint32_t)win_w,
                 (uint32_t)viewport_w(), (uint32_t)viewport_h(), (uint32_t)win_w);
        ts_doc_render(g_doc, &vp_ctx, scroll_x, scroll_y,
                       viewport_w(), viewport_h());
    }

    /* Video overlay: blit decoded frame into viewport if playing */
    if (g_media.rgb_frame && g_media.frame_w > 0 && g_media.frame_h > 0 &&
        g_media.state >= MEDIA_FETCH_SEG_V) {
        int vw = viewport_w();
        int vh = viewport_h();
        /* Fit video to viewport preserving aspect ratio */
        int blit_w = vw;
        int blit_h = (g_media.frame_h * vw) / g_media.frame_w;
        if (blit_h > vh) {
            blit_h = vh;
            blit_w = (g_media.frame_w * vh) / g_media.frame_h;
        }
        int vx = (vw - blit_w) / 2;
        int vy = TOOLBAR_H + (vh - blit_h) / 2;

        ts_video_blit(g_media.rgb_frame, g_media.frame_w, g_media.frame_h,
                       render_buf, win_w, vx, vy, blit_w, blit_h);
    }

    /* UI chrome (drawn on top) */
    render_toolbar(&g_ctx);
    render_status_bar(&g_ctx);
    render_scrollbar(&g_ctx);

    /* Copy to SHM and notify compositor */
    memcpy(pixels, render_buf, (size_t)win_w * (size_t)win_h * 4);
    fry_write(1, &g_upd, sizeof(g_upd));
}

/* ================================================================== */
/* Scroll management                                                   */
/* ================================================================== */

static void scroll_clamp(void) {
    int max_scroll = 0;
    if (g_doc && g_doc->content_height > viewport_h())
        max_scroll = g_doc->content_height - viewport_h();
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    if (scroll_x < 0) scroll_x = 0;
}

static void scroll_by(int dx, int dy) {
    scroll_x += dx;
    scroll_y += dy;
    scroll_clamp();
    needs_redraw = 1;
}

/* ================================================================== */
/* TaterWin message processing                                         */
/* ================================================================== */

static int tw_msg_size(uint32_t type) {
    switch (type) {
        case TW_MSG_KEY_EVENT:      return (int)sizeof(tw_msg_key_t);
        case TW_MSG_MOUSE_EVENT:    return (int)sizeof(tw_msg_mouse_t);
        case TW_MSG_MOUSE_MOVE:     return (int)sizeof(tw_msg_mouse_move_t);
        case TW_MSG_WHEEL_EVENT:    return (int)sizeof(tw_msg_wheel_t);
        case TW_MSG_FOCUS_EVENT:    return (int)sizeof(tw_msg_focus_t);
        case TW_MSG_ENTER_LEAVE:    return (int)sizeof(tw_msg_enter_leave_t);
        case TW_MSG_CLOSE_REQUEST:  return (int)sizeof(tw_msg_close_request_t);
        case TW_MSG_CLIPBOARD_DATA: return (int)sizeof(tw_msg_clipboard_data_t);
        case TW_MSG_RESIZED:        return (int)sizeof(tw_msg_resized_t);
        default: return 0;
    }
}

static void input_consume(int n) {
    if (n <= 0 || n > input_stream_len) return;
    memmove(input_stream, input_stream + n, (size_t)(input_stream_len - n));
    input_stream_len -= n;
}

static void handle_key(tw_msg_key_t *key) {
    if (key->flags != FRY_KEY_PRESSED) return; /* only pressed events */

    /* ---- URL bar focused: typing mode ---- */
    if (url_bar_focused) {
        /* Enter: navigate */
        if (key->ascii == '\r' || key->ascii == '\n') {
            url_bar_focused = 0;
            navigate(url_bar);
            return;
        }
        /* Escape: unfocus, restore current URL */
        if (key->vk == 0x1B || key->scancode == 0x01) {
            url_bar_focused = 0;
            if (history_pos >= 0)
                url_bar_set(history[history_pos]);
            needs_redraw = 1;
            return;
        }
        /* Backspace */
        if (key->ascii == '\b' || key->ascii == 127) {
            if (url_bar_cursor > 0) {
                memmove(url_bar + url_bar_cursor - 1,
                        url_bar + url_bar_cursor,
                        (size_t)(url_bar_len - url_bar_cursor + 1));
                url_bar_cursor--;
                url_bar_len--;
                needs_redraw = 1;
            }
            return;
        }
        /* Printable character */
        if (key->ascii >= 0x20 && key->ascii < 0x7F) {
            if (url_bar_len < (int)sizeof(url_bar) - 1) {
                memmove(url_bar + url_bar_cursor + 1,
                        url_bar + url_bar_cursor,
                        (size_t)(url_bar_len - url_bar_cursor + 1));
                url_bar[url_bar_cursor] = (char)key->ascii;
                url_bar_cursor++;
                url_bar_len++;
                needs_redraw = 1;
            }
            return;
        }
        /* Left/Right arrows for cursor movement */
        if (key->vk == FRY_VK_LEFT && url_bar_cursor > 0) {
            url_bar_cursor--;
            needs_redraw = 1;
            return;
        }
        if (key->vk == FRY_VK_RIGHT && url_bar_cursor < url_bar_len) {
            url_bar_cursor++;
            needs_redraw = 1;
            return;
        }
        /* Home/End in URL bar */
        if (key->vk == FRY_VK_HOME) { url_bar_cursor = 0; needs_redraw = 1; return; }
        if (key->vk == FRY_VK_END) { url_bar_cursor = url_bar_len; needs_redraw = 1; return; }
        return;
    }

    /* ---- Form input mode ---- */
    if (g_doc && g_doc->focused_input >= 0 &&
        g_doc->focused_input < g_doc->form_input_count) {
        struct ts_form_input *fi = &g_doc->form_inputs[g_doc->focused_input];
        /* Enter: submit form (navigate with query string) */
        if (key->ascii == '\r' || key->ascii == '\n') {
            /* Build search URL: action?name=value */
            char query[2048] = {0};
            int qi = 0;
            int fi_idx;
            for (fi_idx = 0; fi_idx < g_doc->form_input_count; fi_idx++) {
                struct ts_form_input *f = &g_doc->form_inputs[fi_idx];
                if (f->name[0] && f->value_len > 0 &&
                    strcmp(f->type, "submit") != 0) {
                    if (qi > 0) query[qi++] = '&';
                    /* URL-encode name=value */
                    {
                        const char *s;
                        for (s = f->name; *s && qi < 2040; s++) {
                            if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                                (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')
                                query[qi++] = *s;
                            else if (*s == ' ')
                                query[qi++] = '+';
                            else {
                                query[qi++] = '%';
                                query[qi++] = "0123456789ABCDEF"[((uint8_t)*s >> 4) & 0xF];
                                query[qi++] = "0123456789ABCDEF"[(uint8_t)*s & 0xF];
                            }
                        }
                    }
                    query[qi++] = '=';
                    {
                        int vi;
                        for (vi = 0; vi < f->value_len && qi < 2040; vi++) {
                            char c = f->value[vi];
                            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
                                query[qi++] = c;
                            else if (c == ' ')
                                query[qi++] = '+';
                            else {
                                query[qi++] = '%';
                                query[qi++] = "0123456789ABCDEF"[((uint8_t)c >> 4) & 0xF];
                                query[qi++] = "0123456789ABCDEF"[(uint8_t)c & 0xF];
                            }
                        }
                    }
                }
            }
            query[qi] = '\0';
            /* Determine action URL */
            {
                char action_url[2048];
                if (fi->action[0]) {
                    /* Resolve relative action URL */
                    struct ts_url base, resolved;
                    ts_url_parse(url_bar, &base);
                    ts_url_resolve(&base, fi->action, &resolved);
                    ts_url_to_string(&resolved, action_url, sizeof(action_url));
                } else {
                    /* Use current page as form action */
                    struct ts_url base;
                    ts_url_parse(url_bar, &base);
                    base.query[0] = '\0';
                    ts_url_to_string(&base, action_url, sizeof(action_url));
                }

                /* POST or GET based on form method */
                if (fi->method[0] == 'P' || fi->method[0] == 'p') {
                    navigate_post(action_url, query, (size_t)qi);
                } else {
                    /* GET: append query string to URL */
                    char nav_url[2048];
                    if (qi > 0)
                        snprintf(nav_url, sizeof(nav_url), "%s?%s", action_url, query);
                    else
                        strncpy(nav_url, action_url, sizeof(nav_url) - 1);
                    navigate(nav_url);
                }
            }
            return;
        }
        /* Escape: unfocus */
        if (key->vk == 0x1B || key->scancode == 0x01) {
            fi->focused = 0;
            g_doc->focused_input = -1;
            needs_redraw = 1;
            return;
        }
        /* Tab: move to next input or URL bar */
        if (key->ascii == '\t') {
            fi->focused = 0;
            g_doc->focused_input = -1;
            url_bar_focused = 1;
            url_bar_cursor = url_bar_len;
            needs_redraw = 1;
            return;
        }
        /* Backspace */
        if (key->ascii == '\b' || key->ascii == 127) {
            if (fi->cursor > 0 && fi->value_len > 0) {
                memmove(fi->value + fi->cursor - 1,
                        fi->value + fi->cursor,
                        (size_t)(fi->value_len - fi->cursor + 1));
                fi->cursor--;
                fi->value_len--;
                needs_redraw = 1;
            }
            return;
        }
        /* Printable character */
        if (key->ascii >= 0x20 && key->ascii < 0x7F) {
            if (fi->value_len < (int)sizeof(fi->value) - 1) {
                memmove(fi->value + fi->cursor + 1,
                        fi->value + fi->cursor,
                        (size_t)(fi->value_len - fi->cursor + 1));
                fi->value[fi->cursor] = (char)key->ascii;
                fi->cursor++;
                fi->value_len++;
                needs_redraw = 1;
            }
            return;
        }
        /* Arrow keys */
        if (key->vk == FRY_VK_LEFT && fi->cursor > 0) {
            fi->cursor--;
            needs_redraw = 1;
            return;
        }
        if (key->vk == FRY_VK_RIGHT && fi->cursor < fi->value_len) {
            fi->cursor++;
            needs_redraw = 1;
            return;
        }
        return; /* absorb all keys when input focused */
    }

    /* ---- Page viewing mode ---- */

    /* Tab: focus URL bar */
    if (key->ascii == '\t') {
        url_bar_focused = 1;
        url_bar_cursor = url_bar_len; /* cursor at end */
        needs_redraw = 1;
        return;
    }

    /* L key: focus URL bar (like Ctrl+L) */
    if ((key->ascii == 'l' || key->ascii == 'L') && (key->mods & FRY_MOD_CTRL)) {
        url_bar_focused = 1;
        /* Select all (set cursor to end, scroll to 0) */
        url_bar_cursor = url_bar_len;
        url_bar_scroll = 0;
        needs_redraw = 1;
        return;
    }

    /* Scroll keys */
    if (key->vk == FRY_VK_UP)    { scroll_by(0, -32); return; }
    if (key->vk == FRY_VK_DOWN)  { scroll_by(0,  32); return; }
    if (key->vk == FRY_VK_PGUP)  { scroll_by(0, -viewport_h()); return; }
    if (key->vk == FRY_VK_PGDN)  { scroll_by(0,  viewport_h()); return; }
    if (key->vk == FRY_VK_HOME)  { scroll_y = 0; needs_redraw = 1; return; }
    if (key->vk == FRY_VK_END) {
        if (g_doc) scroll_y = g_doc->content_height - viewport_h();
        scroll_clamp();
        needs_redraw = 1;
        return;
    }

    /* Space: scroll down one page */
    if (key->ascii == ' ') { scroll_by(0, viewport_h() - 40); return; }

    /* Backspace: go back */
    if (key->ascii == '\b') {
        const char *prev = history_back();
        if (prev) navigate(prev);
        return;
    }

    /* R: reload */
    if (key->ascii == 'r' || key->ascii == 'R') {
        if (history_pos >= 0)
            navigate(history[history_pos]);
        return;
    }

    /* Q: quit */
    if (key->ascii == 'q' || key->ascii == 'Q')
        fry_exit(0);

    /* Dispatch to DOM event system */
    if (g_dom && g_dom->body_node >= 0) {
        struct ts_dom_event ev;
        memset(&ev, 0, sizeof(ev));
        strncpy(ev.type, (key->flags == 1) ? "keydown" : "keyup",
                sizeof(ev.type) - 1);
        ev.key_code = key->vk;
        ev.bubbles = 1;
        ev.cancelable = 1;
        if (key->ascii >= 0x20 && key->ascii < 0x7F)
            ev.key[0] = (char)key->ascii;
        ts_dom_dispatch_event(g_dom, g_dom->body_node, &ev);
    }
}

static void handle_mouse_click(int x, int y, uint8_t btns) {
    fprintf(stderr, "CLICK: x=%d y=%d btns=%d\n", x, y, btns);
    if (btns == 0) return; /* release event */

    /* ---- Toolbar clicks ---- */
    if (y < TOOLBAR_H) {
        /* Back button */
        if (x >= BTN_GAP && x < BTN_GAP + BTN_W) {
            const char *prev = history_back();
            if (prev) navigate(prev);
            return;
        }
        /* Forward button */
        if (x >= BTN_GAP + BTN_W + BTN_GAP &&
            x < BTN_GAP + BTN_W + BTN_GAP + BTN_W) {
            const char *next = history_forward();
            if (next) navigate(next);
            return;
        }
        /* Reload button */
        if (x >= BTN_GAP + (BTN_W + BTN_GAP) * 2 &&
            x < BTN_GAP + (BTN_W + BTN_GAP) * 2 + BTN_W) {
            if (history_pos >= 0) navigate(history[history_pos]);
            return;
        }
        /* URL bar click */
        if (x >= URL_BAR_X) {
            url_bar_focused = 1;
            /* Place cursor near click position */
            int char_pos = (x - URL_BAR_X - 6) / 8 + url_bar_scroll;
            if (char_pos < 0) char_pos = 0;
            if (char_pos > url_bar_len) char_pos = url_bar_len;
            url_bar_cursor = char_pos;
            needs_redraw = 1;
            return;
        }
        return;
    }

    /* ---- Viewport clicks: link + form input detection ---- */
    if (y >= TOOLBAR_H && y < win_h - STATUS_H && g_doc) {
        int vx = x - viewport_x();
        int vy = y - viewport_y();
        int link_idx = ts_doc_hit_test(g_doc, vx, vy, scroll_x, scroll_y);
        fprintf(stderr, "HIT: vx=%d vy=%d+scroll=%d link=%d/%d\n",
                vx, vy, scroll_y, link_idx, g_doc->link_count);
        if (link_idx >= 0) {
            navigate_link(link_idx);
            return;
        }
        /* Check form input click */
        {
            int input_idx = ts_doc_hit_test_input(g_doc, vx, vy,
                                                    scroll_x, scroll_y);
            if (input_idx >= 0) {
                /* Unfocus previous input */
                if (g_doc->focused_input >= 0 &&
                    g_doc->focused_input < g_doc->form_input_count)
                    g_doc->form_inputs[g_doc->focused_input].focused = 0;
                /* Focus new input */
                g_doc->focused_input = input_idx;
                g_doc->form_inputs[input_idx].focused = 1;
                g_doc->form_inputs[input_idx].cursor =
                    g_doc->form_inputs[input_idx].value_len;
                url_bar_focused = 0;
                needs_redraw = 1;
                return;
            }
        }
        /* Unfocus URL bar and form inputs if clicking viewport */
        if (url_bar_focused) {
            url_bar_focused = 0;
            needs_redraw = 1;
        }
        if (g_doc->focused_input >= 0) {
            g_doc->form_inputs[g_doc->focused_input].focused = 0;
            g_doc->focused_input = -1;
            needs_redraw = 1;
        }

        /* Dispatch click to DOM event system */
        if (g_dom && g_dom->body_node >= 0) {
            struct ts_dom_event ev;
            memset(&ev, 0, sizeof(ev));
            strncpy(ev.type, "click", sizeof(ev.type) - 1);
            ev.client_x = vx + scroll_x;
            ev.client_y = vy + scroll_y;
            ev.button = 0;
            ev.bubbles = 1;
            ev.cancelable = 1;
            ts_dom_dispatch_event(g_dom, g_dom->body_node, &ev);
        }
    }
}

static void handle_wheel(int delta) {
    scroll_by(0, -delta * 48);
}

static void handle_resize(int new_shm_id, uint64_t new_shm_ptr,
                           int new_w, int new_h) {
    uint32_t *new_buf;
    (void)new_shm_ptr;

    if (new_w <= 0 || new_h <= 0) return;

    if (new_shm_id >= 0) {
        long ptr = fry_shm_map(new_shm_id);
        if (ptr > 0) {
            pixels = (uint32_t *)(uintptr_t)ptr;
            g_shm_id = new_shm_id;
        }
    }

    new_buf = (uint32_t *)malloc((size_t)new_w * (size_t)new_h * 4);
    if (!new_buf) return;
    free(render_buf);
    render_buf = new_buf;
    win_w = new_w;
    win_h = new_h;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    /* Re-layout for new width */
    if (g_doc && g_doc->node_count > 0) {
        ts_doc_layout(g_doc, viewport_w());
    }

    needs_redraw = 1;
}

static void process_input(void) {
    while (input_stream_len >= (int)sizeof(tw_msg_header_t)) {
        tw_msg_header_t hdr;
        int msg_size;

        memcpy(&hdr, input_stream, sizeof(hdr));
        if (hdr.magic != TW_MAGIC) {
            input_consume(1);
            continue;
        }

        msg_size = tw_msg_size(hdr.type);
        if (msg_size == 0) {
            input_consume((int)sizeof(tw_msg_header_t));
            continue;
        }
        if (input_stream_len < msg_size) break;

        switch (hdr.type) {
        case TW_MSG_KEY_EVENT: {
            tw_msg_key_t k;
            memcpy(&k, input_stream, sizeof(k));
            handle_key(&k);
            break;
        }
        case TW_MSG_MOUSE_EVENT: {
            tw_msg_mouse_t m;
            memcpy(&m, input_stream, sizeof(m));
            handle_mouse_click(m.x, m.y, m.btns);
            break;
        }
        case TW_MSG_WHEEL_EVENT: {
            tw_msg_wheel_t w;
            memcpy(&w, input_stream, sizeof(w));
            handle_wheel(w.delta);
            break;
        }
        case TW_MSG_FOCUS_EVENT:
            needs_redraw = 1;
            break;
        case TW_MSG_CLOSE_REQUEST:
            fry_exit(0);
            break;
        case TW_MSG_RESIZED: {
            tw_msg_resized_t r;
            memcpy(&r, input_stream, sizeof(r));
            handle_resize(r.shm_id, r.shm_ptr, r.new_w, r.new_h);
            break;
        }
        default:
            break;
        }

        input_consume(msg_size);
    }
}

/* ================================================================== */
/* Window creation                                                     */
/* ================================================================== */

static int create_window(void) {
    tw_msg_create_win_t req;
    uint8_t winbuf[sizeof(tw_msg_win_created_t)];
    int winlen = 0;
    int tries = 0;

    memset(&req, 0, sizeof(req));
    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = INIT_W;
    req.h = INIT_H;
    strncpy(req.title, "TaterSurf", sizeof(req.title) - 1);
    fry_write(1, &req, sizeof(req));

    while (tries < 300) {
        uint8_t in[32];
        long n = fry_read(0, in, sizeof(in));
        if (n > 0) {
            long i;
            for (i = 0; i < n; i++) {
                if (winlen < (int)sizeof(winbuf))
                    winbuf[winlen++] = in[i];
                else {
                    memmove(winbuf, winbuf + 1, sizeof(winbuf) - 1);
                    winbuf[sizeof(winbuf) - 1] = in[i];
                }
                if (winlen == (int)sizeof(winbuf)) {
                    tw_msg_win_created_t cand;
                    memcpy(&cand, winbuf, sizeof(cand));
                    if (cand.hdr.magic == TW_MAGIC &&
                        cand.hdr.type == TW_MSG_WINDOW_CREATED &&
                        cand.shm_id >= 0) {
                        long ptr = fry_shm_map(cand.shm_id);
                        if (ptr > 0) {
                            pixels = (uint32_t *)(uintptr_t)ptr;
                            g_shm_id = cand.shm_id;
                        }
                        return 0;
                    }
                }
            }
        } else {
            fry_sleep(10);
            tries++;
        }
    }
    return -1;
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    struct fry_pollfd pfds[8]; /* stdin + main HTTP + up to 4 resource + media */
    int nfds;

    /* Allocate document on heap (it's large — ~1MB) */
    g_doc = (struct ts_document *)malloc(sizeof(struct ts_document));
    if (!g_doc) return 1;
    ts_doc_init(g_doc);

    /* Allocate DOM/JS context */
    g_dom = (struct ts_dom_ctx *)malloc(sizeof(struct ts_dom_ctx));
    if (g_dom) ts_dom_init(g_dom);

    /* Initialize cookie jar */
    ts_cookie_jar_init(&g_cookies);

    /* Initialize HTTP request */
    ts_http_init(&g_req);

    /* Initialize resource manager */
    ts_resource_mgr_init(&g_resources, &g_cookies);

    /* Initialize media pipeline */
    memset(&g_media, 0, sizeof(g_media));

    /* Initialize TrueType font (best-effort — falls back to bitmap) */
    { int fr = ts_font_init();
      fprintf(stderr, "FONT: init=%d loaded=%d\n", fr, ts_font_is_loaded());
    }

    /* Create TaterWin window */
    if (create_window() < 0)
        pixels = fallback_pixels;

    /* Allocate render buffer */
    render_buf = (uint32_t *)malloc((size_t)win_w * (size_t)win_h * 4);
    if (!render_buf) return 1;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    g_upd.type = TW_MSG_UPDATE;
    g_upd.magic = TW_MAGIC;

    /* Load persisted cookies from previous sessions */
    ts_cookie_jar_load(&g_cookies);

    set_status("TaterSurf ready — press Tab to enter URL", COL_STATUS_OK);

    /* Auto-navigate to Google (HTTPS) */
    navigate("https://www.google.com/");

    /* ---- Main event loop ---- */
    for (;;) {
        nfds = 0;

        /* Always poll FD 0 (TaterWin input) */
        pfds[nfds].fd = 0;
        pfds[nfds].events = FRY_POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        /* Poll HTTP socket if loading (including TLS handshake) */
        if (((g_req.state >= TS_HTTP_TLS_HANDSHAKE &&
              g_req.state <= TS_HTTP_RECV_CHUNKED)) &&
            g_req.sock_fd >= 0) {
            pfds[nfds].fd = g_req.sock_fd;
            pfds[nfds].events = FRY_POLLIN |
                (g_req.state == TS_HTTP_TLS_HANDSHAKE ? FRY_POLLOUT : 0);
            pfds[nfds].revents = 0;
            nfds++;
        }

        /* Poll resource loader sockets */
        if (g_resources_loading) {
            nfds += ts_resource_get_pollfds(&g_resources, pfds + nfds);
        }

        /* Poll with 33ms timeout (~30fps) */
        fry_poll(pfds, (uint32_t)nfds, 33);

        /* Process TaterWin input */
        if (pfds[0].revents & (FRY_POLLIN | FRY_POLLERR | FRY_POLLHUP)) {
            uint8_t buf[256];
            long n = fry_read(0, buf, sizeof(buf));
            if (n > 0) {
                int copy = (int)n;
                if (input_stream_len + copy > INPUT_STREAM_MAX)
                    copy = INPUT_STREAM_MAX - input_stream_len;
                if (copy > 0) {
                    memcpy(input_stream + input_stream_len, buf, (size_t)copy);
                    input_stream_len += copy;
                }
                process_input();
            }
        }

        /* Process HTTP response (or TLS handshake progress) */
        if (g_req.state >= TS_HTTP_TLS_HANDSHAKE &&
            g_req.state <= TS_HTTP_RECV_CHUNKED &&
            g_req.sock_fd >= 0) {
            int rc = ts_http_poll(&g_req);
            bytes_loaded = g_req.response.body_len;

            if (rc == 1) {
                /* Response complete */
                if (ts_http_is_redirect(&g_req)) {
                    set_status("Redirecting...", COL_STATUS_LOAD);
                    if (ts_http_follow_redirect(&g_req) < 0) {
                        set_status(g_req.error, COL_STATUS_ERR);
                    }
                } else {
                    if (g_reuse_active) {
                        /* Reuse fetch completed — process resource */
                        reuse_process_completion();
                    } else {
                        /* Main page loaded — build and render */
                        {
                            char sb[128];
                            snprintf(sb, sizeof(sb), "Done — %u bytes",
                                     (unsigned)g_req.response.body_len);
                            set_status(sb, COL_STATUS_OK);
                        }
                        build_page();
                        /* Reuse fetches start on next poll iteration
                         * (build_page uses deep stack; avoid overflow) */
                    }
                }
            } else if (rc < 0) {
                set_status(g_req.error, COL_STATUS_ERR);
            } else {
                /* Still loading — update status */
                char sb[128];
                snprintf(sb, sizeof(sb), "Loading... %u bytes",
                         (unsigned)bytes_loaded);
                set_status(sb, COL_STATUS_LOAD);
                needs_redraw = 1;
            }
        }

        /* Start deferred reuse fetch (separated from build_page to avoid
         * stack overflow — build_page uses deep stack for HTML/CSS parsing) */
        if (g_reuse_count > 0 && g_reuse_idx < g_reuse_count &&
            !g_reuse_active && g_req.state == TS_HTTP_DONE) {
            reuse_start_next();
        }

        /* Tick external resource loader + process completions */
        if (g_resources_loading) {
            ts_resource_tick(&g_resources);
            process_resource_completions();
            if (g_resources_loading) {
                char sb[128];
                snprintf(sb, sizeof(sb), "Loading resources... (%d queued)",
                         g_resources.queue_count);
                set_status(sb, COL_STATUS_LOAD);
            } else {
                set_status("Done", COL_STATUS_OK);
                needs_redraw = 1;
            }
        }

        /* Tick JS timers (setTimeout, setInterval, requestAnimationFrame) */
        if (g_dom) {
            ts_dom_tick_timers(g_dom);
            if (g_dom->dirty && g_doc) {
                g_dom->dirty = 0;
                ts_doc_reinit(g_doc);
                ts_doc_build_from_dom(g_doc, g_dom);
                ts_doc_layout(g_doc, viewport_w());
                needs_redraw = 1;
            }
        }

        /* Tick media pipeline (non-blocking segment fetch + decode) */
        media_tick();

        /* Render if needed */
        if (needs_redraw) {
            render();
            needs_redraw = 0;
        }
    }

    return 0;
}
