// TaterTOS64v3 shell — TaterWin SHM graphical terminal with scrollback + resize-aware rendering

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

/* Fixed constants */
#define CHAR_W    8
#define CHAR_H    16
#define MAX_TERM_COLS  200
#define MAX_TERM_ROWS  100
#define SCROLLBACK_MAX 500
#define SCROLLBAR_W    10

/* Dynamic sizing — updated on resize */
static int win_w = 480;
static int win_h = 320;
static int term_cols;   /* set in main: (win_w - SCROLLBAR_W) / CHAR_W */
static int term_rows;   /* set in main: win_h / CHAR_H */

/* Pixel buffer: points to SHM when available, else malloc'd fallback */
static uint32_t *pixels = NULL;

/* Screen buffer: flat array, each row is (MAX_TERM_COLS + 1) bytes for stable stride */
static char *screen_buf = NULL;

/* Scrollback ring buffer */
static char *scrollback = NULL;
static int sb_count = 0;   /* lines stored (up to SCROLLBACK_MAX) */
static int sb_head = 0;    /* oldest line index in ring */
static int scroll_offset = 0; /* 0 = at bottom, >0 = scrolled up N lines */

/* Terminal cursor */
static int cur_row, cur_col;

/* Input */
static char input_line[256];
static int input_pos;

/* Working directory */
static char cwd[128] = "/";

/* Graphics / TaterWin globals */
static gfx_ctx_t g_ctx;
static tw_msg_header_t g_upd;
static int g_shm_id = -1;

/* Input stream buffer: keyboard bytes + TW protocol frames (e.g. RESIZED). */
#define INPUT_STREAM_MAX 256
static uint8_t input_stream[INPUT_STREAM_MAX];
static int input_stream_len = 0;
static int needs_redraw = 1;

/* Double-buffer: render to local buf, then memcpy to SHM atomically */
static uint32_t *render_buf = NULL;

/* --- Accessor helpers --- */

#define SCREEN_STRIDE (MAX_TERM_COLS + 1)

static char *screen_row(int r) {
    return screen_buf + r * SCREEN_STRIDE;
}

static void build_path(const char *in, char *out, size_t max);

static char *sb_line(int idx) {
    /* idx: 0 = oldest, sb_count-1 = newest */
    int real = (sb_head + idx) % SCROLLBACK_MAX;
    return scrollback + real * SCREEN_STRIDE;
}

static void sb_push_line(const char *line) {
    int slot;
    if (sb_count < SCROLLBACK_MAX) {
        slot = (sb_head + sb_count) % SCROLLBACK_MAX;
        sb_count++;
    } else {
        slot = sb_head;
        sb_head = (sb_head + 1) % SCROLLBACK_MAX;
    }
    char *dst = scrollback + slot * SCREEN_STRIDE;
    int len = term_cols;
    if (len > MAX_TERM_COLS) len = MAX_TERM_COLS;
    memcpy(dst, line, (size_t)len);
    dst[len] = 0;
}

/* ================================================================
 * Terminal emulator
 * ================================================================ */

static void term_init(void) {
    for (int r = 0; r < term_rows; r++) {
        memset(screen_row(r), ' ', (size_t)term_cols);
        screen_row(r)[term_cols] = 0;
    }
    cur_row = 0;
    cur_col = 0;
    needs_redraw = 1;
}

static void term_scroll(void) {
    /* Save scrolled-off top line into scrollback */
    sb_push_line(screen_row(0));

    /* Scroll screen content up by one line */
    for (int r = 0; r < term_rows - 1; r++)
        memcpy(screen_row(r), screen_row(r + 1), (size_t)SCREEN_STRIDE);
    memset(screen_row(term_rows - 1), ' ', (size_t)term_cols);
    screen_row(term_rows - 1)[term_cols] = 0;
    needs_redraw = 1;
}

static void term_putc(char c) {
    needs_redraw = 1;
    /* Any new output resets scroll view to bottom */
    scroll_offset = 0;

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= term_rows) {
            cur_row = term_rows - 1;
            term_scroll();
        }
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            screen_row(cur_row)[cur_col] = ' ';
        }
        return;
    }
    if (c == '\t') {
        int target = (cur_col + 4) & ~3;
        if (target > term_cols) target = term_cols;
        while (cur_col < target) {
            screen_row(cur_row)[cur_col] = ' ';
            cur_col++;
        }
        if (cur_col >= term_cols) {
            cur_col = 0;
            cur_row++;
            if (cur_row >= term_rows) {
                cur_row = term_rows - 1;
                term_scroll();
            }
        }
        return;
    }
    if ((unsigned char)c >= 0x20) {
        screen_row(cur_row)[cur_col] = c;
        cur_col++;
        if (cur_col >= term_cols) {
            cur_col = 0;
            cur_row++;
            if (cur_row >= term_rows) {
                cur_row = term_rows - 1;
                term_scroll();
            }
        }
    }
}

static void term_puts(const char *s) {
    while (*s) term_putc(*s++);
}

static void term_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

