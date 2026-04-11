// PS/2 keyboard driver — Phase 7: Rich key events
//
// Tracks all modifiers (shift, ctrl, alt, caps lock, num lock).
// Generates rich key events with scancode, virtual key, modifiers, ASCII,
// and press/release state.  Maintains both:
//   1. Legacy ASCII ring buffer for fry_read(0) backward compatibility
//   2. Rich event ring buffer for SYS_KBD_EVENT syscall

#include <stdint.h>
#include <fry_input.h>
#include "../../kernel/irq/manage.h"
#include "../../kernel/irq/chip.h"

void kprint(const char *fmt, ...);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* -----------------------------------------------------------------------
 * Scancode-to-ASCII tables (scan set 1, non-extended)
 * ----------------------------------------------------------------------- */
static char scancode_map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 (sc 0x3B-0x44) */
    0, /* Num Lock (0x45) */
    0, /* Scroll Lock (0x46) */
    0,0,0,0,0,0,0,0,0, /* KP 7,8,9,-,4,5,6,+,1 (0x47-0x4F) */
    0,0, /* KP 2,3 (0x50-0x51) */
    0,   /* KP 0 (0x52) */
    0    /* KP . (0x53) */
};

static char scancode_map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* -----------------------------------------------------------------------
 * Scancode-to-virtual-key mapping (non-extended, scan set 1)
 * 0 = use ASCII mapping, nonzero = FRY_VK_* code
 * ----------------------------------------------------------------------- */
static uint16_t scancode_to_vk[128] = {
    [0x01] = FRY_VK_ESCAPE,
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x1C] = '\n',
    [0x1D] = FRY_VK_LCTRL,
    [0x2A] = FRY_VK_LSHIFT,
    [0x36] = FRY_VK_RSHIFT,
    [0x38] = FRY_VK_LALT,
    [0x3A] = FRY_VK_CAPSLOCK,
    [0x3B] = FRY_VK_F1,
    [0x3C] = FRY_VK_F2,
    [0x3D] = FRY_VK_F3,
    [0x3E] = FRY_VK_F4,
    [0x3F] = FRY_VK_F5,
    [0x40] = FRY_VK_F6,
    [0x41] = FRY_VK_F7,
    [0x42] = FRY_VK_F8,
    [0x43] = FRY_VK_F9,
    [0x44] = FRY_VK_F10,
    [0x45] = FRY_VK_NUMLOCK,
    [0x46] = FRY_VK_SCROLLLOCK,
    [0x47] = FRY_VK_KP7,
    [0x48] = FRY_VK_KP8,
    [0x49] = FRY_VK_KP9,
    [0x4A] = FRY_VK_KP_MINUS,
    [0x4B] = FRY_VK_KP4,
    [0x4C] = FRY_VK_KP5,
    [0x4D] = FRY_VK_KP6,
    [0x4E] = FRY_VK_KP_PLUS,
    [0x4F] = FRY_VK_KP1,
    [0x50] = FRY_VK_KP2,
    [0x51] = FRY_VK_KP3,
    [0x52] = FRY_VK_KP0,
    [0x53] = FRY_VK_KP_DOT,
    [0x57] = FRY_VK_F11,
    [0x58] = FRY_VK_F12,
};

/* Extended key (0xE0 prefix) to virtual key mapping */
static uint16_t extended_to_vk(uint8_t sc) {
    switch (sc) {
        case 0x1C: return FRY_VK_KP_ENTER;
        case 0x1D: return FRY_VK_RCTRL;
        case 0x35: return FRY_VK_KP_SLASH;
        case 0x38: return FRY_VK_RALT;
        case 0x47: return FRY_VK_HOME;
        case 0x48: return FRY_VK_UP;
        case 0x49: return FRY_VK_PGUP;
        case 0x4B: return FRY_VK_LEFT;
        case 0x4D: return FRY_VK_RIGHT;
        case 0x4F: return FRY_VK_END;
        case 0x50: return FRY_VK_DOWN;
        case 0x51: return FRY_VK_PGDN;
        case 0x52: return FRY_VK_INSERT;
        case 0x53: return FRY_VK_DELETE;
        default:   return 0;
    }
}

