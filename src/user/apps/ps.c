// ps.fry - TaterWin process monitor panel

#include "../libc/libc.h"
#include "../libc/gfx.h"
#include "../libc/taterwin.h"

#define INIT_W 360
#define INIT_H 180

static uint32_t fallback_pixels[INIT_W * INIT_H];
static int win_w = INIT_W;
static int win_h = INIT_H;

/* Double-buffer: render to local buf, then memcpy to SHM atomically */
static uint32_t *render_buf = NULL;

static int tw_wait_window(tw_msg_win_created_t *out) {
    if (!out) return -1;
    uint8_t winbuf[sizeof(*out)];
    int winlen = 0;
    int tries = 0;
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

static void check_resize(gfx_ctx_t *ctx, uint32_t **pixels_ptr) {
    uint8_t in[64];
    for (;;) {
        long n = fry_read(0, in, sizeof(in));
        if (n <= 0) break;
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

static void render(gfx_ctx_t *ctx) {
    uint32_t pid = (uint32_t)fry_getpid();
    uint32_t count = (uint32_t)fry_proc_count();
    uint64_t ms = (uint64_t)fry_gettime();

    char buf[96];
    gfx_fill(ctx, 0, 0, (uint32_t)win_w, (uint32_t)win_h, 0x0E1014);
    gfx_draw_text(ctx, 12, 10, "Process Monitor", 0x16C77A, 0xFF000000);

    snprintf(buf, sizeof(buf), "Current PID: %u", pid);
    gfx_draw_text(ctx, 12, 42, buf, 0xFFFFFF, 0xFF000000);

    snprintf(buf, sizeof(buf), "Active Process Count: %u", count);
    gfx_draw_text(ctx, 12, 66, buf, 0xFFFFFF, 0xFF000000);

    snprintf(buf, sizeof(buf), "Kernel time (ms): %llu", (unsigned long long)ms);
    gfx_draw_text(ctx, 12, 90, buf, 0xA7B4C6, 0xFF000000);

    gfx_draw_text(ctx, 12, 126, "Detailed process list: pending implementation", 0xA0A7B4, 0xFF000000);
}

int main(void) {
    tw_msg_create_win_t req;
    req.hdr.type = TW_MSG_CREATE_WINDOW;
    req.hdr.magic = TW_MAGIC;
    req.w = INIT_W;
    req.h = INIT_H;
    strncpy(req.title, "Process Monitor", 31);
    fry_write(1, &req, sizeof(req));

    tw_msg_win_created_t resp;
    memset(&resp, 0, sizeof(resp));
    uint32_t *pixels = fallback_pixels;
    if (tw_wait_window(&resp) == 0) {
        long ptr = fry_shm_map(resp.shm_id);
        if (ptr > 0) pixels = (uint32_t *)(uintptr_t)ptr;
    }

    gfx_ctx_t ctx;
    render_buf = malloc((size_t)win_w * (size_t)win_h * 4);
    gfx_init(&ctx, render_buf, (uint32_t)win_w, (uint32_t)win_h, (uint32_t)win_w);
    tw_msg_header_t upd;
    upd.type = TW_MSG_UPDATE;
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
