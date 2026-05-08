#ifndef _TATER_SYS_STAT_H
#define _TATER_SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif
struct stat { int st_size; };
static inline int stat(const char *p, struct stat *s) { (void)p;(void)s; return -1; }
#ifdef __cplusplus
}
#endif
#endif
