// evloop.fry — TaterWin event-loop test application (Phase 9 Step 3)
//
// Demonstrates poll()-based event loop multiplexing:
//   - TaterWin SHM window with keyboard/mouse/wheel/focus events
//   - TCP socket connection (HTTP GET to a configurable host)
//   - Unified fry_poll() over window FD and socket FD
//
// Usage: launched from GUI compositor. Connects to the address shown
// in the status bar and displays the HTTP response in the window.

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

#include <stdint.h>

#define WIN_W   640
#define WIN_H   400

/* Colors */
#define COL_BG        0x0E131B
#define COL_HEADER    0x131A24
#define COL_BORDER    0x2A364A
#define COL_TEXT      0xE6ECF8
#define COL_TEXT_DIM  0x95A4BC
#define COL_ACCENT    0x29B6F6
#define COL_GREEN     0x66BB6A
#define COL_RED       0xEF5350
#define COL_YELLOW    0xF4C96A

#define HEADER_H      28
#define STATUS_H      20
#define LINE_H        14
#define CHAR_W         8
#define MAX_LOG_LINES  24
#define MAX_LINE_LEN   76
#define MAX_BODY_LINES 16
#define MAX_BODY_LINE  76

/* State */
static uint32_t fallback_pixels[WIN_W * WIN_H];
static uint32_t *pixels = fallback_pixels;
static uint32_t *render_buf = 0;
static gfx_ctx_t g_ctx;
static tw_msg_header_t g_upd;
static int g_shm_id = -1;

static int needs_redraw = 1;
static int focused = 1;
static int mouse_x = 0, mouse_y = 0;

/* Event counters */
static uint32_t evt_keys = 0;
static uint32_t evt_mouse = 0;
static uint32_t evt_wheel = 0;
static uint32_t evt_focus = 0;

/* Log buffer: ring of text lines */
static char log_lines[MAX_LOG_LINES][MAX_LINE_LEN + 1];
static int log_count = 0;
static int log_head = 0;  /* next slot to write */

/* Network state */
typedef enum {
    NET_IDLE,
    NET_RESOLVING,
    NET_CONNECTING,
    NET_SENDING,
    NET_RECEIVING,
    NET_DONE,
    NET_ERROR
} net_state_t;

static net_state_t net_state = NET_IDLE;
static int sock_fd = -1;
static char status_msg[80] = "Initializing...";
static char net_host[] = "example.com";
static uint16_t net_port = 80;
static uint32_t resolved_ip = 0;

/* HTTP response body lines */
static char body_lines[MAX_BODY_LINES][MAX_BODY_LINE + 1];
static int body_line_count = 0;
static int body_scroll = 0;
static int body_total_bytes = 0;

/* Input stream buffer for TaterWin messages */
#define INPUT_STREAM_MAX 512
static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len = 0;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void log_add(const char *msg) {
    int idx = log_head;
    strncpy(log_lines[idx], msg, MAX_LINE_LEN);
    log_lines[idx][MAX_LINE_LEN] = 0;
    log_head = (log_head + 1) % MAX_LOG_LINES;
    if (log_count < MAX_LOG_LINES) log_count++;
    needs_redraw = 1;
}

