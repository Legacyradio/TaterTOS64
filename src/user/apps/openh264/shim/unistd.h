#ifndef _TATER_UNISTD_H
#define _TATER_UNISTD_H
#include <stddef.h>
typedef long ssize_t;
static inline unsigned int sleep(unsigned int sec) { (void)sec; return 0; }
static inline int usleep(unsigned int usec) { (void)usec; return 0; }
#endif
