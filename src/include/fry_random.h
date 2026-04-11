#ifndef TATER_FRY_RANDOM_H
#define TATER_FRY_RANDOM_H

/*
 * TaterTOS getrandom flags — shared between kernel and userspace.
 */
#define FRY_GRND_NONBLOCK   1   /* return -EAGAIN if entropy unavailable */
#define FRY_GRND_RANDOM     2   /* reserved (same pool on TaterTOS)     */
#define FRY_GRND_INSECURE   4   /* allow non-crypto fallback            */

/* Maximum bytes per getrandom call */
#define FRY_RANDOM_MAX      256

#endif
