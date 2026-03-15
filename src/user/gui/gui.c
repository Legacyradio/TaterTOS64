// TaterTOS64v3 GUI — Puppy/Garuda layout, dark teal-purple palette

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Color palette
// ---------------------------------------------------------------------------
#define COL_DESKTOP      0x0D1117
#define COL_DESKTOP_LO   0x080C14
#define COL_TASKBAR_LO   0x161B22
#define COL_TASKBAR_HI   0x1C2333
#define COL_WIN_ACTIVE   0x1C2333
#define COL_WIN_INACTIVE 0x13161D
#define COL_ACCENT       0x7C3AED
#define COL_ACCENT2      0x10B981
#define COL_TEXT         0xF0F6FF
#define COL_TEXT_DIM     0x8B949E
#define COL_START_LO     0x5B21B6
#define COL_START_HI     0x7C3AED
#define COL_BTN_CLOSE    0xEF4444
#define COL_BTN_MIN      0xF59E0B
#define COL_BTN_MAX      0x10B981
#define COL_WIN_BG       0x000000
#define COL_TRANSPARENT  0xFF000000

#define TASKBAR_H   36
#define TITLE_H     28
#define START_W     90
#define BTN_SZ      12
#define BTN_GAP     18

// ---------------------------------------------------------------------------
// Window table (slots never moved, z_order[] is the ordering)
// ---------------------------------------------------------------------------
#define MAX_WINDOWS 16

typedef struct {
    int used;
    int minimized;
    int maximized;
    int done;
    long pid;
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h;
    char title[48];
    int shm_id;
    uint32_t *shm_ptr;
    int shm_w, shm_h;
    uint8_t msgbuf[256];
    int msglen;
    char textbuf[1024];
    int textlen;
    uint64_t launch_deadline_ms;
    uint8_t launch_watchdog_fired;
    uint8_t launch_progress_seen;
} window_t;

static window_t windows[MAX_WINDOWS];
static int z_order[MAX_WINDOWS];
static int z_count = 0;

// ---------------------------------------------------------------------------
// GFX state
// ---------------------------------------------------------------------------
static gfx_ctx_t screen;
static gfx_ctx_t backbuffer;
static uint32_t *vram;
static uint32_t *back_buf;
static struct fry_fb_info g_info;
static int g_sw;        /* screen width, set once in main */
static int g_desktop_h; /* screen height minus taskbar, set once in main */

// ---------------------------------------------------------------------------
// App list for start menu
// ---------------------------------------------------------------------------
#define MAX_MENU_APPS 24
#define LAUNCH_WATCHDOG_MS 5000ULL
static char app_names[MAX_MENU_APPS][32];
static char app_paths[MAX_MENU_APPS][96];
static int app_count = 0;

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static int start_menu_open = 0;
static int dragging_slot   = -1;
static int drag_ox = 0, drag_oy = 0;

static int window_has_process(const window_t *w) {
    return w && w->pid >= 0;
}

