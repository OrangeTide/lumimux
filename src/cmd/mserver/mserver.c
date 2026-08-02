/* mserver.c : lumi-mserver single-PTY micro-server */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#define _XOPEN_SOURCE 600
/* expose BSD send() flags (MSG_DONTWAIT) that strict _XOPEN_SOURCE hides */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#define ERR (-1)
#define OK (0)

#include "mserver.h"

#include "multicall.h"

#include "attr_store.h"
#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "window.h"
#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "sessdir.h"
#include "sessdir_control.h"
#include "sessdir_state.h"
#include "vt_buf.h"
#include "rune_width.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* MSG_NOSIGNAL does not exist on macOS; SIGPIPE is ignored globally (see
 * mserver_main), so a 0 fallback is safe. MSG_DONTWAIT is exposed above via
 * _DARWIN_C_SOURCE and is present on Linux/BSD natively. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int listen_fd = -1;
static char socket_path[PATH_MAX];
static char attrs_path[PATH_MAX];
static const char *session_name;
static struct window *win;
static struct iox_loop *loop;
static struct attr_store attrs;
static int last_pty_flags = -1;	/* cached PTY flags for change detection */

/* ---- PTY write helper ---- */

static void
write_to_pty(const char *data, size_t len)
{
	int fd = window_master_fd(win);
	const char *p = data;
	size_t left = len;

	while (left > 0) {
		ssize_t wr = write(fd, p, left);

		if (wr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		p += wr;
		left -= (size_t)wr;
	}
}

/* ---- connected clients ----
 *
 * Several clients may be attached at once. Each owns a growable byte queue
 * drained with non-blocking send(), so a slow, stalled, or dead client can
 * never block the event loop, and therefore can never block accept() for
 * new attaches.
 *
 * Backpressure has to distinguish "the application is faster than the
 * terminals" from "one client is not reading". While any client is keeping
 * up we go on reading the PTY, so a single slow client cannot stall the
 * session for everyone else. Its queue grows until it crosses the hard
 * limit or stops making progress for too long, and then that client is
 * dropped. Only when every attached client is blocked do we stop reading
 * the PTY and push backpressure onto the application, which is what a lone
 * client that stops reading has always done.
 */

#define MCLIENT_MAX	MSERVER_CLIENT_MAX

#define OUTQ_HIGH_WATER	(1u << 20)		/* 1 MiB */
#define OUTQ_LOW_WATER	(256u * 1024)		/* 256 KiB */

/* see mserver.h: variables so a test can lower them before startup */
size_t mserver_outq_hard_limit = 4u << 20;	/* 4 MiB: drop, not throttle */
int mserver_stall_secs = 10;			/* no progress for this long */

struct mclient {
	int		fd;		/* -1 when the slot is free */
	uint8_t		*outq;		/* pending bytes (framed messages) */
	size_t		outq_off;	/* bytes already sent from the front */
	size_t		outq_len;	/* total bytes valid in outq */
	size_t		outq_cap;	/* allocated capacity */
	int		hiwater;	/* PTY reads paused due to backlog */
	int		paused;		/* client asked us to pause (FLOW_CTRL) */
	time_t		stall_since;	/* backlog stopped draining, 0 = no */

	/* from the ATTACH handshake.  the role is what we granted, which
	 * is not always what was asked for. */
	uint8_t		role;
	uint8_t		flags;
	uint32_t	client_id;	/* same on every window of one client */
	uint16_t	rows, cols;	/* the size this client asked for */
	char		name[64];	/* display only, never a decision */
	time_t		last_deny;	/* rate limit for refusal messages */

	/* 13-b: a bracketed multi-message input run (INPUT_BEGIN..INPUT_END)
	 * is buffered here and flushed to the PTY as one write, so another
	 * connection's own INPUT cannot land in the mserver's single-
	 * threaded event loop between two messages of this run -- the
	 * whole reason a paste or send-keys run is bracketed in the first
	 * place. A bare INPUT with no open run bypasses this entirely and
	 * is written immediately, as it always was. */
	uint8_t		*inq;
	size_t		inq_len;
	size_t		inq_cap;
	int		inrun;
};

static struct mclient clients[MCLIENT_MAX];

static void mclient_disconnect(struct mclient *mc);
static void client_event(struct mclient *about, uint8_t kind);

/* a zeroed table would read as fd 0, which is stdin */
static void
mclient_table_init(void)
{
	int i;

	for (i = 0; i < MCLIENT_MAX; i++)
		clients[i].fd = -1;
}

/* iterate live clients: for (mc = mclient_first(); mc; mc = mclient_next(mc)) */
static struct mclient *
mclient_next(struct mclient *mc)
{
	for (mc++; mc < clients + MCLIENT_MAX; mc++) {
		if (mc->fd >= 0)
			return mc;
	}
	return NULL;
}

static struct mclient *
mclient_first(void)
{
	return clients[0].fd >= 0 ? &clients[0] : mclient_next(&clients[0]);
}

/* claim a free slot for a newly accepted connection, NULL when full */
static struct mclient *
mclient_alloc(int fd)
{
	int i;

	for (i = 0; i < MCLIENT_MAX; i++) {
		if (clients[i].fd < 0) {
			clients[i].fd = fd;
			clients[i].role = IPC_ROLE_WRITE;
			clients[i].flags = 0;
			clients[i].client_id = 0;
			clients[i].rows = clients[i].cols = 0;
			clients[i].name[0] = '\0';
			clients[i].last_deny = 0;
			return &clients[i];
		}
	}
	return NULL;
}

static int
mclient_count(void)
{
	struct mclient *mc;
	int n = 0;

	for (mc = mclient_first(); mc; mc = mclient_next(mc))
		n++;
	return n;
}

static size_t
mclient_pending(const struct mclient *mc)
{
	return mc->outq_len - mc->outq_off;
}

static void
mclient_reset_queue(struct mclient *mc)
{
	free(mc->outq);
	mc->outq = NULL;
	mc->outq_off = mc->outq_len = mc->outq_cap = 0;
	mc->hiwater = 0;
	mc->stall_since = 0;
}

/* 13-b: drop any open input run without writing it. Used on disconnect,
 * so a client that dies mid-paste cannot wedge its own future input --
 * there is no "future input" for a dead connection, and the accumulator
 * would otherwise sit allocated in a freed slot's stale state. */
static void
mclient_reset_inq(struct mclient *mc)
{
	free(mc->inq);
	mc->inq = NULL;
	mc->inq_len = mc->inq_cap = 0;
	mc->inrun = 0;
}

#define INRUN_HARD_LIMIT (4u << 20)	/* 4 MiB: an open run this large is
					 * misbehaving, not just a big paste;
					 * drop the connection rather than
					 * grow without bound, mirroring
					 * mclient_enqueue()'s own hard limit
					 * on an output backlog */

/* Append to a connection's open input run, growing as needed. Returns 0,
 * or -1 if the run exceeded INRUN_HARD_LIMIT and the connection was
 * dropped in response -- the caller must not touch *mc again. */
static int
mclient_inq_append(struct mclient *mc, const void *data, size_t len)
{
	if (mc->inq_len + len > INRUN_HARD_LIMIT) {
		log_info("dropping client fd %d: input run exceeded %u bytes",
		    mc->fd, INRUN_HARD_LIMIT);
		mclient_disconnect(mc);
		return -1;
	}
	if (mc->inq_len + len > mc->inq_cap) {
		size_t cap = mc->inq_cap ? mc->inq_cap : 4096;
		uint8_t *p;

		while (cap < mc->inq_len + len)
			cap *= 2;
		p = realloc(mc->inq, cap);
		if (!p) {
			mclient_disconnect(mc);
			return -1;
		}
		mc->inq = p;
		mc->inq_cap = cap;
	}
	memcpy(mc->inq + mc->inq_len, data, len);
	mc->inq_len += len;
	return 0;
}

/* stop reading the PTY master only when every attached client is blocked,
 * either by its own FLOW_CTRL request or by a backlog above the high-water
 * mark.  a client that is keeping up keeps the session moving; the one
 * that is not grows toward the hard limit and gets dropped. */
static void
update_master_interest(void)
{
	struct mclient *mc;
	unsigned want = IOX_READ;
	int live = 0, blocked = 0;

	for (mc = mclient_first(); mc; mc = mclient_next(mc)) {
		live++;
		if (mc->paused || mc->hiwater)
			blocked++;
	}
	if (live > 0 && blocked == live)
		want = 0;
	iox_fd_mod(loop, window_master_fd(win), want);
}

/* watch a client fd for writability only while its backlog exists. */
static void
update_client_interest(struct mclient *mc)
{
	unsigned want = IOX_READ;

	if (mc->fd < 0)
		return;
	if (mclient_pending(mc) > 0)
		want |= IOX_WRITE;
	iox_fd_mod(loop, mc->fd, want);
}

static void
mclient_flush(struct mclient *mc)
{
	size_t before;

	if (mc->fd < 0)
		return;
	before = mclient_pending(mc);

	while (mc->outq_off < mc->outq_len) {
		ssize_t w = send(mc->fd, mc->outq + mc->outq_off,
		    mc->outq_len - mc->outq_off, MSG_DONTWAIT | MSG_NOSIGNAL);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			mclient_disconnect(mc);
			return;
		}
		mc->outq_off += (size_t)w;
	}

	if (mc->outq_off == mc->outq_len)
		mc->outq_off = mc->outq_len = 0;	/* fully drained */

	/* a backlog that never shrinks is a client that stopped reading,
	 * which the socket itself will not report for as long as its send
	 * buffer has room. */
	if (mclient_pending(mc) < before || mclient_pending(mc) == 0)
		mc->stall_since = 0;
	else if (!mc->stall_since)
		mc->stall_since = time(NULL);

	if (mc->hiwater && mclient_pending(mc) <= OUTQ_LOW_WATER) {
		mc->hiwater = 0;
		update_master_interest();
	}
	update_client_interest(mc);
}

