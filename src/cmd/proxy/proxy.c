/* proxy.c : multiplexing proxy for remote session tunneling */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "proxy_msg.h"
#include "sessdir.h"
#include "sessdir_watch.h"
#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "log.h"
#include "byte_order.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCONN_MAX 64

struct pconn {
	int		fd;
	uint32_t	id;		/* mserver PID = window ID */
	uint16_t	rows;
	uint16_t	cols;
	char		title[128];
};

static struct pconn pconns[PCONN_MAX];
static int pconn_count;

static const char *session_name;
static int watch_fd = -1;
static struct iox_loop *loop;

/* ---- pconn management ---- */

static struct pconn *
pconn_find_by_fd(int fd)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		if (pconns[i].fd == fd)
			return &pconns[i];
	}
	return NULL;
}

static struct pconn *
pconn_find_by_id(uint32_t id)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		if (pconns[i].id == id)
			return &pconns[i];
	}
	return NULL;
}

static void on_mserver_read(struct iox_loop *lp, int fd,
    unsigned events, void *arg);

static struct pconn *
pconn_add(uint32_t id, int fd, const char *title,
    uint16_t rows, uint16_t cols)
{
	struct pconn *pc;

	if (pconn_count >= PCONN_MAX)
		return NULL;
	pc = &pconns[pconn_count++];
	pc->fd = fd;
	pc->id = id;
	pc->rows = rows;
	pc->cols = cols;
	if (title) {
		strncpy(pc->title, title, sizeof(pc->title) - 1);
		pc->title[sizeof(pc->title) - 1] = '\0';
	} else {
		pc->title[0] = '\0';
	}

	if (loop)
		iox_fd_add(loop, fd, IOX_READ, on_mserver_read, pc);
	return pc;
}

static void
pconn_remove(uint32_t id)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		if (pconns[i].id == id) {
			if (loop)
				iox_fd_remove(loop, pconns[i].fd);
			ipc_close(pconns[i].fd);
			pconns[i] = pconns[--pconn_count];
			return;
		}
	}
}

/* ---- mserver attach ---- */

static int
attach_mserver(pid_t pid)
{
	char *dir, *title;
	char path[256];
	int fd;
	uint32_t rtype, rlen;
	char rbuf[64];
	struct ipc_size sz;
	uint16_t rows = 24, cols = 80;

	if (pconn_find_by_id((uint32_t)pid))
		return 0; /* already connected */

	dir = sessdir_server_path(session_name, pid);
	if (!dir)
		return -1;
	if (snprintf(path, sizeof(path), "%s/socket", dir) >=
	    (int)sizeof(path)) {
		free(dir);
		return -1;
	}
	free(dir);

	fd = ipc_connect(path);
	if (fd < 0)
		return -1;

	if (ipc_msg_send_empty(fd, IPC_MSG_ATTACH) < 0) {
		ipc_close(fd);
		return -1;
	}
	if (ipc_msg_recv(fd, &rtype, rbuf, sizeof(rbuf), &rlen) == 0 &&
	    rtype == IPC_MSG_ATTACH_REPLY) {
		if (ipc_size_decode(&sz, (const uint8_t *)rbuf,
		    (int)rlen) >= 0 && sz.rows > 0 && sz.cols > 0) {
			rows = sz.rows;
			cols = sz.cols;
		}
	}

	title = sessdir_read_file(session_name, pid, "title");
	pconn_add((uint32_t)pid, fd, title, rows, cols);
	free(title);
	return 0;
}

/* ---- PROXY_READY payload encoding ---- */

static int
encode_ready_payload(uint8_t *buf, size_t bufsz)
{
	uint8_t *p = buf;
	size_t left = bufsz;
	uint16_t count;
	int i;

	if (left < 2)
		return -1;
	count = BE16((uint16_t)pconn_count);
	memcpy(p, &count, 2);
	p += 2;
	left -= 2;

	for (i = 0; i < pconn_count; i++) {
		uint32_t wid;
		uint16_t r, c;
		uint8_t tlen;
		size_t need;

		tlen = (uint8_t)strlen(pconns[i].title);
		need = 4 + 2 + 2 + 1 + tlen;
		if (left < need)
			return -1;

		wid = BE32(pconns[i].id);
		memcpy(p, &wid, 4);
		p += 4;

		r = BE16(pconns[i].rows);
		memcpy(p, &r, 2);
		p += 2;

		c = BE16(pconns[i].cols);
		memcpy(p, &c, 2);
		p += 2;

		*p++ = tlen;
		memcpy(p, pconns[i].title, tlen);
		p += tlen;

		left -= need;
	}
	return (int)(p - buf);
}

static int
encode_win_added(const struct pconn *pc, uint8_t *buf, size_t bufsz)
{
	uint8_t *p = buf;
	uint32_t wid;
	uint16_t r, c;
	uint8_t tlen;
	size_t need;

	tlen = (uint8_t)strlen(pc->title);
	need = 4 + 2 + 2 + 1 + tlen;
	if (bufsz < need)
		return -1;

	wid = BE32(pc->id);
	memcpy(p, &wid, 4);
	p += 4;

	r = BE16(pc->rows);
	memcpy(p, &r, 2);
	p += 2;

	c = BE16(pc->cols);
	memcpy(p, &c, 2);
	p += 2;

	*p++ = tlen;
	memcpy(p, pc->title, tlen);
	p += tlen;

	return (int)(p - buf);
}

/* ---- event handlers ---- */

