// netmgr.fry — TaterWin WiFi network manager

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

#include <stdint.h>

#define INIT_W 820
#define INIT_H 520

#define INPUT_STREAM_MAX 256
#define LOG_LINES 8
#define LOG_LEN   96
#define PASS_MAX  95

#define COL_BG_0      0x0C1218
#define COL_BG_1      0x131D27
#define COL_PANEL     0x18222D
#define COL_PANEL_2   0x1E2A36
#define COL_BORDER    0x324353
#define COL_TEXT      0xE7EEF7
#define COL_TEXT_DIM  0x92A5B9
#define COL_ACCENT    0x4CC9F0
#define COL_ACCENT_2  0x43AA8B
#define COL_WARN      0xF4C96A
#define COL_DANGER    0xE76F51
#define COL_OK        0x52B788
#define COL_SELECT    0x263747
#define COL_FIELD     0x101820
#define COL_BUTTON    0x2C90A9
#define COL_BUTTON_HI 0x44B3D0
#define COL_TRANS     0xFF000000

typedef struct {
    int x, y, w, h;
} rect_t;

static uint32_t fallback_pixels[INIT_W * INIT_H];
static uint32_t *pixels = fallback_pixels;
static uint32_t *render_buf = 0;
static gfx_ctx_t g_ctx;
static int g_shm_id = -1;
static int win_w = INIT_W;
static int win_h = INIT_H;

static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len = 0;
static int needs_redraw = 1;

static struct fry_wifi_status g_status;
static struct fry_wifi_scan_entry g_scan[FRY_WIFI_MAX_SCAN];
static uint32_t g_scan_count = 0;
static int g_selected = -1;
static int g_pass_focus = 0;
static uint64_t g_last_status_ms = 0;

static char g_passphrase[PASS_MAX + 1];
static char g_logs[LOG_LINES][LOG_LEN];

static tw_msg_header_t g_upd;

static int tw_msg_size(uint32_t type) {
    switch (type) {
        case TW_MSG_CREATE_WINDOW: return (int)sizeof(tw_msg_create_win_t);
        case TW_MSG_WINDOW_CREATED:return (int)sizeof(tw_msg_win_created_t);
        case TW_MSG_MOUSE_EVENT:   return (int)sizeof(tw_msg_mouse_t);
        case TW_MSG_KEY_EVENT:     return (int)sizeof(tw_msg_key_t);
        case TW_MSG_UPDATE:        return (int)sizeof(tw_msg_header_t);
        case TW_MSG_RESIZE:        return (int)sizeof(tw_msg_resize_t);
        case TW_MSG_RESIZED:       return (int)sizeof(tw_msg_resized_t);
        default:                   return (int)sizeof(tw_msg_header_t);
    }
}

static void input_consume(int n) {
    if (n <= 0 || n > input_stream_len) return;
    if (n < input_stream_len) {
        memmove(input_stream, input_stream + n, (size_t)(input_stream_len - n));
    }
    input_stream_len -= n;
}

static void input_append(const uint8_t *buf, int n) {
    if (!buf || n <= 0) return;
    if (n > INPUT_STREAM_MAX - input_stream_len) n = INPUT_STREAM_MAX - input_stream_len;
    if (n <= 0) return;
    memcpy(input_stream + input_stream_len, buf, (size_t)n);
    input_stream_len += n;
}

static int maybe_tw_prefix(void) {
    static const uint8_t magic_le[4] = {0x4E, 0x49, 0x57, 0x54};
    if (input_stream_len <= 0) return 0;
    if (input_stream[0] < 1 || input_stream[0] > 7) return 0;
    if (input_stream_len >= 2 && input_stream[1] != 0) return 0;
    if (input_stream_len >= 3 && input_stream[2] != 0) return 0;
    if (input_stream_len >= 4 && input_stream[3] != 0) return 0;
    for (int i = 4; i < input_stream_len && i < 8; i++) {
        if (input_stream[i] != magic_le[i - 4]) return 0;
    }
    return 1;
}

