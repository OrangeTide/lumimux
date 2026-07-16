/* nc_udp.h : UDP transport helpers for netchan (sockaddr <-> nc_addr) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_UDP_H
#define NC_UDP_H

#include <sys/socket.h>
#include "nc_addr.h"

/*
 * UDP packing of nc_addr:
 *
 *   a[0]      family tag: 4 (IPv4) or 6 (IPv6)
 *   a[1..]    raw address bytes (4 for IPv4, 16 for IPv6)
 *   a[..]+2   UDP port, big-endian (network byte order)
 *
 * len is 7 for IPv4, 19 for IPv6. This layout is identical to the
 * CONNECT_REDIRECT wire frame, which is why the netchan core can build a
 * redirect address with a plain memcpy and never includes a socket header.
 *
 * These helpers are the entire seam between the OS socket API and the
 * transport-agnostic core: sockaddr appears here and nowhere else.
 */

/* Pack a sockaddr (AF_INET / AF_INET6) into nc_addr. Returns 0 on success,
 * -1 for an unsupported family (a is cleared to len 0). */
int nc_udp_from_sockaddr(struct nc_addr *a,
                         const struct sockaddr *sa, socklen_t salen);

/* Unpack nc_addr into a sockaddr_storage. Returns the sockaddr length on
 * success, 0 if the address is unset or malformed. */
socklen_t nc_udp_to_sockaddr(const struct nc_addr *a,
                             struct sockaddr_storage *ss);

#endif /* NC_UDP_H */
