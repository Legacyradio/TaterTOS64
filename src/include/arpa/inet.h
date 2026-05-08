/*
 * TaterTOS64v3 — <arpa/inet.h>
 *
 * POSIX network number<->string conversions.
 */

#ifndef _TATERTOS_ARPA_INET_H
#define _TATERTOS_ARPA_INET_H

#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

int         inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, uint32_t size);
in_addr_t   inet_addr(const char *cp);
char       *inet_ntoa(struct in_addr addr);

#ifdef __cplusplus
}
#endif

#endif
