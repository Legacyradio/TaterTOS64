/*
 * TaterTOS64v3 — <linux/random.h>
 *
 * Linux random ioctl constants stub for boringssl.
 */

#ifndef _TATERTOS_LINUX_RANDOM_H
#define _TATERTOS_LINUX_RANDOM_H

/* ioctl for /dev/urandom / /dev/random */
#define RNDGETENTCNT    0x80045200

/* getrandom flags */
#define GRND_NONBLOCK   0x0001
#define GRND_RANDOM     0x0002

#endif
