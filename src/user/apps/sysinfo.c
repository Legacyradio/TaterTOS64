// sysinfo.fry — TaterWin-aware system info panel

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

#define INIT_W 400
#define INIT_H 260

/* Fallback pixel buffer used only if SHM allocation fails */
static uint32_t fallback_pixels[INIT_W * INIT_H];

/* Dynamic size — updated on resize */
static int win_w = INIT_W;
static int win_h = INIT_H;

/* Double-buffer: render to local buf, then memcpy to SHM atomically */
static uint32_t *render_buf = NULL;

static void render(gfx_ctx_t *ctx) {
    gfx_fill(ctx, 0, 0, (uint32_t)win_w, (uint32_t)win_h, 0x101010);

    uint64_t ms    = (uint64_t)fry_gettime();
    uint64_t total = ms / 1000ULL;
    uint32_t hours = (uint32_t)((total / 3600ULL) % 24ULL);
    uint32_t mins  = (uint32_t)((total / 60ULL) % 60ULL);
    uint32_t secs  = (uint32_t)(total % 60ULL);
    uint32_t procs = (uint32_t)fry_proc_count();

    char buf[112];
    gfx_draw_text(ctx, 10, 10, "TaterTOS64v3 System Info", 0x0078D4, 0xFF000000);

    snprintf(buf, sizeof(buf), "Uptime:    %02u:%02u:%02u", hours, mins, secs);
    gfx_draw_text(ctx, 10, 36, buf, 0xFFFFFF, 0xFF000000);

    snprintf(buf, sizeof(buf), "Processes: %u", procs);
    gfx_draw_text(ctx, 10, 60, buf, 0xFFFFFF, 0xFF000000);

    uint32_t bright = (uint32_t)fry_getbrightness();
    snprintf(buf, sizeof(buf), "Backlight: %u%%", bright);
    gfx_draw_text(ctx, 10, 84, buf, 0xFFFFFF, 0xFF000000);

    struct fry_battery_status bat;
    memset(&bat, 0, sizeof(bat));
    if (fry_getbattery(&bat) == 0) {
        snprintf(buf, sizeof(buf), "Battery:   %u mV  rate=%u",
                 bat.present_voltage, bat.present_rate);
        gfx_draw_text(ctx, 10, 108, buf, 0x80FF80, 0xFF000000);
        snprintf(buf, sizeof(buf), "           remain=%u  state=%u",
                 bat.remaining_capacity, bat.state);
        gfx_draw_text(ctx, 10, 124, buf, 0x80FF80, 0xFF000000);
    } else {
        gfx_draw_text(ctx, 10, 108, "Battery:   unavailable", 0x888888, 0xFF000000);
    }

    /* Storage Info */
    struct fry_storage_info sto;
    int sto_y = 140;
    memset(&sto, 0, sizeof(sto));

    if (fry_storage_info(&sto) == 0) {
        if (sto.nvme_detected) {
            uint64_t total_bytes = sto.total_sectors * (uint64_t)sto.sector_size;
            uint32_t gb = (uint32_t)(total_bytes / (1024ULL * 1024ULL * 1024ULL));
            snprintf(buf, sizeof(buf), "NVMe:      detected  %uB sectors  %u GB",
                     sto.sector_size, gb);
            gfx_draw_text(ctx, 10, sto_y, buf, 0x80FF80, 0xFF000000);
        } else {
            gfx_draw_text(ctx, 10, sto_y, "NVMe:      not detected", 0x888888, 0xFF000000);
        }

        const char *fs_names[] = {"none", "FAT32", "ToTFS", "NTFS", "ramdisk"};

        snprintf(buf, sizeof(buf), "Root FS:   %s at %s",
                 sto.root_fs_type <= 4 ? fs_names[sto.root_fs_type] : "?",
                 sto.root_mount[0] ? sto.root_mount : "?");
        gfx_draw_text(ctx, 10, sto_y + 20, buf, 0xFFFFFF, 0xFF000000);

        if (sto.secondary_fs_type > 0) {
            snprintf(buf, sizeof(buf), "Secondary: %s at %s",
                     sto.secondary_fs_type <= 4 ? fs_names[sto.secondary_fs_type] : "?",
                     sto.secondary_mount[0] ? sto.secondary_mount : "?");
            gfx_draw_text(ctx, 10, sto_y + 40, buf, 0xFFFFFF, 0xFF000000);
        } else {
            gfx_draw_text(ctx, 10, sto_y + 40, "Secondary: none", 0x888888, 0xFF000000);
        }
    } else {
        gfx_draw_text(ctx, 10, sto_y, "Storage:   info unavailable", 0x888888, 0xFF000000);
    }
}

/* Check inbuf for TW_MSG_RESIZED and update SHM/ctx accordingly. */
static void check_resize(gfx_ctx_t *ctx, uint32_t **pixels_ptr) {
    uint8_t in[64];
    for (;;) {
        long n = fry_read(0, in, sizeof(in));
        if (n <= 0) break;
        /* Scan for RESIZED message (sizeof(tw_msg_resized_t) = 24 bytes) */
        for (long i = 0; i <= n - (long)sizeof(tw_msg_resized_t); i++) {
            tw_msg_resized_t *rm = (tw_msg_resized_t *)(void *)&in[i];
            if (rm->hdr.magic == TW_MAGIC && rm->hdr.type == TW_MSG_RESIZED &&
                rm->shm_id >= 0 && rm->new_w > 0 && rm->new_h > 0) {
                long ptr = fry_shm_map(rm->shm_id);
                if (ptr > 0) {
                    *pixels_ptr = (uint32_t *)(uintptr_t)ptr;
                    win_w = rm->new_w;
                    win_h = rm->new_h;
                    free(render_buf);
                    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
                    gfx_init(ctx, render_buf,
                             (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);
                }
                return;
            }
        }
    }
}

int main(void) {
    /* 1. Request a TaterWin window via stdout */
    tw_msg_create_win_t req;
    req.hdr.type  = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = INIT_W;
    req.h = INIT_H;
    strncpy(req.title, "System Info", 31);
    fry_write(1, &req, sizeof(req));

    /* 2. Wait for TW_MSG_WINDOW_CREATED from the GUI via stdin. */
    tw_msg_win_created_t resp;
    memset(&resp, 0, sizeof(resp));
    uint8_t winbuf[sizeof(resp)];
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
                        resp = cand;
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

    /* 3. Map SHM into this process's address space (or fall back) */
    uint32_t *pixels = fallback_pixels;
    if (got) {
        long ptr = fry_shm_map(resp.shm_id);
        if (ptr > 0)
            pixels = (uint32_t *)(uintptr_t)ptr;
    }

    gfx_ctx_t ctx;
    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    gfx_init(&ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);

    /* 4. Render loop — redraw every second, signal update via stdout */
    tw_msg_header_t upd;
    upd.type  = TW_MSG_UPDATE;
    upd.magic = TW_MAGIC;

    for (;;) {
        check_resize(&ctx, &pixels);
        render(&ctx);
        memcpy(pixels, render_buf, (size_t)win_w * (size_t)win_h * 4);
        fry_write(1, &upd, sizeof(upd));
        fry_sleep(1000);
    }
    return 0;
}
