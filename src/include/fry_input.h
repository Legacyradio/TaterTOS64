/*
 * fry_input.h — Shared input event definitions (kernel + userspace)
 *
 * Phase 7: Rich keyboard and mouse event structures for the GUI/input ABI.
 * These are shared between the kernel PS/2 drivers, the SYS_KBD_EVENT
 * syscall, and userspace clients (compositor, apps via TaterWin).
 */

#ifndef FRY_INPUT_H
#define FRY_INPUT_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Modifier flags — bitmask, stored in fry_key_event.mods and forwarded
 * through the TaterWin protocol.
 * ----------------------------------------------------------------------- */
#define FRY_MOD_LSHIFT   0x01u
#define FRY_MOD_RSHIFT   0x02u
#define FRY_MOD_SHIFT    (FRY_MOD_LSHIFT | FRY_MOD_RSHIFT)
#define FRY_MOD_LCTRL    0x04u
#define FRY_MOD_RCTRL    0x08u
#define FRY_MOD_CTRL     (FRY_MOD_LCTRL | FRY_MOD_RCTRL)
#define FRY_MOD_LALT     0x10u
#define FRY_MOD_RALT     0x20u
#define FRY_MOD_ALT      (FRY_MOD_LALT | FRY_MOD_RALT)
#define FRY_MOD_CAPSLOCK 0x40u
#define FRY_MOD_NUMLOCK  0x80u

/* -----------------------------------------------------------------------
 * Virtual key codes — for keys that do not produce printable ASCII.
 * Range 0x100+ to avoid collision with ASCII values (0x00-0xFF).
 * ----------------------------------------------------------------------- */
#define FRY_VK_ESCAPE    0x100u
#define FRY_VK_F1        0x101u
#define FRY_VK_F2        0x102u
#define FRY_VK_F3        0x103u
#define FRY_VK_F4        0x104u
#define FRY_VK_F5        0x105u
#define FRY_VK_F6        0x106u
#define FRY_VK_F7        0x107u
#define FRY_VK_F8        0x108u
#define FRY_VK_F9        0x109u
#define FRY_VK_F10       0x10Au
#define FRY_VK_F11       0x10Bu
#define FRY_VK_F12       0x10Cu

#define FRY_VK_UP        0x110u
#define FRY_VK_DOWN      0x111u
#define FRY_VK_LEFT      0x112u
#define FRY_VK_RIGHT     0x113u

#define FRY_VK_HOME      0x114u
#define FRY_VK_END       0x115u
#define FRY_VK_PGUP      0x116u
#define FRY_VK_PGDN      0x117u
#define FRY_VK_INSERT    0x118u
#define FRY_VK_DELETE     0x119u

#define FRY_VK_LSHIFT    0x120u
#define FRY_VK_RSHIFT    0x121u
#define FRY_VK_LCTRL     0x122u
#define FRY_VK_RCTRL     0x123u
#define FRY_VK_LALT      0x124u
#define FRY_VK_RALT      0x125u
#define FRY_VK_CAPSLOCK  0x126u
#define FRY_VK_NUMLOCK   0x127u
#define FRY_VK_SCROLLLOCK 0x128u

#define FRY_VK_PRINTSCR  0x130u
#define FRY_VK_PAUSE     0x131u

/* Numpad keys */
#define FRY_VK_KP0       0x140u
#define FRY_VK_KP1       0x141u
#define FRY_VK_KP2       0x142u
#define FRY_VK_KP3       0x143u
#define FRY_VK_KP4       0x144u
#define FRY_VK_KP5       0x145u
#define FRY_VK_KP6       0x146u
#define FRY_VK_KP7       0x147u
#define FRY_VK_KP8       0x148u
#define FRY_VK_KP9       0x149u
#define FRY_VK_KP_ENTER  0x14Au
#define FRY_VK_KP_DOT    0x14Bu
#define FRY_VK_KP_PLUS   0x14Cu
#define FRY_VK_KP_MINUS  0x14Du
#define FRY_VK_KP_STAR   0x14Eu
#define FRY_VK_KP_SLASH  0x14Fu

/* -----------------------------------------------------------------------
 * Key event flags (fry_key_event.flags)
 * ----------------------------------------------------------------------- */
#define FRY_KEY_PRESSED  0x00u   /* key down */
#define FRY_KEY_RELEASED 0x01u   /* key up */
#define FRY_KEY_REPEAT   0x02u   /* auto-repeat (future) */

/* -----------------------------------------------------------------------
 * Rich key event — returned by SYS_KBD_EVENT syscall and forwarded
 * through TaterWin TW_MSG_KEY_EVENT messages.
 * ----------------------------------------------------------------------- */
struct fry_key_event {
    uint16_t scancode;   /* raw PS/2 scancode (with 0xE0 prefix encoded as 0x80+) */
    uint16_t vk;         /* virtual key code (ASCII or FRY_VK_*) */
    uint8_t  mods;       /* modifier bitmask (FRY_MOD_*) */
    uint8_t  flags;      /* FRY_KEY_PRESSED / FRY_KEY_RELEASED */
    uint8_t  ascii;      /* translated ASCII char (0 if non-printable) */
    uint8_t  _pad;
};

/* -----------------------------------------------------------------------
 * Cursor shapes — app requests a cursor shape change via TaterWin.
 * ----------------------------------------------------------------------- */
#define FRY_CURSOR_ARROW    0u
#define FRY_CURSOR_IBEAM    1u
#define FRY_CURSOR_HAND     2u
#define FRY_CURSOR_RESIZE_H 3u
#define FRY_CURSOR_RESIZE_V 4u
#define FRY_CURSOR_WAIT     5u
#define FRY_CURSOR_CROSSHAIR 6u

/* -----------------------------------------------------------------------
 * Clipboard operations
 * ----------------------------------------------------------------------- */
#define FRY_CLIPBOARD_MAX  4096u  /* max clipboard text size */

#endif /* FRY_INPUT_H */
