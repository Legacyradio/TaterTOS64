/*
 * signal.h shim — stub for mbedTLS
 * mbedTLS doesn't actually use signals, but some code paths reference the header.
 */
#ifndef _TATER_SHIM_SIGNAL_H
#define _TATER_SHIM_SIGNAL_H

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGINT  2
#define SIGTERM 15

sighandler_t signal(int signum, sighandler_t handler);

#endif