static void log_fmt(const char *fmt, ...) {
    char buf[MAX_LINE_LEN + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_add(buf);
}

/* ------------------------------------------------------------------ */
/* IP formatting                                                       */
/* ------------------------------------------------------------------ */

static void ip_to_str(uint32_t ip_net, char *out, size_t len) {
    uint32_t ip = fry_ntohl(ip_net);
    snprintf(out, len, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void render(gfx_ctx_t *ctx) {
    int y;
    char buf[96];

    /* Background */
    gfx_fill(ctx, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Header bar */
    gfx_fill(ctx, 0, 0, WIN_W, HEADER_H, COL_HEADER);
    gfx_draw_text(ctx, 8, 8, "TaterTOS Event-Loop Test", COL_ACCENT, COL_HEADER);

    /* Separator */
    gfx_fill(ctx, 0, HEADER_H, WIN_W, 1, COL_BORDER);

    /* Stats bar */
    y = HEADER_H + 4;
    snprintf(buf, sizeof(buf),
             "Keys:%u  Mouse:%u  Wheel:%u  Focus:%u  Net:%s  Cursor:%d,%d",
             evt_keys, evt_mouse, evt_wheel, evt_focus,
             net_state == NET_IDLE      ? "idle" :
             net_state == NET_RESOLVING  ? "DNS" :
             net_state == NET_CONNECTING ? "TCP" :
             net_state == NET_SENDING    ? "send" :
             net_state == NET_RECEIVING  ? "recv" :
             net_state == NET_DONE       ? "done" : "ERR",
             mouse_x, mouse_y);
    gfx_draw_text(ctx, 8, (uint32_t)y, buf, COL_TEXT_DIM, COL_BG);
    y += LINE_H + 2;

    /* Separator */
    gfx_fill(ctx, 0, (uint32_t)y, WIN_W, 1, COL_BORDER);
    y += 2;

    /* Left panel: event log */
    gfx_draw_text(ctx, 8, (uint32_t)y, "-- Event Log --", COL_YELLOW, COL_BG);
    y += LINE_H;

    {
        int start, i;
        int visible = 12;
        if (log_count < visible) visible = log_count;
        if (log_count <= MAX_LOG_LINES) {
            start = 0;
        } else {
            start = log_head; /* oldest line in ring */
        }
        for (i = 0; i < visible; i++) {
            int idx;
            if (log_count <= MAX_LOG_LINES) {
                idx = (log_count - visible + i);
            } else {
                idx = (start + (log_count - visible) + i) % MAX_LOG_LINES;
            }
            gfx_draw_text(ctx, 8, (uint32_t)y, log_lines[idx], COL_TEXT, COL_BG);
            y += LINE_H;
        }
    }

    /* Separator */
    y += 4;
    gfx_fill(ctx, 0, (uint32_t)y, WIN_W, 1, COL_BORDER);
    y += 2;

    /* Right panel: HTTP response body */
    snprintf(buf, sizeof(buf), "-- HTTP Response (%d bytes, %d lines) --",
             body_total_bytes, body_line_count);
    gfx_draw_text(ctx, 8, (uint32_t)y, buf, COL_GREEN, COL_BG);
    y += LINE_H;

    {
        int visible = 8;
        int i;
        if (body_line_count - body_scroll < visible)
            visible = body_line_count - body_scroll;
        if (visible < 0) visible = 0;
        for (i = 0; i < visible; i++) {
            int idx = body_scroll + i;
            if (idx < body_line_count) {
                gfx_draw_text(ctx, 8, (uint32_t)y, body_lines[idx],
                              COL_TEXT, COL_BG);
            }
            y += LINE_H;
        }
    }

    /* Status bar at bottom */
    gfx_fill(ctx, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_HEADER);
    gfx_fill(ctx, 0, WIN_H - STATUS_H, WIN_W, 1, COL_BORDER);

    {
        uint32_t status_col = COL_TEXT_DIM;
        if (net_state == NET_ERROR) status_col = COL_RED;
        else if (net_state == NET_DONE) status_col = COL_GREEN;
        gfx_draw_text(ctx, 8, WIN_H - STATUS_H + 4, status_msg,
                       status_col, COL_HEADER);
    }
}

/* ------------------------------------------------------------------ */
/* Network: non-blocking HTTP GET                                      */
/* ------------------------------------------------------------------ */

static void net_start(void) {
    long rc;
    struct fry_sockaddr_in addr;
    char ipbuf[20];
    char req_buf[256];
    int req_len;

    /* Step 1: DNS resolve */
    snprintf(status_msg, sizeof(status_msg), "Resolving %s ...", net_host);
    needs_redraw = 1;
    net_state = NET_RESOLVING;

    rc = fry_dns_resolve(net_host, &resolved_ip);
    if (rc < 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "DNS failed for %s (err %ld)", net_host, rc);
        net_state = NET_ERROR;
        log_fmt("DNS resolve failed: %ld", rc);
        return;
    }

    ip_to_str(resolved_ip, ipbuf, sizeof(ipbuf));
    log_fmt("Resolved %s -> %s", net_host, ipbuf);

    /* Step 2: Create socket */
    sock_fd = (int)fry_socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "socket() failed: %d", sock_fd);
        net_state = NET_ERROR;
        log_fmt("socket() failed: %d", sock_fd);
        return;
    }
    log_fmt("Socket created: fd=%d", sock_fd);

    /* Step 3: Connect */
    net_state = NET_CONNECTING;
    snprintf(status_msg, sizeof(status_msg),
             "Connecting to %s:%u ...", ipbuf, net_port);
    needs_redraw = 1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = fry_htons(net_port);
    addr.sin_addr = resolved_ip;

    rc = fry_connect(sock_fd, &addr, sizeof(addr));
    if (rc < 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "connect() failed: %ld", rc);
        net_state = NET_ERROR;
        log_fmt("connect() failed: %ld", rc);
        fry_close(sock_fd);
        sock_fd = -1;
        return;
    }
    log_fmt("Connected to %s:%u", ipbuf, net_port);

    /* Step 4: Send HTTP GET */
    net_state = NET_SENDING;
    snprintf(status_msg, sizeof(status_msg), "Sending HTTP GET...");
    needs_redraw = 1;

    req_len = snprintf(req_buf, sizeof(req_buf),
                       "GET / HTTP/1.0\r\nHost: %s\r\n"
                       "Connection: close\r\n\r\n",
                       net_host);

    rc = fry_send(sock_fd, req_buf, (size_t)req_len, 0);
    if (rc < 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "send() failed: %ld", rc);
        net_state = NET_ERROR;
        log_fmt("send() failed: %ld", rc);
        fry_close(sock_fd);
        sock_fd = -1;
        return;
    }
    log_fmt("Sent HTTP GET (%d bytes)", req_len);

    /* Now enter receive mode — poll loop will handle the rest */
    net_state = NET_RECEIVING;
    snprintf(status_msg, sizeof(status_msg),
             "Receiving from %s ...", net_host);
    needs_redraw = 1;
}

