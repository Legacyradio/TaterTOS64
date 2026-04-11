#ifndef TATERWIN_H
#define TATERWIN_H

#include <stdint.h>
#include <fry_input.h>

typedef enum {
    TW_MSG_CREATE_WINDOW = 1,
    TW_MSG_WINDOW_CREATED = 2,
    TW_MSG_MOUSE_EVENT = 3,
    TW_MSG_KEY_EVENT = 4,
    TW_MSG_UPDATE = 5,
    TW_MSG_RESIZE = 6,
    TW_MSG_RESIZED = 7,

    /* Phase 7 additions */
    TW_MSG_WHEEL_EVENT    = 8,   /* compositor -> app: scroll wheel */
    TW_MSG_FOCUS_EVENT    = 9,   /* compositor -> app: window gained/lost focus */
    TW_MSG_ENTER_LEAVE    = 10,  /* compositor -> app: cursor entered/left window */
    TW_MSG_CLOSE_REQUEST  = 11,  /* compositor -> app: close button pressed */
    TW_MSG_CLIPBOARD_COPY = 12,  /* app -> compositor: request clipboard content */
    TW_MSG_CLIPBOARD_DATA = 13,  /* compositor -> app: clipboard content response */
    TW_MSG_CLIPBOARD_SET  = 14,  /* app -> compositor: set clipboard content */
    TW_MSG_CURSOR_SHAPE   = 15,  /* app -> compositor: request cursor shape change */
    TW_MSG_DAMAGE_RECT    = 16,  /* app -> compositor: declare dirty region */
    TW_MSG_MOUSE_MOVE     = 17,  /* compositor -> app: mouse motion (no click) */
} tw_msg_type_t;

typedef struct {
    uint32_t type;
    uint32_t magic; // 0x5457494E "TWIN"
} tw_msg_header_t;

typedef struct {
    tw_msg_header_t hdr;
    int w, h;
    char title[32];
} tw_msg_create_win_t;

typedef struct {
    tw_msg_header_t hdr;
    int shm_id;
    uint64_t shm_ptr;
} tw_msg_win_created_t;

/* Mouse event — now includes wheel and modifier state */
typedef struct {
    tw_msg_header_t hdr;
    int x, y;
    uint8_t btns;
    uint8_t mods;      /* FRY_MOD_* bitmask (Phase 7) */
    uint8_t _pad[2];
} tw_msg_mouse_t;

/* Rich key event — scancode, virtual key, modifiers, ASCII, press/release */
typedef struct {
    tw_msg_header_t hdr;
    uint16_t scancode;  /* raw PS/2 scancode */
    uint16_t vk;        /* virtual key code (ASCII or FRY_VK_*) */
    uint8_t  mods;      /* modifier bitmask (FRY_MOD_*) */
    uint8_t  flags;     /* FRY_KEY_PRESSED / FRY_KEY_RELEASED */
    uint8_t  ascii;     /* translated ASCII char (0 if non-printable) */
    uint8_t  _pad;
} tw_msg_key_t;

typedef struct {
    tw_msg_header_t hdr;
    int new_w, new_h;
} tw_msg_resize_t;

typedef struct {
    tw_msg_header_t hdr;
    int shm_id;
    uint64_t shm_ptr;
    int new_w, new_h;
} tw_msg_resized_t;

/* Phase 7 message types */

/* Wheel event: scroll wheel delta in window-local coordinates */
typedef struct {
    tw_msg_header_t hdr;
    int x, y;           /* cursor position in window-local coords */
    int32_t delta;       /* positive = scroll up, negative = scroll down */
    uint8_t mods;        /* modifier bitmask */
    uint8_t _pad[3];
} tw_msg_wheel_t;

/* Focus event: window gained or lost focus */
typedef struct {
    tw_msg_header_t hdr;
    uint8_t focused;     /* 1 = gained, 0 = lost */
    uint8_t _pad[3];
} tw_msg_focus_t;

/* Enter/leave event: cursor entered or left window area */
typedef struct {
    tw_msg_header_t hdr;
    uint8_t entered;     /* 1 = entered, 0 = left */
    uint8_t _pad[3];
    int x, y;            /* cursor position at time of event */
} tw_msg_enter_leave_t;

/* Close request: compositor asks app to close (app can ignore or comply) */
typedef struct {
    tw_msg_header_t hdr;
} tw_msg_close_request_t;

/* Clipboard copy request: app asks for clipboard content */
typedef struct {
    tw_msg_header_t hdr;
} tw_msg_clipboard_copy_t;

/* Clipboard data response: compositor sends clipboard text to app */
typedef struct {
    tw_msg_header_t hdr;
    uint16_t len;
    uint8_t _pad[2];
    char data[256];      /* clipboard text (truncated to 256 for message size) */
} tw_msg_clipboard_data_t;

/* Clipboard set: app sets clipboard text */
typedef struct {
    tw_msg_header_t hdr;
    uint16_t len;
    uint8_t _pad[2];
    char data[256];
} tw_msg_clipboard_set_t;

/* Cursor shape request: app requests a cursor shape change */
typedef struct {
    tw_msg_header_t hdr;
    uint32_t shape;      /* FRY_CURSOR_* constant */
} tw_msg_cursor_shape_t;

/* Damage rect: app declares a dirty region for incremental repaint */
typedef struct {
    tw_msg_header_t hdr;
    int x, y, w, h;     /* dirty rectangle in window-local coordinates */
} tw_msg_damage_rect_t;

/* Mouse move: high-frequency motion event (no click required) */
typedef struct {
    tw_msg_header_t hdr;
    int x, y;            /* cursor position in window-local coords */
    uint8_t btns;
    uint8_t mods;
    uint8_t _pad[2];
} tw_msg_mouse_move_t;

#define TW_MAGIC 0x5457494E
#define TW_TASKBAR_H 36
#define TW_TITLE_H   28

#endif
