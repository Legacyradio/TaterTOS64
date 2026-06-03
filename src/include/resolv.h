/*
 * TaterTOS64v3 — <resolv.h>
 *
 * Minimal POSIX resolver state for ports that read DNS configuration directly.
 * TaterTOS exposes configured DNS servers through the native netif ABI.
 */

#ifndef _TATERTOS_RESOLV_H
#define _TATERTOS_RESOLV_H

#include <netinet/in.h>

#define __RES 19991006

#define MAXNS 3
#define MAXDNSRCH 6

#define RES_INIT       0x00000001
#define RES_USEVC      0x00000008
#define RES_IGNTC      0x00000020
#define RES_RECURSE    0x00000040
#define RES_DEFNAMES   0x00000080
#define RES_DNSRCH     0x00000200
#define RES_ROTATE     0x00004000
#define RES_USE_DNSSEC 0x00800000

#ifndef _PATH_RESCONF
#define _PATH_RESCONF "/etc/resolv.conf"
#endif

struct __res_state {
    int retrans;
    int retry;
    unsigned long options;
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
    char *dnsrch[MAXDNSRCH + 1];
    int ndots;
};

typedef struct __res_state *res_state;

#ifdef __cplusplus
extern "C" {
#endif

int  res_ninit(res_state state);
void res_nclose(res_state state);

#ifdef __cplusplus
}
#endif

#endif /* _TATERTOS_RESOLV_H */
