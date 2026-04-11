#ifndef _TATER_SYS_STAT_H
#define _TATER_SYS_STAT_H
struct stat { int st_size; };
static inline int stat(const char *p, struct stat *s) { (void)p;(void)s; return -1; }
#endif
