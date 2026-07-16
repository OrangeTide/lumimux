/* net_proxy.c : netchan bridge between a remote client and local mservers */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * lumi-net-proxy is the remote-host half of FUTURE.md 11C.  It listens on
 * a netchan/UDP endpoint, accepts one client, and bridges TLV messages
 * between that netchan connection and the session's local mserver Unix
 * sockets.  It is the netchan peer on one side and an ordinary Unix-socket
 * IPC client on the other, so mserver itself never needs a network
 * transport.  The endpoint (its UDP port) is published in the session
 * directory as "net-addr" for lumi attach to discover.
 *
 * The client link multiplexes every window over one reliable channel: the
 * window_id rides as a prefix on each transport payload (proxy_msg_xsend /
 * _xdecode), mirroring the stdin/stdout SSH proxy's proxy_msg envelope.
 * The netchan link cannot block the loop, so it is serviced with the
 * non-blocking pump/try_recv/timeout drain rather than iox callbacks.
 */

#include "ipc.h"
#include "ipc_msg.h"
#include "ipc_transport.h"
#include "ipc_transport_netchan.h"
#include "lumi_msg.h"
#include "proxy_msg.h"
#include "sessdir.h"
#include "sessdir_watch.h"
#include "nc_udp.h"
#include "log.h"
#include "byte_order.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_PSK_LEN	32

#define PCONN_MAX	64
#define PROXY_TICK_MS	200

struct pconn {
	int			 fd;	/* mserver Unix fd, for poll() */
	struct ipc_transport	*t;	/* carries TLV to/from the mserver */
	uint32_t		 id;	/* mserver PID = window ID */
	uint16_t		 rows;
	uint16_t		 cols;
	char			 title[128];
};

static struct pconn pconns[PCONN_MAX];
static int pconn_count;

static const char *session_name;
static struct ipc_transport *client;	/* the netchan link to the client */
static int watch_fd = -1;
static volatile sig_atomic_t stop_flag;

/* ---- pconn management ---- */

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

static struct pconn *
pconn_add(uint32_t id, int fd, const char *title, uint16_t rows, uint16_t cols)
{
	struct pconn *pc;

	if (pconn_count >= PCONN_MAX)
		return NULL;
	pc = &pconns[pconn_count++];
	pc->fd = fd;
	pc->t = ipc_transport_unix_new(fd);
	pc->id = id;
	pc->rows = rows;
	pc->cols = cols;
	if (title) {
		strncpy(pc->title, title, sizeof(pc->title) - 1);
		pc->title[sizeof(pc->title) - 1] = '\0';
	} else {
		pc->title[0] = '\0';
	}
	return pc;
}

static void
pconn_remove(uint32_t id)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		if (pconns[i].id == id) {
			ipc_transport_free(pconns[i].t);	/* closes fd */
			pconns[i].t = NULL;
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
		return 0;

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

/* ---- window-list payload encoding (matches the SSH proxy) ---- */

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

static void
notify_win_removed(uint32_t wid)
{
	uint32_t wid_be = BE32(wid);

	proxy_msg_xsend(client, 0, IPC_MSG_PROXY_WIN_REMOVED, &wid_be, 4);
}

/* ---- endpoint publishing ---- */

static int
udp_bind(const char *bindaddr, int port, uint16_t *bound_port)
{
	struct sockaddr_in sin;
	socklen_t slen;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (!bindaddr || inet_pton(AF_INET, bindaddr, &sin.sin_addr) != 1)
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		close(fd);
		return -1;
	}
	slen = sizeof(sin);
	if (getsockname(fd, (struct sockaddr *)&sin, &slen) != 0) {
		close(fd);
		return -1;
	}
	*bound_port = ntohs(sin.sin_port);
	return fd;
}

/* ---- pre-shared key ---- */

/* draw NET_PSK_LEN random bytes from the OS. returns 0 on success. */
static int
gen_psk(uint8_t *psk)
{
	size_t off = 0;
	int fd;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	while (off < NET_PSK_LEN) {
		ssize_t r = read(fd, psk + off, NET_PSK_LEN - off);

		if (r <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)r;
	}
	close(fd);
	return 0;
}

/* render the PSK as a NUL-terminated lowercase hex string (>= 65 bytes). */
static void
psk_to_hex(const uint8_t *psk, char *hex)
{
	int i;

	for (i = 0; i < NET_PSK_LEN; i++)
		snprintf(hex + i * 2, 3, "%02x", psk[i]);
}

/* publish the PSK as hex in the session dir, readable only by the owner,
 * so lumi attach on this host can pick it up.  returns 0 on success. */
static int
write_net_key(const uint8_t *psk)
{
	char hex[NET_PSK_LEN * 2 + 1];
	char *dir, path[256];
	int fd;

	psk_to_hex(psk, hex);

	dir = sessdir_session_path(session_name);
	if (!dir)
		return -1;
	if (snprintf(path, sizeof(path), "%s/net-key", dir) >=
	    (int)sizeof(path)) {
		free(dir);
		return -1;
	}
	free(dir);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, hex, sizeof(hex) - 1) != (ssize_t)(sizeof(hex) - 1)) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);
	return 0;
}

