/* nc_udp.c : UDP transport helpers for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_udp.h"
#include <netinet/in.h>
#include <string.h>

int
nc_udp_from_sockaddr(struct nc_addr *a, const struct sockaddr *sa,
                     socklen_t salen)
{
    memset(a, 0, sizeof(*a));
    if (!sa)
        return -1;

    if (sa->sa_family == AF_INET &&
        salen >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        a->len = 7;
        a->a[0] = 4;
        memcpy(a->a + 1, &sin->sin_addr, 4);
        memcpy(a->a + 5, &sin->sin_port, 2);   /* already network order */
        return 0;
    }

    if (sa->sa_family == AF_INET6 &&
        salen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        a->len = 19;
        a->a[0] = 6;
        memcpy(a->a + 1, &sin6->sin6_addr, 16);
        memcpy(a->a + 17, &sin6->sin6_port, 2);
        return 0;
    }

    return -1;
}

socklen_t
nc_udp_to_sockaddr(const struct nc_addr *a, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof(*ss));
    if (!a || a->len == 0)
        return 0;

    if (a->len == 7 && a->a[0] == 4) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        memcpy(&sin->sin_addr, a->a + 1, 4);
        memcpy(&sin->sin_port, a->a + 5, 2);
        return sizeof(*sin);
    }

    if (a->len == 19 && a->a[0] == 6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
        sin6->sin6_family = AF_INET6;
        memcpy(&sin6->sin6_addr, a->a + 1, 16);
        memcpy(&sin6->sin6_port, a->a + 17, 2);
        return sizeof(*sin6);
    }

    return 0;
}
