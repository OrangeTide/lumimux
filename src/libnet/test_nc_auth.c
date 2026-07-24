/* test_nc_auth.c : ssh-shaped userauth state-machine tests */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_auth.h"
#include "monocypher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/*
 * The two state machines run in one process.  Each side's send callback drops
 * its message into a queue tagged with the peer that should receive it; the
 * driver drains the queue and feeds each message, supplying the client's
 * credentials whenever the conversation suspends.  This mirrors what a
 * transport would do without needing one.
 */
#define QMAX 64

struct qmsg {
	struct nc_auth	*dst;
	uint8_t		buf[NC_AUTH_MAX_MSG];
	size_t		len;
};

static struct qmsg q[QMAX];
static int qh, qt;

struct sctx {
	struct nc_auth	*peer;		/* who receives what we send */
};

static void
qsend(void *ctx, const void *msg, size_t len)
{
	struct sctx *s = ctx;

	if (len > NC_AUTH_MAX_MSG || (qt + 1) % QMAX == qh)
		return;			/* overflow: the guard loop will notice */
	q[qt].dst = s->peer;
	memcpy(q[qt].buf, msg, len);
	q[qt].len = len;
	qt = (qt + 1) % QMAX;
}

/* What the client offers when the conversation suspends. */
struct creds {
	const uint8_t	*sk;		/* 64-byte Ed25519 secret, or NULL */
	const uint8_t	*pk;		/* 32-byte public, or NULL */
	int		give_key;	/* supply the key when asked */
	const char	*password;	/* password to supply, or NULL to skip */
};

/* Server policy, backing the nc_auth_server_cb. */
struct srvcfg {
	const uint8_t	*authorized_pk;	/* the one key we accept, or NULL */
	const char	*password;	/* the password we accept, or NULL */
	unsigned	methods;	/* methods offered */
};

static unsigned
srv_methods(void *ctx, const char *user)
{
	(void)user;
	return ((struct srvcfg *)ctx)->methods;
}

static int
srv_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
	struct srvcfg *s = ctx;

	(void)user;
	return s->authorized_pk && memcmp(pk, s->authorized_pk, 32) == 0;
}

static int
srv_check_password(void *ctx, const char *user, const char *password)
{
	struct srvcfg *s = ctx;

	(void)user;
	return s->password && strcmp(password, s->password) == 0;
}

/* Run the whole conversation to a terminal state (or give up). */
static void
drive(struct nc_auth *cli, struct nc_auth *srv, const struct creds *c)
{
	int guard;

	qh = qt = 0;
	nc_auth_start(cli);

	for (guard = 0; guard < 128 &&
	    nc_auth_state(cli) == NC_AUTH_PENDING; guard++) {
		int need = nc_auth_needs(cli);

		if (need == NC_AUTH_NEED_KEY) {
			if (c->give_key)
				nc_auth_supply_key(cli, c->sk, c->pk);
			else
				nc_auth_supply_key(cli, NULL, NULL);
		} else if (need == NC_AUTH_NEED_PASSWORD) {
			if (c->password)
				nc_auth_supply_password(cli, c->password);
			else
				nc_auth_supply_password(cli, NULL);
		}

		while (qh != qt) {
			struct qmsg *m = &q[qh];

			qh = (qh + 1) % QMAX;
			nc_auth_feed(m->dst, m->buf, m->len);
		}
	}
}

/* Deterministic Ed25519 key pair from a fixed seed byte. */
static void
make_key(uint8_t seed_byte, uint8_t sk[64], uint8_t pk[32])
{
	uint8_t seed[32];

	memset(seed, seed_byte, sizeof(seed));
	crypto_eddsa_key_pair(sk, pk, seed);
}

static void
init_pair(struct nc_auth *cli, struct nc_auth *srv, const uint8_t sid_cli[32],
    const uint8_t sid_srv[32], const struct nc_auth_server_cb *scb,
    struct sctx *cc, struct sctx *cs)
{
	cc->peer = srv;
	cs->peer = cli;
	nc_auth_client_init(cli, sid_cli, "alice", qsend, cc);
	nc_auth_server_init(srv, sid_srv, scb, qsend, cs);
}

/* A key on the server's list authenticates in one round. */
static void
test_pubkey_accepted(void)
{
	struct nc_auth cli, srv;
	struct sctx cc, cs;
	uint8_t sid[32], sk[64], pk[32];
	struct srvcfg cfg;
	struct nc_auth_server_cb scb;
	struct creds c;

	TEST("nc_auth: authorized public key logs in");

	memset(sid, 0x77, sizeof(sid));
	make_key(0x55, sk, pk);

	cfg.authorized_pk = pk;
	cfg.password = NULL;
	cfg.methods = NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
	scb.methods = srv_methods;
	scb.check_key = srv_check_key;
	scb.check_password = srv_check_password;
	scb.ctx = &cfg;

	init_pair(&cli, &srv, sid, sid, &scb, &cc, &cs);

	c.sk = sk;
	c.pk = pk;
	c.give_key = 1;
	c.password = NULL;
	drive(&cli, &srv, &c);

	ASSERT(nc_auth_state(&cli) == NC_AUTH_OK, "client not authenticated");
	ASSERT(nc_auth_state(&srv) == NC_AUTH_OK, "server did not accept");
	ASSERT(strcmp(nc_auth_user(&srv), "alice") == 0, "wrong user");
	PASS();
}

