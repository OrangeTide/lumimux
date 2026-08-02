/* test_proxy.c : end-to-end tests for lumi-proxy (ssh path + 12-b broker) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "proxy_msg.h"
#include "sessdir.h"
#include "sessdir_control.h"

/* entry point under test (declared in src/multicall.h, not on our -I). */
int cmd_proxy_main(int argc, char **argv);

#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

#define SESSION "proxytest"

/* ---- fake mserver: real Unix socket, upper-cases input into output, and
 * records the flags byte of the last ATTACH it decoded so a test can check
 * what the broker asked for on a client's behalf. ---- */

static char flags_path[300];

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

	/* fork per connection, like a real mserver's client fan-out, so a
	 * broker test that connects more than once in sequence is not
	 * itself serialized by this stand-in. */
	signal(SIGCHLD, SIG_IGN);

	for (;;) {
		cfd = ipc_accept(lfd);
		if (cfd < 0)
			_exit(15);

		if (fork() != 0) {
			ipc_close(cfd);
			continue;
		}
		ipc_close(lfd);

		if (ipc_msg_recv(cfd, &type, buf, sizeof(buf), &len) != 0 ||
		    type != IPC_MSG_ATTACH) {
			ipc_close(cfd);
			_exit(0);
		}
		{
			struct ipc_attach at;

			if (ipc_attach_decode(&at, (const uint8_t *)buf,
			    (int)len) >= 0) {
				FILE *f = fopen(flags_path, "w");

				if (f) {
					fprintf(f, "%u\n", (unsigned)at.flags);
					fclose(f);
				}
			}
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

static int
recv_timed(int fd, uint32_t *window_id, uint32_t *type, void *buf,
    size_t bufsz, uint32_t *len, int max_ms)
{
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, max_ms) <= 0)
		return -1;
	return proxy_msg_recv(fd, window_id, type, buf, bufsz, len);
}

static void
broker_sock_path(const char *session, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "/tmp/lumi-broker-%d/%s.sock", (int)getuid(),
	    session);
}

static int
wait_for_socket(const char *path, int max_ms)
{
	int waited = 0;

	while (waited < max_ms) {
		struct stat st;

		if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
			return 0;
		usleep(10000);
		waited += 10;
	}
	return -1;
}

/* write <session>/access with the given content, mode 0600, owned by us
 * (the only mode sessdir_access_check() will trust). */
static int
write_acl(const char *session, const char *content)
{
	char *dir, path[512];
	int fd;

	sessdir_session_create(session);
	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	snprintf(path, sizeof(path), "%s/access", dir);
	free(dir);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (content)
		(void)!write(fd, content, strlen(content));
	close(fd);
	chmod(path, 0600);
	return 0;
}

static uint32_t
decode_ready_wid(const uint8_t *buf, uint32_t len)
{
	if (len < 6)
		return 0;
	return ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
	    ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];
}

/* ---- the ssh/stdio path (regression: attach_mserver() now sends a real
 * IpcAttach instead of a legacy empty ATTACH) ---- */

static void
test_stdio_roundtrip(void)
{
	char tmpl[] = "/tmp/lumi-proxytest-XXXXXX";
	char *rundir;
	pid_t mpid, ppid;
	int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
	uint32_t type, len, wid;
	static uint8_t buf[8192];
	int ok = 0;

	TEST("ssh/stdio path bridges a client end to end");

	rundir = mkdtemp(tmpl);
	if (!rundir) {
		FAIL("mkdtemp failed");
		return;
	}
	setenv("XDG_RUNTIME_DIR", rundir, 1);
	unsetenv("LUMI_SESSION");
	snprintf(flags_path, sizeof(flags_path), "%s/flags", rundir);

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

	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
		FAIL("pipe failed");
		kill(mpid, SIGKILL);
		return;
	}

	ppid = fork();
	if (ppid < 0) {
		FAIL("fork proxy failed");
		kill(mpid, SIGKILL);
		return;
	}
	if (ppid == 0) {
		char *argv[] = { "lumi-proxy", "-s", (char *)SESSION, NULL };

		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		optind = 1;
		_exit(cmd_proxy_main(3, argv));
	}
	close(in_pipe[0]);
	in_pipe[0] = -1;
	close(out_pipe[1]);
	out_pipe[1] = -1;

	if (recv_timed(out_pipe[0], &wid, &type, buf, sizeof(buf), &len,
	    5000) != 0 || type != IPC_MSG_PROXY_READY || wid != 0) {
		FAIL("no PROXY_READY");
		goto done;
	}
	wid = decode_ready_wid(buf, len);
	if (wid != (uint32_t)mpid) {
		FAIL("PROXY_READY window id != mserver pid");
		goto done;
	}

	if (proxy_msg_send(in_pipe[1], wid, IPC_MSG_INPUT, "hello", 5) != 0) {
		FAIL("send input failed");
		goto done;
	}
	if (recv_timed(out_pipe[0], &wid, &type, buf, sizeof(buf), &len,
	    5000) != 0 || type != IPC_MSG_OUTPUT || len != 5 ||
	    memcmp(buf, "HELLO", 5) != 0) {
		FAIL("output mismatch");
		goto done;
	}

	/* the trusted stdio path must still ask for full (non-view) access. */
	{
		FILE *f = fopen(flags_path, "r");
		char line[16];

		if (!f || !fgets(line, sizeof(line), f) || atoi(line) != 0) {
			FAIL("stdio path attached with a view flag set");
			if (f)
				fclose(f);
			goto done;
		}
		fclose(f);
	}

	ok = 1;

