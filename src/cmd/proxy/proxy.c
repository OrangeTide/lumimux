/* proxy.c : multiplexing proxy for remote session tunneling */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "ipc_transport.h"
#include "lumi_msg.h"
#include "proxy_broker.h"
#include "proxy_msg.h"
#include "runtime_dir.h"
#include "sessdir.h"
#include "sessdir_access.h"
#include "sessdir_control.h"
#include "sessdir_watch.h"
#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"
#include "log.h"
#include "byte_order.h"

#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PCONN_MAX 64

/* accept-loop poll granularity for the broker listener (12-b): how often
 * it re-checks broker_stop_flag between connections. */
#define PROXY_LISTEN_TICK_MS 200

struct pconn {
	int		fd;		/* raw fd, kept for the event loop */
	struct ipc_transport *t;	/* carries messages to/from the mserver */
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

/* what attach_mserver() asks each window for: unrestricted by default (the
 * ssh/stdio path, one trusted owner-uid client), overridden by
 * broker_serve_client() to the ACL's granted ceiling for a foreign-uid
 * connection. */
static uint8_t proxy_attach_flags;
static uint32_t proxy_client_id;
static char proxy_client_name[64] = "proxy";

/* 12-d-b: the identity to re-check on every sessdir watch wakeup, so an
 * ACL edit reaches an already-connected client rather than only the next
 * one. Set once in broker_serve_client() alongside the initial admission
 * decision; left inactive (acl_active == 0) for the ssh/stdio path, which
 * has no identity to check in the first place. */
static struct sessdir_access_who acl_who;
static int acl_active;
static int acl_last_granted;

/* 12-d-c: an "ask" match is admitted rather than denied outright, but
 * sees nothing until approved. No one able to approve, or no answer
 * within this long, is still a denial -- fail-closed rule 3 stays the
 * default, "ask" just gets a real chance to be answered first. */
#define PENDING_TIMEOUT_S	60
static int pending_approved;
static unsigned long pending_last_seq;

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

	/* a real IpcAttach rather than the legacy empty form, so the
	 * mserver can tell this connection apart from another and, when
	 * proxy_attach_flags carries IPC_ATTACH_F_VIEW (set by
	 * broker_serve_client() for a foreign-uid client the ACL ceilings
	 * at view), grant it view-only the same way it would a direct -v
	 * attach. */
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
	(void)fd;

	rc = ipc_transport_recv(pc->t, &type, buf, sizeof(buf), &len);
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

	ipc_transport_send(pc->t, type, buf, len);
}

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
 * last decision, so an unrelated write elsewhere in the directory does
 * not spam the audit log or re-request a role that never changed.
 *
 * Returns 1 when the connection was just revoked and torn down (the loop
 * has been told to stop, so the caller should do no further work with
 * pconns this tick), 0 otherwise. */