static void log_line(const char *fmt, ...) {
    char line[LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    for (int i = 0; i < LOG_LINES - 1; i++) {
        strcpy(g_logs[i], g_logs[i + 1]);
    }
    strncpy(g_logs[LOG_LINES - 1], line, LOG_LEN - 1);
    g_logs[LOG_LINES - 1][LOG_LEN - 1] = 0;
    needs_redraw = 1;
}

static void format_ip(uint32_t ip, char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)(ip & 0xFFu));
}

static void format_mac(const uint8_t mac[6], char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *status_label(uint8_t on, const char *yes, const char *no) {
    return on ? yes : no;
}

static void request_update(void) {
    fry_write(1, &g_upd, sizeof(g_upd));
}

static void render_and_present(void) {
    if (!render_buf) return;

    rect_t header = { 16, 16, win_w - 32, 112 };
    rect_t list = { 16, 144, win_w / 2 - 24, win_h - 160 };
    rect_t detail = { win_w / 2 + 8, 144, win_w / 2 - 24, 220 };
    rect_t logs = { win_w / 2 + 8, 380, win_w / 2 - 24, win_h - 396 };

    gfx_gradient_v(&g_ctx, 0, 0, (uint32_t)win_w, (uint32_t)win_h, COL_BG_0, COL_BG_1);

    gfx_fill_rounded(&g_ctx, (uint32_t)header.x, (uint32_t)header.y,
                     (uint32_t)header.w, (uint32_t)header.h, COL_PANEL, 3);
    gfx_rect(&g_ctx, (uint32_t)header.x, (uint32_t)header.y,
             (uint32_t)header.w, (uint32_t)header.h, COL_BORDER);
    gfx_draw_text(&g_ctx, (uint32_t)(header.x + 18), (uint32_t)(header.y + 14),
                  "Network Manager", COL_TEXT, COL_TRANS);
    gfx_draw_text(&g_ctx, (uint32_t)(header.x + 18), (uint32_t)(header.y + 38),
                  "Intel 9260 WiFi control and status", COL_TEXT_DIM, COL_TRANS);

    {
        char line[96];
        char mac[24];
        char ip[20];
        format_mac(g_status.mac, mac, sizeof(mac));
        format_ip(g_status.ip, ip, sizeof(ip));
        snprintf(line, sizeof(line), "Driver: %s   Link: %s   DHCP: %s",
                 status_label(g_status.ready, "ready", "offline"),
                 status_label(g_status.connected, "associated", "idle"),
                 status_label(g_status.configured, "configured", "pending"));
        gfx_draw_text(&g_ctx, (uint32_t)(header.x + 18), (uint32_t)(header.y + 66),
                      line, COL_ACCENT, COL_TRANS);
        snprintf(line, sizeof(line), "MAC %s   IP %s", mac, g_status.configured ? ip : "0.0.0.0");
        gfx_draw_text(&g_ctx, (uint32_t)(header.x + 18), (uint32_t)(header.y + 86),
                      line, COL_TEXT, COL_TRANS);
    }

    gfx_fill_rounded(&g_ctx, (uint32_t)list.x, (uint32_t)list.y,
                     (uint32_t)list.w, (uint32_t)list.h, COL_PANEL_2, 3);
    gfx_rect(&g_ctx, (uint32_t)list.x, (uint32_t)list.y,
             (uint32_t)list.w, (uint32_t)list.h, COL_BORDER);
    gfx_draw_text(&g_ctx, (uint32_t)(list.x + 14), (uint32_t)(list.y + 12),
                  "Visible Networks", COL_TEXT, COL_TRANS);

    {
        rect_t scan_btn = { list.x + list.w - 116, list.y + 10, 96, 24 };
        gfx_gradient_h(&g_ctx, (uint32_t)scan_btn.x, (uint32_t)scan_btn.y,
                       (uint32_t)scan_btn.w, (uint32_t)scan_btn.h,
                       COL_BUTTON, COL_BUTTON_HI);
        gfx_rect(&g_ctx, (uint32_t)scan_btn.x, (uint32_t)scan_btn.y,
                 (uint32_t)scan_btn.w, (uint32_t)scan_btn.h, COL_BORDER);
        gfx_draw_text(&g_ctx, (uint32_t)(scan_btn.x + 24), (uint32_t)(scan_btn.y + 4),
                      "Refresh", COL_TEXT, COL_TRANS);
    }

    if (g_scan_count == 0) {
        gfx_draw_text(&g_ctx, (uint32_t)(list.x + 14), (uint32_t)(list.y + 52),
                      "No scan results yet.", COL_TEXT_DIM, COL_TRANS);
        gfx_draw_text(&g_ctx, (uint32_t)(list.x + 14), (uint32_t)(list.y + 72),
                      "Press Refresh to scan.", COL_TEXT_DIM, COL_TRANS);
    } else {
        for (uint32_t i = 0; i < g_scan_count; i++) {
            int row_y = list.y + 44 + (int)i * 28;
            uint32_t fg = COL_TEXT;
            if ((int)i == g_selected) {
                gfx_fill(&g_ctx, (uint32_t)(list.x + 8), (uint32_t)(row_y - 2),
                         (uint32_t)(list.w - 16), 24, COL_SELECT);
                fg = COL_ACCENT;
            }
            gfx_draw_text(&g_ctx, (uint32_t)(list.x + 14), (uint32_t)row_y,
                          g_scan[i].ssid[0] ? g_scan[i].ssid : "<hidden>", fg, COL_TRANS);
            {
                char meta[64];
                snprintf(meta, sizeof(meta), "ch %u  RSSI %d  %s%s",
                         (unsigned)g_scan[i].channel,
                         (int)g_scan[i].rssi,
                         g_scan[i].secure ? "WPA2" : "OPEN",
                         g_scan[i].connected ? "  CONNECTED" : "");
                gfx_draw_text(&g_ctx, (uint32_t)(list.x + 190), (uint32_t)row_y,
                              meta, g_scan[i].connected ? COL_OK : COL_TEXT_DIM, COL_TRANS);
            }
        }
    }

    gfx_fill_rounded(&g_ctx, (uint32_t)detail.x, (uint32_t)detail.y,
                     (uint32_t)detail.w, (uint32_t)detail.h, COL_PANEL_2, 3);
    gfx_rect(&g_ctx, (uint32_t)detail.x, (uint32_t)detail.y,
             (uint32_t)detail.w, (uint32_t)detail.h, COL_BORDER);
    gfx_draw_text(&g_ctx, (uint32_t)(detail.x + 14), (uint32_t)(detail.y + 12),
                  "Connection", COL_TEXT, COL_TRANS);

    {
        const struct fry_wifi_scan_entry *sel =
            (g_selected >= 0 && (uint32_t)g_selected < g_scan_count) ? &g_scan[g_selected] : 0;
        char line[96];
        char mac[24];
        format_mac(g_status.bssid, mac, sizeof(mac));

        snprintf(line, sizeof(line), "SSID: %s",
                 sel ? (sel->ssid[0] ? sel->ssid : "<hidden>") :
                 (g_status.ssid[0] ? g_status.ssid : "<none>"));
        gfx_draw_text(&g_ctx, (uint32_t)(detail.x + 14), (uint32_t)(detail.y + 44),
                      line, COL_TEXT, COL_TRANS);

        if (sel) {
            snprintf(line, sizeof(line), "Security: %s   Channel: %u   RSSI: %d",
                     sel->secure ? "WPA2" : "Open",
                     (unsigned)sel->channel, (int)sel->rssi);
        } else {
            snprintf(line, sizeof(line), "Security: %s   Channel: %u   RSSI: %d",
                     g_status.secure ? "WPA2" : "Unknown",
                     (unsigned)g_status.channel, (int)g_status.rssi);
        }
        gfx_draw_text(&g_ctx, (uint32_t)(detail.x + 14), (uint32_t)(detail.y + 68),
                      line, COL_TEXT_DIM, COL_TRANS);

        snprintf(line, sizeof(line), "BSSID: %s", g_status.connected ? mac : "--:--:--:--:--:--");
        gfx_draw_text(&g_ctx, (uint32_t)(detail.x + 14), (uint32_t)(detail.y + 92),
                      line, COL_TEXT_DIM, COL_TRANS);
    }

    gfx_draw_text(&g_ctx, (uint32_t)(detail.x + 14), (uint32_t)(detail.y + 126),
                  "Passphrase", COL_TEXT, COL_TRANS);
    {
        rect_t field = { detail.x + 14, detail.y + 148, detail.w - 28, 28 };
        gfx_fill(&g_ctx, (uint32_t)field.x, (uint32_t)field.y,
                 (uint32_t)field.w, (uint32_t)field.h, COL_FIELD);
        gfx_rect(&g_ctx, (uint32_t)field.x, (uint32_t)field.y,
                 (uint32_t)field.w, (uint32_t)field.h,
                 g_pass_focus ? COL_ACCENT : COL_BORDER);
        {
            char stars[PASS_MAX + 1];
            size_t n = strlen(g_passphrase);
            if (n > PASS_MAX) n = PASS_MAX;
            for (size_t i = 0; i < n; i++) stars[i] = '*';
            stars[n] = 0;
            gfx_draw_text(&g_ctx, (uint32_t)(field.x + 8), (uint32_t)(field.y + 6),
                          stars[0] ? stars : "(empty for open network)", COL_TEXT, COL_TRANS);
        }
    }

    {
        rect_t connect_btn = { detail.x + 14, detail.y + 188, 112, 28 };
        gfx_gradient_h(&g_ctx, (uint32_t)connect_btn.x, (uint32_t)connect_btn.y,
                       (uint32_t)connect_btn.w, (uint32_t)connect_btn.h,
                       COL_ACCENT_2, COL_OK);
        gfx_rect(&g_ctx, (uint32_t)connect_btn.x, (uint32_t)connect_btn.y,
                 (uint32_t)connect_btn.w, (uint32_t)connect_btn.h, COL_BORDER);
        gfx_draw_text(&g_ctx, (uint32_t)(connect_btn.x + 26), (uint32_t)(connect_btn.y + 6),
                      "Connect", COL_TEXT, COL_TRANS);
    }

    gfx_fill_rounded(&g_ctx, (uint32_t)logs.x, (uint32_t)logs.y,
                     (uint32_t)logs.w, (uint32_t)logs.h, COL_PANEL, 3);
    gfx_rect(&g_ctx, (uint32_t)logs.x, (uint32_t)logs.y,
             (uint32_t)logs.w, (uint32_t)logs.h, COL_BORDER);
    gfx_draw_text(&g_ctx, (uint32_t)(logs.x + 14), (uint32_t)(logs.y + 12),
                  "Activity", COL_TEXT, COL_TRANS);
    for (int i = 0; i < LOG_LINES; i++) {
        uint32_t fg = (i == LOG_LINES - 1) ? COL_TEXT : COL_TEXT_DIM;
        gfx_draw_text(&g_ctx, (uint32_t)(logs.x + 14), (uint32_t)(logs.y + 36 + i * 18),
                      g_logs[i], fg, COL_TRANS);
    }

    memcpy(pixels, render_buf, (size_t)win_w * (size_t)win_h * 4);
    request_update();
}

static void refresh_status(void) {
    if (fry_wifi_status(&g_status) != 0) {
        memset(&g_status, 0, sizeof(g_status));
    }
    needs_redraw = 1;
}

static void perform_scan(void) {
    uint32_t count = 0;
    log_line("Scanning 2.4 GHz WiFi networks...");
    render_and_present();

    long rc = fry_wifi_scan(g_scan, FRY_WIFI_MAX_SCAN, &count);
    refresh_status();

    if (rc != 0) {
        g_scan_count = 0;
        g_selected = -1;
        log_line("Scan failed rc=%ld", rc);
        return;
    }

    g_scan_count = count;
    if (g_scan_count == 0) {
        g_selected = -1;
        log_line("Scan complete: no networks found");
        return;
    }

    if (g_selected < 0 || (uint32_t)g_selected >= g_scan_count) g_selected = 0;
    log_line("Scan complete: %u network(s)", (unsigned)g_scan_count);
}

static void perform_connect(void) {
    const struct fry_wifi_scan_entry *sel;

    if (g_selected < 0 || (uint32_t)g_selected >= g_scan_count) {
        log_line("Select a network first");
        return;
    }

    sel = &g_scan[g_selected];
    if (sel->secure && !g_passphrase[0]) {
        log_line("Passphrase required for %s", sel->ssid);
        return;
    }

    log_line("Connecting to %s...", sel->ssid[0] ? sel->ssid : "<hidden>");
    render_and_present();

    long rc = fry_wifi_connect(sel->ssid, sel->secure ? g_passphrase : "");
    refresh_status();

    if (rc != 0) {
        log_line("Connect failed rc=%ld", rc);
        return;
    }

    if (g_status.configured) {
        char ip[20];
        format_ip(g_status.ip, ip, sizeof(ip));
        log_line("Connected: %s  IP %s", sel->ssid, ip);
    } else {
        log_line("Associated, DHCP still pending");
    }
}

static void handle_key(char c) {
    if (c == '\r') c = '\n';

    if (!g_pass_focus) {
        if (c == 'r' || c == 'R') {
            perform_scan();
        } else if (c == '\n') {
            perform_connect();
        }
        return;
    }

    if (c == '\n') {
        perform_connect();
        return;
    }

    if (c == '\b' || c == 127) {
        size_t n = strlen(g_passphrase);
        if (n > 0) {
            g_passphrase[n - 1] = 0;
            needs_redraw = 1;
        }
        return;
    }

    if ((unsigned char)c >= 0x20) {
        size_t n = strlen(g_passphrase);
        if (n < PASS_MAX) {
            g_passphrase[n] = c;
            g_passphrase[n + 1] = 0;
            needs_redraw = 1;
        }
    }
}

static void handle_resize(int shm_id, uint64_t shm_ptr, int new_w, int new_h) {
    (void)shm_ptr;
    uint32_t *new_buf;
    if (new_w <= 0 || new_h <= 0) return;
    if (shm_id >= 0) {
        long ptr = fry_shm_map(shm_id);
        if (ptr > 0) {
            pixels = (uint32_t *)(uintptr_t)ptr;
            g_shm_id = shm_id;
        }
    }
    new_buf = malloc((size_t)new_w * (size_t)new_h * 4);
    if (!new_buf) return;
    free(render_buf);
    render_buf = new_buf;
    win_w = new_w;
    win_h = new_h;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);
    needs_redraw = 1;
}