/* ================================================================
 * Rendering
 * ================================================================ */

static void draw_scrollbar(gfx_ctx_t *ctx) {
    int sb_x = win_w - SCROLLBAR_W;
    int total_lines = sb_count + term_rows;
    int visible_lines = term_rows;

    /* Track background */
    gfx_fill(ctx, sb_x, 0, SCROLLBAR_W, win_h, 0x1A1A1A);
    /* Track left border */
    gfx_fill(ctx, sb_x, 0, 1, win_h, 0x333333);

    if (total_lines <= visible_lines) {
        /* Everything fits — full-height thumb */
        gfx_fill(ctx, sb_x + 2, 2, SCROLLBAR_W - 4, win_h - 4, 0x555555);
        return;
    }

    /* Thumb size proportional to visible/total */
    int track_h = win_h - 4;
    int thumb_h = (visible_lines * track_h) / total_lines;
    if (thumb_h < 16) thumb_h = 16;

    /* Thumb position: scroll_offset=0 → bottom, scroll_offset=sb_count → top */
    int max_off = sb_count;
    int thumb_y;
    if (scroll_offset <= 0 || max_off <= 0) {
        /* Pin exactly to bottom when at live tail to avoid jitter. */
        thumb_y = win_h - 2 - thumb_h;
    } else if (scroll_offset >= max_off) {
        /* Pin exactly to top when fully scrolled back. */
        thumb_y = 2;
    } else {
        int range = track_h - thumb_h;
        thumb_y = 2 + ((max_off - scroll_offset) * range) / max_off;
    }
    if (thumb_y < 2) thumb_y = 2;
    if (thumb_y + thumb_h > win_h - 2) thumb_y = win_h - 2 - thumb_h;

    gfx_fill(ctx, sb_x + 2, thumb_y, SCROLLBAR_W - 4, thumb_h, 0x666666);
}

static void term_render(gfx_ctx_t *ctx) {
    gfx_fill(ctx, 0, 0, win_w, win_h, 0x101010);

    if (scroll_offset == 0) {
        /* Normal view: show screen buffer */
        for (int r = 0; r < term_rows; r++) {
            screen_row(r)[term_cols] = 0;
            gfx_draw_text(ctx, 0, (uint32_t)(r * CHAR_H), screen_row(r),
                           0xC0C0C0, 0x101010);
        }
        /* Block cursor */
        uint32_t cx = (uint32_t)(cur_col * CHAR_W);
        uint32_t cy = (uint32_t)(cur_row * CHAR_H);
        gfx_fill(ctx, cx, cy, CHAR_W, CHAR_H, 0xC0C0C0);
        if (screen_row(cur_row)[cur_col] != ' ') {
            gfx_draw_char(ctx, cx, cy, screen_row(cur_row)[cur_col],
                           0x101010, 0xC0C0C0);
        }
    } else {
        /* Scrolled-up view: show scrollback + top of screen */
        int view_start_sb = sb_count - scroll_offset;
        if (view_start_sb < 0) view_start_sb = 0;

        for (int r = 0; r < term_rows; r++) {
            int line_idx = view_start_sb + r;
            const char *line;
            if (line_idx < sb_count) {
                line = sb_line(line_idx);
            } else {
                int screen_r = line_idx - sb_count;
                if (screen_r >= term_rows) break;
                line = screen_row(screen_r);
            }
            gfx_draw_text(ctx, 0, (uint32_t)(r * CHAR_H), line,
                           0x808080, 0x101010); /* dimmer when scrolled up */
        }
        /* No cursor when scrolled up */
    }

    draw_scrollbar(ctx);
}

static void render_and_update(void) {
    term_render(&g_ctx);
    memcpy(pixels, render_buf, (size_t)win_w * (size_t)win_h * 4);
    fry_write(1, &g_upd, sizeof(g_upd));
}

static void handle_resize(int new_shm_id, uint64_t new_shm_ptr,
                           int new_w, int new_h) {
    (void)new_shm_ptr;

    /* Map new SHM into our address space */
    long ptr = fry_shm_map(new_shm_id);
    if (ptr <= 0) return; /* keep old size on failure */

    int old_rows = term_rows;
    int old_cols = term_cols;

    /* Update dimensions */
    win_w = new_w;
    win_h = new_h;
    term_cols = (win_w - SCROLLBAR_W) / CHAR_W;
    term_rows = win_h / CHAR_H;
    if (term_cols > MAX_TERM_COLS) term_cols = MAX_TERM_COLS;
    if (term_rows > MAX_TERM_ROWS) term_rows = MAX_TERM_ROWS;
    if (term_cols < 10) term_cols = 10;
    if (term_rows < 4)  term_rows = 4;

    /* If width grew, clear newly exposed columns on existing rows. */
    if (term_cols > old_cols) {
        int common_rows = (old_rows < term_rows) ? old_rows : term_rows;
        for (int r = 0; r < common_rows; r++) {
            memset(screen_row(r) + old_cols, ' ', (size_t)(term_cols - old_cols));
        }
    }

    /* Keep all visible rows nul-terminated at the new width. */
    {
        int common_rows = (old_rows < term_rows) ? old_rows : term_rows;
        for (int r = 0; r < common_rows; r++) {
            screen_row(r)[term_cols] = 0;
        }
    }

    /* Initialize rows added by height growth (screen_buf is MAX-sized, no realloc needed). */
    for (int r = old_rows; r < term_rows; r++) {
        memset(screen_row(r), ' ', (size_t)term_cols);
        screen_row(r)[term_cols] = 0;
    }

    /* Clamp cursor */
    if (cur_row >= term_rows) cur_row = term_rows - 1;
    if (cur_col >= term_cols) cur_col = term_cols - 1;

    /* Update pixel pointer and gfx context */
    g_shm_id = new_shm_id;
    pixels = (uint32_t *)(uintptr_t)ptr;
    free(render_buf);
    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);
    needs_redraw = 1;
}

