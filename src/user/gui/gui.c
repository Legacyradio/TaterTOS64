/*
 * gui.c — TaterTOS Window Server / GUI Manager
 */

#include "libc.h"
#include "fry.h"

/* POSIX / System headers */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WINDOWS 64

struct window {
    int id;
    int pid;
    int x, y, w, h;
    uint32_t *fb;
    char title[64];
};

static struct window windows[MAX_WINDOWS];
static int num_windows = 0;

/* -----------------------------------------------------------------------
 * GUI Internal Logic
 * ----------------------------------------------------------------------- */

static void redraw_all(void) {
    /* simplified redraw */
    struct fry_fb_info info;
    if (fry_fb_info(&info) < 0) return;

    /* composite windows to backbuffer then flip */
}

/* -----------------------------------------------------------------------
 * Main loop
 * ----------------------------------------------------------------------- */

int main(void) {
    struct fry_fb_info fb;
    if (fry_fb_info(&fb) < 0) {
        puts("gui: failed to get framebuffer info\n");
        return 1;
    }

    printf("GUI started: %ux%u format %u\n", fb.width, fb.height, fb.format);

    while (1) {
        struct fry_key_event kevt;
        while (fry_kbd_event(&kevt) > 0) {
            /* handle keys */
        }

        struct fry_mouse_state mevt;
        if (fry_mouse_get(&mevt) >= 0) {
            /* handle mouse */
        }

        redraw_all();
        fry_sleep(16); /* ~60fps */
    }

    return 0;
}
