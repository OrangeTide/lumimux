/* ipc_transport.c : abstract IPC transport for TLV messages */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "ipc_transport.h"

#include "ipc.h"
#include "ipc_msg.h"

#include <stdlib.h>

/* ---- Unix-socket implementation ---- */

struct ipc_transport_unix {
	struct ipc_transport	base;
	int			fd;
};

static int
ux_send(void *ctx, uint32_t type, const void *payload, uint32_t len)
{
	struct ipc_transport_unix *u = ctx;

	return ipc_msg_send(u->fd, type, payload, len);
}

static int
ux_recv(void *ctx, uint32_t *type, void *buf, size_t bufsz, uint32_t *len)
{
	struct ipc_transport_unix *u = ctx;

	return ipc_msg_recv(u->fd, type, buf, bufsz, len);
}

static int
ux_get_fd(void *ctx)
{
	struct ipc_transport_unix *u = ctx;

	return u->fd;
}

static void
ux_close(void *ctx)
{
	struct ipc_transport_unix *u = ctx;

	if (u->fd >= 0) {
		ipc_close(u->fd);
		u->fd = -1;
	}
}

struct ipc_transport *
ipc_transport_unix_new(int fd)
{
	struct ipc_transport_unix *u = malloc(sizeof(*u));

	if (!u)
		return NULL;
	u->fd = fd;
	u->base.send = ux_send;
	u->base.recv = ux_recv;
	u->base.get_fd = ux_get_fd;
	u->base.close = ux_close;
	u->base.ctx = u;
	return &u->base;
}

/* ---- transport-agnostic wrappers ---- */

int
ipc_transport_send(struct ipc_transport *t, uint32_t type,
    const void *payload, uint32_t len)
{
	return t->send(t->ctx, type, payload, len);
}

int
ipc_transport_send_empty(struct ipc_transport *t, uint32_t type)
{
	return t->send(t->ctx, type, NULL, 0);
}

int
ipc_transport_recv(struct ipc_transport *t, uint32_t *type, void *buf,
    size_t bufsz, uint32_t *len)
{
	return t->recv(t->ctx, type, buf, bufsz, len);
}

int
ipc_transport_get_fd(struct ipc_transport *t)
{
	return t->get_fd(t->ctx);
}

void
ipc_transport_free(struct ipc_transport *t)
{
	if (!t)
		return;
	t->close(t->ctx);
	free(t);
}
