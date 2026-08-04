/* test_mserver.c : multi-client tests for lumi-mserver */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Step 4 coverage for the Phase 12 shared attach plan.  Each test runs the
 * real cmd_mserver_main() in a forked child against a shell on a PTY, in a
 * session directory of its own, then drives it over the same Unix socket a
 * real client would use.
 *
 * What is being proved: several clients attached at once all see the same
 * window, a full table refuses a newcomer instead of evicting someone, and
 * a client that stops reading is dropped rather than stalling the clients
 * that are keeping up.
 */

#include "mserver.h"

#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "sessdir.h"
#include "sessdir_control.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define SESSION	"mstest"

/* ---- server under test ---- */

static pid_t server_pid;
static char runtime_dir[PATH_MAX];

/* start a server for SESSION in a child process and wait for its socket to
 * appear.  returns the socket path (caller frees) or NULL. */
static char *
server_start(void)
{
	char *argv[5];
	char *dir, *path;
	int i;

	server_pid = fork();
	if (server_pid < 0)
		return NULL;
	if (server_pid == 0) {
		/* the child is the server: no test output from here */
		argv[0] = (char *)"lumi-mserver";
		argv[1] = (char *)"-s";
		argv[2] = (char *)SESSION;
		argv[3] = (char *)"/bin/sh";
		argv[4] = NULL;
		if (!getenv("LUMI_TEST_VERBOSE"))
			freopen("/dev/null", "w", stderr);
		_exit(cmd_mserver_main(4, argv));
	}

	/* wait for the socket to show up, up to ~5 seconds */
	for (i = 0; i < 500; i++) {
		pid_t pids[4];
		int n;

		usleep(10000);
		n = sessdir_list_servers(SESSION, pids, 4);
		if (n <= 0)
			continue;
		dir = sessdir_server_path(SESSION, pids[0]);
		if (!dir)
			continue;
		path = malloc(PATH_MAX);
		if (!path) {
			free(dir);
			return NULL;
		}
		snprintf(path, PATH_MAX, "%s/socket", dir);
		free(dir);
		if (access(path, F_OK) == 0)
			return path;
		free(path);
	}
	return NULL;
}

static void
server_stop(void)
{
	int status;

	if (server_pid > 0) {
		kill(server_pid, SIGTERM);
		waitpid(server_pid, &status, 0);
		/* a server that crashed leaves its socket in the session
		 * directory and every later test attaches to a dead one, so
		 * say it plainly rather than reporting seven odd failures */
		if (WIFSIGNALED(status))
			fprintf(stderr, "\n  server died on signal %d\n",
			    WTERMSIG(status));
		server_pid = 0;
	}
}

/* ---- client helpers ---- */

/* connect, ATTACH at the given size, and consume the ATTACH_REPLY.
 * stores the granted role through *role when non-NULL.
 * returns the fd, or -1. */
static int
client_attach_as(const char *path, int rows, int cols, uint8_t flags,
    uint8_t *role)
{
	struct ipc_attach at;
	struct ipc_attach_reply rep;
	uint8_t buf[128];
	uint32_t type, len;
	char rbuf[IPC_MAX_PAYLOAD];
	int fd, n;

	fd = ipc_connect(path);
	if (fd < 0)
		return -1;

	at.rows = (uint16_t)rows;
	at.cols = (uint16_t)cols;
	at.flags = flags;
	at.client_id = (uint32_t)getpid();
	at.name = "test@here";
	at.name_len = (uint16_t)strlen(at.name);
	n = ipc_attach_encode(&at, buf, sizeof(buf));
	if (n <= 0 || ipc_msg_send(fd, IPC_MSG_ATTACH, buf, (uint32_t)n) < 0) {
		ipc_close(fd);
		return -1;
	}
	if (ipc_msg_recv(fd, &type, rbuf, sizeof(rbuf), &len) != 0 ||
	    type != IPC_MSG_ATTACH_REPLY) {
		ipc_close(fd);
		return -1;
	}
	if (role) {
		*role = IPC_ROLE_WRITE;
		if (ipc_attach_reply_decode(&rep, (const uint8_t *)rbuf,
		    (int)len) >= 0)
			*role = rep.role;
	}
	return fd;
}

