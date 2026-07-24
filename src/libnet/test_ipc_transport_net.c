/* test_ipc_transport_net.c : round-trip test for the netchan ipc_transport */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Step-4 coverage for FUTURE.md 11C: drive the netchan-backed
 * ipc_transport over a real loopback UDP link between two processes and
 * confirm TLV messages survive intact, including payloads larger than
 * netchan's per-message limit (which exercise the chunk/reassemble path)
 * and payloads near IPC_MAX_PAYLOAD.
 */

#include "ipc_transport_netchan.h"
#include "ipc_transport.h"
#include "nc_udp.h"
#include "nc_auth.h"
#include "monocypher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

#define MSG_ECHO	0x1234
#define MSG_QUIT	0xDEAD

/* payload sizes to round-trip: small, one MTU worth (fragments inside a
 * single chunk), several chunks, and near the TLV maximum. */
static const uint32_t sizes[] = { 0, 5, 1500, 5000, 60000 };
#define NSIZES		((int)(sizeof(sizes) / sizeof(sizes[0])))

/* fill buf[len] with a size-stamped deterministic pattern. */
static void
fill(uint8_t *buf, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)((len * 31 + i * 7) & 0xff);
}

/* create a UDP socket bound to 127.0.0.1:0 and report the bound port. */
static int
bind_loopback(uint16_t *port)
{
	struct sockaddr_in sin;
	socklen_t slen;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		close(fd);
		return -1;
	}
	slen = sizeof(sin);
	if (getsockname(fd, (struct sockaddr *)&sin, &slen) != 0) {
		close(fd);
		return -1;
	}
	*port = ntohs(sin.sin_port);
	return fd;
}

/* server child: accept one connection and echo every MSG_ECHO back until
 * MSG_QUIT arrives. exits 0 on clean run, non-zero on failure. `nonblock`
 * selects the blocking recv() path or the poll()/pump()/try_recv() path. */
static void
server_child(int fd, int nonblock, const uint8_t *psk, int crypto)
{
	static uint8_t buf[64 * 1024];
	struct ipc_transport *t;
	uint32_t type, len;

	t = crypto ? ipc_transport_netchan_new_crypto(fd, 1, NULL, psk)
	    : ipc_transport_netchan_new(fd, 1, NULL);
	if (!t)
		_exit(1);
	if (ipc_transport_netchan_establish(t, 5000) != 0)
		_exit(2);

	if (!nonblock) {
		for (;;) {
			int r = ipc_transport_recv(t, &type, buf,
			    sizeof(buf), &len);

			if (r != 0)
				_exit(3);
			if (type == MSG_QUIT)
				break;
			if (type != MSG_ECHO)
				_exit(4);
			if (ipc_transport_send(t, type, buf, len) != 0)
				_exit(5);
		}
		ipc_transport_free(t);
		_exit(0);
	}

	/* event-loop style: never block in recv. */
	for (;;) {
		struct pollfd pfd;
		int to, r;

		to = ipc_transport_netchan_timeout(t);
		if (to < 0 || to > 200)
			to = 200;
		pfd.fd = ipc_transport_get_fd(t);
		pfd.events = POLLIN;
		pfd.revents = 0;
		(void)poll(&pfd, 1, to);

		if (ipc_transport_netchan_pump(t) != 0)
			_exit(6);

		for (;;) {
			r = ipc_transport_netchan_try_recv(t, &type, buf,
			    sizeof(buf), &len);
			if (r == 1)
				break;		/* nothing ready */
			if (r < 0) {
				ipc_transport_free(t);
				_exit(7);	/* peer gone before QUIT */
			}
			if (type == MSG_QUIT) {
				ipc_transport_free(t);
				_exit(0);
			}
			if (type != MSG_ECHO)
				_exit(8);
			if (ipc_transport_send(t, type, buf, len) != 0)
				_exit(9);
		}
	}
}