static void
mclient_enqueue(struct mclient *mc, uint32_t type, const void *payload,
    uint32_t len)
{
	size_t need = (size_t)IPC_HDR_SIZE + len;
	int n;

	if (mc->fd < 0)
		return;

	/* a client this far behind is not coming back.  drop it rather
	 * than buffer without bound or throttle the clients that are
	 * keeping up.  no error is sent: its queue is why we are here. */
	if (mclient_pending(mc) + need > mserver_outq_hard_limit ||
	    (mc->stall_since &&
	     time(NULL) - mc->stall_since >= mserver_stall_secs)) {
		log_info("dropping client fd %d: %zu bytes backlogged",
		    mc->fd, mclient_pending(mc));
		mclient_disconnect(mc);
		return;
	}

	/* reclaim space already sent from the front before growing */
	if (mc->outq_off > 0) {
		memmove(mc->outq, mc->outq + mc->outq_off, mclient_pending(mc));
		mc->outq_len -= mc->outq_off;
		mc->outq_off = 0;
	}

	if (mc->outq_len + need > mc->outq_cap) {
		size_t cap = mc->outq_cap ? mc->outq_cap : 8192;
		uint8_t *p;

		while (cap < mc->outq_len + need)
			cap *= 2;
		p = realloc(mc->outq, cap);
		if (!p) {
			/* cannot buffer: drop the client rather than block or
			 * lose loop integrity */
			mclient_disconnect(mc);
			return;
		}
		mc->outq = p;
		mc->outq_cap = cap;
	}

	n = ipc_msg_frame(mc->outq + mc->outq_len, mc->outq_cap - mc->outq_len,
	    type, payload, len);
	if (n < 0)
		return;			/* sized above; should not happen */
	mc->outq_len += (size_t)n;

	if (!mc->hiwater && mclient_pending(mc) >= OUTQ_HIGH_WATER) {
		mc->hiwater = 1;
		update_master_interest();
	}
	mclient_flush(mc);
}

