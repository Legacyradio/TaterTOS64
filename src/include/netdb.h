/*
 * TaterTOS64v3 — <netdb.h>
 *
 * POSIX resolver and network database access.
 */

#ifndef _TATERTOS_NETDB_H
#define _TATERTOS_NETDB_H

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE      0x01
#define AI_CANONNAME    0x02
#define AI_NUMERICHOST  0x04
#define AI_NUMERICSERV  0x08
#define AI_V4MAPPED     0x10
#define AI_ALL          0x20
#define AI_ADDRCONFIG   0x40

#define EAI_AGAIN       1
#define EAI_BADFLAGS    2
#define EAI_FAIL        3
#define EAI_FAMILY      4
#define EAI_MEMORY      5
#define EAI_NONAME      6
#define EAI_SERVICE     7
#define EAI_SOCKTYPE    8
#define EAI_SYSTEM      9
#define EAI_OVERFLOW    10

/* Resolver API */
int         getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void        freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int         getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);

/* Hostname error (stub) */
extern int  h_errno;
const char *hstrerror(int err);

/* Legacy host lookup (stubs — use getaddrinfo instead) */
struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
struct hostent *gethostent(void);
void            sethostent(int stayopen);
void            endhostent(void);

/* Service lookup (stubs — TaterTOS has no /etc/services) */
struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;
    char  *s_proto;
};
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
struct servent *getservent(void);
void            setservent(int stayopen);
void            endservent(void);

/* Protocol lookup (stubs — TaterTOS has no /etc/protocols) */
struct protoent {
    char  *p_name;
    char **p_aliases;
    int    p_proto;
};
struct protoent *getprotobyname(const char *name);
struct protoent *getprotobynumber(int proto);
struct protoent *getprotoent(void);
void             setprotoent(int stayopen);
void             endprotoent(void);

#ifdef __cplusplus
}
#endif

#endif
