/* test_net_proxy.c : end-to-end test for the lumi-net-proxy bridge */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Step-4c coverage for FUTURE.md 11C.  Spin up a fake mserver on a real
 * Unix socket in an isolated session directory, run the real
 * cmd_net_proxy_main against it in a child process, then connect a netchan
 * client and drive one full round-trip: read PROXY_READY, send input to
 * the window, and read the mserver's reply back through the proxy.  The
 * fake mserver upper-cases the input it receives, so a correct "HELLO"
 * reply proves the bytes traversed client -> proxy -> mserver -> proxy ->
 * client rather than echoing inside any one hop.
 */

#include "ipc.h"
#include "ipc_msg.h"
#include "ipc_transport.h"
#include "ipc_transport_netchan.h"
#include "proxy_msg.h"
#include "sessdir.h"
#include "sessdir_control.h"
#include "nc_udp.h"
#include "keystore.h"
#include "nc_auth.h"
#include "monocypher.h"

/* entry point under test (declared in src/multicall.h, not on our -I). */
int cmd_net_proxy_main(int argc, char **argv);

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
		fflush(stdout); \
	} while (0)
#define PASS()	do { printf("ok\n"); } while (0)
#define FAIL(m)	do { printf("FAIL: %s\n", m); fail_count++; } while (0)

#define SESSION	"nettest"

/* ---- fake mserver: real Unix socket, upper-cases input into output ---- */

/* 12-d-b: path a fake_mserver() child appends to on IPC_MSG_ROLE_REQUEST,
 * so a live-ACL-change test can confirm the broker actually asked for a
 * new role rather than only checking wire-level side effects. Set by the
 * test before forking; empty means "don't bother logging". */
static char role_log_path[300];

static void
fake_mserver(void)
{
	char *dir;
	char path[256];
	int lfd, cfd;
	uint32_t type, len;
	static char buf[8192];

	if (sessdir_session_create(SESSION) != 0)
		_exit(11);
	if (sessdir_server_create(SESSION, getpid()) != 0)
		_exit(12);
	dir = sessdir_server_path(SESSION, getpid());
	if (!dir)
		_exit(13);
	snprintf(path, sizeof(path), "%s/socket", dir);
	free(dir);
	sessdir_write_file(SESSION, getpid(), "title", "shell");

	lfd = ipc_listen(path);
	if (lfd < 0)
		_exit(14);

	/* A real mserver fans multiple simultaneous clients out to the same
	 * window (steps 3/4), which is what net-proxy's fork-per-client -L
	 * listener relies on to bridge two concurrent clients through the
	 * same session. Mirror that here with a fork per accepted connection
	 * rather than serving them one at a time, so this stand-in does not
	 * itself become the bottleneck in the concurrency test below. Reap
	 * automatically: this process is SIGTERM-killed by the test when
	 * done, not waited on child by child. */
	signal(SIGCHLD, SIG_IGN);

	for (;;) {
		cfd = ipc_accept(lfd);
		if (cfd < 0)
			_exit(15);

		if (fork() != 0) {
			ipc_close(cfd);		/* parent keeps listening */
			continue;
		}

		ipc_close(lfd);

		/* handshake: proxy sends ATTACH, we reply with a size. */
		if (ipc_msg_recv(cfd, &type, buf, sizeof(buf), &len) != 0 ||
		    type != IPC_MSG_ATTACH) {
			ipc_close(cfd);
			_exit(0);
		}
		ipc_msg_send_size(cfd, IPC_MSG_ATTACH_REPLY, 24, 80);

		for (;;) {
			int rc = ipc_msg_recv(cfd, &type, buf, sizeof(buf),
			    &len);
			uint32_t i;

			if (rc != 0)
				break;		/* proxy detached / EOF */
			if (type == IPC_MSG_DETACH)
				break;
			if (type == IPC_MSG_ROLE_REQUEST && len >= 1 &&
			    role_log_path[0]) {
				FILE *rf = fopen(role_log_path, "a");

				if (rf) {
					fprintf(rf, "%d\n", (int)(uint8_t)buf[0]);
					fclose(rf);
				}
			}
			if (type != IPC_MSG_INPUT)
				continue;
			for (i = 0; i < len; i++)
				buf[i] = (char)toupper((unsigned char)buf[i]);
			ipc_msg_send(cfd, IPC_MSG_OUTPUT, buf, len);
		}
		ipc_close(cfd);
		_exit(0);
	}
}