/* ================================================================
 * Shell utilities
 * ================================================================ */

static void print_prompt(void) {
    term_printf("tater> ");
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int tokenize(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !is_space(*p)) p++;
        if (*p) { *p = 0; p++; }
    }
    return argc;
}

static const char *fs_type_name(uint8_t t) {
    switch (t) {
        case 1: return "FAT32";
        case 2: return "ToTFS";
        case 3: return "NTFS";
        case 4: return "ramdisk";
        default: return "none";
    }
}

static void print_mount_dbg(void) {
    struct fry_mounts_dbg md;
    if (fry_mounts_dbg(&md) != 0) {
        term_printf("mounts: debug unavailable\n");
        return;
    }
    term_printf("mounts dbg (%u):\n", (unsigned)md.count);
    for (uint32_t i = 0; i < md.count && i < FRY_MAX_MOUNT_INFO; i++) {
        struct fry_mount_dbg *m = &md.entries[i];
        term_printf("  %-10s %-6s lba=%llu sector=%u block=%u\n",
                    m->mount,
                    fs_type_name(m->fs_type),
                    (unsigned long long)m->part_lba,
                    (unsigned)m->sector_size,
                    (unsigned)m->block_size);
    }
}

static void print_storage_info(void) {
    struct fry_storage_info si;
    if (fry_storage_info(&si) == 0) {
        term_printf("storage: nvme=%u sector=%u total=%llu root=%s (%s%s) secondary=%s (%s)\n",
                    (unsigned)si.nvme_detected,
                    (unsigned)si.sector_size,
                    (unsigned long long)si.total_sectors,
                    si.root_mount,
                    fs_type_name(si.root_fs_type),
                    (si.flags & FRY_STORAGE_FLAG_ROOT_RAMDISK_SOURCE) ? ", live" : "",
                    si.secondary_mount,
                    fs_type_name(si.secondary_fs_type));
    } else {
        term_printf("storage: unavailable\n");
    }
}

static void print_mounts_info(void) {
    struct fry_mounts_info mi;
    if (fry_mounts_info(&mi) == 0) {
        term_printf("mounts (%u):\n", (unsigned)mi.count);
        for (uint32_t i = 0; i < mi.count && i < FRY_MAX_MOUNT_INFO; i++) {
            term_printf("  %-12s %s\n", mi.entries[i].mount, fs_type_name(mi.entries[i].fs_type));
        }
    } else {
        term_printf("mounts: unavailable\n");
    }
}

static void print_storage_and_mounts(void) {
    print_storage_info();
    print_mounts_info();
    print_mount_dbg();
}

static void cmd_storage(void) {
    print_storage_info();
}

static void cmd_mounts(void) {
    print_mounts_info();
    print_mount_dbg();
}

static void vfs_print_stat(const char *path) {
    struct fry_stat st;
    long rc = fry_stat(path, &st);
    term_printf("  %-20s : %s", path, (rc == 0) ? "ok" : "missing");
    if (rc == 0) {
        term_printf(" size=%llu attr=0x%02x", (unsigned long long)st.size, (unsigned)st.attr);
    }
    term_putc('\n');
}

static void vfs_readdir_brief(const char *path) {
    char buf[2048];
    long n = fry_readdir_ex(path, buf, sizeof(buf));
    if (n < 0) {
        term_printf("  readdir %s: fail\n", path);
        return;
    }
    term_printf("  dir %s (%ld bytes)\n", path, n);
    uint32_t off = 0;
    int shown = 0;
    while (off + sizeof(struct fry_dirent) <= (uint32_t)n && shown < 40) {
        struct fry_dirent *de = (struct fry_dirent *)(buf + off);
        if (de->rec_len < sizeof(struct fry_dirent)) break;
        if (off + (uint32_t)de->rec_len > (uint32_t)n) break;
        uint32_t payload = (uint32_t)de->rec_len - (uint32_t)sizeof(struct fry_dirent);
        if ((uint32_t)de->name_len + 1u > payload) {
            off += de->rec_len;
            continue;
        }
        char type = (de->attr & 0x10) ? 'd' : 'f';
        char name[128];
        uint32_t len = de->name_len;
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        for (uint32_t i = 0; i < len; i++) name[i] = de->name[i];
        name[len] = 0;
        term_printf("    %c %-24s size=%llu attr=0x%02x\n",
                    type, name, (unsigned long long)de->size, (unsigned)de->attr);
        off += de->rec_len;
        shown++;
    }
    if (off < (uint32_t)n) term_printf("    ... (truncated)\n");
}

