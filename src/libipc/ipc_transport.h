/* ipc_transport.h : abstract IPC transport for TLV messages */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef IPC_TRANSPORT_H
#define IPC_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * A transport carries the same TLV messages as ipc_msg.h, but hides the
 * underlying carrier so the attach client, mserver, and proxy can send
 * and receive without being welded to a raw fd.  The Unix-socket impl
 * (ipc_transport_unix_new) preserves the existing behavior exactly; a
 * networked impl can be added later behind the same vtable.
 *
 * get_fd exposes the pollable descriptor so the event loop
 * (iox_fd_add) and mserver's async output queue keep working unchanged.
 * A transport with no single pollable fd returns -1.
 */
struct ipc_transport {
	int	(*send)(void *ctx, uint32_t type, const void *payload,
		    uint32_t len);
	int	(*recv)(void *ctx, uint32_t *type, void *buf, size_t bufsz,
		    uint32_t *len);
	int	(*get_fd)(void *ctx);
	void	(*close)(void *ctx);
	void	*ctx;
};

/* create a transport over an existing connected fd.  the transport takes
 * ownership of the fd and closes it in ipc_transport_free.  returns NULL
 * on allocation failure. */
struct ipc_transport *ipc_transport_unix_new(int fd);

/* send a complete message.  returns 0 on success, -1 on error. */
int ipc_transport_send(struct ipc_transport *t, uint32_t type,
    const void *payload, uint32_t len);

/* convenience: send a message with no payload. */
int ipc_transport_send_empty(struct ipc_transport *t, uint32_t type);

/* receive a complete message.  returns 0 on success, -1 on error,
 * 1 on EOF (same convention as ipc_msg_recv). */
int ipc_transport_recv(struct ipc_transport *t, uint32_t *type, void *buf,
    size_t bufsz, uint32_t *len);

/* pollable fd for the event loop, or -1 if the transport has none. */
int ipc_transport_get_fd(struct ipc_transport *t);

/* close the underlying carrier and free the transport.  NULL is a no-op. */
void ipc_transport_free(struct ipc_transport *t);

#endif /* IPC_TRANSPORT_H */
