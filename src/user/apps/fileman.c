// fileman.fry — TaterWin file manager (keyboard-driven)

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

#include <stdint.h>

#define INIT_W 760
#define INIT_H 460

#define INPUT_STREAM_MAX 256
#define LIST_BUF_SIZE    8192
#define MAX_ENTRIES      256
#define MAX_NAME         96
#define MAX_PATH_LEN     192
#define MAX_PLACES       16
#define SIDEBAR_W 180
#define TOPBAR_H   48
#define STATUS_H   24
#define ROW_H      18

#define CHAR_W      8

/* Colors */
#define COL_BG_0      0x0E131B
#define COL_BG_1      0x171F2B
#define COL_PANEL     0x131A24
#define COL_PANEL_2   0x1A2230
#define COL_BORDER    0x2A364A
#define COL_TEXT      0xE6ECF8
#define COL_TEXT_DIM  0x95A4BC
#define COL_ACCENT    0x29B6F6
#define COL_ACCENT_2  0x26A69A
#define COL_SELECT    0x24344A
#define COL_WARN      0xF4C96A
#define COL_TRANSP    0xFF000000

typedef struct {
    char name[MAX_NAME];
    uint8_t is_dir;
    uint64_t size;
} file_entry_t;

typedef struct {
    char label[24];
    char path[64];
    uint8_t flags;
} place_t;

/* Fallback buffer if SHM fails */
static uint32_t fallback_pixels[INIT_W * INIT_H];

static int win_w = INIT_W;
static int win_h = INIT_H;
static int g_shm_id = -1;
static uint32_t *pixels = fallback_pixels;
static uint32_t *render_buf = 0;
static gfx_ctx_t g_ctx;
static tw_msg_header_t g_upd;

static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len = 0;
static int needs_redraw = 1;

static char cwd[MAX_PATH_LEN] = "/";
static char status_line[128] = "Ready";

static file_entry_t entries[MAX_ENTRIES];
static int entry_count = 0;
static int selected = 0;
static int scroll_top = 0;

static place_t places[MAX_PLACES];
static int place_count = 0;
static int place_unverified_count = 0;

#define PLACE_UNVERIFIED 0x01

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int strcmp_nocase(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        char ca = ascii_lower(a[i]);
        char cb = ascii_lower(b[i]);
        if (ca != cb) return (int)((unsigned char)ca - (unsigned char)cb);
        i++;
    }
    return (int)((unsigned char)a[i] - (unsigned char)b[i]);
}

static int has_suffix_nocase(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t su = strlen(suffix);
    if (sl < su) return 0;
    return strcmp_nocase(s + (sl - su), suffix) == 0;
}

static int is_shell_tot_name(const char *s) {
    return s && strcmp_nocase(s, "SHELL.TOT") == 0;
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_line, sizeof(status_line), fmt, ap);
    va_end(ap);
}

static int path_is_dir(const char *path) {
    struct fry_stat st;
    if (fry_stat(path, &st) != 0) return 0;
    return (st.attr & 0x10) != 0;
}

static void path_join(const char *base, const char *name, char *out, size_t out_len) {
    if (!base || !name || !out || out_len == 0) return;
    if (base[0] == '/' && base[1] == 0) {
        snprintf(out, out_len, "/%s", name);
    } else {
        snprintf(out, out_len, "%s/%s", base, name);
    }
}

static void normalize_dir(char *path) {
    size_t len;
    if (!path || path[0] == 0) return;
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = 0;
        len--;
    }
}

static void move_parent(char *path) {
    char *slash;
    if (!path || path[0] != '/') return;
    if (path[1] == 0) return;
    slash = path + strlen(path) - 1;
    while (slash > path && *slash != '/') slash--;
    if (slash == path) {
        path[1] = 0;
    } else {
        *slash = 0;
    }
}

static const char *fs_name(uint8_t fs_type) {
    switch (fs_type) {
        case 1: return "FAT32";
        case 2: return "ToTFS";
        case 3: return "NTFS";
        case 4: return "ramdisk";
        default: return "none";
    }
}

static int path_is_nvme_prefix(const char *path) {
    if (!path) return 0;
    return path[0] == '/' &&
           path[1] == 'n' &&
           path[2] == 'v' &&
           path[3] == 'm' &&
           path[4] == 'e';
}

