/* test_netchan.c : smoke test for the extracted libnet transport */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Step-2 smoke coverage for FUTURE.md 11C: confirm the extracted netchan
 * core, the nc_crypto decorator (and thus vendored Monocypher), and the
 * nc_udp backend all compile, link, and function at a basic level. The
 * thorough loss/reorder stress of the reliable channel is step 3.
 */

#include "netchan.h"
#include "nc_crypto.h"
#include "nc_udp.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

/* pack a host-order IPv4 + port into an opaque nc_addr (a routing token;
 * no real socket is used in these loopback tests). */
static struct nc_addr
make_addr(uint32_t ip, uint16_t port)
{
	struct nc_addr a;

	memset(&a, 0, sizeof(a));
	a.len = 7;
	a.a[0] = 4;
	a.a[1] = (ip >> 24) & 0xff;
	a.a[2] = (ip >> 16) & 0xff;
	a.a[3] = (ip >> 8) & 0xff;
	a.a[4] = ip & 0xff;
	a.a[5] = (port >> 8) & 0xff;
	a.a[6] = port & 0xff;
	return a;
}

/* drain every pending outgoing packet from `from` into `to` */
static int
pump(struct netchan_conn *from, struct netchan_conn *to,
    const struct nc_addr *from_addr)
{
	uint8_t buf[2048];
	struct nc_addr dst;
	int count = 0;
	size_t n;

	while ((n = netchan_send_next(from, buf, sizeof(buf), &dst)) != 0) {
		netchan_feed(to, buf, n, from_addr);
		count++;
	}
	return count;
}

static void
pump_both(struct netchan_conn *client, struct netchan_conn *server,
    const struct nc_addr *caddr, const struct nc_addr *saddr)
{
	int i;

	for (i = 0; i < 10; i++) {
		int c2s = pump(client, server, caddr);
		int s2c = pump(server, client, saddr);

		if (c2s == 0 && s2c == 0)
			break;
	}
}

/* ---- tests ---- */

/* netchan core: handshake to CONNECTED both ways */
static void
test_handshake(void)
{
	struct netchan_conn *client, *server;
	struct nc_addr caddr, saddr;
	struct netchan_event ev;

	TEST("netchan handshake reaches CONNECTED");

	client = netchan_open(0);
	server = netchan_open(1);
	ASSERT(client && server, "alloc failed");

	caddr = make_addr(0x7f000001, 10000);
	saddr = make_addr(0x7f000001, 20000);

	netchan_connect(client, &saddr);
	pump(client, server, &caddr);
	netchan_accept(server);
	ASSERT(netchan_state(server) == NETCHAN_STATE_CONNECTED,
	    "server not connected");
	while (netchan_poll(server, &ev)) {}

	pump(server, client, &saddr);
	ASSERT(netchan_state(client) == NETCHAN_STATE_CONNECTED,
	    "client not connected");

	netchan_close(client);
	netchan_close(server);
	PASS();
}

