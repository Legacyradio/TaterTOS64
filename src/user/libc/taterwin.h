#ifndef TATERWIN_H
#define TATERWIN_H

#include <stdint.h>

typedef enum {
    TW_MSG_CREATE_WINDOW = 1,
    TW_MSG_WINDOW_CREATED = 2,
    TW_MSG_MOUSE_EVENT = 3,
    TW_MSG_KEY_EVENT = 4,
    TW_MSG_UPDATE = 5,
    TW_MSG_RESIZE = 6,
    TW_MSG_RESIZED = 7,
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

typedef struct {
    tw_msg_header_t hdr;
    int x, y;
    uint8_t btns;
} tw_msg_mouse_t;

typedef struct {
    tw_msg_header_t hdr;
    uint32_t key;
    uint32_t flags;
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

#define TW_MAGIC 0x5457494E
#define TW_TASKBAR_H 36
#define TW_TITLE_H   28

#endif
