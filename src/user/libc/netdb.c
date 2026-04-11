/*
 * netdb.c — getaddrinfo/freeaddrinfo resolver for POSIX compatibility
 *
 * Phase 8: DNS resolver library needed by NSPR/NSS network code.
 * Uses fry_dns_resolve syscall for actual DNS lookups.
 *
 * All implementations are original TaterTOS code.
 */

#include "libc.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * addrinfo structure and constants
 * ----------------------------------------------------------------------- */

/* Error codes, AI flags, struct addrinfo all come from libc.h */

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
    *out = fry_htonl(*out);
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

    /* Check for well-known service names */
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

    /* Determine socket type and protocol from hints */
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

    /* Parse port */
    int port = parse_port(service);
    if (port < 0) return EAI_SERVICE;

    /* Resolve address */
    uint32_t addr = 0;

    if (!node || (flags & AI_PASSIVE)) {
        /* Wildcard/passive: bind to INADDR_ANY */
        addr = 0;
    } else {
        /* Try numeric first */
        if (parse_ipv4(node, &addr) != 0) {
            if (flags & AI_NUMERICHOST) return EAI_NONAME;

            /* DNS lookup via kernel */
            uint32_t ip_net = 0;
            long rc = fry_dns_resolve(node, &ip_net);
            if (rc < 0) return EAI_NONAME;
            addr = ip_net; /* already in network byte order from kernel */
        }
    }

    /* Build result(s) — if no socktype specified, return both TCP and UDP */
    int types[2];
    int protos[2];
    int count = 0;

    if (socktype == SOCK_STREAM || socktype == 0) {
        types[count] = SOCK_STREAM;
        protos[count] = IPPROTO_TCP;
        count++;
    }
    if (socktype == SOCK_DGRAM || socktype == 0) {
        types[count] = SOCK_DGRAM;
        protos[count] = IPPROTO_UDP;
        count++;
    }
    if (count == 0) {
        types[count] = socktype;
        protos[count] = protocol;
        count = 1;
    }

    struct addrinfo *head = 0;
    struct addrinfo *tail = 0;

    for (int i = 0; i < count; i++) {
        /* Allocate addrinfo + sockaddr_in together */
        struct addrinfo *ai = (struct addrinfo *)calloc(1,
            sizeof(struct addrinfo) + sizeof(struct fry_sockaddr_in));
        if (!ai) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }

        struct fry_sockaddr_in *sa = (struct fry_sockaddr_in *)(ai + 1);
        sa->sin_family = AF_INET;
        sa->sin_port = fry_htons((uint16_t)port);
        sa->sin_addr = addr;

        ai->ai_family = AF_INET;
        ai->ai_socktype = types[i];
        ai->ai_protocol = protos[i];
        ai->ai_addrlen = sizeof(struct fry_sockaddr_in);
        ai->ai_addr = (struct fry_sockaddr *)sa;
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
    case EAI_NONAME:   return "Name or service not known";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE:  return "Servname not supported for ai_socktype";
    case EAI_MEMORY:   return "Memory allocation failure";
    case EAI_SYSTEM:   return "System error";
    default:           return "Unknown error";
    }
}

/* -----------------------------------------------------------------------
 * getnameinfo — reverse lookup (numeric-only for now)
 * ----------------------------------------------------------------------- */

int getnameinfo(const struct fry_sockaddr *sa, uint32_t salen,
                char *host, uint32_t hostlen,
                char *serv, uint32_t servlen, int flags) {
    (void)flags;
    if (!sa || salen < sizeof(struct fry_sockaddr_in)) return EAI_FAIL;

    const struct fry_sockaddr_in *sin = (const struct fry_sockaddr_in *)sa;

    if (host && hostlen > 0) {
        uint32_t ip = fry_ntohl(sin->sin_addr);
        snprintf(host, hostlen, "%u.%u.%u.%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF, ip & 0xFF);
    }

    if (serv && servlen > 0) {
        snprintf(serv, servlen, "%u", fry_ntohs(sin->sin_port));
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * inet_pton / inet_ntop / inet_addr
 * ----------------------------------------------------------------------- */

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET || !src || !dst) return 0;
    uint32_t addr;
    if (parse_ipv4(src, &addr) != 0) return 0;
    *(uint32_t *)dst = addr;
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, uint32_t size) {
    if (af != AF_INET || !src || !dst || size < 16) return 0;
    uint32_t ip = fry_ntohl(*(const uint32_t *)src);
    snprintf(dst, size, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
    return dst;
}

uint32_t inet_addr(const char *cp) {
    uint32_t addr;
    if (parse_ipv4(cp, &addr) != 0) return 0xFFFFFFFF;
    return addr;
}

/* -----------------------------------------------------------------------
 * Socket POSIX-style wrappers (BSD-compatible names)
 * ----------------------------------------------------------------------- */

int socket_compat(int domain, int type, int protocol) {
    long rc = fry_socket(domain, type, protocol);
    return (rc < 0) ? -1 : (int)rc;
}

int connect_compat(int fd, const struct fry_sockaddr *addr, uint32_t addrlen) {
    return (fry_connect(fd, (const struct fry_sockaddr_in *)addr, addrlen) < 0) ? -1 : 0;
}

int bind_compat(int fd, const struct fry_sockaddr *addr, uint32_t addrlen) {
    return (fry_bind(fd, (const struct fry_sockaddr_in *)addr, addrlen) < 0) ? -1 : 0;
}

int listen_compat(int fd, int backlog) {
    return (fry_listen(fd, backlog) < 0) ? -1 : 0;
}

int accept_compat(int fd, struct fry_sockaddr *addr, uint32_t *addrlen) {
    long rc = fry_accept(fd, (struct fry_sockaddr_in *)addr, addrlen);
    return (rc < 0) ? -1 : (int)rc;
}

long send_compat(int fd, const void *buf, size_t len, int flags) {
    return fry_send(fd, buf, len, flags);
}

long recv_compat(int fd, void *buf, size_t len, int flags) {
    return fry_recv(fd, buf, len, flags);
}

int shutdown_compat(int fd, int how) {
    return (fry_shutdown_sock(fd, how) < 0) ? -1 : 0;
}

int setsockopt_compat(int fd, int level, int optname,
                      const void *optval, uint32_t optlen) {
    return (fry_setsockopt(fd, level, optname, optval, optlen) < 0) ? -1 : 0;
}

int getsockopt_compat(int fd, int level, int optname,
                      void *optval, uint32_t *optlen) {
    return (fry_getsockopt(fd, level, optname, optval, optlen) < 0) ? -1 : 0;
}