/* mserver -> stdout (client) */
static void
on_mserver_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	struct pconn *pc = arg;
	uint32_t type, len;
	char buf[IPC_MAX_PAYLOAD];
	int rc;

	(void)events;

	rc = ipc_msg_recv(fd, &type, buf, sizeof(buf), &len);
	if (rc != 0) {
		/* mserver died or error */
		uint32_t wid = pc->id;
		uint32_t wid_be;

		pconn_remove(wid);

		wid_be = BE32(wid);
		proxy_msg_send(STDOUT_FILENO, 0,
		    IPC_MSG_PROXY_WIN_REMOVED, &wid_be, 4);
		return;
	}

	/* relay to client with window ID envelope */
	proxy_msg_send(STDOUT_FILENO, pc->id, type, buf, len);
}

/* stdin (client) -> mserver */
static void
on_client_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	uint32_t window_id, type, len;
	char buf[IPC_MAX_PAYLOAD];
	struct pconn *pc;
	int rc;

	(void)events;
	(void)arg;

	rc = proxy_msg_recv(STDIN_FILENO, &window_id, &type, buf,
	    sizeof(buf), &len);
	if (rc != 0) {
		/* client disconnected */
		iox_loop_stop(lp);
		return;
	}

	if (window_id == 0) {
		/* proxy control -- currently no client->proxy control msgs */
		return;
	}

	pc = pconn_find_by_id(window_id);
	if (!pc)
		return;

	ipc_msg_send(pc->fd, type, buf, len);
}

/* sessdir watch -> discover new/dead mservers */
static void
on_watch(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	int flags;
	pid_t pids[PCONN_MAX];
	int n, i;

	(void)events;
	(void)arg;

	flags = sessdir_watch_read(fd);
	if (!(flags & SESSDIR_WATCH_CHANGED))
		return;

	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, PCONN_MAX);
	if (n < 0)
		return;

	/* add new mservers */
	for (i = 0; i < n; i++) {
		if (pconn_find_by_id((uint32_t)pids[i]))
			continue;
		if (attach_mserver(pids[i]) == 0) {
			struct pconn *pc = pconn_find_by_id((uint32_t)pids[i]);

			if (pc) {
				uint8_t buf[256];
				int plen = encode_win_added(pc, buf,
				    sizeof(buf));

				if (plen > 0)
					proxy_msg_send(STDOUT_FILENO, 0,
					    IPC_MSG_PROXY_WIN_ADDED,
					    buf, (uint32_t)plen);
			}
		}
	}

	/* remove dead mservers */
	for (i = pconn_count - 1; i >= 0; i--) {
		int j, found = 0;

		for (j = 0; j < n; j++) {
			if (pconns[i].id == (uint32_t)pids[j]) {
				found = 1;
				break;
			}
		}
		if (!found) {
			uint32_t wid = pconns[i].id;
			uint32_t wid_be = BE32(wid);

			pconn_remove(wid);
			proxy_msg_send(STDOUT_FILENO, 0,
			    IPC_MSG_PROXY_WIN_REMOVED, &wid_be, 4);
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

/* ---- main ---- */

static void
cleanup(void)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		ipc_msg_send_empty(pconns[i].fd, IPC_MSG_DETACH);
		ipc_close(pconns[i].fd);
	}
	pconn_count = 0;

	if (watch_fd >= 0) {
		sessdir_watch_stop(watch_fd);
		watch_fd = -1;
	}
}

static int
proxy_run(void)
{
	pid_t pids[PCONN_MAX];
	int n, i;
	uint8_t ready_buf[4096];
	int ready_len;

	/* discover and attach to all mservers */
	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, PCONN_MAX);
	if (n < 0) {
		log_err("failed to list servers for session '%s'",
		    session_name);
		return 1;
	}

	for (i = 0; i < n; i++)
		attach_mserver(pids[i]);

	if (pconn_count == 0) {
		log_err("no servers found for session '%s'", session_name);
		return 1;
	}

	/* send PROXY_READY to client */
	ready_len = encode_ready_payload(ready_buf, sizeof(ready_buf));
	if (ready_len < 0) {
		log_err("failed to encode ready payload");
		cleanup();
		return 1;
	}
	if (proxy_msg_send(STDOUT_FILENO, 0, IPC_MSG_PROXY_READY,
	    ready_buf, (uint32_t)ready_len) < 0) {
		log_err("failed to send ready message");
		cleanup();
		return 1;
	}

	/* set up event loop */
	loop = iox_loop_new();
	if (!loop) {
		log_err("failed to create event loop");
		cleanup();
		return 1;
	}

	/* register existing pconns with loop */
	for (i = 0; i < pconn_count; i++)
		iox_fd_add(loop, pconns[i].fd, IOX_READ,
		    on_mserver_read, &pconns[i]);

	/* watch stdin for client messages */
	iox_fd_add(loop, STDIN_FILENO, IOX_READ, on_client_read, NULL);

	/* watch session directory for new/dead mservers */
	watch_fd = sessdir_watch_start(session_name);
	if (watch_fd >= 0)
		iox_fd_add(loop, watch_fd, IOX_READ, on_watch, NULL);

	iox_signal_add(loop, SIGTERM, on_sigterm, NULL);

	iox_loop_run(loop);

	cleanup();
	iox_loop_free(loop);
	loop = NULL;
	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: lumi proxy -s <session>\n");
	exit(1);
}

int
cmd_proxy_main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		switch (opt) {
		case 's':
			session_name = optarg;
			break;
		default:
			usage();
		}
	}

	if (!session_name)
		usage();

	return proxy_run();
}