/* parent client: connect, send each size, verify the echo is byte-exact. */
static int
client_run(int fd, uint16_t server_port, const uint8_t *psk, int crypto)
{
	static uint8_t out[64 * 1024];
	static uint8_t in[64 * 1024];
	struct ipc_transport *t;
	struct sockaddr_in sin;
	struct nc_addr peer;
	uint32_t type, len;
	int i, ok = 1;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(server_port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0)
		return 0;

	t = crypto ? ipc_transport_netchan_new_crypto(fd, 0, &peer, psk)
	    : ipc_transport_netchan_new(fd, 0, &peer);
	if (!t)
		return 0;
	if (ipc_transport_netchan_establish(t, 5000) != 0) {
		ipc_transport_free(t);
		return 0;
	}

	for (i = 0; i < NSIZES && ok; i++) {
		uint32_t n = sizes[i];

		fill(out, n);
		if (ipc_transport_send(t, MSG_ECHO, out, n) != 0) {
			ok = 0;
			break;
		}
		if (ipc_transport_recv(t, &type, in, sizeof(in), &len) != 0) {
			ok = 0;
			break;
		}
		if (type != MSG_ECHO || len != n ||
		    (n && memcmp(in, out, n) != 0))
			ok = 0;
	}

	(void)ipc_transport_send_empty(t, MSG_QUIT);
	ipc_transport_free(t);
	return ok;
}

/* fork a server child (blocking or non-blocking path) and run the client
 * against it, echoing every size. returns 1 on a clean, byte-exact run. */
static int
run_roundtrip(int nonblock, int crypto)
{
	/* fixed PSK so both ends mutually authenticate in the test. */
	static const uint8_t psk[32] = {
		0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
		0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
		0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
	};
	uint16_t port = 0;
	int sfd, cfd;
	pid_t pid;
	int status, ok;

	sfd = bind_loopback(&port);
	cfd = bind_loopback(&(uint16_t){ 0 });
	if (sfd < 0 || cfd < 0) {
		if (sfd >= 0)
			close(sfd);
		if (cfd >= 0)
			close(cfd);
		return 0;
	}

	pid = fork();
	if (pid < 0) {
		close(sfd);
		close(cfd);
		return 0;
	}
	if (pid == 0) {
		close(cfd);
		server_child(sfd, nonblock, psk, crypto);	/* no return */
		_exit(99);
	}

	close(sfd);			/* child owns the server socket */
	ok = client_run(cfd, port, psk, crypto);
	if (waitpid(pid, &status, 0) != pid)
		return 0;
	if (!ok)
		return 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 0;
	return 1;
}

/* encrypted client that roams: establish, exchange one message, swap to a
 * fresh socket, and confirm a second message still round-trips.  proves the
 * peer migrates without a re-handshake.  `self` selects the socket source:
 * 0 rebinds to a caller-made socket (ipc_transport_netchan_rebind), 1 uses
 * the transport's own fresh socket (ipc_transport_netchan_roam), the path
 * the attach client's automatic roaming drives.  returns 1 on a clean,
 * byte-exact run. */
static int
roam_client(int fd, uint16_t server_port, const uint8_t *psk, int self)
{
	uint8_t in[64];
	struct ipc_transport *t;
	struct sockaddr_in sin;
	struct nc_addr peer;
	uint32_t type, len;
	int ok = 1;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(server_port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0)
		return 0;

	t = ipc_transport_netchan_new_crypto(fd, 0, &peer, psk);
	if (!t)
		return 0;
	if (ipc_transport_netchan_establish(t, 5000) != 0) {
		ipc_transport_free(t);
		return 0;
	}

	/* first exchange over the original socket. */
	if (ipc_transport_send(t, MSG_ECHO, "one", 3) != 0 ||
	    ipc_transport_recv(t, &type, in, sizeof(in), &len) != 0 ||
	    type != MSG_ECHO || len != 3 || memcmp(in, "one", 3) != 0)
		ok = 0;

	/* roam to a new source address. */
	if (self) {
		if (ipc_transport_netchan_roam(t) < 0)
			ok = 0;
	} else {
		uint16_t newport = 0;
		int nfd = bind_loopback(&newport);

		if (nfd < 0 || newport == server_port ||
		    ipc_transport_netchan_rebind(t, nfd) != 0)
			ok = 0;
	}

	/* second exchange must survive the address change. */
	if (ok && (ipc_transport_send(t, MSG_ECHO, "two", 3) != 0 ||
	    ipc_transport_recv(t, &type, in, sizeof(in), &len) != 0 ||
	    type != MSG_ECHO || len != 3 || memcmp(in, "two", 3) != 0))
		ok = 0;

	(void)ipc_transport_send_empty(t, MSG_QUIT);
	ipc_transport_free(t);
	return ok;
}