/* ---- helpers ---- */

/* wait up to ~5s for the proxy to publish net-addr, return the UDP port. */
static int
wait_net_addr(void)
{
	int i;

	for (i = 0; i < 500; i++) {
		char *s = sessdir_read_session_file(SESSION, "net-addr");

		if (s) {
			int port = 0;

			sscanf(s, "udp %d", &port);
			free(s);
			if (port > 0)
				return port;
		}
		usleep(10000);
	}
	return -1;
}

/* read the proxy's published PSK into psk[32].  returns 0 on success. */
static int
read_net_key(uint8_t *psk)
{
	char *hex = sessdir_read_session_file(SESSION, "net-key");
	int i;

	if (!hex)
		return -1;
	for (i = 0; i < 32; i++) {
		int hi = hex[i * 2], lo = hex[i * 2 + 1];

		if (!isxdigit(hi) || !isxdigit(lo)) {
			free(hex);
			return -1;
		}
		hi = (hi <= '9') ? hi - '0' : (tolower(hi) - 'a' + 10);
		lo = (lo <= '9') ? lo - '0' : (tolower(lo) - 'a' + 10);
		psk[i] = (uint8_t)((hi << 4) | lo);
	}
	free(hex);
	return 0;
}

/* Write <session>/access (mode 0600) with a single rule for the named
 * netchan key, so a -L test's authenticated client is not refused (or is
 * refused on purpose) by the default owner-only fallback (12-c: net-proxy
 * clamps every -L connection to the session's ACL). `role` is "write",
 * "view", or "deny", matching the ACL file's own role column. */
static int
write_key_acl_role(const char *session, const char *keyname,
    const char *role)
{
	char *dir, path[512], content[256];
	FILE *f;

	sessdir_session_create(session);
	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	snprintf(path, sizeof(path), "%s/access", dir);
	free(dir);

	f = fopen(path, "w");
	if (!f)
		return -1;
	snprintf(content, sizeof(content), "owner write\nkey:%s\t%s\n"
	    "* deny\n", keyname, role);
	fputs(content, f);
	fclose(f);
	chmod(path, 0600);
	return 0;
}

static int
write_key_acl(const char *session, const char *keyname)
{
	return write_key_acl_role(session, keyname, "write");
}

/* For testing that an enrolled (authorized_keys) identity can still be
 * refused at the ACL layer after its netchan handshake succeeds, distinct
 * from an unenrolled key failing the handshake itself. */
static int
write_key_acl_deny(const char *session, const char *keyname)
{
	return write_key_acl_role(session, keyname, "deny");
}

/* An "ask" rule (12-d-c): the named key is admitted pending approval at
 * up to `role` rather than granted or denied outright. */
static int
write_key_acl_ask(const char *session, const char *keyname, const char *role)
{
	char *dir, path[512], content[256];
	FILE *f;

	sessdir_session_create(session);
	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	snprintf(path, sizeof(path), "%s/access", dir);
	free(dir);

	f = fopen(path, "w");
	if (!f)
		return -1;
	snprintf(content, sizeof(content), "owner write\nkey:%s\t%s ask\n"
	    "* deny\n", keyname, role);
	fputs(content, f);
	fclose(f);
	chmod(path, 0600);
	return 0;
}

/* blocking recv with a wall-clock bound, so a stuck bridge fails the test
 * instead of hanging.  returns 0 on a message, -1 on timeout/error. */
static int
recv_timed(struct ipc_transport *t, uint32_t *type, void *buf, size_t bufsz,
    uint32_t *len, int max_ms)
{
	struct pollfd pfd;
	int waited = 0;

	pfd.fd = ipc_transport_get_fd(t);
	pfd.events = POLLIN;
	for (;;) {
		int r = ipc_transport_netchan_try_recv(t, type, buf, bufsz,
		    len);

		if (r == 0)
			return 0;
		if (r < 0)
			return -1;
		if (ipc_transport_netchan_pump(t) != 0)
			return -1;
		if (poll(&pfd, 1, 50) < 0)
			return -1;
		waited += 50;
		if (waited > max_ms)
			return -1;
	}
}