static void append_window_text(window_t *w, const char *msg) {
    int avail;
    int n;
    if (!w || !msg || !*msg) return;
    avail = (int)sizeof(w->textbuf) - 1 - w->textlen;
    n = (int)strlen(msg);
    if (n > avail) n = avail;
    if (n > 0) {
        memcpy(w->textbuf + w->textlen, msg, (size_t)n);
        w->textlen += n;
        w->textbuf[w->textlen] = '\0';
    }
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int has_fry_suffix(const char *s) {
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (n < 4) return 0;
    return (ascii_upper(s[n - 4]) == '.' &&
            ascii_upper(s[n - 3]) == 'F' &&
            ascii_upper(s[n - 2]) == 'R' &&
            ascii_upper(s[n - 1]) == 'Y');
}

static int has_tot_suffix(const char *s) {
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (n < 4) return 0;
    return (ascii_upper(s[n - 4]) == '.' &&
            ascii_upper(s[n - 3]) == 'T' &&
            ascii_upper(s[n - 2]) == 'O' &&
            ascii_upper(s[n - 1]) == 'T');
}

static int str_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_upper(*a) != ascii_upper(*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int path_is_nvme_prefix(const char *path) {
    if (!path) return 0;
    return path[0] == '/' &&
           path[1] == 'n' &&
           path[2] == 'v' &&
           path[3] == 'm' &&
           path[4] == 'e';
}

static int gui_path_is_primary_app_source(const char *path) {
    /*
     * Keep the desktop app model deterministic: the live/root app set comes
     * only from the primary root view. Secondary mounts stay available for
     * explicit browsing and install workflows, but the launcher never consults
     * them as implicit app sources.
     */
    if (!path || !*path) return 0;
    return !path_is_nvme_prefix(path);
}

static int path_is_system_dir(const char *path) {
    if (!path) return 0;
    return str_eq(path, "/system");
}

static int app_already_added(const char *name) {
    for (int i = 0; i < app_count; i++) {
        if (str_eq(app_names[i], name)) return 1;
    }
    return 0;
}

static void add_app_entry(const char *dir, const char *entry) {
    char base[32];
    char full[96];
    size_t len;
    if (!entry || !dir) return;
    if (!gui_path_is_primary_app_source(dir)) return;
    if (has_fry_suffix(entry)) {
        len = strlen(entry) - 4; /* trim ".FRY" */
    } else if (has_tot_suffix(entry) && str_eq_ci(entry, "SHELL.TOT")) {
        len = strlen(entry) - 4; /* trim ".TOT" */
    } else {
        return;
    }
    if (len == 0 || len >= sizeof(base)) return;
    for (size_t i = 0; i < len; i++) base[i] = ascii_upper(entry[i]);
    base[len] = 0;
    /* GUI is the compositor itself — never list it as a launchable app */
    if (str_eq(base, "GUI")) return;
    if (str_eq(base, "INIT") && path_is_system_dir(dir)) return;
    if (app_already_added(base)) return;
    if (app_count >= MAX_MENU_APPS) return;

    if (dir[0] == '/' && dir[1] == 0) {
        snprintf(full, sizeof(full), "/%s", entry);
    } else {
        snprintf(full, sizeof(full), "%s/%s", dir, entry);
    }
    if (full[0] == 0) return;

    strcpy(app_names[app_count], base);
    strncpy(app_paths[app_count], full, sizeof(app_paths[app_count]) - 1);
    app_paths[app_count][sizeof(app_paths[app_count]) - 1] = 0;
    app_count++;
}

static void parse_app_list(const char *dir, const char *list) {
    const char *p = list;
    while (p && *p) {
        const char *s = p;
        char token[64];
        size_t len;
        while (*p && *p != '\n' && *p != '\r') p++;
        len = (size_t)(p - s);
        while (*p == '\n' || *p == '\r') p++;
        if (len == 0) continue;
        if (len >= sizeof(token)) len = sizeof(token) - 1;
        memcpy(token, s, len);
        token[len] = 0;
        add_app_entry(dir, token);
    }
}

static void discover_apps_at(const char *path) {
    char buf[2048];
    long n;
    if (!path) return;
    n = fry_readdir(path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        parse_app_list(path, buf);
    }
}

static void discover_primary_apps(void) {
    static const char *dirs[] = {
        "/apps",
        "/",
        "/fry",
        "/FRY",
        "/EFI/fry",
        "/EFI/FRY",
        "/EFI/BOOT",
        "/EFI/BOOT/fry",
        "/EFI/BOOT/FRY"
    };
    for (uint32_t i = 0; i < (uint32_t)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        discover_apps_at(dirs[i]);
    }
}

static void discover_apps(void) {
    /*
     * Keep boot UX deterministic: seed a minimal launch set first, then enrich
     * from directory scans. This guarantees a usable menu even when storage is
     * slow or unavailable.
     */
    app_count = 0;
    add_app_entry("/apps", "SHELL.TOT");
    add_app_entry("/apps", "SYSINFO.FRY");
    add_app_entry("/apps", "UPTIME.FRY");
    add_app_entry("/apps", "PS.FRY");
    add_app_entry("/apps", "FILEMAN.FRY");
    add_app_entry("/apps", "NETMGR.FRY");
    add_app_entry("/", "SHELL.TOT");

    discover_primary_apps();
}

// ---------------------------------------------------------------------------
// Z-order helpers
// ---------------------------------------------------------------------------
static void bring_to_front(int slot) {
    int new_z[MAX_WINDOWS], nc = 0;
    for (int i = 0; i < z_count; i++)
        if (z_order[i] != slot) new_z[nc++] = z_order[i];
    new_z[nc++] = slot;
    for (int i = 0; i < nc; i++) z_order[i] = new_z[i];
    z_count = nc;
}

static int top_slot(void) {
    return (z_count > 0) ? z_order[z_count - 1] : -1;
}

static void clamp_client_size(int *w, int *h) {
    int max_w = g_sw - 2;
    int max_h = g_desktop_h - TITLE_H - 1;
    if (*w < 64)    *w = 64;
    if (*h < 32)    *h = 32;
    if (*w > max_w) *w = max_w;
    if (*h > max_h) *h = max_h;
}

static void clamp_window_pos(window_t *w) {
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > g_sw) w->x = g_sw - w->w;
    if (w->y + w->h > g_desktop_h) w->y = g_desktop_h - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
}

static int resize_client_surface(window_t *w, int req_w, int req_h) {
    int nw = req_w;
    int nh = req_h;
    clamp_client_size(&nw, &nh);

    long sid = fry_shm_alloc((size_t)nw * (size_t)nh * 4);
    if (sid < 0) return 0;

    long ptr = fry_shm_map((int)sid);
    if (ptr <= 0) {
        fry_shm_free((int)sid);
        return 0;
    }

    if (w->shm_id >= 0) fry_shm_free(w->shm_id);
    w->shm_id  = (int)sid;
    w->shm_ptr = (uint32_t *)(uintptr_t)ptr;
    w->shm_w   = nw;
    w->shm_h   = nh;

    /* Notify app per TaterWin protocol. */
    tw_msg_resized_t rresp;
    rresp.hdr.type  = TW_MSG_RESIZED;
    rresp.hdr.magic = TW_MAGIC;
    rresp.shm_id    = (int)sid;
    rresp.shm_ptr   = (uint64_t)(uintptr_t)ptr;
    rresp.new_w     = nw;
    rresp.new_h     = nh;
    fry_proc_input((uint32_t)w->pid, &rresp, sizeof(rresp));

    return 1;
}

// ---------------------------------------------------------------------------
// TaterWin / text output helpers (unchanged logic from prior gui.c)
// ---------------------------------------------------------------------------
static void process_window_output(window_t *w) {
    while (w->msglen >= (int)sizeof(tw_msg_header_t)) {
        tw_msg_header_t *hdr = (tw_msg_header_t *)(void *)w->msgbuf;
        if (hdr->magic == TW_MAGIC) {
            if (hdr->type == TW_MSG_CREATE_WINDOW) {
                if (w->msglen < (int)sizeof(tw_msg_create_win_t)) break;
                tw_msg_create_win_t *msg = (tw_msg_create_win_t *)(void *)w->msgbuf;
                int req_w = msg->w;
                int req_h = msg->h;
                clamp_client_size(&req_w, &req_h);
                long sid = fry_shm_alloc((size_t)req_w * (size_t)req_h * 4);
                if (sid >= 0) {
                    long ptr = fry_shm_map((int)sid);
                    if (ptr > 0) {
                        w->shm_id  = (int)sid;
                        w->shm_ptr = (uint32_t *)(uintptr_t)ptr;
                        w->shm_w   = req_w;
                        w->shm_h   = req_h;
                        w->launch_progress_seen = 1;
                        w->launch_deadline_ms = 0;
                        w->w = w->shm_w + 2;
                        w->h = w->shm_h + TITLE_H + 1;
                        clamp_window_pos(w);
                        {
                            size_t ti = 0;
                            while (ti + 1 < sizeof(w->title) &&
                                   ti < sizeof(msg->title) &&
                                   msg->title[ti]) {
                                w->title[ti] = msg->title[ti];
                                ti++;
                            }
                            w->title[ti] = 0;
                        }
                        tw_msg_win_created_t resp;
                        resp.hdr.type  = TW_MSG_WINDOW_CREATED;
                        resp.hdr.magic = TW_MAGIC;
                        resp.shm_id    = (int)sid;
                        resp.shm_ptr   = (uint64_t)(uintptr_t)ptr;
                        fry_proc_input((uint32_t)w->pid, &resp, sizeof(resp));
                    } else {
                        const char *err = "GUI: SHM map failed\n";
                        int avail = (int)sizeof(w->textbuf) - 1 - w->textlen;
                        int n = (int)strlen(err);
                        if (n > avail) n = avail;
                        if (n > 0) {
                            memcpy(w->textbuf + w->textlen, err, (size_t)n);
                            w->textlen += n;
                            w->textbuf[w->textlen] = '\0';
                        }
                    }
                } else {
                    const char *err = "GUI: SHM alloc failed\n";
                    int avail = (int)sizeof(w->textbuf) - 1 - w->textlen;
                    int n = (int)strlen(err);
                    if (n > avail) n = avail;
                    if (n > 0) {
                        memcpy(w->textbuf + w->textlen, err, (size_t)n);
                        w->textlen += n;
                        w->textbuf[w->textlen] = '\0';
                    }
                }
                int c = (int)sizeof(tw_msg_create_win_t);
                memmove(w->msgbuf, w->msgbuf + c, (size_t)(w->msglen - c));
                w->msglen -= c;
            } else if (hdr->type == TW_MSG_RESIZE) {
                if (w->msglen < (int)sizeof(tw_msg_resize_t)) break;
                tw_msg_resize_t *rmsg = (tw_msg_resize_t *)(void *)w->msgbuf;
                int req_w = rmsg->new_w;
                int req_h = rmsg->new_h;
                if (w->maximized) {
                    /* Maximized windows keep full-desktop geometry for consistency. */
                    req_w = g_sw - 2;
                    req_h = g_desktop_h - TITLE_H - 1;
                }
                if (resize_client_surface(w, req_w, req_h)) {
                    if (w->maximized) {
                        w->x = 0;
                        w->y = 0;
                        w->w = g_sw;
                        w->h = g_desktop_h;
                    } else {
                        w->w = w->shm_w + 2;
                        w->h = w->shm_h + TITLE_H + 1;
                        clamp_window_pos(w);
                    }
                }
                int c = (int)sizeof(tw_msg_resize_t);
                memmove(w->msgbuf, w->msgbuf + c, (size_t)(w->msglen - c));
                w->msglen -= c;
            } else {
                int c = (int)sizeof(tw_msg_header_t);
                memmove(w->msgbuf, w->msgbuf + c, (size_t)(w->msglen - c));
                w->msglen -= c;
            }
        } else {
            int i = 0;
            while (i < w->msglen) {
                if (i + 4 <= w->msglen) {
                    uint32_t m = (uint32_t)w->msgbuf[i]
                               | ((uint32_t)w->msgbuf[i+1] << 8)
                               | ((uint32_t)w->msgbuf[i+2] << 16)
                               | ((uint32_t)w->msgbuf[i+3] << 24);
                    if (m == TW_MAGIC) break;
                }
                i++;
            }
            if (i == 0) break;
            int avail = (int)sizeof(w->textbuf) - 1 - w->textlen;
            if (avail < i) {
                int drop = i - avail + 128;
                if (drop > w->textlen) drop = w->textlen;
                memmove(w->textbuf, w->textbuf + drop, (size_t)(w->textlen - drop));
                w->textlen -= drop;
                avail = (int)sizeof(w->textbuf) - 1 - w->textlen;
            }
            int copy = (i < avail) ? i : avail;
            memcpy(w->textbuf + w->textlen, w->msgbuf, (size_t)copy);
            w->textlen += copy;
            w->textbuf[w->textlen] = '\0';
            memmove(w->msgbuf, w->msgbuf + i, (size_t)(w->msglen - i));
            w->msglen -= i;
        }
    }
}

static void draw_window_text(window_t *w) {
    int cx = w->x + 6;
    int cy = w->y + TITLE_H + 4;
    int lh = 12;
    int max_lines = (w->h - TITLE_H - 4) / lh;
    if (max_lines <= 0) return;
    const char *t = w->textbuf;
    int tlen = w->textlen;
    int nlines = 0;
    for (int j = 0; j < tlen; j++) if (t[j] == '\n') nlines++;
    int skip = nlines - max_lines;
    if (skip < 0) skip = 0;
    int row = 0, skipped = 0, cur = 0;
    char linebuf[128];
    while (cur < tlen && row < max_lines) {
        int end = cur;
        while (end < tlen && t[end] != '\n') end++;
        if (end == tlen) {
            if (skipped < skip) break;
            int len = end - cur;
            if (len > 127) len = 127;
            memcpy(linebuf, t + cur, (size_t)len);
            linebuf[len] = '\0';
            gfx_draw_text(&backbuffer, cx, cy + row * lh, linebuf, 0x00FF00, COL_TRANSPARENT);
            break;
        }
        if (skipped < skip) { skipped++; cur = end + 1; continue; }
        int len = end - cur;
        if (len > 127) len = 127;
        memcpy(linebuf, t + cur, (size_t)len);
        linebuf[len] = '\0';
        gfx_draw_text(&backbuffer, cx, cy + row * lh, linebuf, 0x00FF00, COL_TRANSPARENT);
        row++;
        cur = end + 1;
    }
}

// ---------------------------------------------------------------------------
// Window close
// ---------------------------------------------------------------------------
static void remove_window(int slot) {
    window_t *w = &windows[slot];
    if (!w->done && window_has_process(w)) {
        fry_kill(w->pid);
    }
    if (w->shm_id >= 0) {
        fry_shm_free(w->shm_id);
        w->shm_id = -1;
        w->shm_ptr = 0;
    }
    int new_z[MAX_WINDOWS], nc = 0;
    for (int i = 0; i < z_count; i++)
        if (z_order[i] != slot) new_z[nc++] = z_order[i];
    for (int i = 0; i < nc; i++) z_order[i] = new_z[i];
    z_count = nc;
    windows[slot].used = 0;
    if (dragging_slot == slot) dragging_slot = -1;
}

static void launch_watchdog_expire(window_t *w) {
    const char *msg = "GUI: launch timeout (no app response)\n";
    if (!w || w->launch_watchdog_fired || w->done) return;
    w->launch_watchdog_fired = 1;
    w->done = 1;
    if (window_has_process(w)) fry_kill(w->pid);
    append_window_text(w, msg);
}

// ---------------------------------------------------------------------------
// Window launch
// ---------------------------------------------------------------------------
static const char *launch_error_text(long rc) {
    switch ((int)rc) {
        case -100: return "bad launch arguments";
        case -101: return "file open failed";
        case -102: return "image header too short";
        case -103: return "out of memory while loading image";
        case -104: return "image read failed";
        case -105: return "bad package magic";
        case -106: return "image bounds check failed";
        case -107: return "package CRC mismatch";
        case -108: return "bad ELF header";
        case -109: return "bad ELF magic";
        case -110: return "address space allocation failed";
        case -111: return "segment allocation failed";
        case -112: return "segment mapping failed";
        case -113: return "user stack allocation failed";
        case -200: return "process creation failed";
        default:   return "unknown launch failure";
    }
}

static long spawn_app_with_variants(const char *name, const char *ext,
                                    char *last_path, size_t last_path_len) {
    static const char *primary_dirs[] = {
        "/apps/",
        "/system/",
        "/",
        "/fry/",
        "/FRY/",
        "/EFI/fry/",
        "/EFI/FRY/",
        "/EFI/BOOT/",
        "/EFI/BOOT/fry/",
        "/EFI/BOOT/FRY/"
    };
    char path[96];
    long rc = -1;
    if (!name || !ext) return -1;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(primary_dirs) / sizeof(primary_dirs[0])); i++) {
        if (primary_dirs[i][0] == '/' && primary_dirs[i][1] == 0) {
            snprintf(path, sizeof(path), "/%s.%s", name, ext);
        } else {
            snprintf(path, sizeof(path), "%s%s.%s", primary_dirs[i], name, ext);
        }
        if (last_path && last_path_len > 0) {
            strncpy(last_path, path, last_path_len - 1);
            last_path[last_path_len - 1] = 0;
        }
        rc = fry_spawn(path);
        if (rc >= 0) return rc;
    }
    return rc;
}