done:
	if (in_pipe[1] >= 0)
		close(in_pipe[1]);
	if (out_pipe[0] >= 0)
		close(out_pipe[0]);
	kill(ppid, SIGTERM);
	kill(mpid, SIGTERM);
	waitpid(ppid, NULL, 0);
	waitpid(mpid, NULL, 0);
	{
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", rundir);
		if (system(cmd) != 0)
			; /* ignore */
	}
	if (ok)
		PASS();
}

/* ---- the cross-user broker (-L) ---- */

struct broker_fixture {
	char rundir[128];
	pid_t mpid, ppid;
	char sock[300];
};

static int
broker_start(struct broker_fixture *fx, const char *acl)
{
	char tmpl[] = "/tmp/lumi-proxybroker-XXXXXX";
	char *rundir;

	memset(fx, 0, sizeof(*fx));
	rundir = mkdtemp(tmpl);
	if (!rundir)
		return -1;
	snprintf(fx->rundir, sizeof(fx->rundir), "%s", rundir);
	setenv("XDG_RUNTIME_DIR", fx->rundir, 1);
	unsetenv("LUMI_SESSION");
	snprintf(flags_path, sizeof(flags_path), "%s/flags", fx->rundir);

	if (acl && write_acl(SESSION, acl) != 0)
		return -1;

	fx->mpid = fork();
	if (fx->mpid < 0)
		return -1;
	if (fx->mpid == 0) {
		fake_mserver();
		_exit(99);
	}
	usleep(100000);

	fx->ppid = fork();
	if (fx->ppid < 0) {
		kill(fx->mpid, SIGKILL);
		return -1;
	}
	if (fx->ppid == 0) {
		char *argv[] = { "lumi-proxy", "-s", (char *)SESSION, "-L",
		    NULL };

		optind = 1;
		_exit(cmd_proxy_main(4, argv));
	}

	broker_sock_path(SESSION, fx->sock, sizeof(fx->sock));
	if (wait_for_socket(fx->sock, 3000) != 0)
		return -1;
	return 0;
}

static void
broker_stop(struct broker_fixture *fx)
{
	if (fx->ppid > 0) {
		kill(fx->ppid, SIGTERM);
		waitpid(fx->ppid, NULL, 0);
	}
	if (fx->mpid > 0) {
		kill(fx->mpid, SIGKILL);
		waitpid(fx->mpid, NULL, 0);
	}
	/* fx->ppid is only the listener; each served connection is its own
	 * forked grandchild, reparented away and unreachable by this
	 * process's own waitpid() once the listener exits. Give a served
	 * connection's own client-disconnect path a moment to finish before
	 * the directory it is still touching gets removed out from under
	 * it -- harmless to the connection either way (the test's own
	 * assertions have already run by the time broker_stop() is called),
	 * but a stray "cannot create" from a write that lost the race is
	 * noise a clean test run should not produce. */
	usleep(200000);
	if (fx->rundir[0]) {
		char cmd[256];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fx->rundir);
		if (system(cmd) != 0)
			; /* ignore */
	}
}

