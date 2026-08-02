/* test_ipc.c : tests for libipc */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "microser.h"
#include "str.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* ---- helpers ---- */

/* create a socketpair suitable for IPC message testing */
static void
make_pair(int fds[2])
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(1);
	}
	fds[0] = sv[0];
	fds[1] = sv[1];
}

/* ---- tests ---- */

static void
test_send_recv_empty(void)
{
	int fds[2];
	uint32_t type, len;
	char buf[64];
	int rc;

	TEST("send/recv empty message");

	make_pair(fds);

	rc = ipc_msg_send_empty(fds[0], IPC_MSG_DETACH);
	ASSERT(rc == 0, "send failed");

	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv failed");
	ASSERT(type == IPC_MSG_DETACH, "wrong type");
	ASSERT(len == 0, "expected zero-length payload");

	close(fds[0]);
	close(fds[1]);
	PASS();
}

static void
test_send_recv_payload(void)
{
	int fds[2];
	uint32_t type, len;
	char buf[256];
	const char *msg = "hello, lumi";
	int rc;

	TEST("send/recv with payload");

	make_pair(fds);

	rc = ipc_msg_send(fds[0], IPC_MSG_INPUT, msg, (uint32_t)strlen(msg));
	ASSERT(rc == 0, "send failed");

	memset(buf, 0, sizeof(buf));
	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv failed");
	ASSERT(type == IPC_MSG_INPUT, "wrong type");
	ASSERT(len == strlen(msg), "wrong length");
	ASSERT(memcmp(buf, msg, len) == 0, "payload mismatch");

	close(fds[0]);
	close(fds[1]);
	PASS();
}

static void
test_send_recv_size(void)
{
	int fds[2];
	uint32_t type, len;
	uint8_t buf[64];
	struct ipc_size sz;
	int rc;

	TEST("send/recv size message (attach/resize)");

	make_pair(fds);

	rc = ipc_msg_send_size(fds[0], IPC_MSG_ATTACH, 40, 132);
	ASSERT(rc == 0, "send failed");

	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv failed");
	ASSERT(type == IPC_MSG_ATTACH, "wrong type");

	rc = ipc_size_decode(&sz, buf, (int)len);
	ASSERT(rc >= 0, "decode failed");
	ASSERT(sz.rows == 40, "wrong rows");
	ASSERT(sz.cols == 132, "wrong cols");

	close(fds[0]);
	close(fds[1]);
	PASS();
}

static void
test_recv_eof(void)
{
	int fds[2];
	uint32_t type, len;
	char buf[64];
	int rc;

	TEST("recv returns 1 on EOF");

	make_pair(fds);
	close(fds[0]); /* close sender */

	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 1, "expected EOF return (1)");

	close(fds[1]);
	PASS();
}