static void launch_app_index(int idx) {
    /* find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used) { slot = i; break; }
    }
    if (slot < 0) return; /* all slots full */

    if (idx < 0 || idx >= app_count) return;
    const char *name = app_names[idx];
    const char *path = app_paths[idx];
    char attempted_path[96];
    long pid = -1;
    attempted_path[0] = 0;
    if (path && path[0] && gui_path_is_primary_app_source(path)) {
        strncpy(attempted_path, path, sizeof(attempted_path) - 1);
        attempted_path[sizeof(attempted_path) - 1] = 0;
        pid = fry_spawn(path);
    }
    if (pid < 0) {
        if (str_eq(name, "SHELL")) {
            pid = spawn_app_with_variants(name, "TOT", attempted_path, sizeof(attempted_path));
        } else {
            pid = spawn_app_with_variants(name, "FRY", attempted_path, sizeof(attempted_path));
        }
    }
    if (pid < 0) {
        char msg[256];
        window_t *w = &windows[slot];
        memset(w, 0, sizeof(*w));
        w->used   = 1;
        w->done   = 1;
        w->pid    = -1;
        w->shm_id = -1;
        w->x = 50 + (slot % 6) * 40;
        w->y = 40 + (slot % 5) * 30;
        w->w = 460;
        w->h = 180;
        strncpy(w->title, name, 47);
        w->textbuf[0] = '\0';
        w->textlen = 0;
        snprintf(msg, sizeof(msg),
                 "Launch failed.\n"
                 "App: %s\n"
                 "Path: %s\n"
                 "Error: %ld (%s)\n",
                 name ? name : "(null)",
                 attempted_path[0] ? attempted_path : "(none)",
                 pid, launch_error_text(pid));
        append_window_text(w, msg);
        z_order[z_count++] = slot;
        bring_to_front(slot);
        start_menu_open = 0;
        return;
    }

    window_t *w = &windows[slot];
    memset(w, 0, sizeof(*w));
    w->used   = 1;
    w->pid    = pid;
    w->shm_id = -1;
    w->x = 50 + (slot % 6) * 40;
    w->y = 40 + (slot % 5) * 30;
    w->w = 600;
    w->h = 400;
    strncpy(w->title, name, 47);
    w->textlen = 0;
    strcpy(w->textbuf, "Starting app...\n");
    w->textlen = (int)strlen(w->textbuf);
    w->launch_watchdog_fired = 0;
    w->launch_progress_seen = 0;
    {
        long now_ms = fry_gettime();
        w->launch_deadline_ms = (now_ms > 0) ? ((uint64_t)now_ms + LAUNCH_WATCHDOG_MS) : 0;
    }

    z_order[z_count++] = slot;
    bring_to_front(slot);
    start_menu_open = 0;
}