/* Append received data to body display */
static void body_append(const char *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        char c = data[i];
        body_total_bytes++;

        if (body_line_count >= MAX_BODY_LINES) break;

        if (c == '\n' || c == '\r') {
            /* End current line, start new */
            if (c == '\r') continue; /* skip \r, \n will advance */
            body_line_count++;
            continue;
        }

        if (body_line_count < MAX_BODY_LINES) {
            int cur = body_line_count;
            int col = (int)strlen(body_lines[cur]);
            if (col < MAX_BODY_LINE - 1) {
                body_lines[cur][col] = c;
                body_lines[cur][col + 1] = 0;
            }
        }
    }
}

/* Called from poll loop when socket has data */
static void net_recv_step(void) {
    char buf[512];
    long n;

    if (net_state != NET_RECEIVING || sock_fd < 0) return;

    n = fry_recv(sock_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (n > 0) {
        buf[n] = 0;
        body_append(buf, (int)n);
        snprintf(status_msg, sizeof(status_msg),
                 "Receiving... %d bytes total", body_total_bytes);
        needs_redraw = 1;
    } else if (n == 0) {
        /* Connection closed — done */
        net_state = NET_DONE;
        snprintf(status_msg, sizeof(status_msg),
                 "Done: %d bytes received from %s",
                 body_total_bytes, net_host);
        log_fmt("Connection closed, %d bytes total", body_total_bytes);
        fry_close(sock_fd);
        sock_fd = -1;
        needs_redraw = 1;
    }
    /* n < 0 with DONTWAIT: no data yet, not an error */
}

/* ------------------------------------------------------------------ */
/* TaterWin message processing                                         */
/* ------------------------------------------------------------------ */

static int tw_msg_size(uint32_t type) {
    switch (type) {
        case TW_MSG_KEY_EVENT:     return (int)sizeof(tw_msg_key_t);
        case TW_MSG_MOUSE_EVENT:   return (int)sizeof(tw_msg_mouse_t);
        case TW_MSG_MOUSE_MOVE:    return (int)sizeof(tw_msg_mouse_move_t);
        case TW_MSG_WHEEL_EVENT:   return (int)sizeof(tw_msg_wheel_t);
        case TW_MSG_FOCUS_EVENT:   return (int)sizeof(tw_msg_focus_t);
        case TW_MSG_ENTER_LEAVE:   return (int)sizeof(tw_msg_enter_leave_t);
        case TW_MSG_CLOSE_REQUEST: return (int)sizeof(tw_msg_close_request_t);
        case TW_MSG_CLIPBOARD_DATA:return (int)sizeof(tw_msg_clipboard_data_t);
        case TW_MSG_RESIZED:       return (int)sizeof(tw_msg_resized_t);
        default: return 0;
    }
}

static void input_consume(int n) {
    if (n <= 0 || n > input_stream_len) return;
    memmove(input_stream, input_stream + n, (size_t)(input_stream_len - n));
    input_stream_len -= n;
}

static void process_input(void) {
    while (input_stream_len >= (int)sizeof(tw_msg_header_t)) {
        tw_msg_header_t hdr;
        int msg_size;

        memcpy(&hdr, input_stream, sizeof(hdr));
        if (hdr.magic != TW_MAGIC) {
            /* Not a TW message — skip byte */
            input_consume(1);
            continue;
        }

        msg_size = tw_msg_size(hdr.type);
        if (msg_size == 0) {
            /* Unknown type — skip header */
            input_consume((int)sizeof(tw_msg_header_t));
            continue;
        }

        if (input_stream_len < msg_size) break; /* need more data */

        switch (hdr.type) {
            case TW_MSG_KEY_EVENT: {
                tw_msg_key_t key;
                memcpy(&key, input_stream, sizeof(key));
                evt_keys++;
                if (key.flags == 1 && key.ascii) { /* pressed */
                    log_fmt("Key: '%c' (vk=0x%x sc=0x%x mods=0x%x)",
                            key.ascii, key.vk, key.scancode, key.mods);
                    /* 'q' to quit */
                    if (key.ascii == 'q' || key.ascii == 'Q')
                        fry_exit(0);
                    /* 'r' to retry network */
                    if ((key.ascii == 'r' || key.ascii == 'R') &&
                        (net_state == NET_ERROR || net_state == NET_DONE)) {
                        net_state = NET_IDLE;
                        body_line_count = 0;
                        body_scroll = 0;
                        body_total_bytes = 0;
                        memset(body_lines, 0, sizeof(body_lines));
                    }
                }
                needs_redraw = 1;
                break;
            }
            case TW_MSG_MOUSE_EVENT: {
                tw_msg_mouse_t m;
                memcpy(&m, input_stream, sizeof(m));
                evt_mouse++;
                mouse_x = m.x;
                mouse_y = m.y;
                log_fmt("Click: (%d,%d) btns=0x%x", m.x, m.y, m.btns);
                needs_redraw = 1;
                break;
            }
            case TW_MSG_MOUSE_MOVE: {
                tw_msg_mouse_move_t m;
                memcpy(&m, input_stream, sizeof(m));
                mouse_x = m.x;
                mouse_y = m.y;
                needs_redraw = 1;
                break;
            }
            case TW_MSG_WHEEL_EVENT: {
                tw_msg_wheel_t w;
                memcpy(&w, input_stream, sizeof(w));
                evt_wheel++;
                /* Scroll body view */
                if (w.delta < 0 && body_scroll < body_line_count - 1)
                    body_scroll++;
                else if (w.delta > 0 && body_scroll > 0)
                    body_scroll--;
                log_fmt("Wheel: delta=%d scroll=%d", w.delta, body_scroll);
                needs_redraw = 1;
                break;
            }
            case TW_MSG_FOCUS_EVENT: {
                tw_msg_focus_t f;
                memcpy(&f, input_stream, sizeof(f));
                evt_focus++;
                focused = f.focused;
                log_fmt("Focus: %s", f.focused ? "gained" : "lost");
                needs_redraw = 1;
                break;
            }
            case TW_MSG_ENTER_LEAVE: {
                tw_msg_enter_leave_t el;
                memcpy(&el, input_stream, sizeof(el));
                log_fmt("Cursor: %s at (%d,%d)",
                        el.entered ? "enter" : "leave", el.x, el.y);
                needs_redraw = 1;
                break;
            }
            case TW_MSG_CLOSE_REQUEST: {
                log_add("Close requested");
                fry_exit(0);
                break;
            }
            case TW_MSG_RESIZED: {
                tw_msg_resized_t r;
                memcpy(&r, input_stream, sizeof(r));
                log_fmt("Resized: %dx%d shm=%d", r.new_w, r.new_h, r.shm_id);
                /* Could remap SHM here for dynamic resize */
                needs_redraw = 1;
                break;
            }
            default:
                break;
        }

        input_consume(msg_size);
    }
}

/* ------------------------------------------------------------------ */
/* Window creation                                                     */
/* ------------------------------------------------------------------ */

static int create_window(void) {
    tw_msg_create_win_t req;
    uint8_t winbuf[sizeof(tw_msg_win_created_t)];
    int winlen = 0;
    int tries = 0;

    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = WIN_W;
    req.h = WIN_H;
    strncpy(req.title, "Event Loop Test", sizeof(req.title) - 1);
    req.title[sizeof(req.title) - 1] = 0;
    fry_write(1, &req, sizeof(req));

    while (tries < 300) {
        uint8_t in[32];
        long n = fry_read(0, in, sizeof(in));
        if (n > 0) {
            long i;
            for (i = 0; i < n; i++) {
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

/* ------------------------------------------------------------------ */
/* Main: poll-based event loop                                         */
/* ------------------------------------------------------------------ */

int main(void) {
    struct fry_pollfd pfds[2];
    int nfds;

    /* Initialize body lines */
    memset(body_lines, 0, sizeof(body_lines));
    memset(log_lines, 0, sizeof(log_lines));

    /* Create window */
    if (create_window() < 0) {
        /* Fallback: use static buffer */
        pixels = fallback_pixels;
    }

    render_buf = malloc((size_t)WIN_W * (size_t)WIN_H * 4);
    if (!render_buf) return 1;
    gfx_init(&g_ctx, render_buf, WIN_W, WIN_H, WIN_W);

    g_upd.type = TW_MSG_UPDATE;
    g_upd.magic = TW_MAGIC;

    log_add("Event loop started");
    log_fmt("Window: %dx%d, SHM id=%d", WIN_W, WIN_H, g_shm_id);
    log_add("Press 'q' to quit, 'r' to retry network");
    snprintf(status_msg, sizeof(status_msg),
             "Target: %s:%u | Press 'q' quit, 'r' retry",
             net_host, net_port);

    /* Main event loop using fry_poll() */
    for (;;) {
        /* Start network connection if idle */
        if (net_state == NET_IDLE) {
            net_start();
        }

        /* Set up poll descriptors */
        nfds = 0;

        /* Always poll FD 0 (TaterWin input) */
        pfds[nfds].fd = 0;
        pfds[nfds].events = FRY_POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        /* Poll socket if we're receiving */
        if (net_state == NET_RECEIVING && sock_fd >= 0) {
            pfds[nfds].fd = sock_fd;
            pfds[nfds].events = FRY_POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        /* Poll with 33ms timeout (~30fps) */
        fry_poll(pfds, (uint32_t)nfds, 33);

        /* Check for TaterWin input */
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

        /* Check for socket data */
        if (nfds > 1 && (pfds[1].revents & (FRY_POLLIN | FRY_POLLERR | FRY_POLLHUP))) {
            net_recv_step();
        }

        /* Render if needed */
        if (needs_redraw) {
            render(&g_ctx);
            memcpy(pixels, render_buf, (size_t)WIN_W * (size_t)WIN_H * 4);
            fry_write(1, &g_upd, sizeof(g_upd));
            needs_redraw = 0;
        }
    }

    return 0;
}