/* ---- the test ---- */

static void
test_bridge_roundtrip(void)
{
	char tmpl[] = "/tmp/lumi-nettest-XXXXXX";
	char *rundir;
	pid_t mpid, ppid;
	int port, ok = 0;
	struct ipc_transport *cli = NULL;
	struct sockaddr_in sin;
	struct nc_addr peer;
	static uint8_t buf[8192];
	uint8_t psk[32];
	uint32_t type, len, wid;
	int cfd = -1;

	TEST("net-proxy bridges client <-> mserver end to end");

	rundir = mkdtemp(tmpl);
	if (!rundir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	unsetenv("LUMI_SESSION");

	/* fake mserver child */
	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}

	/* give the mserver a moment to register before the proxy lists it. */
	usleep(100000);

	/* real proxy child */
	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-b", "127.0.0.1", NULL };

		optind = 1;
		_exit(cmd_net_proxy_main(5, argv));
	}

	port = wait_net_addr();
	if (port < 0) {
		FAIL("proxy never published net-addr");
		goto done;
	}
	if (read_net_key(psk) != 0) {
		FAIL("proxy never published net-key");
		goto done;
	}

	/* netchan client */
	cfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (cfd < 0) {
		FAIL("client socket failed");
		goto done;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons((uint16_t)port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		FAIL("nc_addr build failed");
		goto done;
	}
	cli = ipc_transport_netchan_new_crypto(cfd, 0, &peer, psk);
	if (!cli) {
		FAIL("client transport alloc failed");
		close(cfd);
		cfd = -1;
		goto done;
	}
	cfd = -1;			/* transport owns it now */
	if (ipc_transport_netchan_establish(cli, 5000) != 0) {
		FAIL("client handshake failed");
		goto done;
	}

	/* first message must be PROXY_READY listing the one window. */
	if (recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) != 0) {
		FAIL("no PROXY_READY from proxy");
		goto done;
	}
	if (proxy_msg_xdecode(&wid, buf, &len) != 0 || wid != 0 ||
	    type != IPC_MSG_PROXY_READY) {
		FAIL("first message was not PROXY_READY");
		goto done;
	}
	if (len < 2 || ((buf[0] << 8) | buf[1]) < 1) {
		FAIL("PROXY_READY listed no windows");
		goto done;
	}
	/* window id is the mserver pid, at payload offset 2. */
	wid = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
	    ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];
	if (wid != (uint32_t)mpid) {
		FAIL("PROXY_READY window id != mserver pid");
		goto done;
	}

	/* send input to the window; expect the upper-cased echo back. */
	if (proxy_msg_xsend(cli, wid, IPC_MSG_INPUT, "hello", 5) != 0) {
		FAIL("input send failed");
		goto done;
	}
	if (recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) != 0) {
		FAIL("no output from mserver via proxy");
		goto done;
	}
	if (proxy_msg_xdecode(&wid, buf, &len) != 0 ||
	    type != IPC_MSG_OUTPUT || wid != (uint32_t)mpid ||
	    len != 5 || memcmp(buf, "HELLO", 5) != 0) {
		FAIL("output mismatch (bridge did not relay correctly)");
		goto done;
	}
	ok = 1;