static void
test_multiple_messages(void)
{
	int fds[2];
	uint32_t type, len;
	char buf[256];
	const char *m1 = "first";
	const char *m2 = "second";
	int rc;

	TEST("multiple messages in sequence");

	make_pair(fds);

	rc = ipc_msg_send(fds[0], IPC_MSG_INPUT, m1, (uint32_t)strlen(m1));
	ASSERT(rc == 0, "send m1 failed");
	rc = ipc_msg_send(fds[0], IPC_MSG_OUTPUT, m2, (uint32_t)strlen(m2));
	ASSERT(rc == 0, "send m2 failed");

	memset(buf, 0, sizeof(buf));
	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv m1 failed");
	ASSERT(type == IPC_MSG_INPUT, "m1 wrong type");
	ASSERT(len == strlen(m1), "m1 wrong length");
	ASSERT(memcmp(buf, m1, len) == 0, "m1 payload mismatch");

	memset(buf, 0, sizeof(buf));
	rc = ipc_msg_recv(fds[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv m2 failed");
	ASSERT(type == IPC_MSG_OUTPUT, "m2 wrong type");
	ASSERT(len == strlen(m2), "m2 wrong length");
	ASSERT(memcmp(buf, m2, len) == 0, "m2 payload mismatch");

	close(fds[0]);
	close(fds[1]);
	PASS();
}

static void
test_socket_path(void)
{
	char *path;

	TEST("ipc_socket_path returns valid path");

	path = ipc_socket_path("test-session");
	ASSERT(path != NULL, "socket_path returned NULL");
	ASSERT(strstr(path, "test-session") != NULL,
	    "path doesn't contain session name");
	ASSERT(strstr(path, ".sock") != NULL,
	    "path doesn't end with .sock");

	free(path);
	PASS();
}

static void
test_listen_connect(void)
{
	char *path;
	int listen_fd, client_fd, server_fd;

	TEST("listen + connect + accept");

	path = ipc_socket_path("test-listen");
	ASSERT(path != NULL, "socket_path returned NULL");

	listen_fd = ipc_listen(path);
	ASSERT(listen_fd >= 0, "listen failed");

	client_fd = ipc_connect(path);
	ASSERT(client_fd >= 0, "connect failed");

	server_fd = ipc_accept(listen_fd);
	ASSERT(server_fd >= 0, "accept failed");

	/* verify we can send a message through */
	{
		uint32_t type, len;
		char buf[64];
		int rc;

		rc = ipc_msg_send_empty(client_fd, IPC_MSG_KILL);
		ASSERT(rc == 0, "send through socket failed");

		rc = ipc_msg_recv(server_fd, &type, buf, sizeof(buf), &len);
		ASSERT(rc == 0, "recv through socket failed");
		ASSERT(type == IPC_MSG_KILL, "wrong type through socket");
	}

	ipc_close(server_fd);
	ipc_close(client_fd);
	ipc_close(listen_fd);
	ipc_unlink(path);
	free(path);
	PASS();
}

/* the listening socket must be 0600 no matter what umask the caller had,
 * so the directory mode is not the only thing standing between the socket
 * and another local user. */
static void
test_listen_socket_mode(void)
{
	char *path;
	int listen_fd;
	struct stat st;
	mode_t old;

	TEST("listen socket is created 0600");

	path = ipc_socket_path("test-mode");
	ASSERT(path != NULL, "socket_path returned NULL");

	old = umask(0);		/* the hostile case: everything permitted */
	listen_fd = ipc_listen(path);
	umask(old);
	ASSERT(listen_fd >= 0, "listen failed");

	ASSERT(stat(path, &st) == 0, "stat failed");
	ASSERT(S_ISSOCK(st.st_mode), "not a socket");
	ASSERT((st.st_mode & 07777) == 0600, "socket is not mode 0600");

	ipc_close(listen_fd);
	ipc_unlink(path);
	free(path);
	PASS();
}

/* the credentials reported for a socketpair are our own on every platform
 * that reports any at all. */
static void
test_peer_cred_pair(void)
{
	int fds[2];
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = 0;
	int rc;

	TEST("peer credentials on a socketpair");

	make_pair(fds);

	rc = ipc_peer_cred(fds[0], &uid, &gid, &pid);
#ifdef IPC_HAVE_PEER_CRED
	ASSERT(rc == 0, "ipc_peer_cred failed");
	ASSERT(uid == getuid(), "wrong peer uid");
	ASSERT(gid == getgid(), "wrong peer gid");
	/* pid is -1 where the platform does not report one */
	ASSERT(pid == -1 || pid == getpid(), "wrong peer pid");

	/* every out parameter is optional */
	rc = ipc_peer_cred(fds[1], NULL, NULL, NULL);
	ASSERT(rc == 0, "ipc_peer_cred with NULL outputs failed");
#else
	ASSERT(rc == -1, "unsupported platform must fail");
	ASSERT(errno == ENOTSUP, "unsupported platform must set ENOTSUP");
#endif

	close(fds[0]);
	close(fds[1]);
	PASS();
}

/* the same, over a real listen/connect/accept, which is the path the
 * servers actually use.  both ends see the other's credentials. */
static void
test_peer_cred_listen(void)
{
	char *path;
	int listen_fd, client_fd, server_fd;
	uid_t suid = (uid_t)-1, cuid = (uid_t)-1;
	int rc;

	TEST("peer credentials over listen + connect");

	path = ipc_socket_path("test-peercred");
	ASSERT(path != NULL, "socket_path returned NULL");

	listen_fd = ipc_listen(path);
	ASSERT(listen_fd >= 0, "listen failed");

	client_fd = ipc_connect(path);
	ASSERT(client_fd >= 0, "connect failed");

	server_fd = ipc_accept(listen_fd);
	ASSERT(server_fd >= 0, "accept failed");

	rc = ipc_peer_cred(server_fd, &suid, NULL, NULL);
#ifdef IPC_HAVE_PEER_CRED
	ASSERT(rc == 0, "server side ipc_peer_cred failed");
	ASSERT(suid == getuid(), "wrong uid seen by the server");

	rc = ipc_peer_cred(client_fd, &cuid, NULL, NULL);
	ASSERT(rc == 0, "client side ipc_peer_cred failed");
	ASSERT(cuid == getuid(), "wrong uid seen by the client");
#else
	ASSERT(rc == -1, "unsupported platform must fail");
#endif

	ipc_close(server_fd);
	ipc_close(client_fd);
	ipc_close(listen_fd);
	ipc_unlink(path);
	free(path);
	PASS();
}

/* an fd that is not a connected socket has no credentials to report, and
 * must not be mistaken for one belonging to us. */
static void
test_peer_cred_not_a_socket(void)
{
	int fd;
	uid_t uid = 0;
	int rc;

	TEST("peer credentials on a non-socket fd");

	fd = open("/dev/null", O_RDONLY);
	ASSERT(fd >= 0, "cannot open /dev/null");

	rc = ipc_peer_cred(fd, &uid, NULL, NULL);
	ASSERT(rc == -1, "expected failure on a non-socket fd");

	close(fd);
	PASS();
}

static void
test_forward_compat(void)
{
	struct ipc_size sz;
	uint8_t buf[64];
	int pos, rc;

	TEST("forward compatibility: skip unknown fields");

	/* hand-craft a message with fields 1, 2 (known) and 7 (unknown u32) */
	pos = 2;
	pos = ms_write_tag_u16(buf, pos, sizeof(buf), 1, 25);
	pos = ms_write_tag_u32(buf, pos, sizeof(buf), 7, 0xDEADBEEF);
	pos = ms_write_tag_u16(buf, pos, sizeof(buf), 2, 80);
	/* write length prefix */
	buf[0] = (uint8_t)((pos - 2) & 0xff);
	buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);

	rc = ipc_size_decode(&sz, buf, pos);
	ASSERT(rc >= 0, "decode with unknown field failed");
	ASSERT(sz.rows == 25, "wrong rows after skip");
	ASSERT(sz.cols == 80, "wrong cols after skip");

	PASS();
}

/* The shared attach handshake extends the size message rather than
 * replacing it, so a new client works against an old server and the
 * reverse. That only holds while tags 1 and 2 stay rows and cols in all
 * three messages, which is what these two tests pin down. */
static void
test_attach_reads_as_size(void)
{
	struct ipc_attach at;
	struct ipc_size sz;
	uint8_t buf[128];
	int n;

	TEST("an old server reads IpcAttach as IpcSize");

	at.rows = 40;
	at.cols = 132;
	at.flags = IPC_ATTACH_F_VIEW;
	at.client_id = 12345;
	at.name = "someone@somewhere";
	at.name_len = (uint16_t)strlen(at.name);
	n = ipc_attach_encode(&at, buf, sizeof(buf));
	ASSERT(n > 0, "encode failed");

	ASSERT(ipc_size_decode(&sz, buf, n) >= 0, "size decode failed");
	ASSERT(sz.rows == 40, "wrong rows");
	ASSERT(sz.cols == 132, "wrong cols");

	PASS();
}

static void
test_reply_reads_as_size(void)
{
	struct ipc_attach_reply rep;
	struct ipc_size sz;
	struct ipc_attach_reply back;
	uint8_t buf[64];
	int n;

	TEST("an old client reads IpcAttachReply as IpcSize");

	rep.rows = 24;
	rep.cols = 80;
	rep.role = IPC_ROLE_VIEW;
	rep.nclients = 3;
	n = ipc_attach_reply_encode(&rep, buf, sizeof(buf));
	ASSERT(n > 0, "encode failed");

	ASSERT(ipc_size_decode(&sz, buf, n) >= 0, "size decode failed");
	ASSERT(sz.rows == 24, "wrong rows");
	ASSERT(sz.cols == 80, "wrong cols");

	/* and the other direction: a size message from an old server
	 * decodes as a reply granting WRITE, which is what it would do */
	sz.rows = 25;
	sz.cols = 81;
	n = ipc_size_encode(&sz, buf, sizeof(buf));
	ASSERT(n > 0, "size encode failed");
	ASSERT(ipc_attach_reply_decode(&back, buf, n) >= 0,
	    "reply decode failed");
	ASSERT(back.rows == 25 && back.cols == 81, "wrong size");
	ASSERT(back.role == IPC_ROLE_WRITE, "absent role must mean WRITE");
	ASSERT(back.nclients == 0, "absent count must be zero");

	PASS();
}

/* a peer's name is drawn on our screen, so it must not be able to carry
 * escape sequences there */
static void
test_name_sanitizing(void)
{
	char out[32];

	TEST("control sequences are stripped from a name");

	str_sanitize(out, "a\033[31mb\r\nc\177d", sizeof(out));
	ASSERT(strcmp(out, "a?[31mb??c?d") == 0, "sanitize left controls");

	str_sanitize(out, "plain@host", sizeof(out));
	ASSERT(strcmp(out, "plain@host") == 0, "sanitize damaged a name");

	/* UTF-8 survives: the bytes above 0x7f are not terminal controls */
	str_sanitize(out, "\303\251@host", sizeof(out));
	ASSERT(strcmp(out, "\303\251@host") == 0, "sanitize broke UTF-8");

	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libipc tests:\n");

	test_send_recv_empty();
	test_send_recv_payload();
	test_send_recv_size();
	test_recv_eof();
	test_multiple_messages();
	test_socket_path();
	test_listen_connect();
	test_listen_socket_mode();
	test_peer_cred_pair();
	test_peer_cred_listen();
	test_peer_cred_not_a_socket();
	test_forward_compat();
	test_attach_reads_as_size();
	test_reply_reads_as_size();
	test_name_sanitizing();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