static void
mclient_enqueue_empty(struct mclient *mc, uint32_t type)
{
	mclient_enqueue(mc, type, NULL, 0);
}

/* ---- roles ----
 *
 * A client that asks to type gets the keyboard only if nobody else holds
 * it. Everyone else observes. This decides per window, which is enough
 * while one attach process is the only thing connecting to all of them;
 * step 7 of the shared attach plan moves the decision to a session-wide
 * token so it cannot come out differently window by window, and leaves
 * this as the check that a client cannot talk its way past.
 */

static uint8_t
role_for(struct mclient *mc, uint8_t asked)
{
	struct mclient *o;

	if (asked & IPC_ATTACH_F_VIEW)
		return IPC_ROLE_VIEW;

	/* the session write token outranks whoever got here first: it is
	 * the only thing that makes the answer the same on every window
	 * when two clients are attaching at once */
	if (asked & IPC_ATTACH_F_TOKEN) {
		for (o = mclient_first(); o; o = mclient_next(o)) {
			uint8_t view = IPC_ROLE_VIEW;

			if (o == mc || o->role != IPC_ROLE_WRITE)
				continue;
			o->role = IPC_ROLE_VIEW;
			mclient_enqueue(o, IPC_MSG_ROLE_CHANGE, &view, 1);
			client_event(o, IPC_CLIENT_ROLE);
		}
		return IPC_ROLE_WRITE;
	}

	/* 13-a: in multi-writer mode nobody already typing is displaced by
	 * a new asker, so skip straight to granting WRITE. Read fresh on
	 * every call rather than cached, so a mode change takes effect for
	 * the next attach without restarting the mserver; an already-
	 * connected client's role is untouched either way (see
	 * sessdir_share_mode_get()'s doc comment for why this stays a
	 * per-attach check rather than a live one). */
	if (session_name &&
	    sessdir_share_mode_get(session_name) == SESSDIR_SHARE_MULTI_WRITER)
		return IPC_ROLE_WRITE;

	for (o = mclient_first(); o; o = mclient_next(o)) {
		if (o != mc && o->role == IPC_ROLE_WRITE)
			return IPC_ROLE_VIEW;
	}
	return IPC_ROLE_WRITE;
}

/* Resize the window to fit every client that is asking to be fitted.
 *
 * A window has one size and its clients may have several, so the smallest
 * wins: anything larger would put part of the screen where somebody cannot
 * see it. Clients that opted out of sizing (a viewer watching someone
 * else's session, usually) are left out of the vote and crop instead, so
 * that watching does not reshape the session being watched.
 *
 * With no participants at all the size is left alone rather than reset to
 * something arbitrary.
 */
static void
resize_to_fit(void)
{
	struct mclient *mc;
	int rows = 0, cols = 0;

	for (mc = mclient_first(); mc; mc = mclient_next(mc)) {
		if (mc->flags & IPC_ATTACH_F_SIZE_OBSERVE)
			continue;
		if (mc->rows == 0 || mc->cols == 0)
			continue;
		if (rows == 0 || mc->rows < rows)
			rows = mc->rows;
		if (cols == 0 || mc->cols < cols)
			cols = mc->cols;
	}
	if (rows > 0 && cols > 0)
		window_resize(win, rows, cols);
}

/* messages that change the window rather than observe it */
static int
msg_mutates(uint32_t type)
{
	switch (type) {
	case IPC_MSG_INPUT:
	case IPC_MSG_INPUT_BEGIN:
	case IPC_MSG_INPUT_END:
	case IPC_MSG_KILL:
	case IPC_MSG_FLOW_CTRL:
	case IPC_MSG_ATTR_SET:
	case IPC_MSG_ATTR_DELETE:
	case IPC_MSG_ATTR_TXN_COMMIT:
		return 1;
	}
	return 0;
}

