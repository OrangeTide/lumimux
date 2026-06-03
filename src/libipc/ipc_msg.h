/* ipc_msg.h : TLV message framing protocol */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef IPC_MSG_H
#define IPC_MSG_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wire format: 8-byte header (network byte order)
 *   uint32_t type
 *   uint32_t len    (payload length, not including header)
 * followed by len bytes of payload.
 *
 * Message types are hierarchical: high byte is category.
 *   0x00xx  session / connection control
 *   0x01xx  data transfer
 *   0x02xx  window / PTY management
 */

#define IPC_HDR_SIZE	8
#define IPC_MAX_PAYLOAD	(64 * 1024)

/* 0x00xx: session / connection control */
#define IPC_MSG_ATTACH		0x0001	/* client -> server: attach */
#define IPC_MSG_ATTACH_REPLY	0x0006	/* server -> client: size (rows, cols) */
#define IPC_MSG_DETACH		0x0002	/* either direction: detach */
#define IPC_MSG_KILL		0x0003	/* client -> server: kill server */
#define IPC_MSG_OK		0x0004	/* server -> client: success */
#define IPC_MSG_ERROR		0x0005	/* server -> client: error (payload: msg) */

/* 0x01xx: data transfer */
#define IPC_MSG_INPUT		0x0100	/* client -> server: keyboard input */
#define IPC_MSG_OUTPUT		0x0101	/* server -> client: PTY output */
#define IPC_MSG_FLOW_CTRL	0x0102	/* client -> server: 1=pause 0=resume */

/* 0x02xx: window / PTY management */
#define IPC_MSG_PTY_FLAGS	0x0201	/* server -> client: PTY flags (1 byte) */
#define IPC_MSG_WIN_RESIZE	0x0207	/* client -> server: resize PTY */

/* IPC_MSG_PTY_FLAGS payload: single byte, bitmask */
#define IPC_PTY_ECHO		0x01	/* PTY has ECHO enabled */

/* 0x03xx: attribute store */
#define IPC_MSG_ATTR_TXN_BEGIN	0x0300	/* client -> server: begin txn */
#define IPC_MSG_ATTR_TXN_COMMIT	0x0301	/* client -> server: commit txn */
#define IPC_MSG_ATTR_TXN_ROLLBACK 0x0302 /* client -> server: rollback txn */
#define IPC_MSG_ATTR_TXN_OK	0x0303	/* server -> client: txn_id */
#define IPC_MSG_ATTR_SET	0x0310	/* client -> server: txn_id+key+val */
#define IPC_MSG_ATTR_DELETE	0x0311	/* client -> server: txn_id+key */
#define IPC_MSG_ATTR_GET	0x0320	/* client -> server: txn_id+key */
#define IPC_MSG_ATTR_VALUE	0x0321	/* server -> client: key+value */
#define IPC_MSG_ATTR_LIST	0x0322	/* client -> server: txn_id */
#define IPC_MSG_ATTR_ENTRIES	0x0323	/* server -> client: k=v entries */
#define IPC_MSG_ATTR_OK		0x0324	/* server -> client: success */

/* 0x04xx: proxy control (window_id = 0 in proxy envelope) */
#define IPC_MSG_PROXY_READY	0x0400	/* proxy -> client: initial window list */
#define IPC_MSG_PROXY_WIN_ADDED	0x0401	/* proxy -> client: new window */
#define IPC_MSG_PROXY_WIN_REMOVED 0x0402 /* proxy -> client: window died */

/* send a complete message (header + payload). blocks until sent.
 * returns 0 on success, -1 on error. */
int ipc_msg_send(int fd, uint32_t type, const void *payload, uint32_t len);

/* convenience: send a message with no payload */
int ipc_msg_send_empty(int fd, uint32_t type);

/* serialize a complete message (header + payload) into a caller buffer
 * without doing any I/O. returns the framed length (IPC_HDR_SIZE + len)
 * on success, or -1 if it does not fit in outsz. */
int ipc_msg_frame(uint8_t *out, size_t outsz, uint32_t type,
    const void *payload, uint32_t len);

/* convenience: send a message with IpcSize payload (rows, cols) */
int ipc_msg_send_size(int fd, uint32_t type, int rows, int cols);

/* receive a complete message. blocks until header + payload received.
 * stores type in *out_type, copies payload into buf (up to bufsz),
 * stores payload length in *out_len.
 * returns 0 on success, -1 on error, 1 on EOF. */
int ipc_msg_recv(int fd, uint32_t *out_type, void *buf, size_t bufsz,
    uint32_t *out_len);

#endif /* IPC_MSG_H */