/* An unlisted key is refused, and the password fallback then succeeds. */
static void
test_password_fallback(void)
{
	struct nc_auth cli, srv;
	struct sctx cc, cs;
	uint8_t sid[32], sk[64], pk[32];
	struct srvcfg cfg;
	struct nc_auth_server_cb scb;
	struct creds c;

	TEST("nc_auth: password fallback after key refused");

	memset(sid, 0x33, sizeof(sid));
	make_key(0x11, sk, pk);

	cfg.authorized_pk = NULL;		/* no key is accepted */
	cfg.password = "s3cret";
	cfg.methods = NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
	scb.methods = srv_methods;
	scb.check_key = srv_check_key;
	scb.check_password = srv_check_password;
	scb.ctx = &cfg;

	init_pair(&cli, &srv, sid, sid, &scb, &cc, &cs);

	c.sk = sk;
	c.pk = pk;
	c.give_key = 1;				/* tried first, refused */
	c.password = "s3cret";			/* then this wins */
	drive(&cli, &srv, &c);

	ASSERT(nc_auth_state(&cli) == NC_AUTH_OK, "client not authenticated");
	ASSERT(nc_auth_state(&srv) == NC_AUTH_OK, "server did not accept");
	PASS();
}

/* Wrong key and wrong password exhaust every method and deny the login. */
static void
test_all_methods_denied(void)
{
	struct nc_auth cli, srv;
	struct sctx cc, cs;
	uint8_t sid[32], sk[64], pk[32];
	struct srvcfg cfg;
	struct nc_auth_server_cb scb;
	struct creds c;

	TEST("nc_auth: wrong key and password are denied");

	memset(sid, 0x22, sizeof(sid));
	make_key(0x44, sk, pk);

	cfg.authorized_pk = NULL;
	cfg.password = "right";
	cfg.methods = NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
	scb.methods = srv_methods;
	scb.check_key = srv_check_key;
	scb.check_password = srv_check_password;
	scb.ctx = &cfg;

	init_pair(&cli, &srv, sid, sid, &scb, &cc, &cs);

	c.sk = sk;
	c.pk = pk;
	c.give_key = 1;
	c.password = "wrong";
	drive(&cli, &srv, &c);

	ASSERT(nc_auth_state(&cli) == NC_AUTH_DENIED, "client not denied");
	ASSERT(nc_auth_state(&srv) == NC_AUTH_DENIED, "server not denied");
	PASS();
}

/*
 * The security property: a signature is bound to the nc_crypto session id.
 * When client and server hold different session ids (as a man in the middle
 * would produce), the authorized key's signature does not verify, so the
 * server never even consults its key list and the login is denied.
 */
static void
test_session_binding(void)
{
	struct nc_auth cli, srv;
	struct sctx cc, cs;
	uint8_t sid_cli[32], sid_srv[32], sk[64], pk[32];
	uint8_t dig_a[32], dig_b[32];
	struct srvcfg cfg;
	struct nc_auth_server_cb scb;
	struct creds c;

	TEST("nc_auth: signature is bound to the session id");

	memset(sid_cli, 0xAA, sizeof(sid_cli));
	memset(sid_srv, 0xBB, sizeof(sid_srv));	/* different session */
	make_key(0x66, sk, pk);

	/* The signed digest must differ between the two sessions. */
	nc_auth_signed_digest(dig_a, sid_cli, "alice", pk);
	nc_auth_signed_digest(dig_b, sid_srv, "alice", pk);
	ASSERT(memcmp(dig_a, dig_b, 32) != 0,
	    "digest not bound to the session id");

	cfg.authorized_pk = pk;			/* key IS authorized... */
	cfg.password = NULL;
	cfg.methods = NC_AUTH_M_PUBKEY;		/* ...but pubkey is the only way */
	scb.methods = srv_methods;
	scb.check_key = srv_check_key;
	scb.check_password = srv_check_password;
	scb.ctx = &cfg;

	init_pair(&cli, &srv, sid_cli, sid_srv, &scb, &cc, &cs);

	c.sk = sk;
	c.pk = pk;
	c.give_key = 1;
	c.password = NULL;
	drive(&cli, &srv, &c);

	ASSERT(nc_auth_state(&srv) == NC_AUTH_DENIED,
	    "cross-session signature was accepted");
	PASS();
}

int
main(void)
{
	printf("nc_auth tests:\n");

	test_pubkey_accepted();
	test_password_fallback();
	test_all_methods_denied();
	test_session_binding();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