/* Daemon bootstrap for the cross-host ssh path: print the endpoint and key
 * as one line "udp <port> <hexkey>" on stdout for the attaching client to
 * read over ssh, then detach so the ssh command returns while this proxy
 * keeps serving.  The child becomes a session leader with its standard
 * descriptors pointed at /dev/null.  Returns 0 in the surviving child, and
 * does not return in the parent (it _exit()s once the line is written).
 * Returns -1 on fork failure. */
static int
daemonize_and_report(uint16_t bound, const uint8_t *psk)
{
	char hex[NET_PSK_LEN * 2 + 1];
	pid_t pid;
	int nfd;

	psk_to_hex(psk, hex);
	printf("udp %u %s\n", (unsigned)bound, hex);
	fflush(stdout);

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);		/* parent: let the ssh command return */

	setsid();
	nfd = open("/dev/null", O_RDWR);
	if (nfd >= 0) {
		dup2(nfd, STDIN_FILENO);
		dup2(nfd, STDOUT_FILENO);
		dup2(nfd, STDERR_FILENO);
		if (nfd > STDERR_FILENO)
			close(nfd);
	}
	return 0;
}

/* ---- signal handling ---- */

static void
on_signal(int signo)
{
	(void)signo;
	stop_flag = 1;
}

/* ---- client -> mserver ---- */

static int
drain_client(void)
{
	static uint8_t buf[IPC_MAX_PAYLOAD];
	uint32_t type, len, wid;
	struct pconn *pc;

	for (;;) {
		int r = ipc_transport_netchan_try_recv(client, &type, buf,
		    sizeof(buf), &len);

		if (r == 1)
			return 0;	/* nothing more ready */
		if (r < 0)
			return -1;	/* client gone */
		if (proxy_msg_xdecode(&wid, buf, &len) != 0)
			continue;
		if (wid == 0)
			continue;	/* no client->proxy control today */
		pc = pconn_find_by_id(wid);
		if (pc)
			ipc_transport_send(pc->t, type, buf, len);
	}
}

/* ---- mserver -> client ---- */

static void
drain_mserver(struct pconn *pc)
{
	static uint8_t buf[IPC_MAX_PAYLOAD];
	uint32_t type, len;

	if (ipc_transport_recv(pc->t, &type, buf, sizeof(buf), &len) != 0) {
		uint32_t wid = pc->id;

		pconn_remove(wid);
		notify_win_removed(wid);
		return;
	}
	proxy_msg_xsend(client, pc->id, type, buf, len);
}

/* ---- sessdir watch: add/remove mservers ---- */

static void
reconcile_mservers(void)
{
	pid_t pids[PCONN_MAX];
	int n, i;

	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, PCONN_MAX);
	if (n < 0)
		return;

	for (i = 0; i < n; i++) {
		struct pconn *pc;

		if (pconn_find_by_id((uint32_t)pids[i]))
			continue;
		if (attach_mserver(pids[i]) != 0)
			continue;
		pc = pconn_find_by_id((uint32_t)pids[i]);
		if (pc) {
			uint8_t pl[256];
			int plen = encode_win_added(pc, pl, sizeof(pl));

			if (plen > 0)
				proxy_msg_xsend(client, 0,
				    IPC_MSG_PROXY_WIN_ADDED, pl,
				    (uint32_t)plen);
		}
	}

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

			pconn_remove(wid);
			notify_win_removed(wid);
		}
	}
}

/* ---- main bridge loop ---- */

static int
bridge_loop(void)
{
	struct pollfd fds[PCONN_MAX + 2];

	while (!stop_flag) {
		int nfds = 0, client_slot, watch_slot = -1, i, to;

		to = ipc_transport_netchan_timeout(client);
		if (to < 0 || to > PROXY_TICK_MS)
			to = PROXY_TICK_MS;

		client_slot = nfds;
		fds[nfds].fd = ipc_transport_get_fd(client);
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		nfds++;

		for (i = 0; i < pconn_count; i++) {
			fds[nfds].fd = pconns[i].fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
		if (watch_fd >= 0) {
			watch_slot = nfds;
			fds[nfds].fd = watch_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		if (poll(fds, (nfds_t)nfds, to) < 0) {
			if (stop_flag)
				break;
			continue;
		}

		/* always service netchan (a timer may be due). */
		if (ipc_transport_netchan_pump(client) != 0)
			return 0;	/* client link ended */
		if (drain_client() != 0)
			return 0;

		/* mserver fds: match by fd since pconns may shift. */
		for (i = 0; i < pconn_count; ) {
			int slot = client_slot + 1 + i;
			struct pconn *pc = &pconns[i];
			int before = pconn_count;

			if (slot < nfds && fds[slot].fd == pc->fd &&
			    (fds[slot].revents & (POLLIN | POLLHUP | POLLERR)))
				drain_mserver(pc);
			/* drain_mserver may have removed pc (swap-with-last);
			 * only advance when the table did not shrink. */
			if (pconn_count == before)
				i++;
		}

		if (watch_slot >= 0 && (fds[watch_slot].revents & POLLIN)) {
			int flags = sessdir_watch_read(watch_fd);

			if (flags & SESSDIR_WATCH_CHANGED)
				reconcile_mservers();
		}
	}
	return 0;
}

/* ---- lifecycle ---- */

static void
cleanup(void)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		ipc_transport_send_empty(pconns[i].t, IPC_MSG_DETACH);
		ipc_transport_free(pconns[i].t);
		pconns[i].t = NULL;
	}
	pconn_count = 0;

	if (watch_fd >= 0) {
		sessdir_watch_stop(watch_fd);
		watch_fd = -1;
	}
	if (session_name) {
		char *dir = sessdir_session_path(session_name);

		/* best-effort removal of the published endpoint and key. */
		if (dir) {
			char path[256];

			if (snprintf(path, sizeof(path), "%s/net-addr", dir) <
			    (int)sizeof(path))
				unlink(path);
			if (snprintf(path, sizeof(path), "%s/net-key", dir) <
			    (int)sizeof(path))
				unlink(path);
			free(dir);
		}
	}
}

