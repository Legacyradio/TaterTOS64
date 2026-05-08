#ifndef _TATER_SYS_SYSCTL_H
#define _TATER_SYS_SYSCTL_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
static inline int sysctl(const int *n, unsigned int nl, void *o, size_t *ol, const void *nv, size_t nvl)
{ (void)n;(void)nl;(void)o;(void)ol;(void)nv;(void)nvl; return -1; }
static inline int sysctlbyname(const char *n, void *o, size_t *ol, const void *nv, size_t nvl)
{ (void)n;(void)o;(void)ol;(void)nv;(void)nvl; return -1; }
#ifdef __cplusplus
}
#endif
#endif