/* -----------------------------------------------------------------------
 * Legacy ASCII ring buffer (for fry_read(0) / stdin backward compat)
 * ----------------------------------------------------------------------- */
static char keybuf[256];
static uint32_t key_head;
static uint32_t key_tail;
static uint32_t key_seen;

static void keybuf_put(char c) {
    uint32_t next = (key_head + 1) & 0xFF;
    if (next == key_tail) return;
    keybuf[key_head] = c;
    key_head = next;
}

/* -----------------------------------------------------------------------
 * Rich key event ring buffer (for SYS_KBD_EVENT)
 * ----------------------------------------------------------------------- */
#define KBD_EVT_RING_SIZE 64

static struct fry_key_event evt_ring[KBD_EVT_RING_SIZE];
static uint32_t evt_head;
static uint32_t evt_tail;

static void evt_put(const struct fry_key_event *e) {
    uint32_t next = (evt_head + 1) % KBD_EVT_RING_SIZE;
    if (next == evt_tail) return;  /* ring full, drop oldest */
    evt_ring[evt_head] = *e;
    evt_head = next;
}

/* -----------------------------------------------------------------------
 * Modifier state tracking
 * ----------------------------------------------------------------------- */
static uint8_t g_mods;       /* current modifier bitmask */
static uint8_t g_extended;   /* saw 0xE0 prefix */