done:
	if (cli)
		ipc_transport_free(cli);
	if (cfd >= 0)
		close(cfd);
	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	/* best-effort cleanup of the temp run dir. */
	{
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", rundir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/* ---- direct-connect listen path (net-proxy -L) ---- */

/* Client-side credential source for one login attempt. */
struct la_cred {
	const uint8_t	*sk;		/* 64-byte Ed25519 secret, or NULL */
	const uint8_t	*pk;		/* 32-byte public */
};

static int
la_get_key(void *ctx, uint8_t sk[64], uint8_t pk[32])
{
	struct la_cred *c = ctx;

	if (!c->sk)
		return -1;
	memcpy(sk, c->sk, 64);
	memcpy(pk, c->pk, 32);
	return 0;
}

static int
la_get_password(void *ctx, char *buf, size_t sz)
{
	(void)ctx;
	(void)buf;
	(void)sz;
	return -1;			/* this test uses public keys only */
}

/* Accept whatever host key the server presents (trust on first use). */
static int
la_accept_host(void *ctx, const uint8_t *peer_static_pk)
{
	(void)ctx;
	(void)peer_static_pk;
	return 0;
}

/*
 * Connect one client to the listening proxy on `port`, authenticate with the
 * given key, and, when expect_ok, round-trip one message through the window
 * (id `mpid`).  Returns 1 if the outcome matched expect_ok.
 */
static int
listen_client(int port, const uint8_t *sk, const uint8_t *pk, pid_t mpid,
    int expect_ok)
{
	struct la_cred cred = { sk, pk };
	struct ipc_netchan_userauth ua = {
		.user = "testuser",
		.get_key = la_get_key,
		.get_password = la_get_password,
		.cred_ctx = &cred,
	};
	struct ipc_netchan_auth auth = {
		.verify_peer = la_accept_host,
		.require_peer_static = 1,
		.userauth = &ua,
	};
	struct sockaddr_in sin;
	struct nc_addr peer;
	struct ipc_transport *cli;
	static uint8_t buf[8192];
	uint32_t type, len, wid;
	int cfd, ok = 0, established;

	cfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (cfd < 0)
		return 0;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons((uint16_t)port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		close(cfd);
		return 0;
	}
	cli = ipc_transport_netchan_new_crypto_auth(cfd, 0, &peer, &auth);
	if (!cli) {
		close(cfd);
		return 0;
	}

	established = (ipc_transport_netchan_establish(cli, 5000) == 0);
	if (!expect_ok) {
		ok = !established;	/* the login must be refused */
		ipc_transport_free(cli);
		return ok;
	}
	if (!established) {
		ipc_transport_free(cli);
		return 0;
	}

	if (recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) == 0 &&
	    proxy_msg_xdecode(&wid, buf, &len) == 0 && wid == 0 &&
	    type == IPC_MSG_PROXY_READY && len >= 6) {
		wid = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
		    ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];
		if (wid == (uint32_t)mpid &&
		    proxy_msg_xsend(cli, wid, IPC_MSG_INPUT, "hi", 2) == 0 &&
		    recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) == 0 &&
		    proxy_msg_xdecode(&wid, buf, &len) == 0 &&
		    type == IPC_MSG_OUTPUT && len == 2 &&
		    memcmp(buf, "HI", 2) == 0)
			ok = 1;
	}
	ipc_transport_free(cli);	/* graceful close: proxy sees the
					 * disconnect and serves the next client */
	return ok;
}

/* Connect and authenticate one client, leaving the transport open (for a
 * caller that wants two clients live at once, unlike listen_client() above,
 * which closes before returning). Returns NULL on any failure. */
static struct ipc_transport *
listen_client_open(int port, const uint8_t *sk, const uint8_t *pk)
{
	struct la_cred cred = { sk, pk };
	struct ipc_netchan_userauth ua = {
		.user = "testuser",
		.get_key = la_get_key,
		.get_password = la_get_password,
		.cred_ctx = &cred,
	};
	struct ipc_netchan_auth auth = {
		.verify_peer = la_accept_host,
		.require_peer_static = 1,
		.userauth = &ua,
	};
	struct sockaddr_in sin;
	struct nc_addr peer;
	struct ipc_transport *cli;
	int cfd;

	cfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (cfd < 0)
		return NULL;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons((uint16_t)port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		close(cfd);
		return NULL;
	}
	cli = ipc_transport_netchan_new_crypto_auth(cfd, 0, &peer, &auth);
	if (!cli) {
		close(cfd);
		return NULL;
	}
	if (ipc_transport_netchan_establish(cli, 5000) != 0) {
		ipc_transport_free(cli);
		return NULL;
	}
	return cli;
}

/* Read and validate the initial PROXY_READY. Returns 1 on success. */
static int
listen_client_consume_ready(struct ipc_transport *cli)
{
	uint8_t buf[8192];
	uint32_t type, len, wid;

	return recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) == 0 &&
	    proxy_msg_xdecode(&wid, buf, &len) == 0 && wid == 0 &&
	    type == IPC_MSG_PROXY_READY;
}

/* Send msg to window wid on an already-open client and check the fake
 * mserver's upper-cased echo comes back through this client's own bridge,
 * undisturbed by any other client sharing the listener. Returns 1 on match. */
