/* test_ipc.c : tests for libipc */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "lumi_msg.h"
#include "microser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
	test_forward_compat();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
