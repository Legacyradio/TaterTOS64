#ifndef _TATER_OH264_ERRNO_H
#define _TATER_OH264_ERRNO_H
/* Pull in TaterTOS errno defines */
#include "../../include/errno.h"
/* errno macro — thread-local via __errno_location */
extern int *__errno_location(void);
#define errno (*__errno_location())
#endif