static int
client_attach(const char *path, int rows, int cols)
{
	return client_attach_as(path, rows, cols, 0, NULL);
}

static int
client_input(int fd, const char *s)
{
	return ipc_msg_send(fd, IPC_MSG_INPUT, s, (uint32_t)strlen(s));
}

/* read messages until one of them carries `want`, or the deadline passes.
 * returns 1 on match, 0 on timeout, -1 if the connection went away. */
static int
client_expect(int fd, const char *want, int timeout_ms)
{
	char buf[IPC_MAX_PAYLOAD + 1];
	uint32_t type, len;
	struct pollfd pfd;
	time_t deadline = time(NULL) + (timeout_ms + 999) / 1000;

	for (;;) {
		if (time(NULL) > deadline)
			return 0;
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		if (ipc_msg_recv(fd, &type, buf, sizeof(buf) - 1, &len) != 0)
			return -1;
		if (type != IPC_MSG_OUTPUT)
			continue;
		buf[len] = '\0';
		if (strstr(buf, want))
			return 1;
	}
}

/* drain until nothing has arrived for quiet_ms, or budget_ms runs out.
 *
 * A flood leaves a backlog queued for even a client that kept up, and the
 * question asked next has a deadline on it. Chewing the backlog first is
 * the difference between measuring whether the client is alive and
 * measuring how fast the machine can move bytes it no longer cares about. */
static void
client_drain_quiet(int fd, int quiet_ms, int budget_ms)
{
	char buf[IPC_MAX_PAYLOAD];
	uint32_t type, len;
	struct pollfd pfd;
	time_t deadline = time(NULL) + (budget_ms + 999) / 1000;

	while (time(NULL) <= deadline) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, quiet_ms) <= 0)
			return;		/* quiet for long enough */
		if (ipc_msg_recv(fd, &type, buf, sizeof(buf), &len) != 0)
			return;		/* gone: the caller will say so */
	}
}

/* drain whatever is readable without looking at it */
static void
client_drain(int fd)
{
	char buf[IPC_MAX_PAYLOAD];
	uint32_t type, len;
	struct pollfd pfd;

	for (;;) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 0) <= 0)
			return;
		if (ipc_msg_recv(fd, &type, buf, sizeof(buf), &len) != 0)
			return;
	}
}

/* ---- tests ---- */

/* the point of the whole phase: more than one client, all live */
static void
test_fanout(void)
{
	char *path;
	int c[3];
	int i, rc;

	TEST("three clients all see the same output");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	for (i = 0; i < 3; i++) {
		c[i] = client_attach(path, 24, 80);
		if (c[i] < 0) {
			FAIL("attach failed");
			goto out;
		}
	}

	if (client_input(c[0], "echo LUMI-FANOUT\n") < 0) {
		FAIL("input failed");
		goto out;
	}

	for (i = 0; i < 3; i++) {
		rc = client_expect(c[i], "LUMI-FANOUT", 5000);
		if (rc != 1) {
			FAIL(rc < 0 ? "client disconnected"
			    : "client never saw the output");
			goto out;
		}
	}
	PASS();
out:
	for (i = 0; i < 3; i++)
		ipc_close(c[i]);
	free(path);
	server_stop();
}

