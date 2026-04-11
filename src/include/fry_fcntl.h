#ifndef _TATERTOS_FCNTL_H
#define _TATERTOS_FCNTL_H

/*
 * TaterTOS64v3 — File control flags
 *
 * Flags for fry_open() and future descriptor operations.
 * Values match Linux where possible.
 */

#define O_RDONLY    0x00
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_CREAT     0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800

/* fcntl commands */
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4

#endif /* _TATERTOS_FCNTL_H */
