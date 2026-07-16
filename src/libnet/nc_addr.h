/* nc_addr.h : opaque transport address for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_ADDR_H
#define NC_ADDR_H

#include <stdint.h>

/*
 * A transport-agnostic address. The netchan core never interprets the
 * bytes in a[]; it only copies them and compares them for equality. Each
 * transport backend decides how to pack its own addressing into a[]:
 *
 *   nc_udp   packs an IPv4 or IPv6 address plus port (see nc_udp.h).
 *   others   may pack a channel handle, a peer index, whatever fits.
 *
 * This is the single seam that lets one protocol core run over UDP on the
 * desktop and over WebRTC or WebSockets in a browser without the core
 * knowing which. 19 bytes is enough for the largest packing we ship
 * (IPv6 + port); a[] is sized with a little headroom.
 */

#define NC_ADDR_MAX 20

struct nc_addr {
    uint8_t len;              /* bytes used in a[]; 0 means "unset" */
    uint8_t a[NC_ADDR_MAX];
};

#endif /* NC_ADDR_H */
