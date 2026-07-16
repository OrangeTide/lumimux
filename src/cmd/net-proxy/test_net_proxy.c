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
#include "nc_udp.h"

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

	cfd = ipc_accept(lfd);
	if (cfd < 0)
		_exit(15);

	/* handshake: proxy sends ATTACH, we reply with a size. */
	if (ipc_msg_recv(cfd, &type, buf, sizeof(buf), &len) != 0 ||
	    type != IPC_MSG_ATTACH)
		_exit(16);
	ipc_msg_send_size(cfd, IPC_MSG_ATTACH_REPLY, 24, 80);

	for (;;) {
		int rc = ipc_msg_recv(cfd, &type, buf, sizeof(buf), &len);
		uint32_t i;

		if (rc != 0)
			break;			/* proxy detached / EOF */
		if (type == IPC_MSG_DETACH)
			break;
		if (type != IPC_MSG_INPUT)
			continue;
		for (i = 0; i < len; i++)
			buf[i] = (char)toupper((unsigned char)buf[i]);
		ipc_msg_send(cfd, IPC_MSG_OUTPUT, buf, len);
	}
	ipc_close(cfd);
	_exit(0);
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

int
main(void)
{
	printf("lumi-net-proxy tests:\n");

	test_bridge_roundtrip();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