static void nvmefix_probe(const char *path, const char *tag) {
    struct fry_path_fs_info pfi;
    term_printf("%s:\n", tag);
    if (fry_path_fs_info(path, &pfi) == 0) {
        term_printf("  pathfs %-16s -> mount=%s fs=%s\n",
                    path, pfi.mount, fs_type_name(pfi.fs_type));
    } else {
        term_printf("  pathfs %-16s -> unavailable\n", path);
    }
    vfs_print_stat(path);
    vfs_readdir_brief(path);
}

static void build_path(const char *in, char *out, size_t max) {
    if (!in || !out || max == 0) return;
    if (strcmp(in, ".") == 0) {
        strcpy(out, cwd);
        return;
    }
    if (strcmp(in, "..") == 0) {
        strcpy(out, cwd);
        size_t len = strlen(out);
        if (len > 1) {
            if (out[len - 1] == '/') out[len - 1] = 0;
            char *p = out + strlen(out);
            while (p > out && *(p - 1) != '/') p--;
            if (p > out) *p = 0;
            if (out[0] == 0) strcpy(out, "/");
        }
        return;
    }
    if (in[0] == '/') {
        size_t i = 0;
        while (in[i] && i + 1 < max) { out[i] = in[i]; i++; }
        out[i] = 0;
        return;
    }
    size_t i = 0;
    if (cwd[0] == '/' && cwd[1] == 0) {
        out[i++] = '/';
    } else {
        while (cwd[i] && i + 1 < max) { out[i] = cwd[i]; i++; }
        if (i + 1 < max && out[i - 1] != '/') out[i++] = '/';
    }
    size_t j = 0;
    while (in[j] && i + 1 < max) { out[i++] = in[j++]; }
    out[i] = 0;
}