/* a full table refuses the newcomer rather than evicting a working client */
static void
test_table_full(void)
{
	char *path;
	int c[MSERVER_CLIENT_MAX];
	int extra, i;
	uint32_t type, len;
	char buf[256];

	TEST("a full client table refuses the next attach");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	for (i = 0; i < MSERVER_CLIENT_MAX; i++) {
		c[i] = client_attach(path, 24, 80);
		if (c[i] < 0) {
			FAIL("attach failed below the limit");
			goto out;
		}
	}

	extra = client_attach(path, 24, 80);
	if (extra >= 0) {
		FAIL("attach past the limit was accepted");
		ipc_close(extra);
		goto out;
	}

	/* client_attach() failed because the reply was an error rather than
	 * an ATTACH_REPLY; do it by hand to check which error it was */
	extra = ipc_connect(path);
	if (extra < 0) {
		FAIL("connect failed");
		goto out;
	}
	ipc_msg_send_empty(extra, IPC_MSG_ATTACH);
	if (ipc_msg_recv(extra, &type, buf, sizeof(buf), &len) != 0 ||
	    type != IPC_MSG_ERROR) {
		FAIL("expected an ERROR for the extra client");
		ipc_close(extra);
		goto out;
	}
	ipc_close(extra);

	/* and the clients already attached are still working */
	if (client_input(c[0], "echo LUMI-STILL-HERE\n") < 0 ||
	    client_expect(c[0], "LUMI-STILL-HERE", 5000) != 1) {
		FAIL("an existing client was disturbed");
		goto out;
	}
	PASS();
out:
	for (i = 0; i < MSERVER_CLIENT_MAX; i++)
		ipc_close(c[i]);
	free(path);
	server_stop();
}

/* the property that makes view-only clients safe to allow: one client that
 * stops reading is dropped, and the other one never stops receiving. */
static void
test_slow_client_dropped(void)
{
	char *path;
	int good = -1, slow = -1;
	int i, rc, dropped = 0;

	TEST("a client that stops reading is dropped, not throttled");

	/* a small limit keeps this test to kilobytes instead of megabytes.
	 * the watermarks come down with it and stay in the order they have
	 * in production, low < high < hard: the throttle has to be able to
	 * engage before the drop cap, or the client that is keeping up is
	 * dropped for being descheduled on a busy machine rather than for
	 * anything this test is about. */
	mserver_outq_hard_limit = 64 * 1024;
	mserver_outq_high_water = 16 * 1024;
	mserver_outq_low_water = 8 * 1024;

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		goto restore;
	}

	good = client_attach(path, 24, 80);
	slow = client_attach(path, 24, 80);
	if (good < 0 || slow < 0) {
		FAIL("attach failed");
		goto out;
	}

	/* flood: the shell writes forever, `good` keeps up, `slow` never
	 * reads a byte */
	if (client_input(good, "yes LUMI-FLOOD\n") < 0) {
		FAIL("input failed");
		goto out;
	}

	for (i = 0; i < 100; i++) {
		client_drain(good);
		usleep(20000);
	}

	/* stop the flood so the drain below terminates */
	client_input(good, "\003");
	usleep(200000);

	/* the slow client's buffered bytes are finite once the server has
	 * dropped it, so draining reaches EOF.  if it was never dropped the
	 * server is still queueing and this times out. */
	for (i = 0; i < 500; i++) {
		char buf[IPC_MAX_PAYLOAD];
		uint32_t type, len;
		int rc = ipc_msg_recv(slow, &type, buf, sizeof(buf), &len);

		if (rc != 0) {		/* EOF or error: the server let go */
			dropped = 1;
			break;
		}
	}
	if (!dropped) {
		FAIL("the stalled client was never dropped");
		goto out;
	}

	/* and the client that kept reading is still attached and live.
	 * the two ways this can fail are worth telling apart: a connection
	 * that is gone means the server dropped a client that was keeping
	 * up, and a deadline that passes means it is alive but the flood
	 * did not stop. */
	client_drain_quiet(good, 200, 5000);
	if (client_input(good, "echo LUMI-SURVIVOR\n") < 0) {
		FAIL("the healthy client's connection was already gone");
		goto out;
	}
	rc = client_expect(good, "LUMI-SURVIVOR", 5000);
	if (rc < 0) {
		FAIL("the healthy client was dropped");
		goto out;
	}
	if (rc == 0) {
		FAIL("the healthy client never saw its own echo");
		goto out;
	}
	PASS();
out:
	ipc_close(good);
	ipc_close(slow);
	free(path);
	server_stop();
restore:
	mserver_outq_hard_limit = 4u << 20;
	mserver_outq_high_water = 1u << 20;
	mserver_outq_low_water = 256u * 1024;
}