// ---------------------------------------------------------------------------
// Draw window chrome
// ---------------------------------------------------------------------------
static void draw_window(int slot) {
    window_t *w = &windows[slot];
    if (!w->used || w->minimized) return;

    int active = (slot == top_slot());
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;

    /* shadow */
    gfx_fill(&backbuffer, wx + 3, wy + 3, ww, wh, 0x050810);

    /* outer border — violet if active */
    uint32_t border_col = active ? COL_ACCENT : 0x2A2F3A;
    gfx_rect(&backbuffer, wx, wy, ww, wh, border_col);

    /* title bar gradient */
    uint32_t tb_col = active ? COL_WIN_ACTIVE : COL_WIN_INACTIVE;
    uint32_t tb_hi  = active ? 0x242B40 : 0x181C26;
    gfx_gradient_h(&backbuffer, wx+1, wy+1, ww-2, TITLE_H-1, tb_hi, tb_col);

    /* violet left accent for active */
    if (active)
        gfx_fill(&backbuffer, wx+1, wy+1, 2, TITLE_H-1, COL_ACCENT);

    /* title text — truncate to avoid button area */
    char trunc[40];
    strncpy(trunc, w->title, 39);
    trunc[39] = '\0';
    int max_chars = (ww - 80) / 8;
    if (max_chars < 0) max_chars = 0;
    if (max_chars > 38) max_chars = 38;
    trunc[max_chars] = '\0';
    uint32_t title_col = active ? COL_TEXT : COL_TEXT_DIM;
    gfx_draw_text(&backbuffer, wx + 10, wy + 6, trunc, title_col, COL_TRANSPARENT);

    /* title bar buttons: [−][□][×] right side */
    int bx_close = wx + ww - 18;
    int bx_max   = wx + ww - 18 - BTN_GAP;
    int bx_min   = wx + ww - 18 - BTN_GAP*2;
    int by       = wy + (TITLE_H - BTN_SZ) / 2;

    gfx_fill_rounded(&backbuffer, bx_close, by, BTN_SZ, BTN_SZ, COL_BTN_CLOSE, 2);
    gfx_fill_rounded(&backbuffer, bx_max,   by, BTN_SZ, BTN_SZ, COL_BTN_MAX,   2);
    gfx_fill_rounded(&backbuffer, bx_min,   by, BTN_SZ, BTN_SZ, COL_BTN_MIN,   2);

    /* content area */
    gfx_fill(&backbuffer, wx+1, wy+TITLE_H, ww-2, wh-TITLE_H-1, COL_WIN_BG);

    if (w->shm_ptr) {
        gfx_ctx_t shm_ctx;
        gfx_init(&shm_ctx, w->shm_ptr,
                  (uint32_t)w->shm_w, (uint32_t)w->shm_h, (uint32_t)w->shm_w);
        gfx_blit(&backbuffer, &shm_ctx, (uint32_t)(wx+1), (uint32_t)(wy+TITLE_H));
    } else if (w->textlen > 0) {
        draw_window_text(w);
    }
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------
static void draw_taskbar(int screen_w, int screen_h, int mx, int my,
                         uint8_t btns, uint8_t prev_btns) {
    int ty = screen_h - TASKBAR_H;

    /* background */
    gfx_gradient_v(&backbuffer, 0, ty, screen_w, TASKBAR_H,
                   COL_TASKBAR_HI, COL_TASKBAR_LO);
    /* top border */
    gfx_fill(&backbuffer, 0, ty, screen_w, 1, COL_ACCENT);

    /* --- Start button --- */
    int hover_start = (mx >= 0 && mx < START_W && my >= ty);
    uint32_t sb_lo = hover_start ? COL_ACCENT : COL_START_LO;
    uint32_t sb_hi = hover_start ? 0x9D60F5  : COL_START_HI;
    gfx_gradient_h(&backbuffer, 2, ty+3, START_W-4, TASKBAR_H-6, sb_lo, sb_hi);
    gfx_rect(&backbuffer, 2, ty+3, START_W-4, TASKBAR_H-6, COL_ACCENT);
    gfx_draw_text(&backbuffer, 8, ty+10, "TATER>", COL_TEXT, COL_TRANSPARENT);

    /* click start button */
    if ((btns & 1) && !(prev_btns & 1) && hover_start) {
        start_menu_open = !start_menu_open;
        if (start_menu_open) discover_apps();
    }

    /* --- Window task buttons --- */
    int btn_x = START_W + 8;
    int ts = top_slot();
    for (int i = 0; i < z_count; i++) {
        int s = z_order[i];
        window_t *w = &windows[s];
        if (!w->used) continue;
        int bw = 120;
        int hover_btn = (mx >= btn_x && mx < btn_x + bw && my >= ty);
        uint32_t bg = (s == ts && !w->minimized) ? 0x2A2F45 : 0x1A1F2E;
        if (hover_btn) bg = 0x2F3550;
        gfx_fill(&backbuffer, btn_x, ty+4, bw, TASKBAR_H-8, bg);
        if (s == ts && !w->minimized)
            gfx_fill(&backbuffer, btn_x, ty+4, 2, TASKBAR_H-8, COL_ACCENT);
        /* truncated title */
        char tt[13]; strncpy(tt, w->title, 12); tt[12] = '\0';
        uint32_t tc = w->done ? COL_TEXT_DIM : COL_TEXT;
        gfx_draw_text(&backbuffer, btn_x+5, ty+10, tt, tc, COL_TRANSPARENT);

        /* click: restore/focus */
        if ((btns & 1) && !(prev_btns & 1) && hover_btn) {
            if (w->minimized) { w->minimized = 0; bring_to_front(s); }
            else               bring_to_front(s);
            start_menu_open = 0;
        }
        btn_x += bw + 4;
    }

    /* --- Uptime clock --- */
    long ms = fry_gettime();
    long secs = ms / 1000;
    long h = secs / 3600; secs %= 3600;
    long m = secs / 60;   secs %= 60;
    char clk[24];
    snprintf(clk, sizeof(clk), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)secs);
    int clk_x = screen_w - (int)strlen(clk)*8 - 8;
    gfx_draw_text(&backbuffer, clk_x, ty+10, clk, COL_ACCENT2, COL_TRANSPARENT);
}