static void
test_broker_owner_full_access(void)
{
	struct broker_fixture fx;
	int cfd = -1, ok = 0;
	uint32_t type, len, wid;
	static uint8_t buf[8192];

	TEST("broker grants the owner full access with no acl file");

	if (broker_start(&fx, NULL) != 0) {
		FAIL("broker did not start");
		broker_stop(&fx);
		return;
	}

	cfd = ipc_connect(fx.sock);
	if (cfd < 0) {
		FAIL("connect to broker failed");
		goto done;
	}
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 5000) != 0 ||
	    type != IPC_MSG_PROXY_READY) {
		FAIL("owner was not granted PROXY_READY");
		goto done;
	}
	wid = decode_ready_wid(buf, len);
	if (wid != (uint32_t)fx.mpid) {
		FAIL("PROXY_READY window id mismatch");
		goto done;
	}
	if (proxy_msg_send(cfd, wid, IPC_MSG_INPUT, "hi", 2) != 0) {
		FAIL("send input failed");
		goto done;
	}
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 5000) != 0 ||
	    type != IPC_MSG_OUTPUT || len != 2 || memcmp(buf, "HI", 2) != 0) {
		FAIL("output mismatch");
		goto done;
	}

	{
		FILE *f = fopen(flags_path, "r");
		char line[16];

		if (!f || !fgets(line, sizeof(line), f) || atoi(line) != 0) {
			FAIL("broker asked for view when it should have "
			    "asked for full access");
			if (f)
				fclose(f);
			goto done;
		}
		fclose(f);
	}

	ok = 1;
done:
	if (cfd >= 0)
		close(cfd);
	broker_stop(&fx);
	if (ok)
		PASS();
}

static void
test_broker_acl_denies(void)
{
	struct broker_fixture fx;
	int cfd = -1, ok = 0;
	uint32_t type, len, wid;
	static uint8_t buf[8192];

	TEST("broker denies per the acl and leaks nothing first");

	/* denies everyone, including the owner: this is the same code path
	 * a foreign uid would take, just exercised with our own real uid
	 * since a unit test cannot forge kernel-reported credentials. */
	if (broker_start(&fx, "* deny\n") != 0) {
		FAIL("broker did not start");
		broker_stop(&fx);
		return;
	}

	cfd = ipc_connect(fx.sock);
	if (cfd < 0) {
		FAIL("connect to broker failed");
		goto done;
	}
	/* fail-closed rule 5: a denied connection gets no session content
	 * at all -- not even PROXY_READY -- before the broker hangs up. */
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 2000) == 0) {
		FAIL("denied connection received a message");
		goto done;
	}
	ok = 1;
done:
	if (cfd >= 0)
		close(cfd);
	broker_stop(&fx);
	if (ok)
		PASS();
}

static void
test_broker_view_ceiling_clamps_role(void)
{
	struct broker_fixture fx;
	int cfd = -1, ok = 0;
	uint32_t type, len, wid;
	static uint8_t buf[8192];

	TEST("broker clamps to the acl's view ceiling");

	if (broker_start(&fx, "owner view\n* deny\n") != 0) {
		FAIL("broker did not start");
		broker_stop(&fx);
		return;
	}

	cfd = ipc_connect(fx.sock);
	if (cfd < 0) {
		FAIL("connect to broker failed");
		goto done;
	}
	/* view is still access: PROXY_READY must still arrive. */
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 5000) != 0 ||
	    type != IPC_MSG_PROXY_READY) {
		FAIL("view-ceiling connection was not granted PROXY_READY");
		goto done;
	}

	{
		FILE *f = fopen(flags_path, "r");
		char line[16];

		if (!f || !fgets(line, sizeof(line), f) ||
		    (atoi(line) & IPC_ATTACH_F_VIEW) == 0) {
			FAIL("broker did not ask the mserver for view-only");
			if (f)
				fclose(f);
			goto done;
		}
		fclose(f);
	}

	/* 12-d-a: the broker registers this connection into the session-wide
	 * roster on the client's behalf, since the far end has no local
	 * session directory to write into itself. */
	{
		struct sessdir_client list[SESSDIR_CLIENT_MAX];
		int n = sessdir_client_list(SESSION, list, SESSDIR_CLIENT_MAX);
		int found = 0, i;

		for (i = 0; i < n; i++) {
			if (strcmp(list[i].role, "view") == 0 &&
			    strncmp(list[i].who, "uid:", 4) == 0)
				found = 1;
		}
		if (!found) {
			FAIL("broker client did not appear in the roster");
			goto done;
		}
	}

	ok = 1;