/* The point of roles: a view-only client watches and cannot change
 * anything, and the client that has the keyboard is unaffected. */
static void
test_view_only(void)
{
	char *path;
	int writer = -1, viewer = -1;
	uint8_t role = IPC_ROLE_WRITE;
	uint32_t type, len;
	char buf[IPC_MAX_PAYLOAD];
	int rc;

	TEST("a view-only client cannot type");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	writer = client_attach_as(path, 24, 80, 0, &role);
	if (writer < 0 || role != IPC_ROLE_WRITE) {
		FAIL("the first client was not granted write");
		goto out;
	}

	viewer = client_attach_as(path, 24, 80, IPC_ATTACH_F_VIEW, &role);
	if (viewer < 0) {
		FAIL("view-only attach failed");
		goto out;
	}
	if (role != IPC_ROLE_VIEW) {
		FAIL("asking for view-only did not grant view-only");
		goto out;
	}

	/* the viewer's input must not reach the PTY, and must come back as
	 * an error rather than vanishing.  the replay and the PTY flags
	 * arrive first, so read past them. */
	if (client_input(viewer, "echo LUMI-FORBIDDEN\n") < 0) {
		FAIL("send failed");
		goto out;
	}
	rc = 0;
	{
		struct pollfd pfd;
		int i;

		for (i = 0; i < 25 && !rc; i++) {
			pfd.fd = viewer;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 200) <= 0)
				continue;
			if (ipc_msg_recv(viewer, &type, buf, sizeof(buf) - 1,
			    &len) != 0)
				break;
			if (type == IPC_MSG_ERROR)
				rc = 1;
		}
	}
	if (!rc) {
		FAIL("refused input produced no error");
		goto out;
	}

	/* the writer must not see the viewer's text appear */
	if (client_input(writer, "echo LUMI-ALLOWED\n") < 0 ||
	    client_expect(writer, "LUMI-ALLOWED", 5000) != 1) {
		FAIL("the writer stopped working");
		goto out;
	}
	if (client_expect(writer, "LUMI-FORBIDDEN", 500) == 1) {
		FAIL("the viewer's input reached the PTY");
		goto out;
	}
	PASS();
out:
	ipc_close(writer);
	ipc_close(viewer);
	free(path);
	server_stop();
}

/* a second client that wants to type is told it cannot, rather than
 * silently sharing the keyboard */
