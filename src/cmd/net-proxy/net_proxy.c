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
#include "keystore.h"
#include "nc_crypto.h"
#include "nc_auth.h"
#include "lumi_msg.h"
#include "proxy_msg.h"
#include "sessdir.h"
#include "sessdir_access.h"
#include "sessdir_control.h"
#include "sessdir_watch.h"
#include "nc_udp.h"
#include "log.h"
#include "byte_order.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NET_PSK_LEN	32
#define NET_KEY_LEN	32

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

/* what attach_mserver() asks each window for: unrestricted by default (the
 * ssh-bootstrap and PSK-only daemon modes, both reachable only by the
 * session owner's own uid over a channel they already had to have
 * pre-arranged access for), overridden by serve_one_client() to the ACL's
 * granted ceiling once a -L listener's userauth names who connected. */
static uint8_t proxy_attach_flags;
static uint32_t proxy_client_id;
static char proxy_client_name[64] = "net-proxy";

/* 12-d-b: the identity to re-check on every sessdir watch wakeup, so an
 * ACL edit reaches an already-connected client rather than only the next
 * one. Set once in serve_one_client() alongside the initial admission
 * decision; left inactive (acl_active == 0) for the ssh-bootstrap and PSK
 * daemon paths, which have no identity to check in the first place. */
static struct sessdir_access_who acl_who;
static int acl_active;
static int acl_last_granted;

/* 12-d-c: an "ask" match is admitted rather than denied outright, but
 * sees nothing until approved. No one able to approve, or no answer
 * within this long, is still a denial -- fail-closed rule 3 stays the
 * default, "ask" just gets a real chance to be answered first. */
#define PENDING_TIMEOUT_S	60
static int pending_state;	/* nonzero: waiting, not yet attached */
static time_t pending_deadline;
static unsigned long pending_last_seq;

static void detach_pconns(void);

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
	struct ipc_attach_reply rep;
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

	/* a real IpcAttach rather than the legacy empty form, so
	 * proxy_attach_flags (IPC_ATTACH_F_VIEW when serve_one_client()
	 * clamped a -L listener's client to the ACL's view ceiling) reaches
	 * the mserver the same way a direct -v attach would. */
	{
		struct ipc_attach at;
		uint8_t abuf[128];
		int an;

		at.rows = rows;
		at.cols = cols;
		at.flags = proxy_attach_flags;
		at.client_id = proxy_client_id;
		at.name = proxy_client_name;
		at.name_len = (uint16_t)strlen(proxy_client_name);
		an = ipc_attach_encode(&at, abuf, sizeof(abuf));
		if (an < 0 ||
		    ipc_msg_send(fd, IPC_MSG_ATTACH, abuf, (uint32_t)an) < 0) {
			ipc_close(fd);
			return -1;
		}
	}
	if (ipc_msg_recv(fd, &rtype, rbuf, sizeof(rbuf), &rlen) == 0 &&
	    rtype == IPC_MSG_ATTACH_REPLY) {
		if (ipc_attach_reply_decode(&rep, (const uint8_t *)rbuf,
		    (int)rlen) >= 0 && rep.rows > 0 && rep.cols > 0) {
			rows = rep.rows;
			cols = rep.cols;
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
	/* SO_REUSEADDR lets a restarted proxy rebind without waiting out a
	 * TIME_WAIT-like state; SO_REUSEPORT lets the direct-connect listener
	 * (-L) keep its socket bound to <bindaddr, port> for the life of the
	 * process while each forked child binds its own socket to that same
	 * <bindaddr, port> and connect()s it to one client, so the kernel
	 * routes that client's later datagrams to the child instead of the
	 * listener. This "more specific socket wins" routing is exercised and
	 * confirmed under test on Linux; other SO_REUSEPORT-supporting POSIX
	 * systems (this project also targets macOS/BSD -- see ipc_peer_cred())
	 * are expected to behave the same way but have not been tested here. */
	{
		int one = 1;

		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof(one));
#ifdef SO_REUSEPORT
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one,
		    sizeof(one));
#endif
	}
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