static int is_dir(const char *path) {
    struct fry_stat st;
    if (fry_stat(path, &st) != 0) return 0;
    return (st.attr & 0x10) != 0;
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int has_suffix_nocase(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t su = strlen(suffix);
    if (sl < su) return 0;
    for (size_t i = 0; i < su; i++) {
        if (ascii_upper(s[sl - su + i]) != ascii_upper(suffix[i])) return 0;
    }
    return 1;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    if (!path) return "";
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

static int str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_upper(*a) != ascii_upper(*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int is_shell_tot_path(const char *path) {
    return str_eq_nocase(path_basename(path), "SHELL.TOT");
}

static long try_spawn_candidate(const char *path) {
    long fd;
    if (!path || !*path) return -1;
    fd = fry_open(path, 0);
    if (fd < 0) return -1;
    fry_close((int)fd);
    return fry_spawn(path);
}

static long spawn_command_path(const char *cmd, char *resolved, size_t resolved_len) {
    static const char *app_dirs[] = { "/apps/", "/system/" };
    char path[128];
    size_t cmd_len;
    int has_slash;
    long pid;
    if (!cmd || !*cmd) return -1;
    has_slash = 0;
    for (cmd_len = 0; cmd[cmd_len]; cmd_len++) {
        if (cmd[cmd_len] == '/') has_slash = 1;
    }
    build_path(cmd, path, sizeof(path));
    if (!has_suffix_nocase(path, ".fry") && !has_suffix_nocase(path, ".tot")) {
        if (str_eq_nocase(cmd, "shell")) {
            if (strlen(path) + 4 < sizeof(path)) strcat(path, ".tot");
        } else {
            if (strlen(path) + 4 < sizeof(path)) strcat(path, ".fry");
        }
    }
    if (has_suffix_nocase(path, ".tot") && !is_shell_tot_path(path)) return -1;
    pid = try_spawn_candidate(path);
    if (pid >= 0) {
        if (resolved && resolved_len > 0) {
            strncpy(resolved, path, resolved_len - 1);
            resolved[resolved_len - 1] = 0;
        }
        return pid;
    }
    if (has_slash) return -1;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(app_dirs) / sizeof(app_dirs[0])); i++) {
        if (str_eq_nocase(cmd, "shell")) {
            snprintf(path, sizeof(path), "%sSHELL.TOT", app_dirs[i]);
        } else {
            snprintf(path, sizeof(path), "%s%s.FRY", app_dirs[i], cmd);
        }
        if (has_suffix_nocase(path, ".tot") && !is_shell_tot_path(path)) continue;
        pid = try_spawn_candidate(path);
        if (pid >= 0) {
            if (resolved && resolved_len > 0) {
                strncpy(resolved, path, resolved_len - 1);
                resolved[resolved_len - 1] = 0;
            }
            return pid;
        }
    }
    return -1;
}

#define ESPI_PCERR_STATUS_MASK  ((1u << 24) | (1u << 12) | (1u << 4))
#define ESPI_VWERR_STATUS_MASK  ((1u << 12) | (1u << 4))
#define ESPI_FCERR_STATUS_MASK  ((1u << 17) | (1u << 12) | (1u << 4))
#define ESPI_LNKERR_STATUS_MASK ((1u << 31) | (1u << 20))

static const char *ec_probe_step_name(uint8_t step) {
    switch (step) {
        case 1: return "STS_FF";
        case 2: return "IBF_PRE";
        case 3: return "IBF_POST";
        case 4: return "OBF_TMO";
        case 5: return "OK";
        default: return "NO_PROBE";
    }
}

static const char *ec_ports_source_name(uint8_t src) {
    switch (src) {
        case 1: return "ECDT";
        case 2: return "_CRS";
        default: return "default";
    }
}

static const char *ec_recovery_name(uint8_t r) {
    switch (r) {
        case 1: return "direct";
        case 2: return "state_reset";
        case 3: return "burst";
        case 4: return "sci_drain";
        case 5: return "force_enable";
        case 6: return "force+burst";
        case 7: return "espi_clear";
        default: return "none";
    }
}

static void cmd_acpibreak(void) {
    struct fry_acpi_diag d;
    if (fry_acpi_diag(&d) != 0) {
        term_printf("acpibreak: ACPI diag unavailable\n");
        return;
    }

    const uint16_t req_ioe = (uint16_t)((1u << 10) | (1u << 11)); /* 0x60/64 + 0x62/66 */
    uint32_t pcerr = d.espi_raw[2];
    uint32_t vwerr = d.espi_raw[3];
    uint32_t fcerr = d.espi_raw[4];
    uint32_t lnkerr = d.espi_raw[5];
    uint32_t pc_sts = pcerr & ESPI_PCERR_STATUS_MASK;
    uint32_t vw_sts = vwerr & ESPI_VWERR_STATUS_MASK;
    uint32_t fc_sts = fcerr & ESPI_FCERR_STATUS_MASK;
    uint32_t ln_sts = lnkerr & ESPI_LNKERR_STATUS_MASK;

    term_printf("acpibreak: ");
    if (d.ec_ok) {
        term_printf("EC responding (no break at EC probe)\n");
    } else if (!d.ec_node_found && d.ec_ports_source == 0) {
        term_printf("break at EC discovery (no PNP0C09/ECDT ports)\n");
    } else if ((d.lpc_ioe_after & req_ioe) != req_ioe) {
        term_printf("break at LPC decode (IOE missing 0x60/64 or 0x62/66)\n");
    } else if (d.espi_clear_found && d.espi_clear_ok && !d.ec_ok) {
        term_printf("break: eSPI errors cleared but EC still unresponsive\n");
    } else if (d.espi_probed && d.espi_en &&
               (pc_sts != 0 || vw_sts != 0 || fc_sts != 0 || ln_sts != 0)) {
        term_printf("break on eSPI link/channel errors before EC response\n");
    } else if (!d.ec_ibf_seen && d.ec_probe_step != 5) {
        term_printf("break at EC command acceptance (IBF never asserted)\n");
    } else if (d.ec_probe_step == 4) {
        term_printf("break at EC response wait (OBF timeout)\n");
    } else if (d.ec_probe_step == 1) {
        term_printf("break at status read (0xFF / floating decode path)\n");
    } else {
        term_printf("break stage uncertain (see detail lines)\n");
    }

    term_printf("  ec=%s step=%s sts=0x%02x tries=%u src=%s recov=%s\n",
                d.ec_ok ? "OK" : "FAIL",
                ec_probe_step_name(d.ec_probe_step),
                (unsigned)d.ec_probe_status,
                (unsigned)d.ec_probe_attempts,
                ec_ports_source_name(d.ec_ports_source),
                ec_recovery_name(d.ec_recovery_method));

    term_printf("  ioe=0x%04x->0x%04x pcr[0x%02x]=0x%08x->0x%08x mir=%u\n",
                (unsigned)d.lpc_ioe_before, (unsigned)d.lpc_ioe_after,
                (unsigned)d.pcr_pid, d.pcr_ioe_before, d.pcr_ioe_after,
                (unsigned)d.pcr_mirror_done);

    if (d.espi_probed) {
        term_printf("  espi en=%u pcrpid=0x%02x raw pc=%08x vw=%08x fc=%08x ln=%08x\n",
                    (unsigned)d.espi_en, (unsigned)d.espi_pid,
                    pcerr, vwerr, fcerr, lnkerr);
        term_printf("       status pc=%08x vw=%08x fc=%08x ln=%08x\n",
                    pc_sts, vw_sts, fc_sts, ln_sts);
    } else {
        term_printf("  espi not probed\n");
    }

    if (d.espi_clear_run) {
        term_printf("  espi_clr run=0x%02x found=%u ok=%u\n",
                    (unsigned)d.espi_clear_run,
                    (unsigned)d.espi_clear_found,
                    (unsigned)d.espi_clear_ok);
        term_printf("    pre:  pc=%08x vw=%08x fc=%08x ln=%08x\n",
                    d.espi_pre_clear[0], d.espi_pre_clear[1],
                    d.espi_pre_clear[2], d.espi_pre_clear[3]);
        term_printf("    post: pc=%08x vw=%08x fc=%08x ln=%08x\n",
                    d.espi_post_clear[0], d.espi_post_clear[1],
                    d.espi_post_clear[2], d.espi_post_clear[3]);
    }

    /* fry444: eSPI slave channel diagnostics */
    if (d.espi_probed && d.espi_slave_read_ok) {
        term_printf("  slave ch_sup=PC%u VW%u OOB%u FC%u rdok=0x%02x\n",
                    (unsigned)(d.espi_gen_chan_sup & 1u),
                    (unsigned)((d.espi_gen_chan_sup >> 1) & 1u),
                    (unsigned)((d.espi_gen_chan_sup >> 2) & 1u),
                    (unsigned)((d.espi_gen_chan_sup >> 3) & 1u),
                    (unsigned)d.espi_slave_read_ok);
        term_printf("    PC_CAP=%08x rdy=%u  VW_CAP=%08x rdy=%u\n",
                    d.espi_slave_pc_cap, (unsigned)d.espi_slave_pc_en,
                    d.espi_slave_vw_cap, (unsigned)d.espi_slave_vw_en);
    }

    /* G454: query/event + policy diagnostics */
    term_printf("  query disp=%u drop=%u storm=%u frz=%u cands=%u\n",
                d.ec_queries_dispatched, d.ec_queries_dropped,
                d.ec_storm_count, (unsigned)d.ec_events_frozen,
                (unsigned)d.ec_cand_count);
    term_printf("  policy tmo=%u ret=%u mfail=%u flg=0x%02x\n",
                d.ec_policy_timeout, (unsigned)d.ec_policy_retries,
                (unsigned)d.ec_policy_max_fail, (unsigned)d.ec_policy_flags);

    term_printf("  batt found=%u sta=0x%08x  bl found=%u sta=0x%08x\n",
                (unsigned)d.batt_count, d.batt_sta,
                (unsigned)d.bl_found, d.bl_sta);
}

/* ================================================================
 * Command execution
 * ================================================================ */

static void execute_command(void) {
    char *argv[16];
    int argc = tokenize(input_line, argv, 16);
    if (argc == 0) return;

    int background = 0;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argv[argc - 1] = 0;
        argc--;
        if (argc == 0) return;
    }

    if (strcmp(argv[0], "help") == 0) {
        term_printf("commands: ls cd cat echo mkdir rm touch write\n");
        term_printf("          clear reboot shutdown help pwd acpibreak exit quit\n");
        term_printf("          storage mounts mountdbg vfsdbg [path] nvmefix [path]\n");
        term_printf("scroll: Ctrl+U = page up, Ctrl+D = page down\n");
    } else if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        fry_exit(0);
    } else if (strcmp(argv[0], "clear") == 0) {
        term_init();
        sb_count = 0;
        sb_head = 0;
        scroll_offset = 0;
    } else if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            term_puts(argv[i]);
            if (i + 1 < argc) term_putc(' ');
        }
        term_putc('\n');
    } else if (strcmp(argv[0], "reboot") == 0) {
        fry_reboot();
    } else if (strcmp(argv[0], "shutdown") == 0) {
        fry_shutdown();
    } else if (strcmp(argv[0], "ls") == 0) {
        char path[128];
        if (argc > 1) build_path(argv[1], path, sizeof(path));
        else build_path(".", path, sizeof(path));
        char buf[1024];
        long n = fry_readdir(path, buf, sizeof(buf) - 1);
        if (n < 0) {
            term_printf("ls: failed\n");
        } else {
            buf[n] = 0;
            term_puts(buf);
        }
    } else if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2) {
            term_printf("cd: missing path\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            if (is_dir(path)) {
                strcpy(cwd, path);
            } else {
                term_printf("cd: not a dir\n");
            }
        }
    } else if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) {
            term_printf("cat: missing file\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            long fd = fry_open(path, 0);
            if (fd < 0) {
                term_printf("cat: open failed\n");
            } else {
                char buf[256];
                long n;
                while ((n = fry_read((int)fd, buf, sizeof(buf) - 1)) > 0) {
                    buf[n] = 0;
                    term_puts(buf);
                }
                fry_close((int)fd);
            }
        }
    } else if (strcmp(argv[0], "storage") == 0) {
        cmd_storage();
    } else if (strcmp(argv[0], "mounts") == 0) {
        cmd_mounts();
    } else if (strcmp(argv[0], "mountdbg") == 0) {
        print_mount_dbg();
    } else if (strcmp(argv[0], "vfsdbg") == 0) {
        print_storage_and_mounts();

        term_puts("paths:\n");
        static const char *paths[] = {
            "/",
            "/system",
            "/apps",
            "/nvme",
            "/fry",
            "/FRY",
            "/EFI",
            "/EFI/BOOT",
            "/system/INIT.FRY",
            "/system/GUI.FRY",
            "/apps/SHELL.TOT",
            "/GUI.FRY",
            "/SHELL.TOT",
            "/nvme/GUI.FRY",
            "/nvme/SHELL.TOT",
            "/EFI/BOOT/GUI.FRY",
            "/EFI/BOOT/SHELL.TOT"
        };
        for (uint32_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            vfs_print_stat(paths[i]);
        }

        if (argc > 1) {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            vfs_readdir_brief(path);
        }
    } else if (strcmp(argv[0], "nvmefix") == 0) {
        char path[128];
        if (argc > 1) build_path(argv[1], path, sizeof(path));
        else strcpy(path, "/nvme");

        term_printf("nvmefix target=%s\n", path);
        if (strcmp(path, "/nvme") != 0 && strcmp(path, "/nvme/") != 0) {
            term_puts("note: automatic /nvme recovery triggers on exact /nvme path.\n");
        }
        print_storage_and_mounts();
        nvmefix_probe(path, "probe 1");
        nvmefix_probe(path, "probe 2");
        print_storage_and_mounts();
    } else if (strcmp(argv[0], "pwd") == 0) {
        term_printf("%s\n", cwd);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) {
            term_printf("mkdir: missing path\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            if (fry_mkdir(path) != 0) {
                term_printf("mkdir: failed\n");
            }
        }
    } else if (strcmp(argv[0], "rm") == 0) {
        if (argc < 2) {
            term_printf("rm: missing path\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            if (fry_unlink(path) != 0) {
                term_printf("rm: failed\n");
            }
        }
    } else if (strcmp(argv[0], "touch") == 0) {
        if (argc < 2) {
            term_printf("touch: missing path\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            if (fry_create(path, 1) != 0) {
                term_printf("touch: failed\n");
            }
        }
    } else if (strcmp(argv[0], "write") == 0) {
        if (argc < 3) {
            term_printf("write: usage: write <path> <text...>\n");
        } else {
            char path[128];
            build_path(argv[1], path, sizeof(path));
            long fd = fry_open(path, O_CREAT);
            if (fd < 0) {
                term_printf("write: open failed\n");
            } else {
                for (int i = 2; i < argc; i++) {
                    fry_write((int)fd, argv[i], strlen(argv[i]));
                    if (i + 1 < argc) fry_write((int)fd, " ", 1);
                }
                fry_write((int)fd, "\n", 1);
                fry_close((int)fd);
            }
        }
    } else if (strcmp(argv[0], "acpibreak") == 0) {
        cmd_acpibreak();
    } else {
        /* Try spawning user program */
        char path[128];
        long pid = spawn_command_path(argv[0], path, sizeof(path));
        if (pid < 0) {
            term_printf("command not found: %s\n", argv[0]);
        } else {
            if (!background) {
                render_and_update();
                fry_wait((uint32_t)pid);
            }
        }
    }
}