/* Tell a client its message was refused, at most once a second.
 *
 * Saying nothing would leave a view-only client looking wedged, since its
 * keystrokes would vanish with no echo and no error. Saying it every time
 * would let a held-down key fill the queue of a client that is, by
 * definition, not allowed to make us do work. */
static void
mclient_deny(struct mclient *mc, const char *why)
{
	time_t now = time(NULL);

	if (now == mc->last_deny)
		return;
	mc->last_deny = now;
	mclient_enqueue(mc, IPC_MSG_ERROR, why, (uint32_t)strlen(why));
}

/* Tell the other clients that this one arrived, left, or changed role.
 *
 * Presence is symmetric on purpose: a client that only watches still has
 * to know who else is here, or a session can be observed without the
 * people in it being aware of each other.
 */
static int client_event_depth;

static void
client_event(struct mclient *about, uint8_t kind)
{
	uint8_t buf[5 + sizeof(about->name)];
	uint32_t id;
	size_t nlen;
	struct mclient *mc, *next;

	/* Sending to one client can fail, which drops that client, which
	 * would announce its own departure, which can fail again. Presence
	 * is a courtesy: stop rather than recurse. The clients that miss an
	 * event still see the roster, which is pruned by liveness. */
	if (client_event_depth)
		return;
	client_event_depth++;

	id = about->client_id;
	nlen = strlen(about->name);
	buf[0] = kind;
	buf[1] = (uint8_t)(id >> 24);
	buf[2] = (uint8_t)(id >> 16);
	buf[3] = (uint8_t)(id >> 8);
	buf[4] = (uint8_t)id;
	memcpy(buf + 5, about->name, nlen);

	for (mc = mclient_first(); mc; mc = next) {
		next = mclient_next(mc);
		if (mc == about)
			continue;
		mclient_enqueue(mc, IPC_MSG_CLIENT_EVENT, buf,
		    (uint32_t)(5 + nlen));
	}
	client_event_depth--;
}

/* send to every connected client */
static void
broadcast(uint32_t type, const void *payload, uint32_t len)
{
	struct mclient *mc, *next;

	/* a send failure disconnects, so hold the successor first */
	for (mc = mclient_first(); mc; mc = next) {
		next = mclient_next(mc);
		mclient_enqueue(mc, type, payload, len);
	}
}

/* ---- buffered screen dump for replay on attach ---- */

#define REPLAY_BUFSZ	32768

struct replay_buf {
	struct mclient *mc;
	char buf[REPLAY_BUFSZ];
	uint32_t used;
};

static void
replay_flush(struct replay_buf *rb)
{
	if (rb->used > 0) {
		mclient_enqueue(rb->mc, IPC_MSG_OUTPUT, rb->buf, rb->used);
		rb->used = 0;
	}
}

static void
dump_to_client(void *ctx, const char *data, size_t len)
{
	struct replay_buf *rb = ctx;

	while (len > 0) {
		uint32_t space = REPLAY_BUFSZ - rb->used;
		uint32_t chunk = (uint32_t)len;

		if (chunk > space)
			chunk = space;
		memcpy(rb->buf + rb->used, data, chunk);
		rb->used += chunk;
		data += chunk;
		len -= chunk;
		if (rb->used == REPLAY_BUFSZ)
			replay_flush(rb);
	}
}

/* dump the current screen to one client, which is the only per-client
 * traffic the server generates: everything else is broadcast. */
static void
send_replay(struct mclient *mc)
{
	struct replay_buf rb;

	rb.mc = mc;
	rb.used = 0;
	window_dump(win, dump_to_client, &rb);
	replay_flush(&rb);
}

/* ---- title tracking ---- */

static void
sync_title(void)
{
	const char *vt_title;

	vt_title = vt_state_title(window_vt(win));
	if (!vt_title)
		vt_title = "";	/* title cleared (e.g. title-stack pop) */
	if (strcmp(vt_title, window_title(win)) != 0) {
		window_set_title(win, vt_title);
		sessdir_write_file(session_name, getpid(),
		    "title", vt_title);
	}
}

/* ---- PTY flags tracking ---- */

static int
get_pty_flags(void)
{
	struct termios t;
	int flags = 0;

	if (tcgetattr(window_master_fd(win), &t) < 0)
		return -1;
	if (t.c_lflag & ECHO)
		flags |= IPC_PTY_ECHO;
	return flags;
}

static void
sync_pty_flags(void)
{
	int flags;
	uint8_t payload;

	if (!mclient_first())
		return;
	flags = get_pty_flags();
	if (flags < 0 || flags == last_pty_flags)
		return;
	last_pty_flags = flags;
	payload = (uint8_t)flags;
	broadcast(IPC_MSG_PTY_FLAGS, &payload, 1);
}

/* ---- client handling ---- */

/* If the client holding the keyboard left, hand it to the first client
 * still here that wanted it. Otherwise a session whose writer detaches
 * would be left with viewers who cannot type until one of them reattaches.
 * Step 7 replaces this with the session token, which survives the writer
 * rather than being reassigned. */