static int
net_proxy_run(const char *bindaddr, int port, int daemonize)
{
	uint8_t ready_buf[4096];
	uint8_t psk[NET_PSK_LEN];
	uint16_t bound = 0;
	struct pollfd wait_fd;
	char addrbuf[64];
	pid_t pids[PCONN_MAX];
	int udp_fd, ready_len, n, i;

	udp_fd = udp_bind(bindaddr, port, &bound);
	if (udp_fd < 0) {
		log_err("failed to bind UDP endpoint");
		return 1;
	}

	/* mint the session's pre-shared key and publish it before the
	 * endpoint, so a client that sees net-addr can always find net-key. */
	if (gen_psk(psk) != 0 || write_net_key(psk) != 0) {
		log_err("failed to publish net-key for session '%s'",
		    session_name);
		close(udp_fd);
		return 1;
	}

	/* publish the endpoint for lumi attach. */
	snprintf(addrbuf, sizeof(addrbuf), "udp %u", (unsigned)bound);
	if (sessdir_write_session_file(session_name, "net-addr", addrbuf) != 0) {
		log_err("failed to publish net-addr for session '%s'",
		    session_name);
		close(udp_fd);
		return 1;
	}
	log_info("lumi-net-proxy listening on udp/%u for session '%s'",
	    (unsigned)bound, session_name);

	/* cross-host bootstrap: report the endpoint and key, then detach. */
	if (daemonize && daemonize_and_report(bound, psk) != 0) {
		log_err("failed to daemonize");
		close(udp_fd);
		cleanup();
		return 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	/* wait for the first client datagram, then run the handshake. */
	wait_fd.fd = udp_fd;
	wait_fd.events = POLLIN;
	for (;;) {
		int pr = poll(&wait_fd, 1, PROXY_TICK_MS);

		if (stop_flag) {
			close(udp_fd);
			cleanup();
			return 0;
		}
		if (pr < 0)
			continue;
		if (pr > 0)
			break;
	}

	client = ipc_transport_netchan_new_crypto(udp_fd, 1, NULL, psk);
	if (!client) {
		close(udp_fd);
		cleanup();
		return 1;
	}
	if (ipc_transport_netchan_establish(client, 5000) != 0) {
		log_err("client handshake failed");
		ipc_transport_free(client);
		client = NULL;
		cleanup();
		return 1;
	}

	/* discover and attach to all mservers. */
	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, PCONN_MAX);
	if (n < 0) {
		log_err("failed to list servers for session '%s'",
		    session_name);
		ipc_transport_free(client);
		client = NULL;
		cleanup();
		return 1;
	}
	for (i = 0; i < n; i++)
		attach_mserver(pids[i]);

	/* send the initial window list. */
	ready_len = encode_ready_payload(ready_buf, sizeof(ready_buf));
	if (ready_len < 0 ||
	    proxy_msg_xsend(client, 0, IPC_MSG_PROXY_READY, ready_buf,
	    (uint32_t)ready_len) != 0) {
		log_err("failed to send ready message");
		ipc_transport_free(client);
		client = NULL;
		cleanup();
		return 1;
	}

	watch_fd = sessdir_watch_start(session_name);

	bridge_loop();

	ipc_transport_free(client);	/* closes udp_fd */
	client = NULL;
	cleanup();
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi net-proxy -s <session> [-b addr] [-p port] [-d]\n");
	exit(1);
}

int
cmd_net_proxy_main(int argc, char **argv)
{
	const char *bindaddr = NULL;
	int port = 0, daemonize = 0, opt;

	while ((opt = getopt(argc, argv, "s:b:p:d")) != -1) {
		switch (opt) {
		case 's':
			session_name = optarg;
			break;
		case 'b':
			bindaddr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'd':
			daemonize = 1;
			break;
		default:
			usage();
		}
	}

	if (!session_name)
		usage();

	return net_proxy_run(bindaddr, port, daemonize);
}