/* ================================================================
 * Input handling (non-blocking, per-key)
 * ================================================================ */

static void handle_key(char c) {
    if (c == '\r') c = '\n';

    /* Ctrl+U (0x15) = page up (scroll back in history) */
    if (c == 0x15) {
        int page = term_rows / 2;
        if (page < 1) page = 1;
        int old_off = scroll_offset;
        scroll_offset += page;
        if (scroll_offset > sb_count) scroll_offset = sb_count;
        if (scroll_offset != old_off) needs_redraw = 1;
        return;
    }

    /* Ctrl+D (0x04) = page down (scroll toward current) */
    if (c == 0x04) {
        int page = term_rows / 2;
        if (page < 1) page = 1;
        int old_off = scroll_offset;
        scroll_offset -= page;
        if (scroll_offset < 0) scroll_offset = 0;
        if (scroll_offset != old_off) needs_redraw = 1;
        return;
    }

    if (c == '\n') {
        input_line[input_pos] = 0;
        term_putc('\n');
        execute_command();
        input_pos = 0;
        input_line[0] = 0;
        print_prompt();
        return;
    }

    if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            input_pos--;
            term_putc('\b');
        }
        return;
    }

    if ((unsigned char)c >= 0x20 && input_pos + 1 < (int)sizeof(input_line)) {
        input_line[input_pos++] = c;
        term_putc(c);
    }
}

