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
#define IPC_MSG_ATTACH		0x0001	/* client -> server: attach.
					 * optional ipc_attach payload = the
					 * client's desired size, the role it
					 * wants, and who it is; server resizes
					 * to that size before replaying so the
					 * replay is generated at the final
					 * size.  an old client sends an
					 * ipc_size, which is the same message
					 * without the later fields */
#define IPC_MSG_ATTACH_REPLY	0x0006	/* server -> client: ipc_attach_reply,
					 * the resulting size plus the role
					 * actually granted.  an old server
					 * sends an ipc_size, which decodes
					 * with role 0 (WRITE) */
#define IPC_MSG_ROLE_CHANGE	0x0007	/* server -> client: 1 byte, the role
					 * now held; sent when it changes
					 * while attached */
#define IPC_MSG_CLIENT_EVENT	0x0008	/* server -> client: another client
					 * joined, left, or changed role.
					 * payload: kind (u8), client id (u32
					 * big endian), then the name bytes */
#define IPC_MSG_ROLE_REQUEST	0x0009	/* client -> server: 1 byte of
					 * IPC_ATTACH_F_* flags, asking for a
					 * role without reattaching.  the
					 * server answers with ROLE_CHANGE and
					 * demotes another client if the
					 * session token was claimed */

/* IPC_MSG_CLIENT_EVENT kinds.
 *
 * One client holds one connection per window, so a client joining a
 * three-window session produces three of these. The id is what lets the
 * receiver tell that apart from three clients arriving. */
#define IPC_CLIENT_JOIN		1
#define IPC_CLIENT_LEAVE	2
#define IPC_CLIENT_ROLE		3	/* took or lost the keyboard */

/* Roles. WRITE is 0 so that an old server, which sends no role at all,
 * decodes as the behavior it actually has. */
#define IPC_ROLE_WRITE		0	/* may send input, resize, and kill */
#define IPC_ROLE_VIEW		1	/* receives output only */

/* IpcAttach.flags. Each bit asks for a departure from the old behavior,
 * so an old client's absent flags byte means "as before". */
#define IPC_ATTACH_F_VIEW	0x01	/* attach read-only */
#define IPC_ATTACH_F_SIZE_OBSERVE 0x02	/* do not constrain the window size */
#define IPC_ATTACH_F_MIRROR	0x04	/* follow the session layout and focus */
#define IPC_ATTACH_F_TOKEN	0x08	/* holds the session write token */

/*
 * IPC_ATTACH_F_TOKEN settles which client has the keyboard when clients
 * are attaching at the same time. Each server decides per window, and a
 * client attaches to every window, so without it two clients racing
 * through the window list could each be granted a different one. The
 * holder of the session's write token says so here, and the server gives
 * it the keyboard, demoting whoever else had it on that window.
 *
 * A client could claim this without holding the token. The claim is
 * trusted because it comes from the session owner's own uid, which can
 * already do anything to these windows; it settles confusion between
 * cooperating clients rather than defending against a hostile one. A
 * client of another uid never reaches a server at all.
 */
#define IPC_MSG_DETACH		0x0002	/* either direction: detach */
#define IPC_MSG_KILL		0x0003	/* client -> server: kill server */
#define IPC_MSG_OK		0x0004	/* server -> client: success */
#define IPC_MSG_ERROR		0x0005	/* server -> client: error (payload: msg) */

/* 0x01xx: data transfer */
#define IPC_MSG_INPUT		0x0100	/* client -> server: keyboard input */
#define IPC_MSG_OUTPUT		0x0101	/* server -> client: PTY output */
#define IPC_MSG_FLOW_CTRL	0x0102	/* client -> server: 1=pause 0=resume */
#define IPC_MSG_REFRESH		0x0103	/* client -> server: resend screen
					 * replay at the current size */
#define IPC_MSG_INPUT_BEGIN	0x0104	/* client -> server: open a bracketed
					 * multi-message input run (13-b);
					 * the mserver buffers INPUT payloads
					 * per connection until INPUT_END or
					 * disconnect, so a paste cannot be
					 * split by another writer's input in
					 * multi-writer mode */
#define IPC_MSG_INPUT_END	0x0105	/* client -> server: close the run
					 * opened by INPUT_BEGIN and flush it
					 * to the PTY as one write */

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
#define IPC_MSG_NOP		0x0403	/* client -> proxy: ignored; used as a
					 * migration nudge after roaming */

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