static void handle_mouse_click(int x, int y) {
    rect_t list = { 16, 144, win_w / 2 - 24, win_h - 160 };
    rect_t detail = { win_w / 2 + 8, 144, win_w / 2 - 24, 220 };
    rect_t scan_btn = { list.x + list.w - 116, list.y + 10, 96, 24 };
    rect_t field = { detail.x + 14, detail.y + 148, detail.w - 28, 28 };
    rect_t connect_btn = { detail.x + 14, detail.y + 188, 112, 28 };

    if (x >= scan_btn.x && x < scan_btn.x + scan_btn.w &&
        y >= scan_btn.y && y < scan_btn.y + scan_btn.h) {
        perform_scan();
        return;
    }

    if (x >= connect_btn.x && x < connect_btn.x + connect_btn.w &&
        y >= connect_btn.y && y < connect_btn.y + connect_btn.h) {
        perform_connect();
        return;
    }

    g_pass_focus = 0;
    if (x >= field.x && x < field.x + field.w &&
        y >= field.y && y < field.y + field.h) {
        g_pass_focus = 1;
        needs_redraw = 1;
        return;
    }

    if (x >= list.x + 8 && x < list.x + list.w - 8 &&
        y >= list.y + 44 && y < list.y + list.h - 8) {
        int row = (y - (list.y + 44)) / 28;
        if (row >= 0 && (uint32_t)row < g_scan_count) {
            g_selected = row;
            needs_redraw = 1;
        }
    }
}