/* netchan core: a reliable channel delivers a payload intact */
static void
test_reliable_roundtrip(void)
{
	struct netchan_conn *client, *server;
	struct nc_addr caddr, saddr;
	struct netchan_event ev;
	struct netchan_chan *ch, *sch = NULL;
	const char *msg = "reliable payload over libnet";
	char buf[256];
	int rd;

	TEST("netchan reliable channel round-trip");

	client = netchan_open(0);
	server = netchan_open(1);
	ASSERT(client && server, "alloc failed");

	caddr = make_addr(0x7f000001, 10001);
	saddr = make_addr(0x7f000001, 20001);

	netchan_connect(client, &saddr);
	pump(client, server, &caddr);
	netchan_accept(server);
	pump_both(client, server, &caddr, &saddr);
	ASSERT(netchan_state(client) == NETCHAN_STATE_CONNECTED, "not connected");
	while (netchan_poll(client, &ev)) {}
	while (netchan_poll(server, &ev)) {}

	ch = netchan_chan_open(client, NETCHAN_RELIABLE, NETCHAN_DIR_SEND,
	    "smoke");
	ASSERT(ch != NULL, "chan_open failed");
	pump_both(client, server, &caddr, &saddr);
	while (netchan_poll(server, &ev)) {}
	while (netchan_poll(client, &ev)) {}

	ASSERT(netchan_chan_write(ch, msg, strlen(msg)) == (int)strlen(msg),
	    "write failed");
	pump_both(client, server, &caddr, &saddr);

	while (netchan_poll(server, &ev)) {
		if (ev.type == NETCHAN_EV_DATA && ev.ch)
			sch = ev.ch;
	}
	ASSERT(sch != NULL, "no data event on server");

	rd = netchan_chan_read(sch, buf, sizeof(buf));
	ASSERT(rd == (int)strlen(msg), "read wrong size");
	ASSERT(memcmp(buf, msg, (size_t)rd) == 0, "payload mismatch");

	netchan_close(client);
	netchan_close(server);
	PASS();
}

/* nc_crypto (and Monocypher): handshake, then seal/open a datagram.
 * fixed seeds keep it deterministic and off the OS RNG. */
static void
test_crypto_seal_open(void)
{
	struct nc_crypto a, b;
	uint8_t s0[32], s1[32];
	uint8_t hp[128], scratch[64];
	const char *msg = "encrypted payload";
	uint8_t sealed[128], out[128];
	long sn, pn;
	int i;

	TEST("nc_crypto handshake + seal/open (links Monocypher)");

	memset(s0, 0x11, sizeof(s0));
	memset(s1, 0x22, sizeof(s1));
	struct nc_crypto_cfg cfg_a = { .eph_sk_seed = s0 };
	struct nc_crypto_cfg cfg_b = { .eph_sk_seed = s1 };
	ASSERT(nc_crypto_init(&a, 0, &cfg_a) == 0, "init a failed");
	ASSERT(nc_crypto_init(&b, 1, &cfg_b) == 0, "init b failed");

	for (i = 0; i < 4 && !(nc_crypto_ready(&a) && nc_crypto_ready(&b));
	    i++) {
		size_t n = nc_crypto_handshake_packet(&a, hp, sizeof(hp));

		nc_crypto_open(&b, hp, n, scratch, sizeof(scratch));
		n = nc_crypto_handshake_packet(&b, hp, sizeof(hp));
		nc_crypto_open(&a, hp, n, scratch, sizeof(scratch));
	}
	ASSERT(nc_crypto_ready(&a) && nc_crypto_ready(&b),
	    "handshake did not complete");

	sn = nc_crypto_seal(&a, (const uint8_t *)msg, strlen(msg), sealed,
	    sizeof(sealed));
	ASSERT(sn > 0, "seal failed");
	pn = nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out));
	ASSERT(pn == (long)strlen(msg), "open wrong size");
	ASSERT(memcmp(out, msg, strlen(msg)) == 0, "decrypted mismatch");
	PASS();
}

/* ---- static-identity authentication (server host key + verify_peer) ---- */

/* Drive an nc_crypto handshake between initiator a and responder b until both
 * are ready, either has failed, or a bounded number of rounds elapse.  The
 * caller inspects nc_crypto_ready() / nc_crypto_failed() afterward. */
static void
crypto_handshake(struct nc_crypto *a, struct nc_crypto *b)
{
	uint8_t hp[128], scratch[128];
	int i;

	for (i = 0; i < 6 && !(nc_crypto_ready(a) && nc_crypto_ready(b)) &&
	    !nc_crypto_failed(a) && !nc_crypto_failed(b); i++) {
		size_t n = nc_crypto_handshake_packet(a, hp, sizeof(hp));

		nc_crypto_open(b, hp, n, scratch, sizeof(scratch));
		n = nc_crypto_handshake_packet(b, hp, sizeof(hp));
		nc_crypto_open(a, hp, n, scratch, sizeof(scratch));
	}
}

