/* ipc_msg.c : TLV message framing protocol */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc_msg.h"
#include "lumi_msg.h"

#include "byte_order.h"
#include <errno.h>
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
ipc_msg_send(int fd, uint32_t type, const void *payload, uint32_t len)
{
	uint32_t hdr[2];

	hdr[0] = BE32(type);
	hdr[1] = BE32(len);

	if (write_full(fd, hdr, sizeof(hdr)) < 0)
		return -1;
	if (len > 0 && write_full(fd, payload, len) < 0)
		return -1;
	return 0;
}

int
ipc_msg_send_empty(int fd, uint32_t type)
{
	return ipc_msg_send(fd, type, NULL, 0);
}

int
ipc_msg_frame(uint8_t *out, size_t outsz, uint32_t type,
    const void *payload, uint32_t len)
{
	uint32_t hdr[2];

	if ((size_t)IPC_HDR_SIZE + len > outsz)
		return -1;

	hdr[0] = BE32(type);
	hdr[1] = BE32(len);
	memcpy(out, hdr, IPC_HDR_SIZE);
	if (len > 0)
		memcpy(out + IPC_HDR_SIZE, payload, len);

	return (int)(IPC_HDR_SIZE + len);
}

int
ipc_msg_send_size(int fd, uint32_t type, int rows, int cols)
{
	struct ipc_size sz;
	uint8_t buf[16];
	int n;

	sz.rows = (uint16_t)rows;
	sz.cols = (uint16_t)cols;
	n = ipc_size_encode(&sz, buf, sizeof(buf));
	if (n < 0)
		return -1;
	return ipc_msg_send(fd, type, buf, (uint32_t)n);
}

int
ipc_msg_recv(int fd, uint32_t *out_type, void *buf, size_t bufsz,
    uint32_t *out_len)
{
	uint32_t hdr[2];
	uint32_t type, len;
	int rc;

	rc = read_full(fd, hdr, sizeof(hdr));
	if (rc != 0)
		return rc;

	type = BE32(hdr[0]);
	len = BE32(hdr[1]);

	if (len > IPC_MAX_PAYLOAD)
		return -1;

	if (out_type)
		*out_type = type;
	if (out_len)
		*out_len = len;

	if (len == 0)
		return 0;

	if (len <= (uint32_t)bufsz) {
		return read_full(fd, buf, len);
	} else {
		/* payload too large for buffer -- read what fits, discard rest */
		rc = read_full(fd, buf, bufsz);
		if (rc != 0)
			return rc;
		if (out_len)
			*out_len = (uint32_t)bufsz;
		/* discard excess */
		{
			char discard[256];
			size_t left = len - (uint32_t)bufsz;

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

int
ipc_client_attach(int fd, uint8_t flags, const char *name, uint8_t *out_role)
{
	struct ipc_attach at;
	struct ipc_attach_reply rep;
	uint8_t abuf[128], rbuf[512];
	uint32_t rtype = 0, rlen;
	int an, skipped;

	memset(&at, 0, sizeof(at));
	at.rows = 24;			/* observed size; does not constrain */
	at.cols = 80;
	at.flags = flags;
	at.client_id = (uint32_t)getpid();
	at.name = name ? name : "";
	at.name_len = (uint16_t)strlen(at.name);

	an = ipc_attach_encode(&at, abuf, sizeof(abuf));
	if (an < 0 || ipc_msg_send(fd, IPC_MSG_ATTACH, abuf, (uint32_t)an) < 0)
		return -1;

	/* the reply comes first, but a server may have a role change or the
	 * like queued to us by the time it answers; read past it */
	for (skipped = 0; skipped <= 16; skipped++) {
		if (ipc_msg_recv(fd, &rtype, rbuf, sizeof(rbuf), &rlen) != 0)
			return -1;
		if (rtype == IPC_MSG_ATTACH_REPLY)
			break;
	}
	if (rtype != IPC_MSG_ATTACH_REPLY)
		return -1;
	if (ipc_attach_reply_decode(&rep, rbuf, (int)rlen) < 0)
		return -1;
	if (out_role)
		*out_role = rep.role;
	return 0;
}