static int
listen_client_roundtrip(struct ipc_transport *cli, uint32_t wid,
    const char *msg)
{
	uint8_t buf[8192];
	uint32_t type, len, owid;
	size_t mlen = strlen(msg);
	char expect[64];
	size_t i;

	if (mlen >= sizeof(expect))
		return 0;
	for (i = 0; i < mlen; i++)
		expect[i] = (char)toupper((unsigned char)msg[i]);

	if (proxy_msg_xsend(cli, wid, IPC_MSG_INPUT, msg,
	    (uint32_t)mlen) != 0)
		return 0;
	if (recv_timed(cli, &type, buf, sizeof(buf), &len, 5000) != 0)
		return 0;
	if (proxy_msg_xdecode(&owid, buf, &len) != 0 || owid != wid ||
	    type != IPC_MSG_OUTPUT || len != mlen)
		return 0;
	return memcmp(buf, expect, mlen) == 0;
}

/*
 * The direct-connect listener: authenticate a user over netchan (no ssh),
 * gate on the authorized_keys store, and keep serving after a client leaves or
 * is rejected.  A fixed port is required, so the proxy is started with -L -p.
 */
static void
test_listen_userauth(void)
{
	char tmpl[] = "/tmp/lumi-listenrun-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listencfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32], badsk[64], badpk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1;
	FILE *f;

	TEST("net-proxy -L authenticates, gates, and serves repeatedly");

	/* a per-pid port keeps concurrent test runs from colliding. Each -L
	 * test in this file gets its own disjoint 1000-port range (rather
	 * than each picking an independent wide modulus) so two tests in
	 * the same run can never coincide on the same port for the same
	 * pid -- SO_REUSEPORT would silently let a still-tearing-down
	 * listener from an earlier test steal a later test's connection
	 * instead of a bind() failure making the collision obvious. */
	port = 40000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	/* an authorized client key, and a different one that is not enrolled. */
	memset(seed, 0x5C, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);
	memset(seed, 0x77, sizeof(seed));
	crypto_eddsa_key_pair(badsk, badpk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl(SESSION, "testuser") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	/* an authorized key logs in and round-trips... */
	if (!listen_client(port, sk, pk, mpid, 1)) {
		FAIL("authorized-key login or round-trip failed");
		ok = 0;
	/* ...the listener serves a second client after the first has fully
	 * left (each gets its own forked child; see the concurrent-clients
	 * test below for both alive at once)... */
	} else if (!listen_client(port, sk, pk, mpid, 1)) {
		FAIL("listener did not serve a second client");
		ok = 0;
	/* ...and an unenrolled key is refused. */
	} else if (!listen_client(port, badsk, badpk, mpid, 0)) {
		FAIL("unauthorized key was not refused");
		ok = 0;
	}

	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-c: an enrolled key (valid in authorized_keys, so its
 * netchan handshake succeeds) can still be refused at the ACL layer, which
 * is a distinct failure point from test_listen_userauth's unenrolled-key
 * case above (refused before the handshake even completes). Confirm no
 * PROXY_READY, or anything else, ever reaches a client the ACL denies --
 * fail-closed rule 5, no session content before a decision is made.
 */
static void
test_listen_acl_deny(void)
{
	char tmpl[] = "/tmp/lumi-listenacl-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listenaclcfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1;
	struct ipc_transport *cli;
	FILE *f;

	TEST("net-proxy -L refuses an enrolled key the ACL denies");

	port = 41000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0x91, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl_deny(SESSION, "testuser") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	/* the handshake itself must succeed -- the key is enrolled -- but
	 * the ACL denies it, so nothing further ever arrives. */
	cli = listen_client_open(port, sk, pk);
	if (!cli) {
		FAIL("handshake for an enrolled key unexpectedly failed");
		ok = 0;
	} else {
		if (listen_client_consume_ready(cli)) {
			FAIL("ACL-denied client received PROXY_READY");
			ok = 0;
		}
		ipc_transport_free(cli);
	}

	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-c: an "ask" match with no one able to approve it is
 * still an immediate denial -- fail-closed rule 3 -- not a wasted wait.
 * No token holder and no owner-tier roster entry means no approver.
 */
static void
test_listen_acl_pending_no_approver(void)
{
	char tmpl[] = "/tmp/lumi-listennoappr-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listennoapprcfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1;
	struct ipc_transport *cli;
	FILE *f;

	TEST("net-proxy -L denies an ask match with no one available to approve");

	port = 45000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0xE6, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl_ask(SESSION, "testuser", "view") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	/* the handshake succeeds -- the key is enrolled -- but with nobody
	 * around to answer the ask rule, admission is refused immediately. */
	cli = listen_client_open(port, sk, pk);
	if (!cli) {
		FAIL("handshake for an enrolled key unexpectedly failed");
		ok = 0;
	} else {
		if (listen_client_consume_ready(cli)) {
			FAIL("ask match with no approver was admitted");
			ok = 0;
		}
		ipc_transport_free(cli);
	}

	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-c: an "ask" match with a write-token holder present
 * is admitted pending rather than denied. It must receive nothing -- no
 * PROXY_READY, no session content of any kind -- until sessdir_ctl_post()
 * approves it (posted the way `lumi share -a id` would), at which point
 * it proceeds exactly like an ordinary admission.
 */
static void
test_listen_acl_pending_approve(void)
{
	char tmpl[] = "/tmp/lumi-listenpend-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listenpendcfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1, waited;
	struct ipc_transport *cli = NULL;
	unsigned long pending_id = 0;
	FILE *f;

	TEST("net-proxy -L admits an ask match as pending and releases it on approval");

	port = 46000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0xF7, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl_ask(SESSION, "testuser", "view") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	/* stand in for a real owner client holding the keyboard, so
	 * approver_available() finds someone who could answer. */
	if (sessdir_token_acquire(SESSION, 999999, "owner") != 0) {
		FAIL("cannot acquire the write token");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		sessdir_token_release();
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		sessdir_token_release();
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	cli = listen_client_open(port, sk, pk);
	if (!cli) {
		FAIL("handshake for an enrolled key unexpectedly failed");
		ok = 0;
		goto done;
	}

	/* nothing arrives while pending -- checked with a short bound, not
	 * the usual 5s, so this test does not itself take forever. */
	{
		uint8_t buf[8192];
		uint32_t type, len;

		if (recv_timed(cli, &type, buf, sizeof(buf), &len, 800) == 0) {
			FAIL("pending client received content before approval");
			ok = 0;
			goto done;
		}
	}

	/* find the pending roster entry left by 12-d-a's registration to
	 * get the client_id sessdir_ctl_post() must address. */
	for (waited = 0; waited < 20; waited++) {
		struct sessdir_client list[SESSDIR_CLIENT_MAX];
		int n = sessdir_client_list(SESSION, list, SESSDIR_CLIENT_MAX);
		int i;

		for (i = 0; i < n; i++) {
			if (strcmp(list[i].who, "testuser") == 0 &&
			    list[i].pending) {
				pending_id = list[i].client_id;
				break;
			}
		}
		if (pending_id)
			break;
		usleep(50000);
	}
	if (!pending_id) {
		FAIL("pending client never appeared in the roster");
		ok = 0;
		goto done;
	}

	if (sessdir_ctl_post(SESSION, SESSDIR_CTL_APPROVE, pending_id, 0,
	    "test") != 0) {
		FAIL("could not post the approval");
		ok = 0;
		goto done;
	}

	if (!listen_client_consume_ready(cli)) {
		FAIL("approved client never received PROXY_READY");
		ok = 0;
		goto done;
	}

	/* the roster entry must reflect the resolved, non-pending state. */
	{
		struct sessdir_client list[SESSDIR_CLIENT_MAX];
		int n = sessdir_client_list(SESSION, list, SESSDIR_CLIENT_MAX);
		int i, found = 0;

		for (i = 0; i < n; i++) {
			if (list[i].client_id != pending_id)
				continue;
			found = 1;
			if (list[i].pending || strcmp(list[i].role, "view") != 0)
				ok = 0;
		}
		if (!found || !ok)
			FAIL("roster entry did not clear pending on approval");
	}

done:
	if (cli)
		ipc_transport_free(cli);
	sessdir_token_release();
	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-b: an ACL change reaches an already-connected
 * client, not just new ones. A write grant that becomes a deny mid-
 * connection must disconnect the client without it needing to detach
 * and reattach itself.
 */
static void
test_listen_acl_live_revoke(void)
{
	char tmpl[] = "/tmp/lumi-listenrevoke-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listenrevokecfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1;
	struct ipc_transport *cli = NULL;
	FILE *f;

	TEST("net-proxy -L disconnects a client whose ACL grant is revoked live");

	port = 42000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0xC4, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl(SESSION, "testuser") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	cli = listen_client_open(port, sk, pk);
	if (!cli || !listen_client_consume_ready(cli)) {
		FAIL("could not establish the initial connection");
		ok = 0;
		goto done;
	}
	if (!listen_client_roundtrip(cli, (uint32_t)mpid, "before")) {
		FAIL("round trip failed before revocation");
		ok = 0;
		goto done;
	}

	if (write_key_acl_deny(SESSION, "testuser") != 0) {
		FAIL("cannot rewrite access acl");
		ok = 0;
		goto done;
	}

	/* the watch wakeup plus recheck_acl()'s poll granularity (up to
	 * PROXY_TICK_MS) needs a moment; this is generous rather than tight. */
	usleep(600000);

	if (listen_client_roundtrip(cli, (uint32_t)mpid, "after")) {
		FAIL("revoked client's INPUT still round-tripped");
		ok = 0;
	}

done:
	if (cli)
		ipc_transport_free(cli);
	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-b, the other direction: a write grant downgraded to
 * view mid-connection must re-clamp the role on every attached window
 * rather than waiting for a reattach. net_proxy.c's own responsibility
 * ends at sending IPC_MSG_ROLE_REQUEST; actual enforcement is mserver.c's
 * job and is covered by test_mserver.c, so this only confirms the broker
 * asks.
 */
static void
test_listen_acl_live_downgrade(void)
{
	char tmpl[] = "/tmp/lumi-listendowngrade-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listendowngradecfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1, waited;
	struct ipc_transport *cli = NULL;
	FILE *f;

	TEST("net-proxy -L re-requests a downgraded role live");

	port = 43000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0xD5, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl(SESSION, "testuser") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	snprintf(role_log_path, sizeof(role_log_path), "%s/roles", rundir);

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		role_log_path[0] = '\0';
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		role_log_path[0] = '\0';
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	cli = listen_client_open(port, sk, pk);
	if (!cli || !listen_client_consume_ready(cli)) {
		FAIL("could not establish the initial connection");
		ok = 0;
		goto done;
	}

	if (write_key_acl_role(SESSION, "testuser", "view") != 0) {
		FAIL("cannot rewrite access acl");
		ok = 0;
		goto done;
	}

	for (waited = 0; waited < 20; waited++) {
		FILE *rf = fopen(role_log_path, "r");
		char line[16];
		int seen = 0;

		if (rf) {
			while (fgets(line, sizeof(line), rf)) {
				if (atoi(line) & IPC_ATTACH_F_VIEW) {
					seen = 1;
					break;
				}
			}
			fclose(rf);
		}
		if (seen)
			break;
		usleep(50000);
	}
	if (waited == 20) {
		FAIL("no IPC_MSG_ROLE_REQUEST(view) reached the mserver");
		ok = 0;
	}

done:
	if (cli)
		ipc_transport_free(cli);
	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);
	role_log_path[0] = '\0';

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

/*
 * FUTURE.md step 11: the -L listener forks a child per accepted client
 * rather than serving one at a time. Open two clients against the same
 * listener before either finishes, interleave a round trip through each,
 * and check that neither's traffic leaks into the other's bridge -- then
 * confirm the listener still serves a further client once both leave.
 */
static void
test_listen_concurrent_clients(void)
{
	char tmpl[] = "/tmp/lumi-listenconc-XXXXXX";
	char cfgtmpl[] = "/tmp/lumi-listenconccfg-XXXXXX";
	char *rundir, *cfgdir;
	uint8_t seed[32], sk[64], pk[32];
	char path[512], hex[65];
	pid_t mpid, ppid;
	int port, ok = 1;
	struct ipc_transport *a = NULL, *b = NULL;
	FILE *f;

	TEST("net-proxy -L bridges two concurrent clients over one listener");

	port = 44000 + (int)(getpid() % 1000);

	rundir = mkdtemp(tmpl);
	cfgdir = mkdtemp(cfgtmpl);
	if (!rundir || !cfgdir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	setenv("XDG_CONFIG_HOME", cfgdir, 1);
	unsetenv("LUMI_SESSION");

	memset(seed, 0x3A, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);

	snprintf(path, sizeof(path), "%s/lumi", cfgdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/lumi/authorized_keys", cfgdir);
	f = fopen(path, "w");
	if (!f) {
		FAIL("cannot write authorized_keys");
		return;
	}
	ks_hex_encode(hex, pk, 32);
	fprintf(f, "testuser %s\n", hex);
	fclose(f);

	if (write_key_acl(SESSION, "testuser") != 0) {
		FAIL("cannot write access acl");
		return;
	}

	mpid = fork();
	if (mpid < 0) {
		FAIL("fork mserver failed");
		return;
	}
	if (mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char portstr[16];
		char *argv[] = { "lumi-net-proxy", "-s", (char *)SESSION,
		    "-L", "-p", portstr, "-b", "127.0.0.1", NULL };

		snprintf(portstr, sizeof(portstr), "%d", port);
		optind = 1;
		_exit(cmd_net_proxy_main(8, argv));
	}
	usleep(300000);		/* let it bind and load its host key */

	/* open both before either round-trips, so the listener must really
	 * be bridging them concurrently rather than one at a time. */
	a = listen_client_open(port, sk, pk);
	b = listen_client_open(port, sk, pk);
	if (!a || !b) {
		FAIL("could not establish two concurrent clients");
		ok = 0;
		goto done;
	}
	if (!listen_client_consume_ready(a) || !listen_client_consume_ready(b)) {
		FAIL("PROXY_READY missing on a concurrent client");
		ok = 0;
		goto done;
	}

	/* interleave: b's input/output must not disturb a's, and vice versa. */
	if (!listen_client_roundtrip(a, (uint32_t)mpid, "first") ||
	    !listen_client_roundtrip(b, (uint32_t)mpid, "second") ||
	    !listen_client_roundtrip(a, (uint32_t)mpid, "third") ||
	    !listen_client_roundtrip(b, (uint32_t)mpid, "fourth")) {
		FAIL("round trip failed on a concurrent client");
		ok = 0;
	}

	/* 12-d-a: each forked child registers its own client into the
	 * session-wide roster, so both concurrent connections must show up
	 * as distinct entries (distinct client_id, same who) rather than one
	 * clobbering the other. Poll rather than check once: registration is
	 * synchronous within each forked child by the time it reaches
	 * PROXY_READY, but the two children are still independent processes
	 * racing this test process's own read of the roster directory. */
	if (ok) {
		int waited, n = 0, keyed = 0;

		for (waited = 0; waited < 20; waited++) {
			struct sessdir_client list[SESSDIR_CLIENT_MAX];
			int i;

			n = sessdir_client_list(SESSION, list,
			    SESSDIR_CLIENT_MAX);
			keyed = 0;
			for (i = 0; i < n; i++) {
				if (strcmp(list[i].who, "testuser") == 0)
					keyed++;
			}
			if (n == 2 && keyed == 2)
				break;
			usleep(50000);
		}
		if (n != 2 || keyed != 2) {
			FAIL("roster did not show both concurrent clients");
			ok = 0;
		}
	}

done:
	if (a)
		ipc_transport_free(a);
	if (b)
		ipc_transport_free(b);

	/* the listener must still work once both concurrent clients leave. */
	if (ok && !listen_client(port, sk, pk, mpid, 1)) {
		FAIL("listener did not serve a client after the concurrent "
		    "pair left");
		ok = 0;
	}

	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);

	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", rundir, cfgdir);
		if (system(cmd) != 0)
			; /* ignore */
	}

	if (ok)
		PASS();
}

int
main(void)
{
	printf("lumi-net-proxy tests:\n");

	test_bridge_roundtrip();
	test_listen_userauth();
	test_listen_acl_deny();
	test_listen_acl_pending_no_approver();
	test_listen_acl_pending_approve();
	test_listen_acl_live_revoke();
	test_listen_acl_live_downgrade();
	test_listen_concurrent_clients();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