/* Records what the verify_peer callback saw, and dictates its verdict. */
struct vcb_ctx {
	int	calls;		/* how many times the callback fired */
	int	saw_key;	/* the peer presented a (non-zero) identity key */
	int	decision;	/* value the callback returns: 0 accept, else refuse */
	uint8_t	seen[32];	/* the key the peer presented */
};

static int
vcb(void *ctx, const uint8_t *peer_static_pk)
{
	struct vcb_ctx *v = ctx;

	v->calls++;
	if (peer_static_pk) {
		memcpy(v->seen, peer_static_pk, 32);
		v->saw_key = 1;
	} else {
		v->saw_key = 0;
	}
	return v->decision;
}

/* A server presents its long-term identity key; the client's verify_peer
 * accepts it, and the session comes up.  The callback must see exactly the
 * server's public key, once. */
static void
test_crypto_identity_accept(void)
{
	struct nc_crypto a, b;
	struct vcb_ctx v = { .decision = 0 };
	uint8_t s0[32], s1[32], host_sk[32], host_pk[32];
	const char *msg = "authenticated payload";
	uint8_t sealed[128], out[128];
	long sn, pn;
	struct nc_crypto_cfg cfg_a, cfg_b;

	TEST("nc_crypto static identity: accepted key completes handshake");

	memset(s0, 0x11, sizeof(s0));
	memset(s1, 0x22, sizeof(s1));
	memset(host_sk, 0x33, sizeof(host_sk));
	nc_crypto_identity_public(host_pk, host_sk);

	memset(&cfg_a, 0, sizeof(cfg_a));
	cfg_a.eph_sk_seed = s0;
	cfg_a.verify_peer = vcb;
	cfg_a.verify_ctx = &v;
	cfg_a.require_peer_static = 1;

	memset(&cfg_b, 0, sizeof(cfg_b));
	cfg_b.eph_sk_seed = s1;
	cfg_b.static_sk = host_sk;

	ASSERT(nc_crypto_init(&a, 0, &cfg_a) == 0, "init client failed");
	ASSERT(nc_crypto_init(&b, 1, &cfg_b) == 0, "init server failed");

	crypto_handshake(&a, &b);

	ASSERT(nc_crypto_ready(&a) && nc_crypto_ready(&b),
	    "handshake did not complete");
	ASSERT(!nc_crypto_failed(&a), "client reported failure");
	ASSERT(v.calls == 1, "verify_peer not called exactly once");
	ASSERT(v.saw_key, "verify_peer saw no identity key");
	ASSERT(memcmp(v.seen, host_pk, 32) == 0,
	    "verify_peer saw the wrong key");

	sn = nc_crypto_seal(&a, (const uint8_t *)msg, strlen(msg), sealed,
	    sizeof(sealed));
	ASSERT(sn > 0, "seal failed");
	pn = nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out));
	ASSERT(pn == (long)strlen(msg) && memcmp(out, msg, strlen(msg)) == 0,
	    "authenticated channel did not round-trip");
	PASS();
}

/* When the client's verify_peer refuses the presented key, the client's
 * session fails and never becomes ready. */
static void
test_crypto_identity_refused(void)
{
	struct nc_crypto a, b;
	struct vcb_ctx v = { .decision = -1 };
	uint8_t s0[32], s1[32], host_sk[32];
	struct nc_crypto_cfg cfg_a, cfg_b;

	TEST("nc_crypto static identity: refused key fails the client");

	memset(s0, 0x11, sizeof(s0));
	memset(s1, 0x22, sizeof(s1));
	memset(host_sk, 0x44, sizeof(host_sk));

	memset(&cfg_a, 0, sizeof(cfg_a));
	cfg_a.eph_sk_seed = s0;
	cfg_a.verify_peer = vcb;
	cfg_a.verify_ctx = &v;

	memset(&cfg_b, 0, sizeof(cfg_b));
	cfg_b.eph_sk_seed = s1;
	cfg_b.static_sk = host_sk;

	ASSERT(nc_crypto_init(&a, 0, &cfg_a) == 0, "init client failed");
	ASSERT(nc_crypto_init(&b, 1, &cfg_b) == 0, "init server failed");

	crypto_handshake(&a, &b);

	ASSERT(v.calls == 1, "verify_peer not called");
	ASSERT(nc_crypto_failed(&a), "client did not fail on refusal");
	ASSERT(!nc_crypto_ready(&a), "client became ready despite refusal");
	PASS();
}

