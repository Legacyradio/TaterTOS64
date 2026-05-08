/*
 * TaterTOS64v3 — <sys/resource.h>
 *
 * POSIX resource limits + usage. Backed by getrlimit_compat / etc.
 * in src/user/libc/posix.c. TaterTOS currently treats most rlimits
 * as effectively unlimited.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_SYS_RESOURCE_H
#define _TATERTOS_SYS_RESOURCE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long rlim_t;

#define RLIM_INFINITY ((rlim_t)~0UL)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

#define RLIMIT_CPU      0
#define RLIMIT_FSIZE    1
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_CORE     4
#define RLIMIT_RSS      5
#define RLIMIT_NPROC    6
#define RLIMIT_NOFILE   7
#define RLIMIT_MEMLOCK  8
#define RLIMIT_AS       9
#define RLIMIT_LOCKS   10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE     13
#define RLIMIT_RTPRIO   14
#define RLIMIT_RTTIME   15

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD   1

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define PRIO_PROCESS  0
#define PRIO_PGRP     1
#define PRIO_USER     2

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrusage(int who, struct rusage *usage);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_SYS_RESOURCE_H */