/* ================================================================
 * Mixed input stream parser (keyboard + TW protocol)
 * ================================================================ */

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
    static const uint8_t magic_le[4] = {0x4E, 0x49, 0x57, 0x54}; /* "TWIN" LE */
    if (input_stream_len <= 0) return 0;

    /* Valid TW types are 1..7 in low byte, remaining type bytes are zero. */
    if (input_stream[0] < 1 || input_stream[0] > 7) return 0;
    if (input_stream_len >= 2 && input_stream[1] != 0) return 0;
    if (input_stream_len >= 3 && input_stream[2] != 0) return 0;
    if (input_stream_len >= 4 && input_stream[3] != 0) return 0;

    /* If we already have some magic bytes, they must match too. */
    for (int i = 4; i < input_stream_len && i < 8; i++) {
        if (input_stream[i] != magic_le[i - 4]) return 0;
    }

    return 1;
}

static void process_input_stream(void) {
    while (input_stream_len > 0) {
        if (input_stream_len >= (int)sizeof(tw_msg_header_t)) {
            tw_msg_header_t hdr;
            memcpy(&hdr, input_stream, sizeof(hdr));

            if (hdr.magic == TW_MAGIC) {
                int need = tw_msg_size(hdr.type);
                if (need < (int)sizeof(tw_msg_header_t))
                    need = (int)sizeof(tw_msg_header_t);
                if (input_stream_len < need) break; /* wait for full frame */

                if (hdr.type == TW_MSG_RESIZED && need == (int)sizeof(tw_msg_resized_t)) {
                    tw_msg_resized_t resp;
                    memcpy(&resp, input_stream, sizeof(resp));
                    handle_resize(resp.shm_id, resp.shm_ptr, resp.new_w, resp.new_h);
                } else if (hdr.type == TW_MSG_KEY_EVENT && need == (int)sizeof(tw_msg_key_t)) {
                    tw_msg_key_t kev;
                    memcpy(&kev, input_stream, sizeof(kev));
                    handle_key((char)(uint8_t)kev.key);
                }
                /* Consume any protocol frame from stdin; shell only acts on RESIZED. */
                input_consume(need);
                continue;
            }
        }

        /* Keep potential partial TW headers until enough bytes arrive. */
        if (input_stream_len < (int)sizeof(tw_msg_header_t) && maybe_tw_prefix()) {
            break;
        }

        /* Plain keyboard byte. */
        handle_key((char)input_stream[0]);
        input_consume(1);
    }
}