/* A client that requires an identity key refuses a server that presents
 * none, without a verify_peer callback ever running. */
static void
test_crypto_identity_required_missing(void)
{
	struct nc_crypto a, b;
	struct vcb_ctx v = { .decision = 0 };
	uint8_t s0[32], s1[32];
	struct nc_crypto_cfg cfg_a, cfg_b;

	TEST("nc_crypto static identity: required-but-absent key is refused");

	memset(s0, 0x11, sizeof(s0));
	memset(s1, 0x22, sizeof(s1));

	memset(&cfg_a, 0, sizeof(cfg_a));
	cfg_a.eph_sk_seed = s0;
	cfg_a.verify_peer = vcb;
	cfg_a.verify_ctx = &v;
	cfg_a.require_peer_static = 1;

	memset(&cfg_b, 0, sizeof(cfg_b));	/* server has no static_sk */
	cfg_b.eph_sk_seed = s1;

	ASSERT(nc_crypto_init(&a, 0, &cfg_a) == 0, "init client failed");
	ASSERT(nc_crypto_init(&b, 1, &cfg_b) == 0, "init server failed");

	crypto_handshake(&a, &b);

	ASSERT(nc_crypto_failed(&a), "client accepted an anonymous server");
	ASSERT(!nc_crypto_ready(&a), "client became ready without a host key");
	ASSERT(v.calls == 0, "verify_peer ran for an absent key");
	PASS();
}

/* nc_udp backend: pack a sockaddr into nc_addr and back */
static void
test_udp_addr_roundtrip(void)
{
	struct sockaddr_in sin;
	struct sockaddr_storage ss;
	struct nc_addr a;
	socklen_t slen;

	TEST("nc_udp sockaddr <-> nc_addr round-trip");

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(4242);
	sin.sin_addr.s_addr = htonl(0x7f000001);

	ASSERT(nc_udp_from_sockaddr(&a, (const struct sockaddr *)&sin,
	    sizeof(sin)) == 0, "from_sockaddr failed");
	ASSERT(a.len > 0, "empty nc_addr");

	slen = nc_udp_to_sockaddr(&a, &ss);
	ASSERT(slen > 0, "to_sockaddr failed");
	ASSERT(((struct sockaddr *)&ss)->sa_family == AF_INET, "wrong family");
	ASSERT(((struct sockaddr_in *)&ss)->sin_port == htons(4242),
	    "port not preserved");
	PASS();
}

/* ---- reliable-channel stress under loss and reorder ---- */

/* CLOCK_MONOTONIC in ms, matching netchan's internal nc_now_ms().  the
 * virtual clock must be seeded from this: netchan stamps first-send times
 * with the real monotonic clock, so a virtual clock starting at 0 would
 * make elapsed-time math underflow and fire retransmits every round. */
static uint32_t
mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* deterministic PRNG so a failing case is always reproducible */
struct rng {
	uint32_t s;
};

static uint32_t
rng_next(struct rng *r)
{
	r->s ^= r->s << 13;
	r->s ^= r->s >> 17;
	r->s ^= r->s << 5;
	return r->s;
}

/* one captured packet in transit */
struct pkt {
	uint8_t buf[2048];
	size_t len;
};