// Start menu
// ---------------------------------------------------------------------------
#define MENU_ROW_H  24
#define MENU_W      140
#define MENU_HDR_H  28
#define MENU_BTM_H  28

static void draw_start_menu(int screen_h, int mx, int my,
                            uint8_t btns, uint8_t prev_btns) {
    int rows = (app_count > 0) ? app_count : 1;
    int menu_h = MENU_HDR_H + rows * MENU_ROW_H + MENU_BTM_H;
    int mx0 = 2;
    int my0 = screen_h - TASKBAR_H - menu_h;

    /* background */
    gfx_fill(&backbuffer, mx0, my0, MENU_W, menu_h, 0x1A1F2E);
    gfx_rect(&backbuffer, mx0, my0, MENU_W, menu_h, COL_ACCENT);

    /* header */
    gfx_gradient_h(&backbuffer, mx0+1, my0+1, MENU_W-2, MENU_HDR_H-1,
                   COL_START_LO, COL_START_HI);
    gfx_draw_text(&backbuffer, mx0+8, my0+6, "TaterTOS v3", COL_TEXT, COL_TRANSPARENT);

    /* app rows */
    if (app_count > 0) {
        for (int i = 0; i < app_count; i++) {
            int ry = my0 + MENU_HDR_H + i * MENU_ROW_H;
            int hover = (mx >= mx0 && mx < mx0+MENU_W && my >= ry && my < ry+MENU_ROW_H);
            if (hover) gfx_fill(&backbuffer, mx0+1, ry, MENU_W-2, MENU_ROW_H, 0x2A3050);
            gfx_draw_text(&backbuffer, mx0+12, ry+4, app_names[i], COL_TEXT, COL_TRANSPARENT);
            if (hover && (btns & 1) && !(prev_btns & 1))
                launch_app_index(i);
        }
    } else {
        int ry = my0 + MENU_HDR_H;
        gfx_draw_text(&backbuffer, mx0+12, ry+4, "(no apps)", COL_TEXT_DIM, COL_TRANSPARENT);
    }

    /* bottom bar: Reboot | Shutdown */
    int by = my0 + MENU_HDR_H + rows * MENU_ROW_H;
    gfx_fill(&backbuffer, mx0+1, by, MENU_W-2, MENU_BTM_H, 0x110D1A);
    int hover_rb = (mx >= mx0 && mx < mx0+MENU_W/2 && my >= by && my < by+MENU_BTM_H);
    int hover_sd = (mx >= mx0+MENU_W/2 && mx < mx0+MENU_W && my >= by && my < by+MENU_BTM_H);
    if (hover_rb) gfx_fill(&backbuffer, mx0+1, by, MENU_W/2-1, MENU_BTM_H, 0x2A1540);
    if (hover_sd) gfx_fill(&backbuffer, mx0+MENU_W/2, by, MENU_W/2-1, MENU_BTM_H, 0x2A1540);
    gfx_draw_text(&backbuffer, mx0+4, by+6, "Reboot", COL_TEXT_DIM, COL_TRANSPARENT);
    gfx_draw_text(&backbuffer, mx0+MENU_W/2+4, by+6, "Shutdn", COL_TEXT_DIM, COL_TRANSPARENT);
    if (hover_rb && (btns & 1) && !(prev_btns & 1)) fry_reboot();
    if (hover_sd && (btns & 1) && !(prev_btns & 1)) fry_shutdown();
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------
static const uint8_t cursor_arrow[16] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
    0xFF, 0xF8, 0xD8, 0x8C, 0x0C, 0x06, 0x06, 0x00
};
static void draw_cursor(int x, int y) {
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++)
            if (cursor_arrow[i] & (1 << (7 - j)))
                gfx_putpixel(&backbuffer, x+j, y+i, 0xFFFFFF);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    if (fry_fb_info(&g_info) != 0) return 1;
    vram = (uint32_t *)(uintptr_t)fry_fb_map();
    if (!vram) return 1;

    gfx_init(&screen, vram, g_info.width, g_info.height, g_info.stride);
    back_buf = malloc(g_info.stride * g_info.height * 4);
    gfx_init(&backbuffer, back_buf, g_info.width, g_info.height, g_info.stride);

    int sw = (int)g_info.width;
    int sh = (int)g_info.height;
    int desktop_h = sh - TASKBAR_H;
    g_sw = sw;
    g_desktop_h = desktop_h;
    discover_apps();

    int mx = sw / 2, my = sh / 2;
    uint8_t prev_btns = 0;

    for (;;) {
        /* --- input --- */
        struct fry_mouse_state ms;
        fry_mouse_get(&ms);
        mx += ms.dx * 4;
        my += ms.dy * 4;
        if (mx < 0) mx = 0;
        if (mx >= sw) mx = sw - 1;
        if (my < 0) my = 0;
        if (my >= sh) my = sh - 1;

        uint8_t cur_btns = ms.btns;

        /* keyboard -> focused window */
        int ts = top_slot();
        if (ts >= 0 && windows[ts].used && !windows[ts].minimized) {
            char keybuf[16];
            long kn = fry_read(0, keybuf, sizeof(keybuf));
            if (kn > 0 && window_has_process(&windows[ts])) {
                for (long i = 0; i < kn; i++) {
                    tw_msg_key_t kmsg;
                    kmsg.hdr.type  = TW_MSG_KEY_EVENT;
                    kmsg.hdr.magic = TW_MAGIC;
                    kmsg.key = (uint32_t)(uint8_t)keybuf[i];
                    kmsg.flags = 0;
                    fry_proc_input((uint32_t)windows[ts].pid, &kmsg, sizeof(kmsg));
                }
            }
        }

        /* --- hit testing on left click --- */
        if ((cur_btns & 1) && !(prev_btns & 1)) {
            int handled = 0;

            /* start menu takes priority */
            if (start_menu_open) {
                int rows = (app_count > 0) ? app_count : 1;
                int menu_h = MENU_HDR_H + rows * MENU_ROW_H + MENU_BTM_H;
                int mx0 = 2, my0 = sh - TASKBAR_H - menu_h;
                if (mx < mx0 || mx >= mx0 + MENU_W || my < my0 || my >= sh - TASKBAR_H) {
                    start_menu_open = 0;
                }
                handled = 1; /* menu draw function handles button clicks */
            }

            if (!handled) {
                /* check title bar buttons top-to-bottom in reverse z_order */
                for (int zi = z_count - 1; zi >= 0; zi--) {
                    int s = z_order[zi];
                    window_t *w = &windows[s];
                    if (!w->used || w->minimized) continue;
                    if (mx < w->x || mx >= w->x + w->w) continue;
                    if (my < w->y || my >= w->y + TITLE_H) continue;

                    /* title bar click — check buttons */
                    int bx_close = w->x + w->w - 18;
                    int bx_max   = w->x + w->w - 18 - BTN_GAP;
                    int bx_min   = w->x + w->w - 18 - BTN_GAP*2;
                    int by_btn   = w->y + (TITLE_H - BTN_SZ) / 2;

                    if (mx >= bx_close && mx < bx_close+BTN_SZ &&
                        my >= by_btn && my < by_btn+BTN_SZ) {
                        remove_window(s);
                        handled = 1; break;
                    }
                    if (mx >= bx_max && mx < bx_max+BTN_SZ &&
                        my >= by_btn && my < by_btn+BTN_SZ) {
                        if (!window_has_process(w)) {
                            handled = 1; break;
                        }
                        if (!w->maximized) {
                            w->saved_x=w->x; w->saved_y=w->y;
                            w->saved_w=w->w; w->saved_h=w->h;
                            if (resize_client_surface(w, sw - 2, desktop_h - TITLE_H - 1)) {
                                w->x=0; w->y=0; w->w=sw; w->h=desktop_h;
                                w->maximized=1;
                            }
                        } else {
                            if (resize_client_surface(w,
                                                      w->saved_w - 2,
                                                      w->saved_h - TITLE_H - 1)) {
                                w->x=w->saved_x; w->y=w->saved_y;
                                w->w=w->shm_w + 2;
                                w->h=w->shm_h + TITLE_H + 1;
                                clamp_window_pos(w);
                                w->maximized=0;
                            }
                        }
                        bring_to_front(s);
                        handled = 1; break;
                    }
                    if (mx >= bx_min && mx < bx_min+BTN_SZ &&
                        my >= by_btn && my < by_btn+BTN_SZ) {
                        w->minimized = 1;
                        if (dragging_slot == s) dragging_slot = -1;
                        handled = 1; break;
                    }

                    /* title bar drag */
                    bring_to_front(s);
                    dragging_slot = s;
                    drag_ox = mx - w->x;
                    drag_oy = my - w->y;
                    handled = 1; break;
                }
            }

            if (!handled) {
                /* click window body to focus + forward mouse to app */
                for (int zi = z_count - 1; zi >= 0; zi--) {
                    int s = z_order[zi];
                    window_t *w = &windows[s];
                    if (!w->used || w->minimized) continue;
                    if (mx >= w->x && mx < w->x+w->w &&
                        my >= w->y && my < w->y+w->h) {
                        bring_to_front(s);
                        if (w->shm_ptr && window_has_process(w)) {
                            int local_x = mx - (w->x + 1);
                            int local_y = my - (w->y + TITLE_H);
                            if (local_x >= 0 && local_x < w->shm_w &&
                                local_y >= 0 && local_y < w->shm_h) {
                                tw_msg_mouse_t mm;
                                mm.hdr.type  = TW_MSG_MOUSE_EVENT;
                                mm.hdr.magic = TW_MAGIC;
                                mm.x = local_x;
                                mm.y = local_y;
                                mm.btns = (uint8_t)cur_btns;
                                fry_proc_input((uint32_t)w->pid, &mm, sizeof(mm));
                            }
                        }
                        break;
                    }
                }
            }

        }

        if (!(cur_btns & 1)) dragging_slot = -1;
        if (dragging_slot >= 0 && windows[dragging_slot].used) {
            windows[dragging_slot].x = mx - drag_ox;
            windows[dragging_slot].y = my - drag_oy;
        }

        /* --- poll process output --- */
        long now_ms = fry_gettime();
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if (!w->used) continue;
            uint8_t rawbuf[128];
            long rn = window_has_process(w)
                    ? fry_proc_output((uint32_t)w->pid, rawbuf, sizeof(rawbuf))
                    : 0;
            if (rn > 0) {
                w->launch_progress_seen = 1;
                w->launch_deadline_ms = 0;
                int space = (int)sizeof(w->msgbuf) - w->msglen;
                int copy  = (rn < (long)space) ? (int)rn : space;
                memcpy(w->msgbuf + w->msglen, rawbuf, (size_t)copy);
                w->msglen += copy;
                process_window_output(w);
            } else if (rn < 0 && rn != -EAGAIN && !w->done) {
                w->done = 1;
                int tlen = (int)strlen(w->title);
                if (tlen < 40) strcat(w->title, " [done]");
            }
            if (w->shm_ptr) {
                w->launch_progress_seen = 1;
                w->launch_deadline_ms = 0;
            }
            if (!w->done &&
                !w->launch_progress_seen &&
                !w->launch_watchdog_fired &&
                w->launch_deadline_ms != 0 &&
                now_ms > 0 &&
                (uint64_t)now_ms >= w->launch_deadline_ms) {
                launch_watchdog_expire(w);
            }
        }

        /* --- draw --- */
        gfx_gradient_v(&backbuffer, 0, 0, sw, desktop_h, COL_DESKTOP, COL_DESKTOP_LO);

        /* windows back to front */
        for (int zi = 0; zi < z_count; zi++)
            draw_window(z_order[zi]);

        draw_taskbar(sw, sh, mx, my, cur_btns, prev_btns);

        if (start_menu_open)
            draw_start_menu(sh, mx, my, cur_btns, prev_btns);

        draw_cursor(mx, my);

        memcpy(vram, back_buf, g_info.stride * g_info.height * 4);

        prev_btns = cur_btns;
        fry_sleep(16);
    }
    return 0;
}