static int
run_roaming(int self)
{
	/* same fixed PSK as run_roundtrip's encrypted cases. */
	static const uint8_t psk[32] = {
		0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
		0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
		0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
	};
	uint16_t port = 0;
	int sfd, cfd, status, ok;
	pid_t pid;

	sfd = bind_loopback(&port);
	cfd = bind_loopback(&(uint16_t){ 0 });
	if (sfd < 0 || cfd < 0) {
		if (sfd >= 0)
			close(sfd);
		if (cfd >= 0)
			close(cfd);
		return 0;
	}

	pid = fork();
	if (pid < 0) {
		close(sfd);
		close(cfd);
		return 0;
	}
	if (pid == 0) {
		close(cfd);
		server_child(sfd, 0, psk, 1);	/* never returns */
		_exit(99);
	}

	close(sfd);
	ok = roam_client(cfd, port, psk, self);
	if (waitpid(pid, &status, 0) != pid)
		return 0;
	if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 0;
	return 1;
}

static void
test_netchan_transport_roundtrip(void)
{
	TEST("netchan ipc_transport TLV round-trip over loopback UDP");
	if (run_roundtrip(0, 0))
		PASS();
	else
		FAIL("blocking round-trip failed");
}

static void
test_netchan_transport_nonblocking(void)
{
	TEST("netchan ipc_transport round-trip via non-blocking drain");
	if (run_roundtrip(1, 0))
		PASS();
	else
		FAIL("event-loop drain round-trip failed");
}

static void
test_netchan_transport_encrypted(void)
{
	TEST("netchan ipc_transport encrypted (PSK) round-trip");
	if (run_roundtrip(0, 1))
		PASS();
	else
		FAIL("encrypted blocking round-trip failed");
}

static void
test_netchan_transport_encrypted_nonblocking(void)
{
	TEST("netchan ipc_transport encrypted round-trip via drain");
	if (run_roundtrip(1, 1))
		PASS();
	else
		FAIL("encrypted event-loop drain round-trip failed");
}

static void
test_netchan_transport_roaming(void)
{
	TEST("netchan ipc_transport roams to a new source address");
	if (run_roaming(0))
		PASS();
	else
		FAIL("session did not survive an address change");
}

static void
test_netchan_transport_roaming_self(void)
{
	TEST("netchan ipc_transport self-roam (transport-made socket)");
	if (run_roaming(1))
		PASS();
	else
		FAIL("session did not survive a self-roam");
}

/* ---- user-authentication phase over the loopback link ---- */

/* One client identity, generated once and shared with the forked child. */
static uint8_t g_client_sk[64];
static uint8_t g_client_pk[32];
#define AUTH_PASSWORD	"corr3ct"

/* Server policy: offer both methods, accept the one client key and the one
 * password. */
static unsigned
auth_methods(void *ctx, const char *user)
{
	(void)ctx;
	(void)user;
	return NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
}

static int
auth_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
	(void)ctx;
	(void)user;
	return memcmp(pk, g_client_pk, 32) == 0;
}

static int
auth_check_password(void *ctx, const char *user, const char *password)
{
	(void)ctx;
	(void)user;
	return strcmp(password, AUTH_PASSWORD) == 0;
}

/* Client credential source, selected per scenario. */
struct cred_cfg {
	int		give_key;	/* present the authorized key */
	const char	*password;	/* password to offer, or NULL to skip */
};

static int
cred_get_key(void *ctx, uint8_t sk[64], uint8_t pk[32])
{
	struct cred_cfg *c = ctx;

	if (!c->give_key)
		return -1;
	memcpy(sk, g_client_sk, 64);
	memcpy(pk, g_client_pk, 32);
	return 0;
}

static int
cred_get_password(void *ctx, char *buf, size_t sz)
{
	struct cred_cfg *c = ctx;

	if (!c->password)
		return -1;
	snprintf(buf, sz, "%s", c->password);
	return 0;
}

static const uint8_t auth_psk[32] = {
	0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
	0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
	0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
};

/* Server side: establish with a userauth phase, then echo one message. */
static void
auth_server_child(int fd)
{
	static uint8_t buf[4096];
	struct ipc_netchan_userauth ua = {
		.methods = auth_methods,
		.check_key = auth_check_key,
		.check_password = auth_check_password,
	};
	struct ipc_netchan_auth auth = { .psk = auth_psk, .userauth = &ua };
	struct ipc_transport *t;
	uint32_t type, len;

	t = ipc_transport_netchan_new_crypto_auth(fd, 1, NULL, &auth);
	if (!t)
		_exit(1);
	if (ipc_transport_netchan_establish(t, 5000) != 0)
		_exit(2);		/* auth denied */

	if (ipc_transport_recv(t, &type, buf, sizeof(buf), &len) != 0)
		_exit(3);
	if (type == MSG_ECHO)
		(void)ipc_transport_send(t, type, buf, len);
	ipc_transport_free(t);
	_exit(0);
}