static int live_root_is_ramdisk(void) {
    struct fry_storage_info sto;
    memset(&sto, 0, sizeof(sto));
    if (fry_storage_info(&sto) != 0) return 0;
    return (sto.flags & FRY_STORAGE_FLAG_ROOT_RAMDISK_SOURCE) != 0;
}

static void format_size(uint64_t bytes, char *out, size_t out_len) {
    const uint64_t kb = 1024ULL;
    const uint64_t mb = kb * 1024ULL;
    const uint64_t gb = mb * 1024ULL;
    if (bytes >= gb) {
        uint64_t whole = bytes / gb;
        uint64_t dec = (bytes % gb) * 10ULL / gb;
        snprintf(out, out_len, "%llu.%lluG",
                 (unsigned long long)whole, (unsigned long long)dec);
    } else if (bytes >= mb) {
        uint64_t whole = bytes / mb;
        uint64_t dec = (bytes % mb) * 10ULL / mb;
        snprintf(out, out_len, "%llu.%lluM",
                 (unsigned long long)whole, (unsigned long long)dec);
    } else if (bytes >= kb) {
        uint64_t whole = bytes / kb;
        uint64_t dec = (bytes % kb) * 10ULL / kb;
        snprintf(out, out_len, "%llu.%lluK",
                 (unsigned long long)whole, (unsigned long long)dec);
    } else {
        snprintf(out, out_len, "%lluB", (unsigned long long)bytes);
    }
}

