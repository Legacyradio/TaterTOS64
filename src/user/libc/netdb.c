/*
 * netdb.c — getaddrinfo/freeaddrinfo resolver for POSIX compatibility
 *
 * Phase 8: DNS resolver library needed by NSPR/NSS network code.
 * Uses fry_dns_resolve syscall for actual DNS lookups.
 *
 * All implementations are original TaterTOS code.
 */

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Public POSIX headers */
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Private TaterTOS ABI */
#include "libc.h"
#include "fry.h"

/* -----------------------------------------------------------------------
 * Helper: parse dotted-decimal IP
 * ----------------------------------------------------------------------- */

static int parse_ipv4(const char *str, uint32_t *out) {
    if (!str || !out) return -1;
    uint32_t parts[4];
    int count = 0;
    const char *s = str;

    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            if (*s != '.') return -1;
            s++;
        }
        if (*s < '0' || *s > '9') return -1;
        unsigned long val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return -1;
            s++;
        }
        parts[count++] = (uint32_t)val;
    }

    if (*s != '\0' || count != 4) return -1;

    /* Network byte order (big-endian) */
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    *out = htonl(*out);
    return 0;
}

/* -----------------------------------------------------------------------
 * Helper: parse port string
 * ----------------------------------------------------------------------- */

static int parse_port(const char *service) {
    if (!service) return 0;
    int port = 0;
    const char *s = service;
    while (*s >= '0' && *s <= '9') {
        port = port * 10 + (*s - '0');
        s++;
    }
    if (port > 65535) return -1;

    if (port == 0 && service[0] != '0') {
        if (strcmp(service, "http") == 0) return 80;
        if (strcmp(service, "https") == 0) return 443;
        if (strcmp(service, "ftp") == 0) return 21;
        if (strcmp(service, "ssh") == 0) return 22;
        if (strcmp(service, "telnet") == 0) return 23;
        if (strcmp(service, "smtp") == 0) return 25;
        if (strcmp(service, "dns") == 0) return 53;
        if (strcmp(service, "pop3") == 0) return 110;
        if (strcmp(service, "imap") == 0) return 143;
        return -1;
    }

    return port;
}

/* -----------------------------------------------------------------------
 * getaddrinfo — main resolver entry point
 * ----------------------------------------------------------------------- */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = 0;

    if (!node && !service) return EAI_NONAME;

    int socktype = 0;
    int protocol = 0;
    int flags = 0;

    if (hints) {
        if (hints->ai_family != 0 && hints->ai_family != AF_INET)
            return EAI_FAMILY;
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
        flags = hints->ai_flags;
    }

    int port = parse_port(service);
    if (port < 0) return EAI_SERVICE;

    uint32_t ip_net = 0;
    if (node) {
        if (parse_ipv4(node, &ip_net) < 0) {
            if (flags & AI_NUMERICHOST) return EAI_NONAME;

            long rc = fry_dns_resolve(node, &ip_net);
            if (rc < 0) {
                if (rc == -ETIMEDOUT) return EAI_AGAIN;
                return EAI_NONAME;
            }
        }
    } else {
        ip_net = (flags & AI_PASSIVE) ? 0 : htonl(0x7F000001);
    }

    int types[3], protos[3], n_results = 0;
    if (socktype != 0) {
        types[0] = socktype;
        protos[0] = protocol;
        n_results = 1;
    } else {
        types[0] = SOCK_STREAM; protos[0] = IPPROTO_TCP;
        types[1] = SOCK_DGRAM;  protos[1] = IPPROTO_UDP;
        types[2] = SOCK_RAW;    protos[2] = 0;
        n_results = 3;
    }

    struct addrinfo *head = 0, *tail = 0;
    for (int i = 0; i < n_results; i++) {
        struct addrinfo *ai = (struct addrinfo *)malloc(
            sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        if (!ai) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }

        struct sockaddr_in *sa = (struct sockaddr_in *)(ai + 1);
        memset(ai, 0, sizeof(*ai));
        memset(sa, 0, sizeof(*sa));

        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t)port);
        sa->sin_addr.s_addr = ip_net;

        ai->ai_family = AF_INET;
        ai->ai_socktype = types[i];
        ai->ai_protocol = protos[i];
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)sa;
        ai->ai_canonname = 0;
        ai->ai_next = 0;

        if (tail) tail->ai_next = ai;
        else head = ai;
        tail = ai;
    }

    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
    case 0:            return "Success";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_BADFLAGS: return "Invalid value for ai_flags";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "ai_family not supported";
    case EAI_MEMORY:   return "Memory allocation failure";
    case EAI_NONAME:   return "Name or service not known";
    case EAI_SERVICE:  return "Servname not supported for ai_socktype";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SYSTEM:   return "System error";
    default:           return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)flags;
    if (!sa || salen < sizeof(struct sockaddr_in)) return EAI_FAIL;

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (host && hostlen > 0) {
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        snprintf(host, hostlen, "%u.%u.%u.%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF, ip & 0xFF);
    }

    if (serv && servlen > 0) {
        snprintf(serv, servlen, "%u", ntohs(sin->sin_port));
    }

    return 0;
}