static void
role_reassign(void)
{
	struct mclient *mc, *cand = NULL;

	for (mc = mclient_first(); mc; mc = mclient_next(mc)) {
		if (mc->role == IPC_ROLE_WRITE)
			return;			/* still held */
		if (!cand && !(mc->flags & IPC_ATTACH_F_VIEW))
			cand = mc;		/* asked for it originally */
	}
	if (cand) {
		uint8_t role = IPC_ROLE_WRITE;

		cand->role = role;
		mclient_enqueue(cand, IPC_MSG_ROLE_CHANGE, &role, 1);
		client_event(cand, IPC_CLIENT_ROLE);
	}
}

static void
mclient_disconnect(struct mclient *mc)
{
	if (mc->fd >= 0) {
		client_event(mc, IPC_CLIENT_LEAVE);
		attr_store_txn_rollback_client(&attrs, mc->fd);
		iox_fd_remove(loop, mc->fd);
		ipc_close(mc->fd);
		mc->fd = -1;
	}

	/* drop its pending output and re-evaluate PTY reads */
	mclient_reset_queue(mc);
	mclient_reset_inq(mc);
	mc->paused = 0;
	mc->rows = mc->cols = 0;	/* no longer a vote on the size */
	mc->role = IPC_ROLE_VIEW;	/* frees the keyboard if it held it */
	update_master_interest();
	role_reassign();
	resize_to_fit();		/* the smallest client may have left */
}

static void
disconnect_all(void)
{
	int i;

	for (i = 0; i < MCLIENT_MAX; i++)
		mclient_disconnect(&clients[i]);
}

/* ---- attribute IPC handler ---- */

static void
handle_attr_msg(struct mclient *mc, uint32_t type, const char *buf,
    uint32_t len)
{
	struct ipc_attr_kv kv;
	struct ipc_attr_key ak;
	struct ipc_attr_txn_ok tok;
	uint32_t txn_id;
	uint8_t enc[IPC_MAX_PAYLOAD];
	int n;

	switch (type) {
	case IPC_MSG_ATTR_TXN_BEGIN:
		/* the store keys transaction ownership by fd, so a client
		 * that goes away has its transactions rolled back */
		if (attr_store_txn_begin(&attrs, mc->fd, &txn_id) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "txn pool full", 13);
			return;
		}
		tok.txn_id = txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			mclient_enqueue(mc, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_TXN_COMMIT:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		if (attr_store_txn_commit(&attrs, ak.txn_id) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "conflict", 8);
			return;
		}
		attr_store_save(&attrs, attrs_path);
		tok.txn_id = ak.txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			mclient_enqueue(mc, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_TXN_ROLLBACK:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		attr_store_txn_rollback(&attrs, ak.txn_id);
		tok.txn_id = ak.txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			mclient_enqueue(mc, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_SET:
		if (ipc_attr_kv_decode(&kv,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			/* NUL-terminate strings from wire */
			char key[ATTR_MAX_KEY_LEN];
			char val[ATTR_MAX_VALUE_LEN];

			snprintf(key, sizeof(key), "%.*s",
			    (int)kv.key_len, kv.key);
			snprintf(val, sizeof(val), "%.*s",
			    (int)kv.value_len, kv.value);
			if (attr_store_set(&attrs, kv.txn_id,
			    key, val) < 0) {
				mclient_enqueue(mc, IPC_MSG_ERROR,
				    "set failed", 10);
				return;
			}
		}
		mclient_enqueue_empty(mc, IPC_MSG_ATTR_OK);
		return;

	case IPC_MSG_ATTR_DELETE:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			char key[ATTR_MAX_KEY_LEN];

			snprintf(key, sizeof(key), "%.*s",
			    (int)ak.key_len, ak.key);
			if (attr_store_delete(&attrs, ak.txn_id,
			    key) < 0) {
				mclient_enqueue(mc, IPC_MSG_ERROR,
				    "delete failed", 13);
				return;
			}
		}
		mclient_enqueue_empty(mc, IPC_MSG_ATTR_OK);
		return;

	case IPC_MSG_ATTR_GET:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			char key[ATTR_MAX_KEY_LEN];
			char val[ATTR_MAX_VALUE_LEN];
			struct ipc_attr_kv resp;

			snprintf(key, sizeof(key), "%.*s",
			    (int)ak.key_len, ak.key);
			if (attr_store_get(&attrs, ak.txn_id,
			    key, val, (int)sizeof(val)) < 0) {
				mclient_enqueue(mc, IPC_MSG_ERROR,
				    "not found", 9);
				return;
			}
			resp.txn_id = ak.txn_id;
			resp.key = key;
			resp.key_len = (uint16_t)strlen(key);
			resp.value = val;
			resp.value_len = (uint16_t)strlen(val);
			n = ipc_attr_kv_encode(&resp, enc,
			    (int)sizeof(enc));
			if (n > 0)
				mclient_enqueue(mc, IPC_MSG_ATTR_VALUE,
				    enc, (uint32_t)n);
		}
		return;

	case IPC_MSG_ATTR_LIST:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			mclient_enqueue(mc, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			char listing[IPC_MAX_PAYLOAD - 64];
			struct ipc_attr_entries resp;

			if (attr_store_list(&attrs, ak.txn_id,
			    listing, (int)sizeof(listing)) < 0) {
				mclient_enqueue(mc, IPC_MSG_ERROR,
				    "list failed", 11);
				return;
			}
			resp.entries = listing;
			resp.entries_len = (uint16_t)strlen(listing);
			n = ipc_attr_entries_encode(&resp, enc,
			    (int)sizeof(enc));
			if (n > 0)
				mclient_enqueue(mc, IPC_MSG_ATTR_ENTRIES,
				    enc, (uint32_t)n);
		}
		return;
	}
}