static void
test_second_writer_is_viewer(void)
{
	char *path;
	int first = -1, second = -1;
	uint8_t role = IPC_ROLE_VIEW;

	TEST("a second client asking to type gets view-only");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	first = client_attach_as(path, 24, 80, 0, &role);
	if (first < 0 || role != IPC_ROLE_WRITE) {
		FAIL("the first client was not granted write");
		goto out;
	}
	second = client_attach_as(path, 24, 80, 0, &role);
	if (second < 0) {
		FAIL("second attach failed");
		goto out;
	}
	if (role != IPC_ROLE_VIEW) {
		FAIL("two clients were both granted write");
		goto out;
	}

	/* when the writer leaves, the keyboard is offered to the client
	 * that wanted it */
	ipc_close(first);
	first = -1;
	{
		uint32_t type, len;
		char buf[256];
		struct pollfd pfd;
		int got = 0, i;

		for (i = 0; i < 25 && !got; i++) {
			pfd.fd = second;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 200) <= 0)
				continue;
			if (ipc_msg_recv(second, &type, buf, sizeof(buf),
			    &len) != 0)
				break;
			if (type == IPC_MSG_ROLE_CHANGE && len >= 1 &&
			    (uint8_t)buf[0] == IPC_ROLE_WRITE)
				got = 1;
		}
		if (!got) {
			FAIL("no role change after the writer left");
			goto out;
		}
	}

	if (client_input(second, "echo LUMI-PROMOTED\n") < 0 ||
	    client_expect(second, "LUMI-PROMOTED", 5000) != 1) {
		FAIL("the promoted client still cannot type");
		goto out;
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* Passing the keyboard must not need a reattach: the client that has taken
 * the session token says so, and the server moves it. */
static void
test_role_request(void)
{
	char *path;
	int first = -1, second = -1;
	uint8_t role = IPC_ROLE_VIEW;
	uint8_t claim = IPC_ATTACH_F_TOKEN;
	int demoted = 0, promoted = 0, i;

	TEST("a token claim moves the keyboard without reattaching");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	first = client_attach_as(path, 24, 80, IPC_ATTACH_F_TOKEN, &role);
	if (first < 0 || role != IPC_ROLE_WRITE) {
		FAIL("the token holder was not granted write");
		goto out;
	}
	second = client_attach_as(path, 24, 80, 0, &role);
	if (second < 0 || role != IPC_ROLE_VIEW) {
		FAIL("the second client should be view-only");
		goto out;
	}

	/* the second client takes the token and announces it */
	if (ipc_msg_send(second, IPC_MSG_ROLE_REQUEST, &claim, 1) < 0) {
		FAIL("role request failed");
		goto out;
	}

	/* both ends should hear: one promoted, one demoted */
	for (i = 0; i < 25 && !(promoted && demoted); i++) {
		struct pollfd pfd[2];
		char buf[256];
		uint32_t type, len;

		pfd[0].fd = second;
		pfd[0].events = POLLIN;
		pfd[1].fd = first;
		pfd[1].events = POLLIN;
		if (poll(pfd, 2, 200) <= 0)
			continue;
		if ((pfd[0].revents & POLLIN) &&
		    ipc_msg_recv(second, &type, buf, sizeof(buf), &len) == 0 &&
		    type == IPC_MSG_ROLE_CHANGE && len >= 1 &&
		    (uint8_t)buf[0] == IPC_ROLE_WRITE)
			promoted = 1;
		if ((pfd[1].revents & POLLIN) &&
		    ipc_msg_recv(first, &type, buf, sizeof(buf), &len) == 0 &&
		    type == IPC_MSG_ROLE_CHANGE && len >= 1 &&
		    (uint8_t)buf[0] == IPC_ROLE_VIEW)
			demoted = 1;
	}
	if (!promoted) {
		FAIL("the claiming client was not given the keyboard");
		goto out;
	}
	if (!demoted) {
		FAIL("the previous holder was not told it lost it");
		goto out;
	}

	/* and the keyboard really did move */
	if (client_input(second, "echo LUMI-HANDOVER\n") < 0 ||
	    client_expect(second, "LUMI-HANDOVER", 5000) != 1) {
		FAIL("the new holder cannot type");
		goto out;
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* Presence is symmetric: a client that only watches still learns who else
 * is here, or a session could be observed without the people in it being
 * aware of each other. */
static void
test_client_events(void)
{
	char *path;
	int first = -1, second = -1;
	int i, joined = 0, left = 0;

	TEST("attached clients are told who joins and leaves");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	first = client_attach_as(path, 24, 80, 0, NULL);
	if (first < 0) {
		FAIL("attach failed");
		goto out;
	}
	second = client_attach_as(path, 24, 80, IPC_ATTACH_F_VIEW, NULL);
	if (second < 0) {
		FAIL("second attach failed");
		goto out;
	}

	/* the first client hears about the second arriving */
	for (i = 0; i < 25 && !joined; i++) {
		struct pollfd pfd;
		char buf[256];
		uint32_t type, len;

		pfd.fd = first;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		if (ipc_msg_recv(first, &type, buf, sizeof(buf), &len) != 0)
			break;
		if (type == IPC_MSG_CLIENT_EVENT && len >= 5 &&
		    (uint8_t)buf[0] == IPC_CLIENT_JOIN)
			joined = 1;
	}
	if (!joined) {
		FAIL("no join event");
		goto out;
	}

	/* and about it leaving */
	ipc_close(second);
	second = -1;
	for (i = 0; i < 25 && !left; i++) {
		struct pollfd pfd;
		char buf[256];
		uint32_t type, len;

		pfd.fd = first;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		if (ipc_msg_recv(first, &type, buf, sizeof(buf), &len) != 0)
			break;
		if (type == IPC_MSG_CLIENT_EVENT && len >= 5 &&
		    (uint8_t)buf[0] == IPC_CLIENT_LEAVE)
			left = 1;
	}
	if (!left) {
		FAIL("no leave event");
		goto out;
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* One window, several clients, one size: the smallest wins, and a client
 * that opted out of sizing is not counted. */
static void
test_size_negotiation(void)
{
	char *path;
	int big = -1, small = -1, observer = -1;
	struct ipc_attach_reply rep;
	uint32_t type, len;
	char rbuf[IPC_MAX_PAYLOAD];
	uint8_t sbuf[32];
	struct ipc_size sz;
	int n;

	TEST("the window fits the smallest client that asked to be fitted");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}

	big = client_attach_as(path, 40, 120, 0, NULL);
	if (big < 0) {
		FAIL("attach failed");
		goto out;
	}

	/* a smaller client joins: the window has to shrink to fit it */
	small = client_attach_as(path, 24, 80, 0, NULL);
	if (small < 0) {
		FAIL("second attach failed");
		goto out;
	}
	{
		int fd = ipc_connect(path);
		struct ipc_attach at;

		/* a third connection asks the size without changing it */
		at.rows = 10;
		at.cols = 40;
		at.flags = IPC_ATTACH_F_VIEW | IPC_ATTACH_F_SIZE_OBSERVE;
		at.client_id = 999;
		at.name = "observer";
		at.name_len = 8;
		n = ipc_attach_encode(&at, sbuf, sizeof(sbuf));
		if (fd < 0 || n <= 0 ||
		    ipc_msg_send(fd, IPC_MSG_ATTACH, sbuf, (uint32_t)n) < 0) {
			FAIL("observer attach failed");
			ipc_close(fd);
			goto out;
		}
		observer = fd;
		if (ipc_msg_recv(observer, &type, rbuf, sizeof(rbuf),
		    &len) != 0 || type != IPC_MSG_ATTACH_REPLY ||
		    ipc_attach_reply_decode(&rep, (const uint8_t *)rbuf,
		    (int)len) < 0) {
			FAIL("no reply to the observer");
			goto out;
		}
		/* the reply reports the window, which must not have
		 * shrunk to the observer's 10x40 */
		if (rep.rows != 24 || rep.cols != 80) {
			FAIL("an observing client changed the window size");
			goto out;
		}
	}

	/* the smallest participant leaving lets the window grow again */
	ipc_close(small);
	small = -1;
	usleep(200000);
	sz.rows = 40;
	sz.cols = 120;
	n = ipc_size_encode(&sz, sbuf, sizeof(sbuf));
	if (n <= 0 || ipc_msg_send(big, IPC_MSG_WIN_RESIZE, sbuf,
	    (uint32_t)n) < 0) {
		FAIL("resize failed");
		goto out;
	}
	usleep(200000);
	if (ipc_msg_send_empty(big, IPC_MSG_REFRESH) < 0) {
		FAIL("refresh failed");
		goto out;
	}
	{
		/* a fresh connection is told the current window size, which
		 * is how we see that it grew back */
		int fd = ipc_connect(path);
		struct ipc_attach at;

		at.rows = 40;
		at.cols = 120;
		at.flags = IPC_ATTACH_F_VIEW | IPC_ATTACH_F_SIZE_OBSERVE;
		at.client_id = 1000;
		at.name = "probe";
		at.name_len = 5;
		n = ipc_attach_encode(&at, sbuf, sizeof(sbuf));
		if (fd < 0 || n <= 0 ||
		    ipc_msg_send(fd, IPC_MSG_ATTACH, sbuf, (uint32_t)n) < 0 ||
		    ipc_msg_recv(fd, &type, rbuf, sizeof(rbuf), &len) != 0 ||
		    type != IPC_MSG_ATTACH_REPLY ||
		    ipc_attach_reply_decode(&rep, (const uint8_t *)rbuf,
		    (int)len) < 0) {
			FAIL("probe attach failed");
			ipc_close(fd);
			goto out;
		}
		ipc_close(fd);
		if (rep.rows != 40 || rep.cols != 120) {
			FAIL("the window did not grow when the small client "
			    "left");
			goto out;
		}
	}
	PASS();
out:
	ipc_close(big);
	ipc_close(small);
	ipc_close(observer);
	free(path);
	server_stop();
}

/* 13-a: in multi-writer mode, a second (or third) client asking to type
 * gets WRITE instead of VIEW, and nobody already typing is demoted. */
static void
test_multi_writer_mode(void)
{
	char *path;
	int first = -1, second = -1;
	uint8_t role = IPC_ROLE_VIEW;

	TEST("multi-writer mode grants write to more than one client");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}
	if (sessdir_share_mode_set(SESSION, SESSDIR_SHARE_MULTI_WRITER) < 0) {
		FAIL("could not set multi-writer mode");
		goto out;
	}

	first = client_attach_as(path, 24, 80, 0, &role);
	if (first < 0 || role != IPC_ROLE_WRITE) {
		FAIL("the first client was not granted write");
		goto out;
	}
	second = client_attach_as(path, 24, 80, 0, &role);
	if (second < 0) {
		FAIL("second attach failed");
		goto out;
	}
	if (role != IPC_ROLE_WRITE) {
		FAIL("the second client was not granted write in "
		    "multi-writer mode");
		goto out;
	}

	/* the first client must not have been silently demoted */
	{
		uint32_t type, len;
		char buf[256];
		struct pollfd pfd;
		int i;

		for (i = 0; i < 5; i++) {
			pfd.fd = first;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 100) <= 0)
				continue;
			if (ipc_msg_recv(first, &type, buf, sizeof(buf),
			    &len) != 0)
				break;
			if (type == IPC_MSG_ROLE_CHANGE) {
				FAIL("the first writer was demoted");
				goto out;
			}
		}
	}

	if (client_input(first, "echo LUMI-FIRST\n") < 0 ||
	    client_expect(first, "LUMI-FIRST", 5000) != 1) {
		FAIL("the first writer cannot type");
		goto out;
	}
	if (client_input(second, "echo LUMI-SECOND\n") < 0 ||
	    client_expect(second, "LUMI-SECOND", 5000) != 1) {
		FAIL("the second writer cannot type");
		goto out;
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* 13-c: resize_to_fit() already treats every connection alike regardless
 * of role, filtering only on IPC_ATTACH_F_SIZE_OBSERVE -- confirm that
 * holds with two WRITE-role connections, not just a writer and a viewer. */
static void
test_multi_writer_size(void)
{
	char *path;
	int first = -1, second = -1;
	struct ipc_attach_reply rep;
	uint32_t type, len;
	char rbuf[IPC_MAX_PAYLOAD];
	uint8_t sbuf[32];
	int n;

	TEST("two writers negotiate size to the minimum");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}
	if (sessdir_share_mode_set(SESSION, SESSDIR_SHARE_MULTI_WRITER) < 0) {
		FAIL("could not set multi-writer mode");
		goto out;
	}

	first = client_attach_as(path, 40, 120, 0, NULL);
	if (first < 0) {
		FAIL("attach failed");
		goto out;
	}
	second = client_attach_as(path, 24, 80, 0, NULL);
	if (second < 0) {
		FAIL("second attach failed");
		goto out;
	}

	{
		int fd = ipc_connect(path);
		struct ipc_attach at;

		at.rows = 10;
		at.cols = 40;
		at.flags = IPC_ATTACH_F_VIEW | IPC_ATTACH_F_SIZE_OBSERVE;
		at.client_id = 999;
		at.name = "observer";
		at.name_len = 8;
		n = ipc_attach_encode(&at, sbuf, sizeof(sbuf));
		if (fd < 0 || n <= 0 ||
		    ipc_msg_send(fd, IPC_MSG_ATTACH, sbuf, (uint32_t)n) < 0) {
			FAIL("observer attach failed");
			ipc_close(fd);
			goto out;
		}
		if (ipc_msg_recv(fd, &type, rbuf, sizeof(rbuf),
		    &len) != 0 || type != IPC_MSG_ATTACH_REPLY ||
		    ipc_attach_reply_decode(&rep, (const uint8_t *)rbuf,
		    (int)len) < 0) {
			FAIL("no reply to the observer");
			ipc_close(fd);
			goto out;
		}
		ipc_close(fd);
		if (rep.rows != 24 || rep.cols != 80) {
			FAIL("the window was not the minimum of the two "
			    "writers' sizes");
			goto out;
		}
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* 13-b: a run bracketed with INPUT_BEGIN/INPUT_END must reach the PTY as
 * one atomic write, never split by another connection's own input that
 * lands on the mserver's event loop in between. Without the fix, the
 * bytes below would arrive at the shell as one torn line --
 * "echo LUMI-Aecho LUMI-B" -- rather than two clean commands. */
static void
test_input_run_bracketing(void)
{
	char *path;
	int first = -1, second = -1;

	TEST("a bracketed input run reaches the PTY intact, unsplit by "
	    "another writer");

	path = server_start();
	if (!path) {
		FAIL("server did not start");
		return;
	}
	if (sessdir_share_mode_set(SESSION, SESSDIR_SHARE_MULTI_WRITER) < 0) {
		FAIL("could not set multi-writer mode");
		goto out;
	}

	first = client_attach(path, 24, 80);
	second = client_attach(path, 24, 80);
	if (first < 0 || second < 0) {
		FAIL("attach failed");
		goto out;
	}

	/* first opens a run and sends half of a command line -- no
	 * newline yet, so nothing would execute even if this leaked to
	 * the PTY unbracketed */
	if (ipc_msg_send_empty(first, IPC_MSG_INPUT_BEGIN) < 0 ||
	    client_input(first, "echo LUMI-A") < 0) {
		FAIL("could not open the run");
		goto out;
	}
	usleep(200000);	/* let the mserver receive and buffer it */

	/* second, not inside any run, types an ordinary complete command:
	 * this must reach the PTY immediately, cleanly, on its own */
	if (client_input(second, "echo LUMI-B\n") < 0) {
		FAIL("the second writer's input failed to send");
		goto out;
	}
	if (client_expect(first, "LUMI-B", 5000) != 1) {
		FAIL("the interleaved command did not run cleanly");
		goto out;
	}

	/* first finishes and closes its run: the whole line reaches the
	 * PTY as one write, so it executes as a single intact command */
	if (client_input(first, "-END\n") < 0 ||
	    ipc_msg_send_empty(first, IPC_MSG_INPUT_END) < 0) {
		FAIL("could not close the run");
		goto out;
	}
	if (client_expect(first, "LUMI-A-END", 5000) != 1) {
		FAIL("the bracketed run did not reach the PTY intact");
		goto out;
	}
	PASS();
out:
	ipc_close(first);
	ipc_close(second);
	free(path);
	server_stop();
}

/* ---- main ---- */

int
main(void)
{
	char tmpl[] = "/tmp/lumi-mstest-XXXXXX";
	char cmd[PATH_MAX + 16];

	printf("mserver tests:\n");

	/* keep every session this test creates out of the real runtime
	 * directory, so a failure cannot disturb a live lumi session */
	if (!mkdtemp(tmpl)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(runtime_dir, sizeof(runtime_dir), "%s", tmpl);
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);

	/* a dying client must not take the test process with it */
	signal(SIGPIPE, SIG_IGN);

	test_fanout();
	test_table_full();
	test_slow_client_dropped();
	test_view_only();
	test_second_writer_is_viewer();
	test_role_request();
	test_client_events();
	test_size_negotiation();
	test_multi_writer_mode();
	test_multi_writer_size();
	test_input_run_bracketing();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", runtime_dir);
	if (system(cmd) < 0)
		perror("rm -rf");

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