static void process_input_stream(void) {
    while (input_stream_len > 0) {
        if (input_stream_len >= (int)sizeof(tw_msg_header_t)) {
            tw_msg_header_t hdr;
            memcpy(&hdr, input_stream, sizeof(hdr));
            if (hdr.magic == TW_MAGIC) {
                int need = tw_msg_size(hdr.type);
                if (need < (int)sizeof(tw_msg_header_t)) need = (int)sizeof(tw_msg_header_t);
                if (input_stream_len < need) break;

                if (hdr.type == TW_MSG_RESIZED && need == (int)sizeof(tw_msg_resized_t)) {
                    tw_msg_resized_t rm;
                    memcpy(&rm, input_stream, sizeof(rm));
                    handle_resize(rm.shm_id, rm.shm_ptr, rm.new_w, rm.new_h);
                } else if (hdr.type == TW_MSG_KEY_EVENT && need == (int)sizeof(tw_msg_key_t)) {
                    tw_msg_key_t km;
                    memcpy(&km, input_stream, sizeof(km));
                    handle_key((char)(uint8_t)km.key);
                } else if (hdr.type == TW_MSG_MOUSE_EVENT && need == (int)sizeof(tw_msg_mouse_t)) {
                    tw_msg_mouse_t mm;
                    memcpy(&mm, input_stream, sizeof(mm));
                    handle_mouse_click(mm.x, mm.y);
                }
                input_consume(need);
                continue;
            }
        }

        if (input_stream_len < (int)sizeof(tw_msg_header_t) && maybe_tw_prefix()) break;
        handle_key((char)input_stream[0]);
        input_consume(1);
    }
}