/* Apply caps lock to ASCII */
static char apply_capslock(char c, uint8_t mods) {
    if (!(mods & FRY_MOD_CAPSLOCK)) return c;
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

/* -----------------------------------------------------------------------
 * Core scancode processing — called from IRQ and fallback poll
 * ----------------------------------------------------------------------- */
static void process_scancode(uint8_t sc) {
    if (sc == 0xE0) {
        g_extended = 1;
        return;
    }

    uint8_t release = (sc & 0x80) ? 1 : 0;
    uint8_t base_sc = sc & 0x7F;
    uint8_t is_ext = g_extended;
    g_extended = 0;

    /* Build the event */
    struct fry_key_event evt;
    evt.scancode = is_ext ? (uint16_t)(0xE000u | base_sc) : (uint16_t)base_sc;
    evt.flags = release ? FRY_KEY_RELEASED : FRY_KEY_PRESSED;
    evt.ascii = 0;
    evt._pad = 0;

    /* Determine virtual key */
    uint16_t vk = 0;
    if (is_ext) {
        vk = extended_to_vk(base_sc);
    } else {
        vk = scancode_to_vk[base_sc];
    }
    /* If no VK mapping, use ASCII value as VK */
    if (vk == 0 && !is_ext && base_sc < 128) {
        char c = scancode_map[base_sc];
        if (c) vk = (uint16_t)(uint8_t)c;
    }
    evt.vk = vk;

    /* Update modifier state */
    if (!release) {
        /* Key press */
        switch (vk) {
            case FRY_VK_LSHIFT:  g_mods |= FRY_MOD_LSHIFT; break;
            case FRY_VK_RSHIFT:  g_mods |= FRY_MOD_RSHIFT; break;
            case FRY_VK_LCTRL:   g_mods |= FRY_MOD_LCTRL;  break;
            case FRY_VK_RCTRL:   g_mods |= FRY_MOD_RCTRL;  break;
            case FRY_VK_LALT:    g_mods |= FRY_MOD_LALT;    break;
            case FRY_VK_RALT:    g_mods |= FRY_MOD_RALT;    break;
            case FRY_VK_CAPSLOCK:
                g_mods ^= FRY_MOD_CAPSLOCK;
                break;
            case FRY_VK_NUMLOCK:
                g_mods ^= FRY_MOD_NUMLOCK;
                break;
        }
    } else {
        /* Key release */
        switch (vk) {
            case FRY_VK_LSHIFT:  g_mods &= ~FRY_MOD_LSHIFT; break;
            case FRY_VK_RSHIFT:  g_mods &= ~FRY_MOD_RSHIFT; break;
            case FRY_VK_LCTRL:   g_mods &= ~FRY_MOD_LCTRL;  break;
            case FRY_VK_RCTRL:   g_mods &= ~FRY_MOD_RCTRL;  break;
            case FRY_VK_LALT:    g_mods &= ~FRY_MOD_LALT;    break;
            case FRY_VK_RALT:    g_mods &= ~FRY_MOD_RALT;    break;
        }
    }

    evt.mods = g_mods;

    /* Generate ASCII for key press (not release) */
    if (!release && !is_ext && base_sc < 128) {
        char c;
        if (g_mods & FRY_MOD_SHIFT)
            c = scancode_map_shift[base_sc];
        else
            c = scancode_map[base_sc];
        c = apply_capslock(c, g_mods);
        evt.ascii = (uint8_t)c;

        /* Legacy ASCII buffer — only on press */
        if (c) {
            keybuf_put(c);
        }
    }
    /* Extended keys that produce ASCII equivalents for legacy path */
    if (!release && is_ext) {
        switch (vk) {
            case FRY_VK_KP_ENTER: evt.ascii = '\n'; keybuf_put('\n'); break;
            case FRY_VK_DELETE:   evt.ascii = 127;  keybuf_put(127);  break;
        }
    }

    /* Push rich event */
    evt_put(&evt);

    if (key_seen == 0 && !release) {
        key_seen = 1;
        kprint("PS2: key input detected\n");
    }
}

/* -----------------------------------------------------------------------
 * Public API — legacy ASCII read (backward compatible)
 * ----------------------------------------------------------------------- */
int ps2_kbd_read(char *buf, uint32_t len) {
    uint32_t n = 0;
    while (n < len && key_tail != key_head) {
        buf[n++] = keybuf[key_tail];
        key_tail = (key_tail + 1) & 0xFF;
    }
    if (n == 0 && len > 0) {
        /* Fallback poll if IRQs are not delivering key events.
         * Check AUXB bit 5 to avoid stealing mouse bytes. */
        for (int i = 0; i < 8 && n == 0; i++) {
            uint8_t st = inb(0x64);
            if (!(st & 0x01)) continue;
            if (st & 0x20) continue;      /* AUXB=1: mouse byte */
            uint8_t sc = inb(0x60);
            process_scancode(sc);
            /* Now try reading from the buffer */
            if (key_tail != key_head) {
                buf[n++] = keybuf[key_tail];
                key_tail = (key_tail + 1) & 0xFF;
            }
        }
    }
    return (int)n;
}

/* -----------------------------------------------------------------------
 * Public API — rich key event read (for SYS_KBD_EVENT)
 * Returns: 1 if event was copied, 0 if ring empty
 * ----------------------------------------------------------------------- */
int ps2_kbd_read_event(struct fry_key_event *out) {
    if (evt_tail == evt_head) return 0;
    *out = evt_ring[evt_tail];
    evt_tail = (evt_tail + 1) % KBD_EVT_RING_SIZE;
    return 1;
}

/* -----------------------------------------------------------------------
 * Public API — get current modifier state
 * ----------------------------------------------------------------------- */
uint8_t ps2_kbd_get_mods(void) {
    return g_mods;
}

/* -----------------------------------------------------------------------
 * IRQ handler
 * ----------------------------------------------------------------------- */
static void ps2_kbd_irq(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)ctx; (void)dev_id; (void)error; (void)vector;
    uint8_t sc = inb(0x60);
    process_scancode(sc);
}

/* -----------------------------------------------------------------------
 * Init
 * ----------------------------------------------------------------------- */
void ps2_kbd_init(void) {
    key_head = key_tail = 0;
    evt_head = evt_tail = 0;
    key_seen = 0;
    g_mods = 0;
    g_extended = 0;
    /* IRQ1 is vector 33 */
    if (request_irq(33, ps2_kbd_irq, 0, "ps2_kbd", 0) != 0) {
        kprint("PS2: irq register failed\n");
    }
}