static void
on_client_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	struct mclient *mc = arg;	/* registered with the watcher */
	uint32_t type, len;
	char buf[4096];
	int rc;

	/* drain pending output when the socket becomes writable */
	if (events & IOX_WRITE) {
		mclient_flush(mc);
		if (mc->fd < 0)
			return;
	}
	if (!(events & IOX_READ))
		return;

	rc = ipc_msg_recv(fd, &type, buf, sizeof(buf), &len);
	if (rc != 0) {
		mclient_disconnect(mc);
		return;
	}

	/* One check covers every message that could change the window, so a
	 * message type added later is refused by default rather than
	 * quietly allowed. */
	if (mc->role != IPC_ROLE_WRITE && msg_mutates(type)) {
		mclient_deny(mc, "read-only");
		return;
	}

	switch (type) {
	case IPC_MSG_INPUT:
		if (mc->inrun) {
			if (mclient_inq_append(mc, buf, len) < 0)
				return;		/* connection was dropped */
		} else {
			write_to_pty(buf, len);
		}
		break;

	case IPC_MSG_INPUT_BEGIN:
		/* a BEGIN with a run already open replaces it rather than
		 * appending to stale data -- a well-behaved client never
		 * does this, but starting fresh is the safe response to a
		 * protocol violation, not silently concatenating unrelated
		 * runs into one write. */
		mc->inq_len = 0;
		mc->inrun = 1;
		break;

	case IPC_MSG_INPUT_END:
		if (mc->inrun) {
			write_to_pty((const char *)mc->inq, mc->inq_len);
			mc->inrun = 0;
			mc->inq_len = 0;
		}
		/* an END with no open run is a no-op, not an error: nothing
		 * unsafe happens either way */
		break;

	case IPC_MSG_FLOW_CTRL:
		if (len >= 1) {
			int pause = (uint8_t)buf[0] ? 1 : 0;

			if (pause != mc->paused) {
				mc->paused = pause;
				update_master_interest();
			}
		}
		break;

	case IPC_MSG_WIN_RESIZE:
		/* what this client would like, not what the window becomes:
		 * the others get a say too */
		{
			struct ipc_size sz;

			if (ipc_size_decode(&sz, (const uint8_t *)buf,
			    (int)len) >= 0 && sz.rows > 0 && sz.cols > 0) {
				mc->rows = sz.rows;
				mc->cols = sz.cols;
				resize_to_fit();
			}
		}
		break;

	case IPC_MSG_REFRESH:
		/* client asked us to re-send the screen; used to resync a
		 * newly-sized pane without relying on the child to repaint */
		send_replay(mc);
		break;

	case IPC_MSG_ROLE_REQUEST:
		/* Not a mutating message: a view-only client has to be able
		 * to ask for the keyboard, and a writer to give it up.  Who
		 * may actually have it is still decided here. */
		if (len >= 1) {
			uint8_t want = role_for(mc, (uint8_t)buf[0]);

			mc->flags = (uint8_t)buf[0];
			resize_to_fit();	/* it may have opted in or out */
			if (want != mc->role) {
				mc->role = want;
				mclient_enqueue(mc, IPC_MSG_ROLE_CHANGE,
				    &want, 1);
				client_event(mc, IPC_CLIENT_ROLE);
			}
		}
		break;

	case IPC_MSG_DETACH:
		mclient_disconnect(mc);
		break;

	case IPC_MSG_KILL:
		ipc_msg_send_empty(fd, IPC_MSG_OK);
		mclient_disconnect(mc);
		iox_loop_stop(lp);
		break;

	case IPC_MSG_ATTR_TXN_BEGIN:
	case IPC_MSG_ATTR_TXN_COMMIT:
	case IPC_MSG_ATTR_TXN_ROLLBACK:
	case IPC_MSG_ATTR_SET:
	case IPC_MSG_ATTR_DELETE:
	case IPC_MSG_ATTR_GET:
	case IPC_MSG_ATTR_LIST:
		handle_attr_msg(mc, type, buf, len);
		break;

	default:
		break;
	}
}