/*
 * Drain up to cap packets out of `from`, optionally reorder them, drop
 * some per the loss policy, and feed the survivors into `to`.
 *   drop_period > 0 : deterministically drop every Nth packet (global g)
 *   drop_pct    > 0 : drop with that percent chance (uses rng)
 * Returns the number of packets delivered.
 */
static int
lossy_pump(struct netchan_conn *from, struct netchan_conn *to,
    const struct nc_addr *from_addr, int reorder, struct rng *rng,
    int drop_period, int drop_pct, unsigned *g)
{
	static struct pkt pkts[128];
	int order[128];
	struct nc_addr dst;
	int n = 0, delivered = 0, i;

	while (n < 128) {
		size_t m = netchan_send_next(from, pkts[n].buf,
		    sizeof(pkts[n].buf), &dst);

		if (m == 0)
			break;
		pkts[n].len = m;
		order[n] = n;
		n++;
	}

	if (reorder) {			/* Fisher-Yates shuffle */
		for (i = n - 1; i > 0; i--) {
			int j = (int)(rng_next(rng) % (uint32_t)(i + 1));
			int t = order[i];

			order[i] = order[j];
			order[j] = t;
		}
	}

	for (i = 0; i < n; i++) {
		int k = order[i];
		int drop = 0;

		(*g)++;
		if (drop_period > 0 && (*g % (unsigned)drop_period) == 0)
			drop = 1;
		if (drop_pct > 0 &&
		    (int)(rng_next(rng) % 100u) < drop_pct)
			drop = 1;
		if (!drop) {
			netchan_feed(to, pkts[k].buf, pkts[k].len, from_addr);
			delivered++;
		}
	}
	return delivered;
}

#define STRESS_N		40	/* messages per run (< outgoing slots) */
#define STRESS_MAXLEN		1800	/* biggest message: > MTU (fragments),
					 * < NC_MAX_MSG (2048) datagram limit */

struct deliver_result {
	int delivered;		/* messages the server read */
	int order_ok;		/* all in-order and byte-exact */
	int channel_dead;	/* sender channel died mid-flight */
};

/* build message i: a distinct, index-stamped payload of varying size */
static size_t
make_msg(int i, uint8_t *out)
{
	size_t len, j;

	len = 16 + (size_t)((i * 37) % 200);
	if (i == 10 || i == 25 || i == 37)
		len = STRESS_MAXLEN;		/* larger than the MTU */
	out[0] = (uint8_t)i;
	out[1] = (uint8_t)(len & 0xff);
	for (j = 2; j < len; j++)
		out[j] = (uint8_t)(i * 7 + (int)j);
	return len;
}

/*
 * Send STRESS_N reliable messages from client to server through a link
 * that drops and/or reorders packets, driving retransmission with a
 * virtual clock, and confirm every message arrives once, in order, and
 * byte-exact.
 */