static int wait_for_window(tw_msg_win_created_t *out) {
    uint8_t winbuf[sizeof(*out)];
    int winlen = 0;
    int tries = 0;
    memset(out, 0, sizeof(*out));

    while (tries < 300) {
        uint8_t in[32];
        long n = fry_read(0, in, sizeof(in));
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                if (winlen < (int)sizeof(winbuf)) {
                    winbuf[winlen++] = in[i];
                } else {
                    memmove(winbuf, winbuf + 1, sizeof(winbuf) - 1);
                    winbuf[sizeof(winbuf) - 1] = in[i];
                }
                if (winlen == (int)sizeof(winbuf)) {
                    tw_msg_win_created_t cand;
                    memcpy(&cand, winbuf, sizeof(cand));
                    if (cand.hdr.magic == TW_MAGIC &&
                        cand.hdr.type == TW_MSG_WINDOW_CREATED &&
                        cand.shm_id >= 0) {
                        *out = cand;
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

int main(void) {
    tw_msg_create_win_t req;
    tw_msg_win_created_t resp;

    memset(&req, 0, sizeof(req));
    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = INIT_W;
    req.h = INIT_H;
    strncpy(req.title, "Network Manager", sizeof(req.title) - 1);
    fry_write(1, &req, sizeof(req));

    if (wait_for_window(&resp) == 0) {
        long ptr = fry_shm_map(resp.shm_id);
        if (ptr > 0) {
            pixels = (uint32_t *)(uintptr_t)ptr;
            g_shm_id = resp.shm_id;
        }
    }

    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    if (!render_buf) return 1;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    g_upd.type = TW_MSG_UPDATE;
    g_upd.magic = TW_MAGIC;

    log_line("Ready");
    refresh_status();
    render_and_present();
    perform_scan();

    for (;;) {
        char in[64];
        long n = fry_read(0, in, sizeof(in));
        if (n > 0) {
            input_append((const uint8_t *)in, (int)n);
            process_input_stream();
        }

        uint64_t now = (uint64_t)fry_gettime();
        if (now - g_last_status_ms >= 1000ULL) {
            refresh_status();
            g_last_status_ms = now;
        }

        if (needs_redraw) {
            render_and_present();
            needs_redraw = 0;
        }

        fry_sleep(33);
    }

    return 0;
}