static void
on_new_client(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	struct mclient *mc;
	int new_fd;
	uint32_t type, len;
	char buf[256];
	int rc;

	(void)events;
	(void)arg;

	new_fd = ipc_accept(fd);
	if (new_fd < 0)
		return;

	/* the session directory is 0700, so reaching this socket already
	 * means being its owner.  where the kernel will tell us, check
	 * anyway: it costs one comparison and makes "a foreign uid never
	 * reaches an mserver" an invariant this file enforces rather than
	 * a property of the filesystem.  clients of another user arrive
	 * through a broker instead, which is where policy belongs. */
#ifdef IPC_HAVE_PEER_CRED
	{
		uid_t peer;

		if (ipc_peer_cred(new_fd, &peer, NULL, NULL) < 0 ||
		    peer != getuid()) {
			log_err("refusing connection from uid %ld",
			    (long)peer);
			ipc_close(new_fd);
			return;
		}
	}
#endif

	rc = ipc_msg_recv(new_fd, &type, buf, sizeof(buf), &len);
	if (rc != 0) {
		ipc_close(new_fd);
		return;
	}

	/* one-shot commands */
	if (type != IPC_MSG_ATTACH) {
		switch (type) {
		case IPC_MSG_KILL:
			ipc_msg_send_empty(new_fd, IPC_MSG_OK);
			ipc_close(new_fd);
			iox_loop_stop(lp);
			return;
		}
		ipc_close(new_fd);
		return;
	}

	/* an attach joins the clients already here rather than replacing
	 * them.  a full table refuses the newcomer instead of evicting
	 * someone who is working. */
	mc = mclient_alloc(new_fd);
	if (!mc) {
		ipc_msg_send(new_fd, IPC_MSG_ERROR, "too many clients", 16);
		ipc_close(new_fd);
		return;
	}

	iox_fd_add(lp, mc->fd, IOX_READ, on_client_read, mc);

	/* record who attached and at what size.  an old client sends an
	 * ipc_size, which is this message without the trailing fields, so
	 * they arrive as zeroes and mean what they meant before. */
	{
		struct ipc_attach at;

		if (ipc_attach_decode(&at, (const uint8_t *)buf,
		    (int)len) >= 0) {
			mc->flags = at.flags;
			mc->client_id = at.client_id;
			mc->rows = at.rows;
			mc->cols = at.cols;
			if (at.name && at.name_len > 0)
				snprintf(mc->name, sizeof(mc->name), "%.*s",
				    (int)at.name_len, at.name);

			mc->role = role_for(mc, at.flags);

			/* fit the window to its clients before the replay
			 * below, so the replay is generated at the size
			 * this client will actually be shown */
			resize_to_fit();
		}
	}

	/* tell client our current VT size and its role, then send the
	 * screen replay.  the client creates its VT at this size so replay
	 * matches.  it will issue WIN_RESIZE afterward if it needs a
	 * different size. */
	{
		struct vt_state *v = window_vt(win);
		struct ipc_attach_reply rep;
		uint8_t sbuf[32];
		int sn;

		rep.rows = (uint16_t)vt_buf_rows(v->buf);
		rep.cols = (uint16_t)vt_buf_cols(v->buf);
		rep.role = mc->role;
		rep.nclients = (uint8_t)mclient_count();
		sn = ipc_attach_reply_encode(&rep, sbuf, sizeof(sbuf));
		if (sn > 0)
			mclient_enqueue(mc, IPC_MSG_ATTACH_REPLY, sbuf,
			    (uint32_t)sn);
	}
	send_replay(mc);

	/* and tell the others who just arrived */
	client_event(mc, IPC_CLIENT_JOIN);

	/* tell the joining client the echo state.  the others already know
	 * it, so this goes to the newcomer rather than through the
	 * broadcast that sync_pty_flags() uses for real changes. */
	{
		int flags = get_pty_flags();

		if (flags >= 0) {
			uint8_t payload = (uint8_t)flags;

			last_pty_flags = flags;
			mclient_enqueue(mc, IPC_MSG_PTY_FLAGS, &payload, 1);
		}
	}
}

/* ---- PTY handling ---- */

static void
on_master_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	char buf[4096];
	ssize_t n;

	(void)fd;
	(void)events;
	(void)arg;

	n = read(window_master_fd(win), buf, sizeof(buf));
	if (n <= 0) {
		iox_loop_stop(lp);
		return;
	}

	window_feed(win, buf, (size_t)n);
	sync_title();
	sync_pty_flags();

	broadcast(IPC_MSG_OUTPUT, buf, (uint32_t)n);
}

/* ---- signals ---- */

static void
on_sigchld(struct iox_loop *lp, int signo, void *arg)
{
	int status;
	pid_t pid;

	(void)signo;
	(void)arg;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == window_child_pid(win)) {
			iox_loop_stop(lp);
			return;
		}
	}
}

static void
on_sigterm(struct iox_loop *lp, int signo, void *arg)
{
	(void)signo;
	(void)arg;

	iox_loop_stop(lp);
}

/* ---- cleanup ---- */

static void
do_cleanup(void)
{
	struct sessdir_state *st;

	/* no clients can exist until the event loop is up and accepting,
	 * so skip a walk over them when register_server() failed before
	 * the loop was ever created (disconnect_all() reaches back into
	 * the loop through update_master_interest()). */
	if (loop)
		disconnect_all();

	if (loop) {
		iox_loop_free(loop);
		loop = NULL;
	}

	if (listen_fd >= 0) {
		ipc_close(listen_fd);
		ipc_unlink(socket_path);
		listen_fd = -1;
	}

	st = sessdir_state_open(session_name);
	if (st) {
		sessdir_state_remove_server(st, getpid());
		sessdir_state_close(st);
	}

	sessdir_server_destroy(session_name, getpid());

	if (win) {
		window_free(win);
		win = NULL;
	}
}

/* ---- server ---- */

static const char *shell;

