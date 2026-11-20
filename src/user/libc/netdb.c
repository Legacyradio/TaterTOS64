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

int h_errno = 0;

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

/* -----------------------------------------------------------------------
 * fry1696: inet_* + service/proto/iface helpers — required to link _socket
 * (CPython socketmodule.c). IPv4 fully supported; IPv6 reported unsupported;
 * service DB empty (numeric ports only); protocol DB = tcp/udp/icmp/ip.
 * ----------------------------------------------------------------------- */

int inet_aton(const char *cp, struct in_addr *addr) {
    uint32_t v;
    if (parse_ipv4(cp, &v) != 0) return 0;      /* POSIX: 0 = failure */
    if (addr) addr->s_addr = v;                 /* network byte order */
    return 1;                                   /* 1 = success */
}

char *inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    uint32_t h = ntohl(in.s_addr);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)((h >> 24) & 0xFF), (unsigned)((h >> 16) & 0xFF),
             (unsigned)((h >> 8) & 0xFF), (unsigned)(h & 0xFF));
    return buf;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET) {
        uint32_t v;
        if (!src || parse_ipv4(src, &v) != 0) return 0;  /* 0 = unparseable */
        if (dst) ((struct in_addr *)dst)->s_addr = v;
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;   /* IPv6 / other families not supported */
}

const char *inet_ntop(int af, const void *src, char *dst, uint32_t size) {
    if (af == AF_INET) {
        if (!src || !dst) { errno = EINVAL; return 0; }
        uint32_t h = ntohl(((const struct in_addr *)src)->s_addr);
        char tmp[INET_ADDRSTRLEN];
        int n = snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
                         (unsigned)((h >> 24) & 0xFF), (unsigned)((h >> 16) & 0xFF),
                         (unsigned)((h >> 8) & 0xFF), (unsigned)(h & 0xFF));
        if (n < 0 || (uint32_t)n >= size) { errno = ENOSPC; return 0; }
        memcpy(dst, tmp, (size_t)n + 1);
        return dst;
    }
    errno = EAFNOSUPPORT;
    return 0;
}

/* No /etc/services — numeric ports only; return NULL so callers fall back. */
struct servent *getservbyname(const char *name, const char *proto) {
    (void)name; (void)proto; return 0;
}
struct servent *getservbyport(int port, const char *proto) {
    (void)port; (void)proto; return 0;
}

/* Minimal /etc/protocols. */
struct protoent *getprotobyname(const char *name) {
    static char nbuf[8];
    static char *aliases[1] = { 0 };
    static struct protoent pe;
    int proto;
    if (!name) return 0;
    if (strcmp(name, "tcp") == 0) proto = 6;
    else if (strcmp(name, "udp") == 0) proto = 17;
    else if (strcmp(name, "icmp") == 0) proto = 1;
    else if (strcmp(name, "ip") == 0) proto = 0;
    else return 0;
    strncpy(nbuf, name, sizeof(nbuf) - 1);
    nbuf[sizeof(nbuf) - 1] = '\0';
    pe.p_name = nbuf;
    pe.p_aliases = aliases;
    pe.p_proto = proto;
    return &pe;
}

/* No interface-name table. */
char *if_indextoname(unsigned int ifindex, char *ifname) {
    (void)ifindex; (void)ifname;
    errno = ENXIO;
    return 0;
}

/* fry1696 (batch 2): host/iface lookups also referenced by socketmodule.c. */
const char *hstrerror(int err) {
    switch (err) {
    case 0:  return "Resolver Error 0 (no error)";
    case 1:  return "Unknown host";                    /* HOST_NOT_FOUND */
    case 2:  return "Host name lookup failure";         /* TRY_AGAIN */
    case 3:  return "Unknown server error";             /* NO_RECOVERY */
    case 4:  return "No address associated with name";  /* NO_DATA */
    default: return "Unknown resolver error";
    }
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static char namebuf[256];
    static uint32_t addr;            /* network byte order */
    static char *addr_list[2];
    static char *aliases[1] = { 0 };
    uint32_t ip = 0;
    if (!name) { h_errno = 1; return 0; }
    if (parse_ipv4(name, &ip) < 0) {           /* not dotted-decimal -> DNS */
        if (fry_dns_resolve(name, &ip) < 0) { h_errno = 1; return 0; }
    }
    addr = ip;
    strncpy(namebuf, name, sizeof(namebuf) - 1);
    namebuf[sizeof(namebuf) - 1] = '\0';
    addr_list[0] = (char *)&addr;
    addr_list[1] = 0;
    he.h_name = namebuf;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addr_list;
    h_errno = 0;
    return &he;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    (void)addr; (void)len; (void)type;
    h_errno = 1;                     /* HOST_NOT_FOUND — no reverse DNS */
    return 0;
}

unsigned int if_nametoindex(const char *ifname) {
    (void)ifname;
    return 0;                        /* unknown interface */
}

struct if_nameindex *if_nameindex(void) {
    errno = ENOSYS;
    return 0;                        /* no interface enumeration */
}

void if_freenameindex(struct if_nameindex *ptr) {
    (void)ptr;
}
