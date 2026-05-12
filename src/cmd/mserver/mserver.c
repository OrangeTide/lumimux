/* mserver.c : lumi-mserver single-PTY micro-server */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#define _XOPEN_SOURCE 600

#define ERR (-1)
#define OK (0)

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
#include "sessdir_state.h"
#include "vt_buf.h"
#include "rune_width.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int listen_fd = -1;
static int client_fd = -1;
static char socket_path[PATH_MAX];
static char attrs_path[PATH_MAX];
static const char *session_name;
static struct window *win;
static struct iox_loop *loop;
static struct attr_store attrs;
static int last_pty_flags = -1;	/* cached PTY flags for change detection */
static int output_paused;		/* flow control: client requested pause */

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

/* ---- buffered screen dump for replay on attach ---- */

#define REPLAY_BUFSZ	32768

struct replay_buf {
	int fd;
	char buf[REPLAY_BUFSZ];
	uint32_t used;
};

static void
replay_flush(struct replay_buf *rb)
{
	if (rb->used > 0) {
		ipc_msg_send(rb->fd, IPC_MSG_OUTPUT,
		    rb->buf, rb->used);
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

static void
send_replay(int fd)
{
	struct replay_buf rb;

	rb.fd = fd;
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
	if (vt_title && strcmp(vt_title, window_title(win)) != 0) {
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

	if (client_fd < 0)
		return;
	flags = get_pty_flags();
	if (flags < 0 || flags == last_pty_flags)
		return;
	last_pty_flags = flags;
	payload = (uint8_t)flags;
	ipc_msg_send(client_fd, IPC_MSG_PTY_FLAGS, &payload, 1);
}

/* ---- client handling ---- */

static void
disconnect_client(void)
{
	if (client_fd >= 0) {
		attr_store_txn_rollback_client(&attrs, client_fd);
		iox_fd_remove(loop, client_fd);
		ipc_close(client_fd);
		client_fd = -1;
	}

	/* resume PTY reads if paused */
	if (output_paused) {
		output_paused = 0;
		iox_fd_mod(loop, window_master_fd(win), IOX_READ);
	}
}

/* ---- attribute IPC handler ---- */

static void
handle_attr_msg(int fd, uint32_t type, const char *buf, uint32_t len)
{
	struct ipc_attr_kv kv;
	struct ipc_attr_key ak;
	struct ipc_attr_txn_ok tok;
	uint32_t txn_id;
	uint8_t enc[IPC_MAX_PAYLOAD];
	int n;

	switch (type) {
	case IPC_MSG_ATTR_TXN_BEGIN:
		if (attr_store_txn_begin(&attrs, fd, &txn_id) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "txn pool full", 13);
			return;
		}
		tok.txn_id = txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			ipc_msg_send(fd, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_TXN_COMMIT:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		if (attr_store_txn_commit(&attrs, ak.txn_id) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "conflict", 8);
			return;
		}
		attr_store_save(&attrs, attrs_path);
		tok.txn_id = ak.txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			ipc_msg_send(fd, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_TXN_ROLLBACK:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		attr_store_txn_rollback(&attrs, ak.txn_id);
		tok.txn_id = ak.txn_id;
		n = ipc_attr_txn_ok_encode(&tok, enc, (int)sizeof(enc));
		if (n > 0)
			ipc_msg_send(fd, IPC_MSG_ATTR_TXN_OK, enc,
			    (uint32_t)n);
		return;

	case IPC_MSG_ATTR_SET:
		if (ipc_attr_kv_decode(&kv,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
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
				ipc_msg_send(fd, IPC_MSG_ERROR,
				    "set failed", 10);
				return;
			}
		}
		ipc_msg_send_empty(fd, IPC_MSG_ATTR_OK);
		return;

	case IPC_MSG_ATTR_DELETE:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			char key[ATTR_MAX_KEY_LEN];

			snprintf(key, sizeof(key), "%.*s",
			    (int)ak.key_len, ak.key);
			if (attr_store_delete(&attrs, ak.txn_id,
			    key) < 0) {
				ipc_msg_send(fd, IPC_MSG_ERROR,
				    "delete failed", 13);
				return;
			}
		}
		ipc_msg_send_empty(fd, IPC_MSG_ATTR_OK);
		return;

	case IPC_MSG_ATTR_GET:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
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
				ipc_msg_send(fd, IPC_MSG_ERROR,
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
				ipc_msg_send(fd, IPC_MSG_ATTR_VALUE,
				    enc, (uint32_t)n);
		}
		return;

	case IPC_MSG_ATTR_LIST:
		if (ipc_attr_key_decode(&ak,
		    (const uint8_t *)buf, (int)len) < 0) {
			ipc_msg_send(fd, IPC_MSG_ERROR,
			    "bad message", 11);
			return;
		}
		{
			char listing[IPC_MAX_PAYLOAD - 64];
			struct ipc_attr_entries resp;

			if (attr_store_list(&attrs, ak.txn_id,
			    listing, (int)sizeof(listing)) < 0) {
				ipc_msg_send(fd, IPC_MSG_ERROR,
				    "list failed", 11);
				return;
			}
			resp.entries = listing;
			resp.entries_len = (uint16_t)strlen(listing);
			n = ipc_attr_entries_encode(&resp, enc,
			    (int)sizeof(enc));
			if (n > 0)
				ipc_msg_send(fd, IPC_MSG_ATTR_ENTRIES,
				    enc, (uint32_t)n);
		}
		return;
	}
}

static void
on_client_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	uint32_t type, len;
	char buf[4096];
	int rc;

	(void)events;
	(void)arg;

	rc = ipc_msg_recv(fd, &type, buf, sizeof(buf), &len);
	if (rc != 0) {
		disconnect_client();
		return;
	}

	switch (type) {
	case IPC_MSG_INPUT:
		write_to_pty(buf, len);
		break;

	case IPC_MSG_FLOW_CTRL:
		if (len >= 1) {
			int pause = (uint8_t)buf[0];
			int master_fd = window_master_fd(win);

			if (pause && !output_paused) {
				output_paused = 1;
				iox_fd_mod(lp, master_fd, 0);
			} else if (!pause && output_paused) {
				output_paused = 0;
				iox_fd_mod(lp, master_fd, IOX_READ);
			}
		}
		break;

	case IPC_MSG_WIN_RESIZE:
		{
			struct ipc_size sz;

			if (ipc_size_decode(&sz, (const uint8_t *)buf,
			    (int)len) >= 0) {
				window_resize(win, sz.rows, sz.cols);
			}
		}
		break;

	case IPC_MSG_DETACH:
		disconnect_client();
		break;

	case IPC_MSG_KILL:
		ipc_msg_send_empty(fd, IPC_MSG_OK);
		disconnect_client();
		iox_loop_stop(lp);
		break;

	case IPC_MSG_ATTR_TXN_BEGIN:
	case IPC_MSG_ATTR_TXN_COMMIT:
	case IPC_MSG_ATTR_TXN_ROLLBACK:
	case IPC_MSG_ATTR_SET:
	case IPC_MSG_ATTR_DELETE:
	case IPC_MSG_ATTR_GET:
	case IPC_MSG_ATTR_LIST:
		handle_attr_msg(fd, type, buf, len);
		break;

	default:
		break;
	}
}

static void
on_new_client(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	int new_fd;
	uint32_t type, len;
	char buf[256];
	int rc;

	(void)events;
	(void)arg;

	new_fd = ipc_accept(fd);
	if (new_fd < 0)
		return;

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

	/* disconnect previous client */
	if (client_fd >= 0)
		disconnect_client();

	client_fd = new_fd;

	/* tell client our current VT size, then send screen replay.
	 * the client creates its VT at this size so replay matches.
	 * it will issue WIN_RESIZE afterward if it needs a different
	 * size. */
	{
		struct vt_state *v = window_vt(win);

		ipc_msg_send_size(client_fd, IPC_MSG_ATTACH_REPLY,
		    vt_buf_rows(v->buf), vt_buf_cols(v->buf));
	}
	send_replay(client_fd);

	/* send initial PTY flags so client knows echo state */
	last_pty_flags = -1;
	sync_pty_flags();

	iox_fd_add(lp, client_fd, IOX_READ, on_client_read, NULL);
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

	if (client_fd >= 0) {
		if (ipc_msg_send(client_fd, IPC_MSG_OUTPUT,
		    buf, (uint32_t)n) < 0)
			disconnect_client();
	}
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

	disconnect_client();

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

static int
server(void)
{
	struct sessdir_state *st;
	char *dir;
	const char *pty_path;

	rune_width_init();

	/* create session directory (idempotent) */
	if (sessdir_session_create(session_name) < 0) {
		log_err("failed to create session directory");
		return ERR;
	}

	/* prevent recursive attach inside this session --
	 * must be set before window_new forks the shell */
	setenv("LUMI_SESSION", session_name, 1);

	/* create the window (forks the shell) */
	win = window_new(shell, 24, 80);
	if (!win) {
		log_err("failed to create window");
		return ERR;
	}

	/* register in session directory */
	if (sessdir_server_create(session_name, getpid()) < 0) {
		log_err("failed to create server directory");
		do_cleanup();
		return ERR;
	}

	/* build socket path inside server directory */
	dir = sessdir_server_path(session_name, getpid());
	if (!dir) {
		log_err("failed to get server path");
		do_cleanup();
		return ERR;
	}
	if (snprintf(socket_path, sizeof(socket_path), "%s/socket",
	    dir) >= (int)sizeof(socket_path)) {
		log_err("socket path too long");
		free(dir);
		do_cleanup();
		return ERR;
	}
	if (snprintf(attrs_path, sizeof(attrs_path), "%s/attrs",
	    dir) >= (int)sizeof(attrs_path)) {
		log_err("attrs path too long");
		free(dir);
		do_cleanup();
		return ERR;
	}
	free(dir);

	listen_fd = ipc_listen(socket_path);
	if (listen_fd < 0) {
		log_err("failed to listen on %s", socket_path);
		do_cleanup();
		return ERR;
	}

	/* initialize attribute store */
	attr_store_init(&attrs);
	attr_store_load(&attrs, attrs_path);

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
