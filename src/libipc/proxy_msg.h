/* proxy_msg.h : multiplexed proxy envelope framing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PROXY_MSG_H
#define PROXY_MSG_H

#include <stddef.h>
#include <stdint.h>

/*
 * Proxy envelope wire format: 12-byte header (network byte order)
 *   uint32_t window_id    (0 = proxy control, else mserver PID)
 *   uint32_t type         (standard IPC message type or 0x04xx)
 *   uint32_t len          (payload length, not including header)
 * followed by len bytes of payload.
 *
 * The inner type+len+payload matches the standard IPC TLV format.
 * The proxy strips/adds window_id when relaying to/from mservers.
 */

#define PROXY_HDR_SIZE	12

/* send a proxy-enveloped message. blocks until sent.
 * returns 0 on success, -1 on error. */
int proxy_msg_send(int fd, uint32_t window_id, uint32_t type,
    const void *payload, uint32_t len);

/* receive a proxy-enveloped message. blocks until header + payload received.
 * stores window_id, type, and payload length.
 * returns 0 on success, -1 on error, 1 on EOF. */
int proxy_msg_recv(int fd, uint32_t *window_id, uint32_t *type,
    void *buf, size_t bufsz, uint32_t *len);

#endif /* PROXY_MSG_H */
