/* proxy_msg.c : multiplexed proxy envelope framing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "proxy_msg.h"
#include "ipc_msg.h"
#include "ipc_transport.h"

#include "byte_order.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* read exactly n bytes, retrying on EINTR.
 * returns 0 on success, -1 on error, 1 on EOF. */
static int
read_full(int fd, void *buf, size_t n)
{
	char *p = buf;
	size_t left = n;

	while (left > 0) {
		ssize_t r = read(fd, p, left);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return 1; /* EOF */
		p += r;
		left -= (size_t)r;
	}
	return 0;
}

/* write exactly n bytes, retrying on EINTR.
 * returns 0 on success, -1 on error. */
static int
write_full(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	size_t left = n;

	while (left > 0) {
		ssize_t w = write(fd, p, left);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		left -= (size_t)w;
	}
	return 0;
}

int
proxy_msg_send(int fd, uint32_t window_id, uint32_t type,
    const void *payload, uint32_t len)
{
	uint32_t hdr[3];

	hdr[0] = BE32(window_id);
	hdr[1] = BE32(type);
	hdr[2] = BE32(len);

	if (write_full(fd, hdr, sizeof(hdr)) < 0)
		return -1;
	if (len > 0 && write_full(fd, payload, len) < 0)
		return -1;
	return 0;
}

int
proxy_msg_recv(int fd, uint32_t *window_id, uint32_t *type,
    void *buf, size_t bufsz, uint32_t *len)
{
	uint32_t hdr[3];
	uint32_t wid, t, l;
	int rc;

	rc = read_full(fd, hdr, sizeof(hdr));
	if (rc != 0)
		return rc;

	wid = BE32(hdr[0]);
	t = BE32(hdr[1]);
	l = BE32(hdr[2]);

	if (l > IPC_MAX_PAYLOAD)
		return -1;

	if (window_id)
		*window_id = wid;
	if (type)
		*type = t;
	if (len)
		*len = l;

	if (l == 0)
		return 0;

	if (l <= (uint32_t)bufsz) {
		return read_full(fd, buf, l);
	} else {
		/* payload too large for buffer -- read what fits, discard rest */
		rc = read_full(fd, buf, bufsz);
		if (rc != 0)
			return rc;
		if (len)
			*len = (uint32_t)bufsz;
		/* discard excess */
		{
			char discard[256];
			size_t left = l - (uint32_t)bufsz;

			while (left > 0) {
				size_t chunk = left < sizeof(discard) ?
				    left : sizeof(discard);

				rc = read_full(fd, discard, chunk);
				if (rc != 0)
					return rc;
				left -= chunk;
			}
		}
		return 0;
	}
}

/*
 * The stdin/stdout proxy carrier uses proxy_msg_send/recv above, which put
 * window_id in the frame header.  The netchan carrier instead moves TLV
 * frames through an ipc_transport, whose own header already carries the
 * message type.  These two helpers ride the window_id as a 4-byte
 * big-endian prefix on the transport payload, so one reliable channel
 * still multiplexes every window.  The caller drains the transport with
 * ipc_transport_recv (blocking) or ipc_transport_netchan_try_recv
 * (event loop) and then splits the prefix with proxy_msg_xdecode.
 */
int
proxy_msg_xsend(struct ipc_transport *t, uint32_t window_id, uint32_t type,
    const void *payload, uint32_t len)
{
	uint8_t stackbuf[4096 + 4];
	uint8_t *p = stackbuf;
	uint32_t wid;
	int rc;

	if (len > IPC_MAX_PAYLOAD)
		return -1;
	if ((size_t)len + 4 > sizeof(stackbuf)) {
		p = malloc((size_t)len + 4);
		if (!p)
			return -1;
	}
	wid = BE32(window_id);
	memcpy(p, &wid, 4);
	if (len)
		memcpy(p + 4, payload, len);
	rc = ipc_transport_send(t, type, p, len + 4);
	if (p != stackbuf)
		free(p);
	return rc;
}

int
proxy_msg_xdecode(uint32_t *window_id, void *buf, uint32_t *len)
{
	uint8_t *b = buf;
	uint32_t wid;

	if (!len || *len < 4)
		return -1;
	memcpy(&wid, b, 4);
	if (window_id)
		*window_id = BE32(wid);
	*len -= 4;
	if (*len)
		memmove(b, b + 4, *len);
	return 0;
}