static int
recheck_acl(void)
{
	struct sessdir_access_decision dec;
	int granted;

	if (!acl_active)
		return 0;

	sessdir_access_check(session_name, getuid(), &acl_who, &dec);
	/* dec.ask only matters at initial admission (broker_serve_client()):
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
		return 0;

	sessdir_access_audit(session_name, &acl_who, &dec, dec.role, granted);
	acl_last_granted = granted;

	if (granted == SESSDIR_ACCESS_DENY) {
		log_err("broker: ACL change revoked uid %ld (%s)",
		    (long)acl_who.uid, dec.subject);
		iox_loop_stop(loop);
		return 1;
	}

	proxy_attach_flags = (granted == SESSDIR_ACCESS_VIEW) ?
	    IPC_ATTACH_F_VIEW : 0;
	broadcast_role_request(proxy_attach_flags);
	return 0;
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

	if (recheck_acl())
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

/* ---- cross-user broker (FUTURE.md 12-b) ----
 *
 * lumi share -u/-G starts this instead of the plain ssh/stdio path: a
 * listening Unix socket that a foreign uid can reach, one forked child per
 * accepted connection (mirroring lumi-net-proxy's -L listener from step
 * 11), each child running the exact same proxy engine as the ssh path with
 * the accepted socket standing in for stdin and stdout. The only thing
 * that differs per connection is what attach_mserver() asks each window
 * for: the ACL's granted ceiling for that peer's uid, established before
 * the proxy engine ever starts relaying a byte.
 */

static int proxy_run(void);

static volatile sig_atomic_t broker_stop_flag;

static void
on_broker_signal(int signo)
{
	(void)signo;
	broker_stop_flag = 1;
}

int
proxy_broker_dir(char *buf, size_t bufsz)
{
	int n = snprintf(buf, bufsz, "/tmp/lumi-broker-%d", (int)getuid());

	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

int
proxy_broker_sock_path(const char *session, char *buf, size_t bufsz)
{
	char dir[256];
	int n;

	if (proxy_broker_dir(dir, sizeof(dir)) < 0)
		return -1;
	n = snprintf(buf, bufsz, "%s/%s.sock", dir, session);
	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

int
proxy_broker_pidfile_path(const char *session, char *buf, size_t bufsz)
{
	char dir[256];
	int n;

	if (proxy_broker_dir(dir, sizeof(dir)) < 0)
		return -1;
	n = snprintf(buf, bufsz, "%s/%s.pid", dir, session);
	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

/* Is anyone attached who could actually answer an "ask" admission -- the
 * write-token holder, or (per FUTURE.md 12C) any owner-uid client if
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

static void
on_pending_timeout(struct iox_loop *lp, void *arg)
{
	(void)arg;
	log_err("broker: pending admission for uid %ld timed out",
	    (long)acl_who.uid);
	pending_approved = 0;
	iox_loop_stop(lp);
}

static void
on_pending_watch(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	int flags;
	struct sessdir_ctl ctl;

	(void)events;
	(void)arg;

	flags = sessdir_watch_read(fd);
	if (!(flags & SESSDIR_WATCH_CHANGED))
		return;
	if (sessdir_ctl_read(session_name, &ctl) != 0 ||
	    ctl.seq <= pending_last_seq)
		return;
	pending_last_seq = ctl.seq;
	if (ctl.target != proxy_client_id)
		return;

	if (ctl.verb == SESSDIR_CTL_APPROVE) {
		pending_approved = 1;
		iox_loop_stop(lp);
	} else if (ctl.verb == SESSDIR_CTL_REJECT) {
		log_err("broker: pending admission for uid %ld rejected",
		    (long)acl_who.uid);
		pending_approved = 0;
		iox_loop_stop(lp);
	}
}

/* Hold a broker connection open without attaching to anything, waiting
 * for approval (FUTURE.md 12-d-c). No replay, no output: STDIN stays
 * wired so the client's own disconnect is still noticed, but nothing
 * about the session reaches it until approved. Returns 1 if approved, 0
 * otherwise (rejected, timed out, or the client hung up first -- the
 * caller treats all three the same way, as a denial). */
static int
broker_wait_for_approval(void)
{
	struct sessdir_ctl ctl;

	pending_approved = 0;
	pending_last_seq = 0;
	/* baseline against any backlog: only a verb posted after this point
	 * resolves us. */
	if (sessdir_ctl_read(session_name, &ctl) == 0)
		pending_last_seq = ctl.seq;

	loop = iox_loop_new();
	if (!loop)
		return 0;

	iox_fd_add(loop, STDIN_FILENO, IOX_READ, on_client_read, NULL);

	watch_fd = sessdir_watch_start(session_name);
	if (watch_fd >= 0)
		iox_fd_add(loop, watch_fd, IOX_READ, on_pending_watch, NULL);

	iox_timer_add(loop, PENDING_TIMEOUT_S * 1000, on_pending_timeout, NULL);
	iox_signal_add(loop, SIGTERM, on_sigterm, NULL);

	iox_loop_run(loop);

	if (watch_fd >= 0) {
		sessdir_watch_stop(watch_fd);
		watch_fd = -1;
	}
	iox_loop_free(loop);
	loop = NULL;
	return pending_approved;
}

/* One accepted broker connection: check the peer's credentials against the
 * session's ACL, set what attach_mserver() will ask each window for, then
 * run the ordinary proxy engine with cfd standing in for stdin and stdout.
 * Always exits the process rather than returning -- there is nothing left
 * for a broker child to do once its one client is done. */
static void
broker_serve_client(int cfd)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;
	int granted;

	memset(&who, 0, sizeof(who));
#ifdef IPC_HAVE_PEER_CRED
	if (ipc_peer_cred(cfd, &who.uid, &who.gid, NULL) < 0) {
		log_err("broker: could not read peer credentials, refusing");
		_exit(1);
	}
#else
	/* fail-closed rule 1: no peer credential support on this platform
	 * means no cross-uid sharing at all. */
	log_err("broker: no peer credential support on this platform, "
	    "refusing cross-user connections");
	_exit(1);
#endif

	sessdir_access_check(session_name, getuid(), &who, &dec);
	granted = dec.role;
	{
		int pending = 0;

		if (dec.ask) {
			/* Admit as pending rather than deny outright, but
			 * only if someone could plausibly answer; fail-closed
			 * rule 3 otherwise ("no one available to answer ...
			 * means deny"), with no wasted timeout. */
			acl_who = who;
			if (approver_available())
				pending = 1;
			else
				granted = SESSDIR_ACCESS_DENY;
		}
		sessdir_access_audit(session_name, &who, &dec, dec.role,
		    granted);

		if (!pending && granted == SESSDIR_ACCESS_DENY) {
			log_err("broker: denied uid %ld (%s)", (long)who.uid,
			    dec.subject);
			_exit(0);
		}

		proxy_attach_flags = (granted == SESSDIR_ACCESS_VIEW) ?
		    IPC_ATTACH_F_VIEW : 0;
		acl_who = who;
		acl_active = 1;
		acl_last_granted = granted;
		/* this child's own pid, not who.uid: two connections from
		 * the same uid would otherwise send identical client_id and
		 * collide in the mserver's roster, exactly what the ssh path
		 * and net_proxy avoid by using getpid(). */
		proxy_client_id = (uint32_t)getpid();
		snprintf(proxy_client_name, sizeof(proxy_client_name),
		    "uid:%ld", (long)who.uid);

		if (dup2(cfd, STDIN_FILENO) < 0 || dup2(cfd, STDOUT_FILENO) < 0)
			_exit(1);
		if (cfd > STDOUT_FILENO)
			close(cfd);

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
			snprintf(sc.who, sizeof(sc.who), "uid:%ld",
			    (long)who.uid);
			snprintf(sc.role, sizeof(sc.role), "%s", pending ?
			    "ask" : (granted == SESSDIR_ACCESS_VIEW ?
			    "view" : "write"));
			snprintf(sc.mode, sizeof(sc.mode), "-");
			snprintf(sc.coupling, sizeof(sc.coupling), "-");
			sc.since = (long)time(NULL);
			sc.pending = pending;
			sessdir_client_register(session_name, &sc);
		}

		if (pending) {
			if (!broker_wait_for_approval()) {
				sessdir_client_unregister(session_name,
				    proxy_client_id);
				_exit(0);
			}

			/* approved: the ceiling asked for at admission time
			 * is what is granted now -- nothing renegotiates a
			 * higher role than the ACL rule that admitted this
			 * connection in the first place. */
			granted = dec.role;
			proxy_attach_flags = (granted == SESSDIR_ACCESS_VIEW) ?
			    IPC_ATTACH_F_VIEW : 0;
			acl_last_granted = granted;
			{
				struct sessdir_client sc;

				memset(&sc, 0, sizeof(sc));
				sc.client_id = proxy_client_id;
				sc.pid = getpid();
				snprintf(sc.name, sizeof(sc.name), "%s",
				    proxy_client_name);
				snprintf(sc.who, sizeof(sc.who), "uid:%ld",
				    (long)who.uid);
				snprintf(sc.role, sizeof(sc.role), "%s",
				    granted == SESSDIR_ACCESS_VIEW ?
				    "view" : "write");
				snprintf(sc.mode, sizeof(sc.mode), "-");
				snprintf(sc.coupling, sizeof(sc.coupling), "-");
				sc.since = (long)time(NULL);
				sessdir_client_register(session_name, &sc);
			}
		}
	}

	{
		int ret = proxy_run();

		sessdir_client_unregister(session_name, proxy_client_id);
		_exit(ret);
	}
}

static int
proxy_listen_run(const char *group)
{
	char dir[256], path[256], pidpath[256];
	mode_t dirmode;
	int listen_fd;

	if (proxy_broker_dir(dir, sizeof(dir)) < 0 ||
	    proxy_broker_sock_path(session_name, path, sizeof(path)) < 0 ||
	    proxy_broker_pidfile_path(session_name, pidpath,
	    sizeof(pidpath)) < 0) {
		log_err("broker path too long");
		return 1;
	}

	/* 12C's endpoint table: group mode is preferred when available since
	 * the kernel rejects a non-member before lumi code ever runs; world
	 * mode has no such gate and leans entirely on the ACL/peer-cred
	 * check in broker_serve_client(), so its directory carries the
	 * sticky bit even though nothing else writes there. */
	dirmode = group ? 0710 : (mode_t)(S_ISVTX | 0733);
	if (lu_runtime_dir_ensure_mode(dir, dirmode) < 0)
		return 1;

	ipc_unlink(path);		/* stale socket from a prior run */
	listen_fd = ipc_listen(path);
	if (listen_fd < 0) {
		log_err("failed to listen on %s", path);
		return 1;
	}

	if (group) {
		struct group *gr = getgrnam(group);

		if (!gr) {
			log_err("unknown group '%s'", group);
			ipc_close(listen_fd);
			ipc_unlink(path);
			return 1;
		}
		if (chmod(path, 0660) != 0 ||
		    chown(path, (uid_t)-1, gr->gr_gid) != 0) {
			log_err("failed to set broker socket permissions on "
			    "%s", path);
			ipc_close(listen_fd);
			ipc_unlink(path);
			return 1;
		}
	} else if (chmod(path, 0666) != 0) {
		log_err("failed to set broker socket permissions on %s", path);
		ipc_close(listen_fd);
		ipc_unlink(path);
		return 1;
	}

	signal(SIGTERM, on_broker_signal);
	signal(SIGINT, on_broker_signal);
	signal(SIGPIPE, SIG_IGN);
	/* auto-reap: each forked child is short-lived and self-contained
	 * once it starts (it owns its own mserver connections independent
	 * of this process), so there is nothing left to track once it
	 * exits. */
	signal(SIGCHLD, SIG_IGN);

	/* published last, once the socket is actually ready to accept: a
	 * caller polling for this pidfile (lumi share -u/-G) must never see
	 * it before the endpoint it names is live. */
	{
		FILE *f = fopen(pidpath, "w");

		if (f) {
			fprintf(f, "%ld\n", (long)getpid());
			fclose(f);
		}
	}

	log_info("lumi-proxy broker listening on %s for session '%s'", path,
	    session_name);

	while (!broker_stop_flag) {
		struct pollfd pfd;
		int cfd, pr;
		pid_t pid;

		/* poll with a timeout rather than blocking in accept():
		 * signal() installs SIGTERM/SIGINT with SA_RESTART on this
		 * platform, so a blocked accept() would just resume without
		 * ever re-checking broker_stop_flag if no further connection
		 * ever arrived. */
		pfd.fd = listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		pr = poll(&pfd, 1, PROXY_LISTEN_TICK_MS);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;

		cfd = ipc_accept(listen_fd);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		pid = fork();
		if (pid < 0) {
			ipc_close(cfd);
			continue;
		}
		if (pid == 0) {
			ipc_close(listen_fd);
			broker_serve_client(cfd);
			/* unreachable: broker_serve_client() always exits */
		}
		ipc_close(cfd);		/* parent's copy */
	}

	ipc_close(listen_fd);
	ipc_unlink(path);
	ipc_unlink(pidpath);
	return 0;
}

/* ---- main ---- */

static void
cleanup(void)
{
	int i;

	for (i = 0; i < pconn_count; i++) {
		ipc_transport_send_empty(pconns[i].t, IPC_MSG_DETACH);
		ipc_transport_free(pconns[i].t);	/* closes fd */
		pconns[i].t = NULL;
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
	fprintf(stderr, "       lumi proxy -s <session> -L [-g group]\n");
	exit(1);
}

int
cmd_proxy_main(int argc, char **argv)
{
	int opt;
	int listen_mode = 0;
	const char *group = NULL;

	while ((opt = getopt(argc, argv, "s:Lg:")) != -1) {
		switch (opt) {
		case 's':
			session_name = optarg;
			break;
		case 'L':
			listen_mode = 1;
			break;
		case 'g':
			group = optarg;
			break;
		default:
			usage();
		}
	}

	if (!session_name)
		usage();

	if (listen_mode)
		return proxy_listen_run(group);

	/* the ssh/stdio path: one trusted, unrestricted connection as the
	 * session owner, identified by this process's own pid so two
	 * concurrent ssh-driven proxies do not collide in the roster. */
	proxy_client_id = (uint32_t)getpid();
	return proxy_run();
}