/* (re)build this server's on-disk presence: the session directory, this
 * server's directory, its listening socket, its descriptor files, and its
 * entry in the session state file. 'win' must already exist. called at
 * startup and again from on_sighup() when the session directory was removed
 * out from under a still-running server. on success listen_fd holds the new
 * listener; on failure returns ERR with listen_fd unchanged. */
static int
register_server(void)
{
	struct sessdir_state *st;
	char *dir;
	const char *pty_path;

	/* create session directory (idempotent) */
	if (sessdir_session_create(session_name) < 0) {
		log_err("failed to create session directory");
		return ERR;
	}

	/* register in session directory */
	if (sessdir_server_create(session_name, getpid()) < 0) {
		log_err("failed to create server directory");
		return ERR;
	}

	/* build socket path inside server directory */
	dir = sessdir_server_path(session_name, getpid());
	if (!dir) {
		log_err("failed to get server path");
		return ERR;
	}
	if (snprintf(socket_path, sizeof(socket_path), "%s/socket",
	    dir) >= (int)sizeof(socket_path)) {
		log_err("socket path too long");
		free(dir);
		return ERR;
	}
	if (snprintf(attrs_path, sizeof(attrs_path), "%s/attrs",
	    dir) >= (int)sizeof(attrs_path)) {
		log_err("attrs path too long");
		free(dir);
		return ERR;
	}
	free(dir);

	listen_fd = ipc_listen(socket_path);
	if (listen_fd < 0) {
		log_err("failed to listen on %s", socket_path);
		return ERR;
	}

	/* write descriptor files */
	pty_path = ptsname(window_master_fd(win));
	if (pty_path)
		sessdir_write_file(session_name, getpid(),
		    "pty", pty_path);
	sessdir_write_file(session_name, getpid(), "title",
	    shell ? shell : "shell");

	/* register in session state */
	st = sessdir_state_open(session_name);
	if (st) {
		sessdir_state_add_server(st, getpid());
		sessdir_state_close(st);
	}

	return OK;
}

/* SIGHUP: our session directory was removed while we kept running (an
 * attach client tore it down, or it was cleaned up after that client
 * crashed). the listening socket is still open but its pathname is gone,
 * so no client can reach us. rebuild the directory and a fresh socket so
 * "lumi attach" can rediscover and relink this window. */
static void
on_sighup(struct iox_loop *lp, int signo, void *arg)
{
	(void)signo;
	(void)arg;

	/* still reachable on disk: nothing to do. */
	if (listen_fd >= 0 && socket_path[0] &&
	    access(socket_path, F_OK) == 0)
		return;

	/* the old listener is bound to a now-nameless path; drop it. */
	if (listen_fd >= 0) {
		iox_fd_remove(lp, listen_fd);
		ipc_close(listen_fd);
		listen_fd = -1;
	}

	if (register_server() != OK) {
		log_err("re-registration after SIGHUP failed");
		return;
	}

	iox_fd_add(lp, listen_fd, IOX_READ, on_new_client, NULL);
	log_info("re-registered session directory after SIGHUP");
}

static int
server(void)
{
	rune_width_init();
	mclient_table_init();

	/* a client dropping mid-write must not kill the server -- ignore
	 * SIGPIPE so writes to a gone client return EPIPE and we drop just
	 * that client instead of terminating the whole window. */
	signal(SIGPIPE, SIG_IGN);

	/* prevent recursive attach inside this session --
	 * must be set before window_new forks the shell */
	setenv("LUMI_SESSION", session_name, 1);

	/* create the window (forks the shell) */
	win = window_new(shell, 24, 80);
	if (!win) {
		log_err("failed to create window");
		return ERR;
	}

	/* build our on-disk presence: session dir, server dir, socket,
	 * descriptor files, and session-state entry */
	if (register_server() != OK) {
		do_cleanup();
		return ERR;
	}

	/* initialize attribute store */
	attr_store_init(&attrs);
	attr_store_load(&attrs, attrs_path);

	/* set up event loop */
	loop = iox_loop_new();
	if (!loop) {
		log_err("failed to create event loop");
		do_cleanup();
		return ERR;
	}

	iox_fd_add(loop, listen_fd, IOX_READ, on_new_client, NULL);
	iox_fd_add(loop, window_master_fd(win), IOX_READ,
	    on_master_read, NULL);
	iox_signal_add(loop, SIGCHLD, on_sigchld, NULL);
	iox_signal_add(loop, SIGTERM, on_sigterm, NULL);
	iox_signal_add(loop, SIGHUP, on_sighup, NULL);

	iox_loop_run(loop);

	do_cleanup();
	return OK;
}

/* ---- main ---- */

static void
usage(void)
{
	fprintf(stderr, "usage: lumi-mserver -s <name> [shell]\n");
	fprintf(stderr, "  -s name  session name (required)\n");
	exit(1);
}


static void
process_opts(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		switch (opt) {
		case 's':
			session_name = optarg;
			break;
		default:
			usage();
			return;
		}
	}
	if (!session_name) {
		usage();
		return;
	}
	if (optind < argc)
		shell = argv[optind];
}

int
cmd_mserver_main(int argc, char **argv)
{
	process_opts(argc, argv);

	if (server() != OK)
		return 1;

	return 0;
}