static void copy_token(char *dst, size_t dst_len, const char *src, size_t src_len) {
    size_t n = src_len;
    if (!dst || dst_len == 0) return;
    if (n + 1 > dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int entry_cmp(const file_entry_t *a, const file_entry_t *b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcmp_nocase(a->name, b->name);
}

static void sort_entries(void) {
    int i, j;
    for (i = 1; i < entry_count; i++) {
        file_entry_t key = entries[i];
        j = i - 1;
        while (j >= 0 && entry_cmp(&entries[j], &key) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void ensure_selection_visible(void) {
    int list_y = TOPBAR_H + 28;
    int list_h = win_h - list_y - STATUS_H - 6;
    int visible = list_h / ROW_H;
    if (visible < 1) visible = 1;

    if (selected < 0) selected = 0;
    if (selected >= entry_count) selected = entry_count - 1;
    if (selected < 0) selected = 0;

    if (scroll_top > selected) scroll_top = selected;
    if (selected >= scroll_top + visible) scroll_top = selected - visible + 1;
    if (scroll_top < 0) scroll_top = 0;
}

static int add_place(const char *label, const char *path, uint8_t flags) {
    int i;
    if (!label || !path) return 0;
    for (i = 0; i < place_count; i++) {
        if (strcmp(places[i].path, path) == 0) return 0;
    }
    if (place_count >= MAX_PLACES) return 0;
    strncpy(places[place_count].label, label, sizeof(places[place_count].label) - 1);
    places[place_count].label[sizeof(places[place_count].label) - 1] = 0;
    strncpy(places[place_count].path, path, sizeof(places[place_count].path) - 1);
    places[place_count].path[sizeof(places[place_count].path) - 1] = 0;
    places[place_count].flags = flags;
    place_count++;
    return 1;
}

static void add_place_if_dir(const char *label, const char *path) {
    if (!path_is_dir(path)) return;
    add_place(label, path, 0);
}

static void add_mount_place(const char *mountpoint, uint8_t fs_type) {
    char label[24];
    int is_dir;
    uint8_t flags = 0;
    if (!mountpoint || !mountpoint[0]) return;
    if (strcmp(mountpoint, "/") == 0) return;
    if (strcmp(mountpoint, "/fry") == 0 || strcmp(mountpoint, "/FRY") == 0) return;
    if (fs_type) {
        snprintf(label, sizeof(label), "%s(%s)", mountpoint, fs_name(fs_type));
    } else {
        snprintf(label, sizeof(label), "%s", mountpoint);
    }
    is_dir = path_is_dir(mountpoint);
    if (!is_dir) flags |= PLACE_UNVERIFIED;
    if (add_place(label, mountpoint, flags) && !is_dir) {
        place_unverified_count++;
    }
}

static void rebuild_places(void) {
    struct fry_mounts_info mi;
    uint32_t i;
    uint32_t max;
    place_count = 0;
    place_unverified_count = 0;
    add_place_if_dir("Root", "/");
    add_place_if_dir("System", "/system");
    add_place_if_dir("Apps", "/apps");
    add_place_if_dir("Legacy", "/fry");

    memset(&mi, 0, sizeof(mi));
    if (fry_mounts_info(&mi) == 0) {
        max = mi.count;
        if (max > FRY_MAX_MOUNT_INFO) max = FRY_MAX_MOUNT_INFO;
        for (i = 0; i < max; i++) {
            add_mount_place(mi.entries[i].mount, mi.entries[i].fs_type);
        }
    }

    /* Fallback for older kernels without SYS_MOUNTS_INFO. */
    add_place_if_dir("NVMe", "/nvme");
    add_place_if_dir("EFI", "/EFI");
    add_place_if_dir("Boot", "/boot");
    if (place_count == 0) add_place_if_dir("Root", "/");
}

static int reload_dir(void) {
    char buf[LIST_BUF_SIZE];
    char keep_name[MAX_NAME];
    long n;
    int old_selected = selected;
    int i;

    keep_name[0] = 0;
    if (old_selected >= 0 && old_selected < entry_count) {
        strncpy(keep_name, entries[old_selected].name, sizeof(keep_name) - 1);
        keep_name[sizeof(keep_name) - 1] = 0;
    }

    entry_count = 0;
    selected = 0;
    scroll_top = 0;

    n = fry_readdir_ex(cwd, buf, sizeof(buf));
    if (n >= 0) {
        uint32_t pos = 0;
        while (pos + sizeof(struct fry_dirent) <= (uint32_t)n && entry_count < MAX_ENTRIES) {
            struct fry_dirent *de = (struct fry_dirent *)(buf + pos);
            if (de->rec_len < sizeof(struct fry_dirent)) break;
            if (pos + de->rec_len > (uint32_t)n) break;
            uint32_t payload = (uint32_t)de->rec_len - (uint32_t)sizeof(struct fry_dirent);
            if (de->name_len + 1 > payload) {
                pos += de->rec_len;
                continue;
            }
            uint32_t name_len = de->name_len;
            if (name_len >= MAX_NAME) name_len = MAX_NAME - 1;
            for (uint32_t i = 0; i < name_len; i++) {
                entries[entry_count].name[i] = de->name[i];
            }
            entries[entry_count].name[name_len] = 0;
            if (strcmp(entries[entry_count].name, ".") == 0 ||
                strcmp(entries[entry_count].name, "..") == 0) {
                pos += de->rec_len;
                continue;
            }
            entries[entry_count].is_dir = (uint8_t)((de->attr & 0x10) != 0);
            entries[entry_count].size = de->size;
            entry_count++;
            pos += de->rec_len;
        }
    } else {
        n = fry_readdir(cwd, buf, sizeof(buf) - 1);
        if (n < 0) {
            set_status("readdir failed: %s", cwd);
            return -1;
        }
        buf[n] = 0;

        {
            char *p = buf;
            while (*p && entry_count < MAX_ENTRIES) {
                char *start = p;
                size_t len;
                struct fry_stat st;
                char full[MAX_PATH_LEN];

                while (*p && *p != '\n' && *p != '\r') p++;
                len = (size_t)(p - start);
                while (*p == '\n' || *p == '\r') p++;
                if (len == 0) continue;

                copy_token(entries[entry_count].name, sizeof(entries[entry_count].name), start, len);
                if (strcmp(entries[entry_count].name, ".") == 0 ||
                    strcmp(entries[entry_count].name, "..") == 0) {
                    continue;
                }

                path_join(cwd, entries[entry_count].name, full, sizeof(full));
                if (fry_stat(full, &st) == 0) {
                    entries[entry_count].is_dir = (uint8_t)((st.attr & 0x10) != 0);
                    entries[entry_count].size = st.size;
                } else {
                    entries[entry_count].is_dir = 0;
                    entries[entry_count].size = 0;
                }
                entry_count++;
            }
        }
    }

    sort_entries();

    if (keep_name[0]) {
        for (i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, keep_name) == 0) {
                selected = i;
                break;
            }
        }
    }

    ensure_selection_visible();
    set_status("%d item(s) in %s", entry_count, cwd);
    return 0;
}

static int change_dir(const char *path) {
    char next[MAX_PATH_LEN];

    if (!path || !path[0]) return -1;
    if (path[0] == '/') {
        strncpy(next, path, sizeof(next) - 1);
        next[sizeof(next) - 1] = 0;
    } else {
        path_join(cwd, path, next, sizeof(next));
    }
    normalize_dir(next);

    if (!path_is_dir(next)) {
        set_status("not a directory: %s", next);
        return -1;
    }

    strncpy(cwd, next, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
    reload_dir();
    return 0;
}

static void open_selected(void) {
    char full[MAX_PATH_LEN];
    long pid;

    if (entry_count <= 0 || selected < 0 || selected >= entry_count) return;
    path_join(cwd, entries[selected].name, full, sizeof(full));

    if (entries[selected].is_dir) {
        change_dir(full);
        return;
    }

    if (has_suffix_nocase(entries[selected].name, ".FRY") ||
        is_shell_tot_name(entries[selected].name)) {
        if (live_root_is_ramdisk() && path_is_nvme_prefix(full)) {
            set_status("live mode: launch blocked from secondary storage");
            return;
        }
        pid = fry_spawn(full);
        if (pid >= 0) {
            set_status("launched %s (pid=%ld)", entries[selected].name, pid);
        } else {
            set_status("spawn failed: %s (rc=%ld)", entries[selected].name, pid);
        }
    } else {
        char sz[24];
        format_size(entries[selected].size, sz, sizeof(sz));
        set_status("file: %s (%s)", entries[selected].name, sz);
    }
}

static void go_parent(void) {
    char next[MAX_PATH_LEN];
    strncpy(next, cwd, sizeof(next) - 1);
    next[sizeof(next) - 1] = 0;
    move_parent(next);
    change_dir(next);
}

static void draw_text_clip(gfx_ctx_t *ctx, int x, int y, int max_chars,
                           const char *text, uint32_t fg, uint32_t bg) {
    char tmp[128];
    int len;
    int out_len;

    if (max_chars <= 0) return;
    len = (int)strlen(text);
    if (len <= max_chars) {
        gfx_draw_text(ctx, (uint32_t)x, (uint32_t)y, text, fg, bg);
        return;
    }
    out_len = max_chars;
    if (out_len >= (int)sizeof(tmp)) out_len = (int)sizeof(tmp) - 1;
    if (out_len < 4) return;
    memcpy(tmp, text, (size_t)(out_len - 3));
    tmp[out_len - 3] = '.';
    tmp[out_len - 2] = '.';
    tmp[out_len - 1] = '.';
    tmp[out_len] = 0;
    gfx_draw_text(ctx, (uint32_t)x, (uint32_t)y, tmp, fg, bg);
}

static void render(gfx_ctx_t *ctx) {
    int sidebar_h;
    int list_x;
    int list_y;
    int list_w;
    int list_h;
    int rows_visible;
    int i;
    struct fry_storage_info sto;
    struct fry_path_fs_info pfs;
    char line[160];

    gfx_gradient_v(ctx, 0, 0, (uint32_t)win_w, (uint32_t)win_h, COL_BG_1, COL_BG_0);

    /* Top bar */
    gfx_gradient_h(ctx, 0, 0, (uint32_t)win_w, TOPBAR_H, 0x1B2433, 0x101723);
    gfx_fill(ctx, 0, TOPBAR_H - 1, (uint32_t)win_w, 1, COL_BORDER);
    gfx_draw_text(ctx, 10, 6, "File Manager", COL_TEXT, COL_TRANSP);
    gfx_fill(ctx, 140, 8, (uint32_t)(win_w - 150), 30, COL_PANEL);
    gfx_rect(ctx, 140, 8, (uint32_t)(win_w - 150), 30, COL_BORDER);
    draw_text_clip(ctx, 148, 14, (win_w - 168) / CHAR_W, cwd, COL_ACCENT, COL_TRANSP);

    /* Sidebar */
    sidebar_h = win_h - TOPBAR_H - STATUS_H;
    if (sidebar_h < 20) sidebar_h = 20;
    gfx_fill(ctx, 0, TOPBAR_H, SIDEBAR_W, (uint32_t)sidebar_h, COL_PANEL);
    gfx_rect(ctx, 0, TOPBAR_H, SIDEBAR_W, (uint32_t)sidebar_h, COL_BORDER);
    gfx_draw_text(ctx, 10, TOPBAR_H + 8, "Places", COL_TEXT_DIM, COL_TRANSP);
    for (i = 0; i < place_count; i++) {
        int py = TOPBAR_H + 30 + i * 24;
        int active = (strcmp(cwd, places[i].path) == 0);
        uint32_t fg = active ? COL_TEXT : COL_TEXT_DIM;
        if (places[i].flags & PLACE_UNVERIFIED) fg = COL_WARN;
        if (active) {
            gfx_fill(ctx, 6, py - 2, SIDEBAR_W - 12, 20, COL_SELECT);
            gfx_fill(ctx, 6, py - 2, 3, 20, COL_ACCENT_2);
        }
        if (places[i].flags & PLACE_UNVERIFIED) {
            snprintf(line, sizeof(line), "%d %s?", i + 1, places[i].label);
        } else {
            snprintf(line, sizeof(line), "%d %s", i + 1, places[i].label);
        }
        draw_text_clip(ctx, 12, py, 20, line, fg, COL_TRANSP);
    }

    /* File list panel */
    list_x = SIDEBAR_W + 8;
    list_y = TOPBAR_H + 8;
    list_w = win_w - list_x - 8;
    list_h = win_h - list_y - STATUS_H - 8;
    if (list_w < 64) list_w = 64;
    if (list_h < 60) list_h = 60;

    gfx_fill(ctx, (uint32_t)list_x, (uint32_t)list_y, (uint32_t)list_w, (uint32_t)list_h, COL_PANEL_2);
    gfx_rect(ctx, (uint32_t)list_x, (uint32_t)list_y, (uint32_t)list_w, (uint32_t)list_h, COL_BORDER);
    gfx_fill(ctx, (uint32_t)(list_x + 1), (uint32_t)(list_y + 1), (uint32_t)(list_w - 2), 22, 0x202B3D);

    gfx_draw_text(ctx, (uint32_t)(list_x + 8), (uint32_t)(list_y + 4), "Name", COL_TEXT_DIM, COL_TRANSP);
    gfx_draw_text(ctx, (uint32_t)(list_x + list_w - 170), (uint32_t)(list_y + 4), "Type", COL_TEXT_DIM, COL_TRANSP);
    gfx_draw_text(ctx, (uint32_t)(list_x + list_w - 86), (uint32_t)(list_y + 4), "Size", COL_TEXT_DIM, COL_TRANSP);

    rows_visible = (list_h - 26) / ROW_H;
    if (rows_visible < 1) rows_visible = 1;
    ensure_selection_visible();

    for (i = 0; i < rows_visible; i++) {
        int idx = scroll_top + i;
        int ry = list_y + 24 + i * ROW_H;
        int is_sel;
        const char *kind;
        char sizebuf[24];
        uint32_t fg;
        int name_chars;
        if (idx >= entry_count) break;

        is_sel = (idx == selected);
        if (is_sel) {
            gfx_fill(ctx, (uint32_t)(list_x + 1), (uint32_t)ry, (uint32_t)(list_w - 2), (uint32_t)ROW_H, COL_SELECT);
            gfx_fill(ctx, (uint32_t)(list_x + 1), (uint32_t)ry, 2, (uint32_t)ROW_H, COL_ACCENT);
        }

        fg = entries[idx].is_dir ? 0x9DD9FF : COL_TEXT;
        kind = entries[idx].is_dir ? "dir" : "file";
        if (entries[idx].is_dir) strcpy(sizebuf, "-");
        else format_size(entries[idx].size, sizebuf, sizeof(sizebuf));

        draw_text_clip(ctx, list_x + 8, ry + 1, 3, entries[idx].is_dir ? "[D]" : "[F]", fg, COL_TRANSP);
        name_chars = (list_w - 250) / CHAR_W;
        if (name_chars < 6) name_chars = 6;
        draw_text_clip(ctx, list_x + 36, ry + 1, name_chars, entries[idx].name, fg, COL_TRANSP);
        draw_text_clip(ctx, list_x + list_w - 170, ry + 1, 8, kind, COL_TEXT_DIM, COL_TRANSP);
        draw_text_clip(ctx, list_x + list_w - 86, ry + 1, 10, sizebuf, COL_TEXT_DIM, COL_TRANSP);
    }

    /* Status bar */
    gfx_fill(ctx, 0, (uint32_t)(win_h - STATUS_H), (uint32_t)win_w, STATUS_H, 0x0F151F);
    gfx_fill(ctx, 0, (uint32_t)(win_h - STATUS_H), (uint32_t)win_w, 1, COL_BORDER);
    draw_text_clip(ctx, 8, win_h - STATUS_H + 5, (win_w / CHAR_W) - 44, status_line, COL_WARN, COL_TRANSP);

    memset(&sto, 0, sizeof(sto));
    memset(&pfs, 0, sizeof(pfs));
    if (fry_storage_info(&sto) == 0) {
        const char *cwd_fs = "?";
        const char *cwd_mount = "?";
        if (fry_path_fs_info(cwd, &pfs) == 0) {
            cwd_fs = fs_name(pfs.fs_type);
            if (pfs.mount[0]) cwd_mount = pfs.mount;
        }
        if (place_unverified_count > 0) {
            snprintf(line, sizeof(line), "cwd:%s@%s root:%s nvme:%s mounts:%d?",
                     cwd_fs, cwd_mount,
                     fs_name(sto.root_fs_type),
                     sto.nvme_detected ? "yes" : "no",
                     place_unverified_count);
        } else {
            snprintf(line, sizeof(line), "cwd:%s@%s root:%s nvme:%s",
                     cwd_fs, cwd_mount,
                     fs_name(sto.root_fs_type),
                     sto.nvme_detected ? "yes" : "no");
        }
    } else {
        snprintf(line, sizeof(line), "cwd:? root:? nvme:?");
    }
    draw_text_clip(ctx, win_w - 330, win_h - STATUS_H + 5, 40, line, COL_TEXT_DIM, COL_TRANSP);
}

static void handle_resize(int new_shm_id, uint64_t new_shm_ptr, int new_w, int new_h) {
    long ptr;
    uint32_t *new_pixels;
    uint32_t *new_render;

    if (new_shm_id < 0 || new_shm_ptr == 0 || new_w <= 0 || new_h <= 0) return;
    ptr = fry_shm_map(new_shm_id);
    if (ptr <= 0) return;

    new_pixels = (uint32_t *)(uintptr_t)ptr;
    new_render = malloc((size_t)new_w * (size_t)new_h * 4);
    if (!new_render) return;

    g_shm_id = new_shm_id;
    pixels = new_pixels;
    win_w = new_w;
    win_h = new_h;

    if (render_buf) free(render_buf);
    render_buf = new_render;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);
    needs_redraw = 1;
}

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

static void input_append(const uint8_t *src, int n) {
    if (!src || n <= 0) return;
    if (n > INPUT_STREAM_MAX) {
        src += (n - INPUT_STREAM_MAX);
        n = INPUT_STREAM_MAX;
    }
    if (input_stream_len + n > INPUT_STREAM_MAX) {
        int drop = input_stream_len + n - INPUT_STREAM_MAX;
        input_consume(drop);
    }
    memcpy(input_stream + input_stream_len, src, (size_t)n);
    input_stream_len += n;
}

static int maybe_tw_prefix(void) {
    static const uint8_t magic_le[4] = {0x4E, 0x49, 0x57, 0x54}; /* TWIN little-endian */
    int i;
    if (input_stream_len <= 0) return 0;
    if (input_stream[0] < 1 || input_stream[0] > 7) return 0;
    if (input_stream_len >= 2 && input_stream[1] != 0) return 0;
    if (input_stream_len >= 3 && input_stream[2] != 0) return 0;
    if (input_stream_len >= 4 && input_stream[3] != 0) return 0;
    for (i = 4; i < input_stream_len && i < 8; i++) {
        if (input_stream[i] != magic_le[i - 4]) return 0;
    }
    return 1;
}

static void handle_key(char c) {
    int idx;
    if (c == '\r') c = '\n';

    if (c >= '1' && c <= '9') {
        idx = c - '1';
        if (idx >= 0 && idx < place_count) {
            change_dir(places[idx].path);
            needs_redraw = 1;
        }
        return;
    }

    switch ((unsigned char)c) {
        case 'q':
        case 'Q':
            fry_exit(0);
            return;
        case 'j':
        case 's':
            if (selected + 1 < entry_count) {
                selected++;
                ensure_selection_visible();
                needs_redraw = 1;
            }
            return;
        case 'k':
        case 'w':
            if (selected > 0) {
                selected--;
                ensure_selection_visible();
                needs_redraw = 1;
            }
            return;
        case 'g':
            selected = 0;
            ensure_selection_visible();
            needs_redraw = 1;
            return;
        case 'G':
            selected = entry_count - 1;
            ensure_selection_visible();
            needs_redraw = 1;
            return;
        case 'h':
        case '\b':
        case 127:
            go_parent();
            needs_redraw = 1;
            return;
        case 'l':
        case '\n':
            open_selected();
            rebuild_places();
            needs_redraw = 1;
            return;
        case 'r':
        case 'R':
            reload_dir();
            rebuild_places();
            needs_redraw = 1;
            return;
        default:
            return;
    }
}

static void handle_mouse_click(int x, int y) {
    int i;

    /* Sidebar place click */
    if (x < SIDEBAR_W && y >= TOPBAR_H) {
        for (i = 0; i < place_count; i++) {
            int py = TOPBAR_H + 30 + i * 24;
            if (y >= py - 2 && y < py + 18) {
                change_dir(places[i].path);
                rebuild_places();
                needs_redraw = 1;
                return;
            }
        }
        return;
    }

    /* File list click */
    {
        int list_x = SIDEBAR_W + 8;
        int list_y = TOPBAR_H + 8;
        int data_y = list_y + 24;

        if (x >= list_x && x < win_w - 8 && y >= data_y) {
            int row = (y - data_y) / ROW_H;
            int idx = scroll_top + row;
            if (idx >= 0 && idx < entry_count) {
                selected = idx;
                ensure_selection_visible();
                open_selected();
                rebuild_places();
                needs_redraw = 1;
            }
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

int main(void) {
    tw_msg_create_win_t req;
    tw_msg_win_created_t wresp;
    uint8_t winbuf[sizeof(wresp)];
    int winlen = 0;
    int got = 0;
    int tries = 0;

    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = INIT_W;
    req.h = INIT_H;
    strncpy(req.title, "File Manager", sizeof(req.title) - 1);
    req.title[sizeof(req.title) - 1] = 0;
    fry_write(1, &req, sizeof(req));

    memset(&wresp, 0, sizeof(wresp));
    while (!got && tries < 300) {
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
                        wresp = cand;
                        got = 1;
                        break;
                    }
                }
            }
        } else {
            fry_sleep(10);
            tries++;
        }
    }

    pixels = fallback_pixels;
    if (got) {
        long ptr = fry_shm_map(wresp.shm_id);
        if (ptr > 0) {
            pixels = (uint32_t *)(uintptr_t)ptr;
            g_shm_id = wresp.shm_id;
        }
    }

    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    if (!render_buf) return 1;
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    g_upd.type = TW_MSG_UPDATE;
    g_upd.magic = TW_MAGIC;

    rebuild_places();
    change_dir("/");

    for (;;) {
        char in[64];
        long n = fry_read(0, in, sizeof(in));
        if (n > 0) {
            input_append((const uint8_t *)in, (int)n);
            process_input_stream();
        }

        if (needs_redraw) {
            render(&g_ctx);
            memcpy(pixels, render_buf, (size_t)win_w * (size_t)win_h * 4);
            fry_write(1, &g_upd, sizeof(g_upd));
            needs_redraw = 0;
        }
        fry_sleep(33);
    }

    return 0;
}