/* ================================================================
 * Main — TaterWin setup + cooperative loop
 * ================================================================ */

int main(void) {
    /* Compute initial terminal dimensions */
    term_cols = (win_w - SCROLLBAR_W) / CHAR_W;
    term_rows = win_h / CHAR_H;
    if (term_cols > MAX_TERM_COLS) term_cols = MAX_TERM_COLS;
    if (term_rows > MAX_TERM_ROWS) term_rows = MAX_TERM_ROWS;
    if (term_cols < 10) term_cols = 10;
    if (term_rows < 4)  term_rows = 4;

    /* Allocate screen buffer (MAX size so resize never needs realloc) */
    screen_buf = malloc((size_t)MAX_TERM_ROWS * SCREEN_STRIDE);
    if (!screen_buf) return 1;

    /* Allocate scrollback buffer */
    scrollback = malloc((size_t)SCROLLBACK_MAX * SCREEN_STRIDE);
    if (!scrollback) {
        free(screen_buf);
        return 1;
    }

    /* Allocate fallback pixel buffer */
    pixels = malloc((size_t)win_w * (size_t)win_h * 4);
    if (!pixels) {
        free(scrollback);
        free(screen_buf);
        return 1;
    }

    /* 1. Request TaterWin window */
    tw_msg_create_win_t req;
    req.hdr.type  = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = win_w;
    req.h = win_h;
    strncpy(req.title, "Shell", 31);
    fry_write(1, &req, sizeof(req));

    /* 2. Wait for WINDOW_CREATED response */
    tw_msg_win_created_t wresp;
    memset(&wresp, 0, sizeof(wresp));
    uint8_t winbuf[sizeof(wresp)];
    int winlen = 0;
    int got = 0, tries = 0;
    while (!got && tries < 300) {
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
                        cand.hdr.type  == TW_MSG_WINDOW_CREATED &&
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

    /* 3. Map SHM (or keep malloc'd fallback) */
    if (got) {
        long ptr = fry_shm_map(wresp.shm_id);
        if (ptr > 0) {
            pixels = (uint32_t *)(uintptr_t)ptr;
            g_shm_id = wresp.shm_id;
        }
    }

    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    gfx_init(&g_ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    g_upd.type  = TW_MSG_UPDATE;
    g_upd.magic = TW_MAGIC;

    /* 4. Initialize terminal and show banner + prompt */
    term_init();
    term_printf("TaterTOS64v3 shell\n");
    print_prompt();
    input_pos = 0;
    input_line[0] = 0;

    /* 5. Main loop — cooperative, non-blocking */
    for (;;) {
        /* Read input (keyboard + potential TW messages) */
        char kbuf[64];
        long kn = fry_read(0, kbuf, sizeof(kbuf));
        if (kn > 0) {
            input_append((const uint8_t *)kbuf, (int)kn);
            process_input_stream();
        }

        /* Render only when state changed; avoids visible crawl/flicker. */
        if (needs_redraw) {
            render_and_update();
            needs_redraw = 0;
        }

        /* Cooperative sleep ~30fps */
        fry_sleep(33);
    }

    return 0;
}