/*
 * Fork an authenticating server and run a client with the given credentials.
 * When expect_ok, require establish to succeed and one message to round-trip;
 * otherwise require both ends to reject the login.  Returns 1 if the run
 * matched the expectation.
 */
static int
run_auth(struct cred_cfg *cc, int expect_ok)
{
	struct ipc_netchan_userauth ua = {
		.user = "alice",
		.get_key = cred_get_key,
		.get_password = cred_get_password,
		.cred_ctx = cc,
	};
	struct ipc_netchan_auth auth = { .psk = auth_psk, .userauth = &ua };
	struct sockaddr_in sin;
	struct nc_addr peer;
	struct ipc_transport *t;
	uint16_t port = 0, cport = 0;
	int sfd, cfd, status, established, ok;
	pid_t pid;

	sfd = bind_loopback(&port);
	cfd = bind_loopback(&cport);
	if (sfd < 0 || cfd < 0) {
		if (sfd >= 0)
			close(sfd);
		if (cfd >= 0)
			close(cfd);
		return 0;
	}

	pid = fork();
	if (pid < 0) {
		close(sfd);
		close(cfd);
		return 0;
	}
	if (pid == 0) {
		close(cfd);
		auth_server_child(sfd);		/* no return */
		_exit(99);
	}
	close(sfd);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		close(cfd);
		waitpid(pid, NULL, 0);
		return 0;
	}

	t = ipc_transport_netchan_new_crypto_auth(cfd, 0, &peer, &auth);
	if (!t) {
		close(cfd);
		waitpid(pid, NULL, 0);
		return 0;
	}

	established = (ipc_transport_netchan_establish(t, 5000) == 0);
	ok = 0;
	if (expect_ok && established) {
		uint8_t out[64], in[64];
		uint32_t type, len;

		memset(out, 0x5A, sizeof(out));
		ok = ipc_transport_send(t, MSG_ECHO, out, sizeof(out)) == 0 &&
		    ipc_transport_recv(t, &type, in, sizeof(in), &len) == 0 &&
		    type == MSG_ECHO && len == sizeof(out) &&
		    memcmp(in, out, sizeof(out)) == 0;
	} else if (!expect_ok) {
		ok = !established;	/* the login must be refused */
	}
	ipc_transport_free(t);

	waitpid(pid, &status, 0);
	if (!WIFEXITED(status))
		return 0;
	/* server exits 0 on a good auth+echo, 2 when it rejects the login. */
	if (expect_ok && WEXITSTATUS(status) != 0)
		return 0;
	if (!expect_ok && WEXITSTATUS(status) == 0)
		return 0;
	return ok;
}

static void
test_userauth_pubkey(void)
{
	struct cred_cfg cc = { .give_key = 1, .password = NULL };

	TEST("netchan userauth: authorized key logs in and TLV flows");
	if (run_auth(&cc, 1))
		PASS();
	else
		FAIL("pubkey login or round-trip failed");
}

static void
test_userauth_password(void)
{
	struct cred_cfg cc = { .give_key = 0, .password = AUTH_PASSWORD };

	TEST("netchan userauth: password fallback logs in");
	if (run_auth(&cc, 1))
		PASS();
	else
		FAIL("password login failed");
}

static void
test_userauth_denied(void)
{
	struct cred_cfg cc = { .give_key = 0, .password = "wrong" };

	TEST("netchan userauth: bad credentials are refused");
	if (run_auth(&cc, 0))
		PASS();
	else
		FAIL("bad login was not refused");
}

int
main(void)
{
	uint8_t seed[32];

	printf("netchan ipc_transport tests:\n");

	memset(seed, 0x5C, sizeof(seed));
	crypto_eddsa_key_pair(g_client_sk, g_client_pk, seed);

	test_netchan_transport_roundtrip();
	test_netchan_transport_nonblocking();
	test_netchan_transport_encrypted();
	test_netchan_transport_encrypted_nonblocking();
	test_netchan_transport_roaming();
	test_netchan_transport_roaming_self();
	test_userauth_pubkey();
	test_userauth_password();
	test_userauth_denied();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