/* Build ~/.config/lumi/<name> (honoring XDG_CONFIG_HOME) into buf, creating
 * the directory if needed.  Returns 0 on success, -1 on no home dir,
 * truncation, or a directory that cannot be created. */
static int
lumi_config_path(char *buf, size_t sz, const char *name)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char dir[256];
	int n;

	if (xdg && *xdg)
		n = snprintf(dir, sizeof(dir), "%s/lumi", xdg);
	else if (home && *home)
		n = snprintf(dir, sizeof(dir), "%s/.config/lumi", home);
	else
		return -1;
	if (n <= 0 || (size_t)n >= sizeof(dir))
		return -1;

	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -1;

	n = snprintf(buf, sz, "%s/%s", dir, name);
	return (n > 0 && (size_t)n < sz) ? 0 : -1;
}

/* Load (or generate on first use) the server's long-term X25519 identity
 * secret from ~/.config/lumi/host_key, and derive its public half.  Returns
 * 0 on success, -1 on error. */
static int
load_host_key(uint8_t sk[NET_KEY_LEN], uint8_t pk[NET_KEY_LEN])
{
	char path[256];

	if (lumi_config_path(path, sizeof(path), "host_key") != 0)
		return -1;
	if (ks_host_key(path, sk) != 0)
		return -1;
	nc_crypto_identity_public(pk, sk);
	return 0;
}

/* ---- direct-connect user authentication (nc_auth server side) ---- */

static char authkeys_path[256];
static char passwd_path[256];
static int have_authkeys;		/* ~/.config/lumi/authorized_keys exists */
static int have_passwd;			/* ~/.config/lumi/passwd exists */

/* Resolve the server credential stores.  Returns 0 if at least one usable
 * store is present, -1 if the paths cannot be built or none exists. */
static int
setup_authstore(void)
{
	if (lumi_config_path(authkeys_path, sizeof(authkeys_path),
	    "authorized_keys") != 0 ||
	    lumi_config_path(passwd_path, sizeof(passwd_path), "passwd") != 0)
		return -1;
	have_authkeys = (access(authkeys_path, R_OK) == 0);
	have_passwd = (access(passwd_path, R_OK) == 0);
	return (have_authkeys || have_passwd) ? 0 : -1;
}

/* Offer the same methods for every name, so the handshake cannot be used to
 * probe which accounts exist. */
static unsigned
ua_methods(void *ctx, const char *user)
{
	unsigned m = 0;

	(void)ctx;
	(void)user;
	if (have_authkeys)
		m |= NC_AUTH_M_PUBKEY;
	if (have_passwd)
		m |= NC_AUTH_M_PASSWORD;
	return m;
}

static int
ua_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
	(void)ctx;
	return have_authkeys && ks_authorized_key(authkeys_path, user, pk);
}

static int
ua_check_password(void *ctx, const char *user, const char *password)
{
	(void)ctx;
	return have_passwd && ks_check_password(passwd_path, user, password);
}

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
/* publish the host public key as hex in the session dir (mode 0600) so a
 * local `attach -n -V` can pin it without an ssh bootstrap.  returns 0 on
 * success. */