done:
	if (cfd >= 0)
		close(cfd);

	/* the served connection is a separate forked grandchild from the
	 * broker listener fx.ppid tracks, so give it a moment to notice the
	 * close and unregister before declaring the roster entry stuck. */
	if (ok) {
		int waited;

		for (waited = 0; waited < 20; waited++) {
			struct sessdir_client list[SESSDIR_CLIENT_MAX];
			int n = sessdir_client_list(SESSION, list,
			    SESSDIR_CLIENT_MAX);

			if (n == 0)
				break;
			usleep(50000);
		}
		if (waited == 20) {
			FAIL("broker client's roster entry outlived the connection");
			ok = 0;
		}
	}

	broker_stop(&fx);
	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-c: an "ask" match with no one able to approve it is
 * still an immediate denial -- fail-closed rule 3 -- not a wasted wait.
 * The test connects as its own uid (a unit test cannot forge another),
 * so the "owner" subject stands in for whatever identity is being asked
 * about; what matters is that "ask" alone, with an empty roster and no
 * write-token holder, refuses rather than hangs.
 */
static void
test_broker_pending_no_approver(void)
{
	struct broker_fixture fx;
	int cfd = -1, ok = 0;
	uint32_t type, len, wid;
	static uint8_t buf[8192];

	TEST("broker denies an ask match with no one available to approve");

	if (broker_start(&fx, "owner view ask\n* deny\n") != 0) {
		FAIL("broker did not start");
		broker_stop(&fx);
		return;
	}

	cfd = ipc_connect(fx.sock);
	if (cfd < 0) {
		FAIL("connect to broker failed");
		goto done;
	}
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 2000) == 0) {
		FAIL("ask match with no approver was admitted");
		goto done;
	}

	ok = 1;
done:
	if (cfd >= 0)
		close(cfd);
	broker_stop(&fx);
	if (ok)
		PASS();
}

/*
 * FUTURE.md step 12-d-c: an "ask" match with a write-token holder present
 * is admitted pending rather than denied, receives nothing until
 * sessdir_ctl_post() approves it (the way `lumi share -a id` would), and
 * proceeds exactly like an ordinary admission once approved.
 */
static void
test_broker_pending_approve(void)
{
	struct broker_fixture fx;
	int cfd = -1, ok = 0, waited;
	uint32_t type, len, wid;
	static uint8_t buf[8192];
	unsigned long pending_id = 0;

	TEST("broker admits an ask match as pending and releases it on approval");

	if (broker_start(&fx, "owner view ask\n* deny\n") != 0) {
		FAIL("broker did not start");
		broker_stop(&fx);
		return;
	}

	if (sessdir_token_acquire(SESSION, 999999, "owner") != 0) {
		FAIL("cannot acquire the write token");
		broker_stop(&fx);
		return;
	}

	cfd = ipc_connect(fx.sock);
	if (cfd < 0) {
		FAIL("connect to broker failed");
		goto done;
	}

	/* nothing arrives while pending -- checked with a short bound. */
	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 800) == 0) {
		FAIL("pending client received content before approval");
		goto done;
	}

	for (waited = 0; waited < 20; waited++) {
		struct sessdir_client list[SESSDIR_CLIENT_MAX];
		int n = sessdir_client_list(SESSION, list, SESSDIR_CLIENT_MAX);
		int i;

		for (i = 0; i < n; i++) {
			if (list[i].pending &&
			    strncmp(list[i].who, "uid:", 4) == 0) {
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
		goto done;
	}

	if (sessdir_ctl_post(SESSION, SESSDIR_CTL_APPROVE, pending_id, 0,
	    "test") != 0) {
		FAIL("could not post the approval");
		goto done;
	}

	if (recv_timed(cfd, &wid, &type, buf, sizeof(buf), &len, 5000) != 0 ||
	    type != IPC_MSG_PROXY_READY) {
		FAIL("approved client never received PROXY_READY");
		goto done;
	}

	{
		struct sessdir_client list[SESSDIR_CLIENT_MAX];
		int n = sessdir_client_list(SESSION, list, SESSDIR_CLIENT_MAX);
		int i, found = 0;

		for (i = 0; i < n; i++) {
			if (list[i].client_id != pending_id)
				continue;
			found = 1;
			if (list[i].pending ||
			    strcmp(list[i].role, "view") != 0) {
				FAIL("roster entry did not clear pending on "
				    "approval");
				goto done;
			}
		}
		if (!found) {
			FAIL("approved client vanished from the roster");
			goto done;
		}
	}

	ok = 1;
done:
	if (cfd >= 0)
		close(cfd);
	sessdir_token_release();
	broker_stop(&fx);
	if (ok)
		PASS();
}

int
main(void)
{
	printf("lumi-proxy tests:\n");

	test_stdio_roundtrip();
	test_broker_owner_full_access();
	test_broker_acl_denies();
	test_broker_view_ceiling_clamps_role();
	test_broker_pending_no_approver();
	test_broker_pending_approve();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
