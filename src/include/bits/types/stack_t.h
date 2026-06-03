#ifndef _TATERTOS_BITS_TYPES_STACK_T_H
#define _TATERTOS_BITS_TYPES_STACK_T_H

#include <stddef.h>

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

#endif