static struct deliver_result
run_reliable(int drop_period, int drop_pct, int reorder, uint32_t seed)
{
	struct deliver_result res = { 0, 1, 0 };
	struct netchan_conn *client, *server;
	struct netchan_cfg cfg;
	struct nc_addr caddr, saddr;
	struct netchan_event ev;
	struct netchan_chan *ch, *sch = NULL;
	struct rng rng = { seed ? seed : 1 };
	static uint8_t sent[STRESS_N][STRESS_MAXLEN];
	size_t sent_len[STRESS_N];
	uint8_t rbuf[STRESS_MAXLEN];
	unsigned g = 0;
	uint32_t vnow;
	int i, round;

	client = netchan_open(0);
	server = netchan_open(1);
	if (!client || !server)
		return res;

	/* neutralize idle timeout so the virtual clock can leap ahead */
	netchan_cfg_default(&cfg);
	cfg.idle_timeout_ms = 0xFFFFFF00u;
	netchan_config(client, &cfg);
	netchan_config(server, &cfg);

	caddr = make_addr(0x7f000001, 40000);
	saddr = make_addr(0x7f000001, 40001);

	/* handshake without loss */
	netchan_connect(client, &saddr);
	pump(client, server, &caddr);
	netchan_accept(server);
	pump_both(client, server, &caddr, &saddr);
	while (netchan_poll(client, &ev)) {}
	while (netchan_poll(server, &ev)) {}

	ch = netchan_chan_open(client, NETCHAN_RELIABLE, NETCHAN_DIR_SEND,
	    "stream");
	if (!ch)
		return res;
	pump_both(client, server, &caddr, &saddr);
	while (netchan_poll(server, &ev)) {}
	while (netchan_poll(client, &ev)) {}

	for (i = 0; i < STRESS_N; i++) {
		sent_len[i] = make_msg(i, sent[i]);
		if (netchan_chan_write(ch, sent[i], sent_len[i]) !=
		    (int)sent_len[i])
			return res;		/* queue rejected a write */
	}

	vnow = mono_ms();		/* seed from the same clock netchan uses */
	for (round = 0; round < 4000 && res.delivered < STRESS_N; round++) {
		lossy_pump(client, server, &caddr, reorder, &rng,
		    drop_period, drop_pct, &g);
		lossy_pump(server, client, &saddr, reorder, &rng,
		    drop_period, drop_pct, &g);

		vnow += 60;
		netchan_service(client, vnow);
		netchan_service(server, vnow);

		while (netchan_poll(server, &ev)) {
			if ((ev.type == NETCHAN_EV_DATA ||
			    ev.type == NETCHAN_EV_CHAN_OPEN) && ev.ch)
				sch = ev.ch;
		}
		while (netchan_poll(client, &ev)) {
			if (ev.type == NETCHAN_EV_CHAN_CLOSE)
				res.channel_dead = 1;
		}

		while (sch) {
			int rd = netchan_chan_read(sch, rbuf, sizeof(rbuf));

			if (rd <= 0)
				break;
			i = res.delivered;
			if (i >= STRESS_N || rd != (int)sent_len[i] ||
			    memcmp(rbuf, sent[i], (size_t)rd) != 0)
				res.order_ok = 0;
			res.delivered++;
		}
	}

	netchan_close(client);
	netchan_close(server);
	return res;
}

static void
test_stress_periodic_loss(void)
{
	struct deliver_result r;

	TEST("reliable delivery under 1-in-3 packet loss");
	r = run_reliable(3, 0, 0, 0);
	ASSERT(!r.channel_dead, "sender channel died");
	ASSERT(r.delivered == STRESS_N, "not all messages delivered");
	ASSERT(r.order_ok, "out-of-order or corrupt delivery");
	PASS();
}

static void
test_stress_reorder(void)
{
	struct deliver_result r;

	TEST("reliable delivery under packet reordering");
	r = run_reliable(0, 0, 1, 0);
	ASSERT(!r.channel_dead, "sender channel died");
	ASSERT(r.delivered == STRESS_N, "not all messages delivered");
	ASSERT(r.order_ok, "out-of-order or corrupt delivery");
	PASS();
}

static void
test_stress_loss_and_reorder(void)
{
	struct deliver_result r;

	TEST("reliable delivery under 15% loss + reordering");
	r = run_reliable(0, 15, 1, 0xC0FFEEu);
	ASSERT(!r.channel_dead, "sender channel died");
	ASSERT(r.delivered == STRESS_N, "not all messages delivered");
	ASSERT(r.order_ok, "out-of-order or corrupt delivery");
	PASS();
}

int
main(void)
{
	printf("libnet tests:\n");

	test_handshake();
	test_reliable_roundtrip();
	test_crypto_seal_open();
	test_crypto_identity_accept();
	test_crypto_identity_refused();
	test_crypto_identity_required_missing();
	test_udp_addr_roundtrip();
	test_stress_periodic_loss();
	test_stress_reorder();
	test_stress_loss_and_reorder();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
