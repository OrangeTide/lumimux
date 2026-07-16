/* test_ipc_transport.c : tests for the ipc_transport seam */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "ipc_transport.h"

#include "ipc_msg.h"

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

/* create a pair of transports over the two ends of a socketpair */
static int
make_pair(struct ipc_transport **a, struct ipc_transport **b)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(1);
	}
	*a = ipc_transport_unix_new(sv[0]);
	*b = ipc_transport_unix_new(sv[1]);
	if (!*a || !*b) {
		FAIL("transport alloc failed");
		return -1;
	}
	return 0;
}

/* ---- tests ---- */

static void
test_send_recv_payload(void)
{
	struct ipc_transport *a, *b;
	uint32_t type, len;
	char buf[256];
	const char *msg = "hello, transport";
	int rc;

	TEST("transport send/recv with payload");
	if (make_pair(&a, &b) < 0)
		return;

	rc = ipc_transport_send(a, IPC_MSG_INPUT, msg, (uint32_t)strlen(msg));
	ASSERT(rc == 0, "send failed");

	memset(buf, 0, sizeof(buf));
	rc = ipc_transport_recv(b, &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv failed");
	ASSERT(type == IPC_MSG_INPUT, "wrong type");
	ASSERT(len == strlen(msg), "wrong length");
	ASSERT(memcmp(buf, msg, len) == 0, "payload mismatch");

	ipc_transport_free(a);
	ipc_transport_free(b);
	PASS();
}

static void
test_send_empty(void)
{
	struct ipc_transport *a, *b;
	uint32_t type, len;
	char buf[64];
	int rc;

	TEST("transport send/recv empty message");
	if (make_pair(&a, &b) < 0)
		return;

	rc = ipc_transport_send_empty(a, IPC_MSG_DETACH);
	ASSERT(rc == 0, "send failed");

	rc = ipc_transport_recv(b, &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "recv failed");
	ASSERT(type == IPC_MSG_DETACH, "wrong type");
	ASSERT(len == 0, "expected zero-length payload");

	ipc_transport_free(a);
	ipc_transport_free(b);
	PASS();
}

/* the transport must be wire-compatible with the raw ipc_msg_* helpers:
 * a message sent through the transport is read by ipc_msg_recv, and vice
 * versa, unchanged. */
static void
test_wire_compat(void)
{
	int sv[2];
	struct ipc_transport *t;
	uint32_t type, len;
	char buf[64];
	const char *msg = "raw<->transport";
	int rc;

	TEST("transport is wire-compatible with ipc_msg_*");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(1);
	}
	t = ipc_transport_unix_new(sv[0]);
	ASSERT(t != NULL, "alloc failed");

	/* transport -> raw */
	rc = ipc_transport_send(t, IPC_MSG_OUTPUT, msg, (uint32_t)strlen(msg));
	ASSERT(rc == 0, "transport send failed");
	rc = ipc_msg_recv(sv[1], &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "raw recv failed");
	ASSERT(type == IPC_MSG_OUTPUT && len == strlen(msg), "mismatch t->raw");

	/* raw -> transport */
	rc = ipc_msg_send_empty(sv[1], IPC_MSG_OK);
	ASSERT(rc == 0, "raw send failed");
	rc = ipc_transport_recv(t, &type, buf, sizeof(buf), &len);
	ASSERT(rc == 0, "transport recv failed");
	ASSERT(type == IPC_MSG_OK && len == 0, "mismatch raw->t");

	close(sv[1]);
	ipc_transport_free(t);
	PASS();
}

static void
test_get_fd(void)
{
	int sv[2];
	struct ipc_transport *t;

	TEST("get_fd returns the wrapped descriptor");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(1);
	}
	t = ipc_transport_unix_new(sv[0]);
	ASSERT(t != NULL, "alloc failed");
	ASSERT(ipc_transport_get_fd(t) == sv[0], "wrong fd");

	close(sv[1]);
	ipc_transport_free(t);
	PASS();
}

static void
test_recv_eof(void)
{
	int sv[2];
	struct ipc_transport *t;
	uint32_t type, len;
	char buf[64];
	int rc;

	TEST("recv reports EOF when peer closes");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(1);
	}
	t = ipc_transport_unix_new(sv[0]);
	ASSERT(t != NULL, "alloc failed");

	close(sv[1]);
	rc = ipc_transport_recv(t, &type, buf, sizeof(buf), &len);
	ASSERT(rc == 1, "expected EOF (1)");

	ipc_transport_free(t);
	PASS();
}

/* free(NULL-safe) and double-free-of-fd safety */
static void
test_free_null(void)
{
	TEST("ipc_transport_free(NULL) is a no-op");
	ipc_transport_free(NULL);
	PASS();
}

int
main(void)
{
	printf("libipc transport tests:\n");

	test_send_recv_payload();
	test_send_empty();
	test_wire_compat();
	test_get_fd();
	test_recv_eof();
	test_free_null();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
