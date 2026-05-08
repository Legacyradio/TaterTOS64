#ifndef _TATER_SHIM_SYS_TIME_H
#define _TATER_SHIM_SYS_TIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval *tv, void *tz);
#ifdef __cplusplus
}
#endif
#endif