static int
write_net_hostkey(const uint8_t *pk)
{
	char hex[NET_KEY_LEN * 2 + 1];
	char *dir, path[256];
	int fd;

	ks_hex_encode(hex, pk, NET_KEY_LEN);

	dir = sessdir_session_path(session_name);
	if (!dir)
		return -1;
	if (snprintf(path, sizeof(path), "%s/net-hostkey", dir) >=
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

static int
daemonize_and_report(uint16_t bound, const uint8_t *psk, const uint8_t *hostpk)
{
	char hex[NET_PSK_LEN * 2 + 1];
	char hkhex[NET_KEY_LEN * 2 + 1];
	pid_t pid;
	int nfd;

	psk_to_hex(psk, hex);
	if (hostpk) {
		ks_hex_encode(hkhex, hostpk, NET_KEY_LEN);
		printf("udp %u %s %s\n", (unsigned)bound, hex, hkhex);
	} else {
		printf("udp %u %s\n", (unsigned)bound, hex);
	}
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

/* Ask every currently-attached mserver window to re-clamp this connection's
 * role. mserver's own IPC_MSG_ROLE_REQUEST handler answers with
 * IPC_MSG_ROLE_CHANGE and enforces it from then on, so nothing further
 * needs to happen here for a client already relying on that enforcement
 * for INPUT et al. */
static void
broadcast_role_request(uint8_t flags)
{
	int i;

	for (i = 0; i < pconn_count; i++)
		ipc_transport_send(pconns[i].t, IPC_MSG_ROLE_REQUEST, &flags,
		    1);
}

/* 12-d-b: an ACL change re-evaluates a live connection, not just new ones.
 * Called on every sessdir watch wakeup (so on any session directory
 * change, not just an edit to "access" -- there is no finer-grained watch
 * today), but only acts when the granted role actually differs from the
 * last decision, so an unrelated write elsewhere in the directory (a
 * window switching focus, another client's own roster update) does not
 * spam the audit log or re-request a role that never changed. */
static void
recheck_acl(void)
{
	struct sessdir_access_decision dec;
	int granted;

	if (!acl_active)
		return;

	sessdir_access_check(session_name, getuid(), &acl_who, &dec);
	/* dec.ask only matters at initial admission (serve_one_client()):
	 * it decides whether a brand new connection is admitted pending or
	 * denied outright. This connection already got past that, whether
	 * by an outright grant or by 12-d-c's approval flow, and the ACL
	 * rule that produced it is not expected to change out from under
	 * it just because it still carries "ask" -- that would revoke every
	 * pending admission the instant it was approved, since approval is
	 * a one-time control message, not an edit to the rule itself. Use
	 * the role ceiling directly; an owner who wants to revoke a live
	 * connection changes the rule to an outright deny, which this still
	 * catches below exactly as before. */
	granted = dec.role;
	if (granted == acl_last_granted)
		return;

	sessdir_access_audit(session_name, &acl_who, &dec, dec.role, granted);
	acl_last_granted = granted;

	if (granted == SESSDIR_ACCESS_DENY) {
		log_err("net-proxy: ACL change revoked key '%s' (%s)",
		    acl_who.key, dec.subject);
		stop_flag = 1;
		return;
	}

	proxy_attach_flags = (granted == SESSDIR_ACCESS_VIEW) ?
	    IPC_ATTACH_F_VIEW : 0;
	broadcast_role_request(proxy_attach_flags);
}

/* Discover every mserver in the session, attach to each (replaying its
 * screen into this connection's own pconn), and tell the far client the
 * resulting window list. Called once immediately for an ordinary
 * admission, or once on approval for a client that was pending -- never
 * before either point, since attaching is exactly the step that starts
 * sending session content. Returns 0 on success, -1 on failure (the
 * caller tears the connection down either way). */
static int
attach_all_and_announce(void)
{
	uint8_t ready_buf[4096];
	pid_t pids[PCONN_MAX];
	int ready_len, n, i;

	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, PCONN_MAX);
	if (n < 0) {
		log_err("failed to list servers for session '%s'",
		    session_name);
		return -1;
	}
	for (i = 0; i < n; i++)
		attach_mserver(pids[i]);

	ready_len = encode_ready_payload(ready_buf, sizeof(ready_buf));
	if (ready_len < 0 ||
	    proxy_msg_xsend(client, 0, IPC_MSG_PROXY_READY, ready_buf,
	    (uint32_t)ready_len) != 0) {
		log_err("failed to send ready message");
		detach_pconns();
		return -1;
	}
	return 0;
}

/* Is anyone attached who could actually answer an "ask" admission --
 * the write-token holder, or (per FUTURE.md 12C) any owner-uid client if
 * there is no writer? A local, tier-0 attach.c client always registers
 * with an empty `who` (12-d-a); that is the only signal this broker has
 * for "an owner client is here," short of re-deriving uids the roster
 * does not otherwise need to carry. */
static int
approver_available(void)
{
	struct sessdir_client list[SESSDIR_CLIENT_MAX];
	char holder[64];
	int n, i;

	if (sessdir_token_holder(session_name, holder, sizeof(holder), NULL) == 0)
		return 1;

	n = sessdir_client_list(session_name, list, SESSDIR_CLIENT_MAX);
	for (i = 0; i < n; i++) {
		if (list[i].who[0] == '\0')
			return 1;
	}
	return 0;
}

/* Resolve a pending admission: timeout, an approve/reject verb addressed
 * to this connection's client_id, or nothing yet. Called every bridge
 * loop tick regardless of watch activity, since a timeout must fire even
 * when the session directory never changes again. */
static void
pending_tick(void)
{
	struct sessdir_ctl ctl;

	if (!pending_state)
		return;

	if (time(NULL) >= pending_deadline) {
		log_err("net-proxy: pending admission for '%s' timed out",
		    acl_who.key);
		pending_state = 0;
		stop_flag = 1;
		return;
	}

	if (sessdir_ctl_read(session_name, &ctl) != 0 ||
	    ctl.seq <= pending_last_seq)
		return;
	pending_last_seq = ctl.seq;
	if (ctl.target != proxy_client_id)
		return;

	if (ctl.verb == SESSDIR_CTL_APPROVE) {
		struct sessdir_client sc;

		pending_state = 0;
		memset(&sc, 0, sizeof(sc));
		sc.client_id = proxy_client_id;
		sc.pid = getpid();
		snprintf(sc.name, sizeof(sc.name), "%s", proxy_client_name);
		snprintf(sc.who, sizeof(sc.who), "%.*s",
		    (int)sizeof(sc.who) - 1, acl_who.key);
		snprintf(sc.role, sizeof(sc.role), "%s",
		    proxy_attach_flags & IPC_ATTACH_F_VIEW ? "view" : "write");
		snprintf(sc.mode, sizeof(sc.mode), "-");
		snprintf(sc.coupling, sizeof(sc.coupling), "-");
		sc.since = (long)time(NULL);
		sessdir_client_register(session_name, &sc);

		if (attach_all_and_announce() != 0)
			stop_flag = 1;
	} else if (ctl.verb == SESSDIR_CTL_REJECT) {
		log_err("net-proxy: pending admission for '%s' rejected",
		    acl_who.key);
		pending_state = 0;
		stop_flag = 1;
	}
}

static void
reconcile_mservers(void)
{
	pid_t pids[PCONN_MAX];
	int n, i;

	if (pending_state)
		return;	/* nothing attached yet; nothing to reconcile */

	recheck_acl();
	if (stop_flag)
		return;

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

		/* checked every tick, not only on watch wakeup, so a pending
		 * admission's timeout fires even if the session directory
		 * never changes again. */
		pending_tick();
		if (stop_flag)
			break;

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

/* Tear down everything tied to one served client: detach from the session's
 * mservers and stop the sessdir watch.  Safe to call when nothing is attached.
 * The session itself and any published files are left alone, so a persistent
 * listener can serve the next client. */
static void
detach_pconns(void)
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
}

static void
cleanup(void)
{
	detach_pconns();

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

/* Serve exactly one client on udp_fd (ownership transferred; freed with the
 * transport): wait for its first datagram, run the crypto handshake and, when
 * `auth` requests it, the user-authentication phase, bridge every window until
 * the client disconnects, then detach from the session.  Returns 0 when a
 * client was served and left, -1 on a setup or handshake failure. */
static int
serve_one_client(int udp_fd, const struct ipc_netchan_auth *auth)
{
	struct pollfd wait_fd;

	wait_fd.fd = udp_fd;
	wait_fd.events = POLLIN;
	for (;;) {
		int pr = poll(&wait_fd, 1, PROXY_TICK_MS);

		if (stop_flag) {
			close(udp_fd);
			return 0;
		}
		if (pr < 0)
			continue;
		if (pr > 0)
			break;
	}

	client = ipc_transport_netchan_new_crypto_auth(udp_fd, 1, NULL, auth);
	if (!client) {
		close(udp_fd);
		return -1;
	}
	if (ipc_transport_netchan_establish(client, 5000) != 0) {
		log_err("client handshake or authentication failed");
		ipc_transport_free(client);
		client = NULL;
		return -1;
	}

	/* A -L listener names who connected via its userauth phase; check
	 * that identity against the session's ACL and clamp what
	 * attach_mserver() will ask each window for accordingly. NULL here
	 * means no userauth ran at all (the ssh-bootstrap and PSK daemon
	 * modes), reachable only by the session owner's own uid over a
	 * channel that already needed pre-arranged access, so it keeps the
	 * unrestricted default rather than being denied for lacking an
	 * identity to check. */
	proxy_client_id = (uint32_t)getpid();
	pending_state = 0;
	{
		const char *authuser = ipc_transport_netchan_auth_user(client);
		const char *role_str = "write";
		int pending = 0;

		if (authuser) {
			struct sessdir_access_who who;
			struct sessdir_access_decision dec;
			int granted;

			memset(&who, 0, sizeof(who));
			who.is_key = 1;
			snprintf(who.key, sizeof(who.key), "%s", authuser);

			sessdir_access_check(session_name, getuid(), &who,
			    &dec);
			granted = dec.role;
			if (dec.ask) {
				/* Admit as pending rather than deny outright,
				 * but only if someone could plausibly answer;
				 * fail-closed rule 3 otherwise ("no one
				 * available to answer ... means deny"), with
				 * no wasted timeout. */
				if (approver_available()) {
					pending = 1;
					granted = dec.role;
				} else {
					granted = SESSDIR_ACCESS_DENY;
				}
			}
			sessdir_access_audit(session_name, &who, &dec,
			    dec.role, granted);

			if (!pending && granted == SESSDIR_ACCESS_DENY) {
				log_err("net-proxy: denied key '%s' (%s)",
				    authuser, dec.subject);
				ipc_transport_free(client);
				client = NULL;
				return -1;
			}

			proxy_attach_flags = (granted == SESSDIR_ACCESS_VIEW) ?
			    IPC_ATTACH_F_VIEW : 0;
			snprintf(proxy_client_name, sizeof(proxy_client_name),
			    "%s", authuser);
			role_str = granted == SESSDIR_ACCESS_VIEW ?
			    "view" : "write";

			acl_who = who;
			acl_active = 1;
			acl_last_granted = granted;
		}

		/* attach.c never sees this connection to register it itself
		 * (it has no local session directory to write into), so the
		 * broker does it on the client's behalf, the same way it
		 * already stands in for this client's IpcAttach to each
		 * mserver. */
		{
			struct sessdir_client sc;

			memset(&sc, 0, sizeof(sc));
			sc.client_id = proxy_client_id;
			sc.pid = getpid();
			snprintf(sc.name, sizeof(sc.name), "%s",
			    proxy_client_name);
			snprintf(sc.who, sizeof(sc.who), "%s",
			    authuser ? authuser : "");
			snprintf(sc.role, sizeof(sc.role), "%s",
			    pending ? "ask" : role_str);
			snprintf(sc.mode, sizeof(sc.mode), "-");
			snprintf(sc.coupling, sizeof(sc.coupling), "-");
			sc.since = (long)time(NULL);
			sc.pending = pending;
			sessdir_client_register(session_name, &sc);
		}

		if (pending) {
			pending_state = 1;
			pending_deadline = time(NULL) + PENDING_TIMEOUT_S;
			pending_last_seq = 0;
			{
				struct sessdir_ctl ctl;

				/* baseline against any backlog: only a verb
				 * posted after this point resolves us. */
				if (sessdir_ctl_read(session_name, &ctl) == 0)
					pending_last_seq = ctl.seq;
			}
		}
	}

	/* discover and attach to all mservers -- unless still pending, in
	 * which case nothing happens until pending_tick() resolves it from
	 * inside bridge_loop(). */
	if (!pending_state && attach_all_and_announce() != 0) {
		sessdir_client_unregister(session_name, proxy_client_id);
		ipc_transport_free(client);
		client = NULL;
		return -1;
	}

	watch_fd = sessdir_watch_start(session_name);
	bridge_loop();

	sessdir_client_unregister(session_name, proxy_client_id);
	detach_pconns();
	ipc_transport_free(client);	/* closes udp_fd */
	client = NULL;
	return 0;
}

/* How many clients net_proxy_listen() will bridge at once. Each is a
 * separate forked process, so this bounds worst-case fork/memory load
 * rather than any protocol limit. */
#define NET_PROXY_MAX_CHILDREN 16

static pid_t listen_children[NET_PROXY_MAX_CHILDREN];
static int listen_child_count;

static void
listen_child_add(pid_t pid)
{
	if (listen_child_count < NET_PROXY_MAX_CHILDREN)
		listen_children[listen_child_count++] = pid;
}

static void
listen_child_remove(pid_t pid)
{
	int i;

	for (i = 0; i < listen_child_count; i++) {
		if (listen_children[i] == pid) {
			listen_children[i] =
			    listen_children[--listen_child_count];
			return;
		}
	}
}

/* Reap every child that has already exited, without blocking. */
static void
listen_reap_children(void)
{
	pid_t pid;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
		listen_child_remove(pid);
}

/*
 * Direct-connect deployment: listen persistently on a fixed port and bridge
 * up to NET_PROXY_MAX_CHILDREN clients at once, each authenticating itself
 * over netchan with no ssh bootstrap. The connection is encrypted and the
 * server is pinned by its host key; the user proves identity with a key or
 * password from ~/.config/lumi. There is no PSK: the endpoint is public and
 * every client authenticates.
 *
 * The listening socket stays bound for the process lifetime (SO_REUSEPORT).
 * A new client's first datagram is only ever peeked (MSG_PEEK) on it, never
 * consumed there: the parent forks a child, the child binds its own socket
 * to the same <bindaddr, port> and connect()s it to that one peer, and only
 * then does the parent drain the peeked datagram from the listening socket.
 * From that point on the kernel routes that peer's packets to the child's
 * more specific connected socket instead of the listener, so the child never
 * actually reads the datagram the parent peeked -- it relies on the netchan
 * handshake's own retry (nct_send_hello() repeats until answered) to see the
 * client's next attempt, now on its own socket. This keeps the fork/accept
 * logic here self-contained, with no changes to the netchan transport.
 *
 * Known race: if that same peer retransmits its HELLO again before the new
 * child's connect() completes, the retransmission queues on the listener
 * like a brand new client and this loop forks a second child for the same
 * peer. Both race to complete the handshake; the loser just times out
 * against a peer who has already moved on to the winner, bounded by
 * ipc_transport_netchan_establish()'s own timeout. Harmless but wasteful,
 * and not expected in practice given fork()+connect() is far faster than a
 * handshake retry interval.
 */
static int
net_proxy_listen(const char *bindaddr, int port)
{
	uint8_t host_sk[NET_KEY_LEN], host_pk[NET_KEY_LEN];
	char hex[NET_KEY_LEN * 2 + 1];
	uint16_t bound = 0;
	int listen_fd;

	if (port == 0) {
		log_err("listen mode (-L) requires a fixed port (-p)");
		return 1;
	}
	if (load_host_key(host_sk, host_pk) != 0) {
		log_err("failed to load host key");
		return 1;
	}
	if (setup_authstore() != 0) {
		log_err("no authorized_keys or passwd under ~/.config/lumi; "
		    "refusing to listen with no way to authenticate a user");
		return 1;
	}

	listen_fd = udp_bind(bindaddr, port, &bound);
	if (listen_fd < 0) {
		log_err("failed to bind udp/%d", port);
		return 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	ks_hex_encode(hex, host_pk, NET_KEY_LEN);
	log_info("lumi-net-proxy direct-connect on %s:%d for session '%s'",
	    bindaddr ? bindaddr : "*", port, session_name);
	log_info("host key: %s", hex);

	while (!stop_flag) {
		struct pollfd pfd;
		struct sockaddr_storage ss;
		socklen_t slen;
		uint8_t peek[64];
		pid_t pid;
		int pr;

		listen_reap_children();

		pfd.fd = listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		pr = poll(&pfd, 1, PROXY_TICK_MS);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;

		slen = sizeof(ss);
		if (recvfrom(listen_fd, peek, sizeof(peek), MSG_PEEK,
		    (struct sockaddr *)&ss, &slen) < 0)
			continue;

		if (listen_child_count >= NET_PROXY_MAX_CHILDREN) {
			/* no slot free: drain and drop. the client's own
			 * handshake retry gets it served once one frees up. */
			(void)recvfrom(listen_fd, peek, sizeof(peek), 0,
			    NULL, NULL);
			log_err("dropping a connection attempt: %d clients "
			    "already active", listen_child_count);
			continue;
		}

		pid = fork();
		if (pid < 0) {
			/* drain and drop, same as the cap-exceeded case above:
			 * without this a failing fork() re-peeks the same
			 * datagram every iteration with no backoff. */
			(void)recvfrom(listen_fd, peek, sizeof(peek), 0,
			    NULL, NULL);
			log_err("fork failed accepting a connection: %s",
			    strerror(errno));
			continue;
		}
		if (pid == 0) {
			struct ipc_netchan_userauth ua = {
				.methods = ua_methods,
				.check_key = ua_check_key,
				.check_password = ua_check_password,
			};
			struct ipc_netchan_auth auth = {
				.static_sk = host_sk,
				.userauth = &ua,
			};
			uint16_t child_bound = 0;
			int child_fd;

			close(listen_fd);

			child_fd = udp_bind(bindaddr, port, &child_bound);
			if (child_fd < 0 || connect(child_fd,
			    (struct sockaddr *)&ss, slen) != 0)
				_exit(1);

			/* serve_one_client owns and closes child_fd. */
			_exit(serve_one_client(child_fd, &auth) == 0 ? 0 : 1);
		}

		/* parent: this datagram's retransmission is now the child's
		 * to receive; drop our peeked copy so it is not repeatedly
		 * re-peeked into a fork storm for the same client. */
		(void)recvfrom(listen_fd, peek, sizeof(peek), 0, NULL, NULL);
		listen_child_add(pid);
	}

	close(listen_fd);

	/* stop accepting; ask every still-running child to finish up. */
	{
		int i;

		for (i = 0; i < listen_child_count; i++)
			kill(listen_children[i], SIGTERM);
		for (i = 0; i < listen_child_count; i++)
			waitpid(listen_children[i], NULL, 0);
	}
	return 0;
}

static int
net_proxy_run(const char *bindaddr, int port, int daemonize, int use_hostkey)
{
	uint8_t psk[NET_PSK_LEN];
	uint8_t host_sk[NET_KEY_LEN], host_pk[NET_KEY_LEN];
	uint16_t bound = 0;
	char addrbuf[64];
	int udp_fd;

	udp_fd = udp_bind(bindaddr, port, &bound);
	if (udp_fd < 0) {
		log_err("failed to bind UDP endpoint");
		return 1;
	}

	/* load our long-term identity key so a client can pin it across
	 * restarts, and publish its public half beside the endpoint. */
	if (use_hostkey && (load_host_key(host_sk, host_pk) != 0 ||
	    write_net_hostkey(host_pk) != 0)) {
		log_err("failed to load or publish host key");
		close(udp_fd);
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
	if (daemonize && daemonize_and_report(bound, psk,
	    use_hostkey ? host_pk : NULL) != 0) {
		log_err("failed to daemonize");
		close(udp_fd);
		cleanup();
		return 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	{
		struct ipc_netchan_auth auth = { .psk = psk };
		int rc;

		if (use_hostkey)
			auth.static_sk = host_sk;
		rc = serve_one_client(udp_fd, &auth);
		cleanup();
		return rc == 0 ? 0 : 1;
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi net-proxy -s <session> [-b addr] [-p port] [-d] "
	    "[-k]\n"
	    "       lumi net-proxy -s <session> -L -p <port> [-b addr]\n");
	exit(1);
}

int
cmd_net_proxy_main(int argc, char **argv)
{
	const char *bindaddr = NULL;
	int port = 0, daemonize = 0, use_hostkey = 0, listen_mode = 0, opt;

	while ((opt = getopt(argc, argv, "s:b:p:dkL")) != -1) {
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
		case 'L':
			listen_mode = 1;
			break;
		case 'k':
			use_hostkey = 1;
			break;
		default:
			usage();
		}
	}

	if (!session_name)
		usage();

	if (listen_mode)
		return net_proxy_listen(bindaddr, port);

	return net_proxy_run(bindaddr, port, daemonize, use_hostkey);
}
