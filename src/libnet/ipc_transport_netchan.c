/* ipc_transport_netchan.c : netchan-backed ipc_transport */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "ipc_transport_netchan.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "netchan.h"
#include "nc_udp.h"
#include "nc_crypto.h"
#include "nc_auth.h"
#include "ipc_msg.h"

/* One TLV frame is chunked into netchan reliable messages this large.
 * Must stay under NC_MAX_MSG (2048); netchan fragments each chunk across
 * the wire on its own. */
#define NCT_CHUNK	2000

/* Upper bound on the reassembly buffer: one full frame plus slack. */
#define NCT_RX_MAX	(IPC_MAX_PAYLOAD + IPC_HDR_SIZE + NCT_CHUNK)

/* Default establish timeout when the caller passes <= 0. */
#define NCT_ESTABLISH_MS	5000

/* Poll granularity so retransmit timers still fire while blocked. */
#define NCT_TICK_MS	100

struct nct {
	struct netchan_conn	*conn;
	struct netchan_chan	*tx;	/* our reliable send channel */
	struct netchan_chan	*rx;	/* peer's channel, learned on first data */
	int			 udp_fd;
	int			 server;
	int			 accepted;	/* server: netchan_accept called */
	int			 closed;	/* peer gone / disconnected */
	struct nc_addr		 peer;		/* client: fixed peer address */
	int			 have_peer;
	int			 encrypted;	/* nc_crypto layer active */
	int			 roam_pending;	/* client send hit a dead local
						 * address; caller should roam */
	struct nc_crypto	 crypto;	/* per-connection session keys */
	uint8_t			*rxbuf;		/* reassembled inbound frames */
	size_t			 rxlen;
	size_t			 rxcap;
	int			 use_userauth;	/* run the nc_auth phase */
	struct ipc_netchan_userauth uacfg;	/* userauth config (copied) */
};

/* CLOCK_MONOTONIC in ms, matching netchan's internal clock. */
static uint32_t
nct_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* send one nc_crypto HELLO to the (known) peer, unsealed. */
static void
nct_send_hello(struct nct *n)
{
	uint8_t hp[NC_CRYPTO_HELLO_LEN];
	struct sockaddr_storage ss;
	socklen_t slen;
	size_t hn;

	if (!n->have_peer)
		return;
	hn = nc_crypto_handshake_packet(&n->crypto, hp, sizeof(hp));
	if (hn == 0)
		return;
	slen = nc_udp_to_sockaddr(&n->peer, &ss);
	if (slen == 0)
		return;
	(void)sendto(n->udp_fd, hp, hn, 0, (struct sockaddr *)&ss, slen);
}

/* drain every outgoing packet netchan has queued to the UDP socket.
 * when encrypted, each datagram is sealed first; if the session keys are
 * not ready yet the packet is dropped and netchan resends it later. */
static void
nct_flush_out(struct nct *n)
{
	uint8_t buf[2048];
	uint8_t sealed[2048 + NC_CRYPTO_OVERHEAD];
	struct nc_addr to;
	struct sockaddr_storage ss;
	socklen_t slen;
	size_t m;

	while ((m = netchan_send_next(n->conn, buf, sizeof(buf), &to)) != 0) {
		const uint8_t *out = buf;
		size_t outlen = m;

		if (n->encrypted) {
			long sn = nc_crypto_seal(&n->crypto, buf, m,
			    sealed, sizeof(sealed));

			if (sn < 0)
				continue;	/* not ready; netchan resends */
			out = sealed;
			outlen = (size_t)sn;
		}
		slen = nc_udp_to_sockaddr(&to, &ss);
		if (slen == 0)
			continue;
		if (sendto(n->udp_fd, out, outlen, 0,
		    (struct sockaddr *)&ss, slen) < 0 && !n->server) {
			/* the local address or route is gone: the client
			 * has likely moved networks.  flag a roam; the
			 * caller swaps in a fresh socket. */
			if (errno == EADDRNOTAVAIL || errno == ENETUNREACH ||
			    errno == ENETDOWN || errno == EHOSTUNREACH)
				n->roam_pending = 1;
		}
	}
}

/* service timers, then flush.  the initiator repeats its HELLO here until
 * the session is ready; the responder answers reactively (see feed). */
static void
nct_service(struct nct *n)
{
	if (n->encrypted && !n->server && !nc_crypto_ready(&n->crypto))
		nct_send_hello(n);
	netchan_service(n->conn, nct_now_ms());
	nct_flush_out(n);
}

/* wait up to timeout_ms for a datagram and feed it. returns 1 if a packet
 * was fed, 0 on timeout/no data, -1 on hard error. */
static int
nct_feed_one(struct nct *n, int timeout_ms)
{
	struct pollfd pfd;
	uint8_t buf[2048];
	uint8_t plain[2048];
	struct sockaddr_storage ss;
	struct nc_addr from;
	socklen_t slen;
	const uint8_t *data;
	size_t dlen;
	ssize_t r;
	int pr;

	pfd.fd = n->udp_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	pr = poll(&pfd, 1, timeout_ms);
	if (pr < 0)
		return (errno == EINTR) ? 0 : -1;
	if (pr == 0)
		return 0;

	slen = sizeof(ss);
	r = recvfrom(n->udp_fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&ss, &slen);
	if (r < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK ||
		    errno == EINTR) ? 0 : -1;
	if (r == 0)
		return 0;

	if (nc_udp_from_sockaddr(&from, (struct sockaddr *)&ss, slen) != 0)
		return 0;

	/* the server learns and pins its peer from the first datagram, so it
	 * can answer the crypto handshake before netchan is accepted. */
	if (n->server && !n->have_peer) {
		n->peer = from;
		n->have_peer = 1;
	}

	data = buf;
	dlen = (size_t)r;
	if (n->encrypted) {
		int was_hello = (r > 0 && buf[0] == NC_CRYPTO_HELLO);
		long pn = nc_crypto_open(&n->crypto, buf, (size_t)r,
		    plain, sizeof(plain));

		/* the responder answers each HELLO until the peer moves to
		 * sealed DATA, covering a lost handshake reply. */
		if (was_hello && n->server)
			nct_send_hello(n);
		if (pn <= 0)
			return 1;	/* HELLO consumed, replay, or bad auth */
		data = plain;
		dlen = (size_t)pn;
	}

	/* server accepts on the first (decrypted) netchan datagram. */
	if (n->server && !n->accepted) {
		netchan_feed(n->conn, data, dlen, &from);
		netchan_accept(n->conn);
		n->accepted = 1;
		return 1;
	}
	netchan_feed(n->conn, data, dlen, &from);
	return 1;
}

/* append inbound channel bytes to the reassembly buffer. returns -1 on
 * allocation failure. */
static int
nct_rx_append(struct nct *n, const uint8_t *data, size_t len)
{
	if (n->rxlen + len > n->rxcap) {
		size_t want = n->rxlen + len;
		size_t cap = n->rxcap ? n->rxcap : NCT_CHUNK;
		uint8_t *p;

		while (cap < want)
			cap *= 2;
		if (cap > NCT_RX_MAX && want <= NCT_RX_MAX)
			cap = NCT_RX_MAX;
		p = realloc(n->rxbuf, cap);
		if (!p)
			return -1;
		n->rxbuf = p;
		n->rxcap = cap;
	}
	memcpy(n->rxbuf + n->rxlen, data, len);
	n->rxlen += len;
	return 0;
}

/* drain netchan events, reading any inbound channel data into rxbuf. */
static int
nct_pump_events(struct nct *n)
{
	struct netchan_event ev;
	uint8_t buf[2048];

	while (netchan_poll(n->conn, &ev)) {
		switch (ev.type) {
		case NETCHAN_EV_CHAN_OPEN:
		case NETCHAN_EV_DATA:
			if (ev.ch)
				n->rx = ev.ch;
			break;
		case NETCHAN_EV_DISCONNECTED:
			n->closed = 1;
			break;
		default:
			break;
		}
	}
	if (n->rx) {
		int rd;

		while ((rd = netchan_chan_read(n->rx, buf, sizeof(buf))) > 0) {
			if (nct_rx_append(n, buf, (size_t)rd) != 0)
				return -1;
		}
	}
	return 0;
}

/* service + wait for a datagram + drain events. one blocking step. */
static int
nct_step(struct nct *n, int timeout_ms)
{
	int r;

	nct_service(n);
	r = nct_feed_one(n, timeout_ms);
	if (r < 0)
		return -1;
	if (nct_pump_events(n) != 0)
		return -1;
	return 0;
}

/* extract one complete TLV frame from rxbuf if present. returns 0 and
 * fills the caller buffer, 1 if no complete frame yet, -1 on error. */
static int
nct_take_frame(struct nct *n, uint32_t *type, void *buf, size_t bufsz,
    uint32_t *len)
{
	const uint8_t *b = n->rxbuf;
	uint32_t t, l;
	size_t need;

	if (n->rxlen < IPC_HDR_SIZE)
		return 1;
	t = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	    ((uint32_t)b[2] << 8) | (uint32_t)b[3];
	l = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
	    ((uint32_t)b[6] << 8) | (uint32_t)b[7];
	if (l > IPC_MAX_PAYLOAD)
		return -1;
	need = IPC_HDR_SIZE + l;
	if (n->rxlen < need)
		return 1;
	if (l > bufsz)
		return -1;
	if (l)
		memcpy(buf, b + IPC_HDR_SIZE, l);
	*type = t;
	*len = l;

	/* consume the frame, shifting any trailing bytes down. */
	n->rxlen -= need;
	if (n->rxlen)
		memmove(n->rxbuf, b + need, n->rxlen);
	return 0;
}

/* ---- vtable ops ---- */

static int
nct_send(void *ctx, uint32_t type, const void *payload, uint32_t len)
{
	struct nct *n = ctx;
	uint8_t stackframe[NCT_CHUNK * 2];
	uint8_t *frame = stackframe;
	size_t framelen, off;
	int rc = 0, framed;

	if (len > IPC_MAX_PAYLOAD)
		return -1;
	framelen = IPC_HDR_SIZE + len;
	if (framelen > sizeof(stackframe)) {
		frame = malloc(framelen);
		if (!frame)
			return -1;
	}
	framed = ipc_msg_frame(frame, framelen, type, payload, len);
	if (framed < 0) {
		rc = -1;
		goto out;
	}

	for (off = 0; off < framelen; ) {
		size_t clen = framelen - off;
		int w, spins;

		if (clen > NCT_CHUNK)
			clen = NCT_CHUNK;
		for (spins = 0; spins < 10000; spins++) {
			w = netchan_chan_write(n->tx, frame + off, clen);
			if (w == (int)clen)
				break;
			if (w != NETCHAN_ERR_FLOW && w != NETCHAN_ERR_AGAIN) {
				rc = -1;
				goto out;
			}
			/* window/queue full: drain acks and retry. */
			if (nct_step(n, NCT_TICK_MS) != 0 || n->closed) {
				rc = -1;
				goto out;
			}
		}
		if (w != (int)clen) {
			rc = -1;
			goto out;
		}
		off += clen;
	}
	nct_service(n);
out:
	if (frame != stackframe)
		free(frame);
	return rc;
}

static int
nct_recv(void *ctx, uint32_t *type, void *buf, size_t bufsz, uint32_t *len)
{
	struct nct *n = ctx;

	for (;;) {
		int r = nct_take_frame(n, type, buf, bufsz, len);

		if (r == 0)
			return 0;
		if (r < 0)
			return -1;
		if (n->closed && n->rxlen < IPC_HDR_SIZE)
			return 1;	/* EOF: peer gone, nothing buffered */
		if (nct_step(n, NCT_TICK_MS) != 0)
			return -1;
	}
}

/* ---- non-blocking event-loop interface ---- */

int
ipc_transport_netchan_pump(struct ipc_transport *t)
{
	struct nct *n;
	int r;

	if (!t || !t->ctx)
		return -1;
	n = t->ctx;

	nct_service(n);			/* timers + flush */
	for (;;) {			/* drain every ready datagram */
		r = nct_feed_one(n, 0);	/* timeout 0: non-blocking */
		if (r < 0)
			return -1;
		if (r == 0)
			break;
	}
	if (nct_pump_events(n) != 0)
		return -1;
	nct_flush_out(n);		/* flush acks the reads produced */
	return 0;
}

int
ipc_transport_netchan_try_recv(struct ipc_transport *t, uint32_t *type,
    void *buf, size_t bufsz, uint32_t *len)
{
	struct nct *n;
	int r;

	if (!t || !t->ctx)
		return -1;
	n = t->ctx;

	r = nct_take_frame(n, type, buf, bufsz, len);
	if (r == 0)
		return 0;		/* got a frame */
	if (r < 0)
		return -1;		/* fatal frame error */
	if (n->closed && n->rxlen < IPC_HDR_SIZE)
		return -1;		/* peer gone, nothing buffered */
	return 1;			/* nothing ready yet */
}

int
ipc_transport_netchan_roam_pending(struct ipc_transport *t)
{
	struct nct *n;
	int p;

	if (!t || !t->ctx)
		return 0;
	n = t->ctx;
	p = n->roam_pending;
	n->roam_pending = 0;
	return p;
}

int
ipc_transport_netchan_roam(struct ipc_transport *t)
{
	struct nct *n;
	int family, fd;

	if (!t || !t->ctx)
		return -1;
	n = t->ctx;
	if (n->server)
		return -1;		/* only the moving (client) end roams */

	/* fresh unbound socket in the peer's address family; the OS assigns a
	 * new source when we next send. */
	family = (n->peer.len > 0 && n->peer.a[0] == 6) ? AF_INET6 : AF_INET;
	fd = socket(family, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	if (ipc_transport_netchan_rebind(t, fd) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int
ipc_transport_netchan_rebind(struct ipc_transport *t, int new_udp_fd)
{
	struct nct *n;
	int fl;

	if (!t || !t->ctx || new_udp_fd < 0)
		return -1;
	n = t->ctx;

	if (n->udp_fd >= 0 && n->udp_fd != new_udp_fd)
		close(n->udp_fd);
	n->udp_fd = new_udp_fd;
	fl = fcntl(new_udp_fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(new_udp_fd, F_SETFL, fl | O_NONBLOCK);

	/* push whatever is queued (acks, keepalive) out the new socket so the
	 * peer sees the new source address and migrates without waiting for
	 * the next application write. */
	nct_service(n);
	return 0;
}

int
ipc_transport_netchan_timeout(struct ipc_transport *t)
{
	struct nct *n;
	int ms;

	if (!t || !t->ctx)
		return -1;
	n = t->ctx;
	ms = netchan_service(n->conn, nct_now_ms());
	nct_flush_out(n);
	return ms;
}

static int
nct_get_fd(void *ctx)
{
	struct nct *n = ctx;

	return n->udp_fd;
}

static void
nct_close(void *ctx)
{
	struct nct *n = ctx;

	if (!n)
		return;
	if (n->conn) {
		/* Tell the peer we are leaving and put the DISCONNECT on the
		 * wire before dropping the socket, so a persistent server frees
		 * the session at once instead of waiting for a keepalive
		 * timeout (which would stall the next client). */
		netchan_disconnect(n->conn);
		nct_flush_out(n);
		netchan_close(n->conn);
	}
	if (n->udp_fd >= 0)
		close(n->udp_fd);
	free(n->rxbuf);
	free(n);
}

/* ---- construction ---- */

static struct ipc_transport *
nct_build(int udp_fd, int server, const struct nc_addr *peer,
    const struct ipc_netchan_auth *auth, int use_crypto)
{
	struct ipc_transport *t;
	struct nct *n;
	int fl;

	if (!server && !peer)
		return NULL;

	t = calloc(1, sizeof(*t));
	n = calloc(1, sizeof(*n));
	if (!t || !n) {
		free(t);
		free(n);
		return NULL;
	}

	n->conn = netchan_open(server ? 1 : 0);
	if (!n->conn) {
		free(t);
		free(n);
		return NULL;
	}
	n->udp_fd = udp_fd;
	n->server = server ? 1 : 0;
	if (peer) {
		n->peer = *peer;
		n->have_peer = 1;
	}
	if (use_crypto) {
		/* role: 0 = initiator (client), 1 = responder (server). */
		struct nc_crypto_cfg cfg = { 0 };
		if (auth) {
			cfg.psk = auth->psk;
			cfg.static_sk = auth->static_sk;
			cfg.require_peer_static = auth->require_peer_static;
			cfg.verify_peer = auth->verify_peer;
			cfg.verify_ctx = auth->verify_ctx;
		}
		if (nc_crypto_init(&n->crypto, n->server, &cfg) != 0) {
			netchan_close(n->conn);
			free(t);
			free(n);
			return NULL;
		}
		n->encrypted = 1;
		if (auth && auth->userauth) {
			n->uacfg = *auth->userauth;
			n->use_userauth = 1;
		}
	}

	fl = fcntl(udp_fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(udp_fd, F_SETFL, fl | O_NONBLOCK);

	t->send = nct_send;
	t->recv = nct_recv;
	t->get_fd = nct_get_fd;
	t->close = nct_close;
	t->ctx = n;
	return t;
}

struct ipc_transport *
ipc_transport_netchan_new(int udp_fd, int server, const struct nc_addr *peer)
{
	return nct_build(udp_fd, server, peer, NULL, 0);
}

struct ipc_transport *
ipc_transport_netchan_new_crypto(int udp_fd, int server,
    const struct nc_addr *peer, const uint8_t *psk)
{
	struct ipc_netchan_auth auth = { .psk = psk };

	return nct_build(udp_fd, server, peer, &auth, 1);
}

struct ipc_transport *
ipc_transport_netchan_new_crypto_auth(int udp_fd, int server,
    const struct nc_addr *peer, const struct ipc_netchan_auth *auth)
{
	return nct_build(udp_fd, server, peer, auth, 1);
}

/* ---- user authentication phase (nc_auth over the reliable channel) ---- */

/*
 * nc_auth emits at most one whole message per call.  The send callback stashes
 * it here, length-framed, and the driver writes it to the reliable channel.
 * Auth messages precede any TLV on the channel, so a 2-byte length prefix is
 * all the framing the receiver needs to split them back out.
 */
struct ua_send_state {
	uint8_t	buf[2 + NC_AUTH_MAX_MSG];
	size_t	len;			/* pending framed bytes, 0 if none */
	int	overflow;		/* a message did not fit / doubled up */
};

static void
ua_send(void *ctx, const void *msg, size_t len)
{
	struct ua_send_state *s = ctx;

	if (len > NC_AUTH_MAX_MSG || s->len != 0) {
		s->overflow = 1;
		return;
	}
	s->buf[0] = (uint8_t)(len >> 8);
	s->buf[1] = (uint8_t)(len & 0xff);
	memcpy(s->buf + 2, msg, len);
	s->len = 2 + len;
}

/* Write any pending auth message to the reliable channel and flush. */
static int
ua_flush(struct nct *n, struct ua_send_state *s)
{
	size_t off = 0;
	int spins = 0;

	if (s->overflow)
		return -1;
	if (s->len == 0) {
		nct_service(n);
		return 0;
	}
	while (off < s->len) {
		int w = netchan_chan_write(n->tx, s->buf + off, s->len - off);

		if (w < 0)
			return -1;
		off += (size_t)w;
		nct_service(n);
		if (w == 0 && ++spins > 1000)
			return -1;
	}
	s->len = 0;
	nct_service(n);
	return 0;
}

/*
 * Run the ssh-shaped userauth conversation to completion.  Returns 0 if the
 * user authenticated, -1 on denial, malformed input, disconnect, or timeout.
 * Client credential fetchers run here, so they should hand over a preloaded
 * key or a already-collected password rather than block on a human, or the
 * establish deadline may fire.
 */
static int
nct_run_userauth(struct nct *n, uint32_t start, uint32_t budget)
{
	struct nc_auth ua;
	struct nc_auth_server_cb scb;
	struct ua_send_state s;
	const uint8_t *sid;
	uint8_t abuf[512];
	size_t alen = 0;

	sid = nc_crypto_session_id(&n->crypto);
	if (!sid)
		return -1;

	memset(&s, 0, sizeof(s));
	if (n->server) {
		scb.methods = n->uacfg.methods;
		scb.check_key = n->uacfg.check_key;
		scb.check_password = n->uacfg.check_password;
		scb.ctx = n->uacfg.server_ctx;
		nc_auth_server_init(&ua, sid, &scb, ua_send, &s);
	} else {
		nc_auth_client_init(&ua, sid,
		    n->uacfg.user ? n->uacfg.user : "", ua_send, &s);
		nc_auth_start(&ua);
	}
	if (ua_flush(n, &s) != 0)
		return -1;

	while (nc_auth_state(&ua) == NC_AUTH_PENDING) {
		struct netchan_event ev;
		int rd;

		if (nct_now_ms() - start > budget)
			return -1;

		/* Client: satisfy a suspended credential request. */
		if (!n->server) {
			int need = nc_auth_needs(&ua);

			if (need == NC_AUTH_NEED_KEY) {
				uint8_t sk[64], pk[32];

				if (n->uacfg.get_key && n->uacfg.get_key(
				    n->uacfg.cred_ctx, sk, pk) == 0)
					nc_auth_supply_key(&ua, sk, pk);
				else
					nc_auth_supply_key(&ua, NULL, NULL);
				memset(sk, 0, sizeof(sk));
				if (ua_flush(n, &s) != 0)
					return -1;
				continue;
			}
			if (need == NC_AUTH_NEED_PASSWORD) {
				char pw[NC_AUTH_MAX_PASS + 1];

				if (n->uacfg.get_password && n->uacfg.get_password(
				    n->uacfg.cred_ctx, pw, sizeof(pw)) == 0)
					nc_auth_supply_password(&ua, pw);
				else
					nc_auth_supply_password(&ua, NULL);
				memset(pw, 0, sizeof(pw));
				if (ua_flush(n, &s) != 0)
					return -1;
				continue;
			}
		}

		/* Move a datagram in, learn the peer channel, read auth bytes. */
		nct_service(n);
		if (nct_feed_one(n, NCT_TICK_MS) < 0)
			return -1;
		while (netchan_poll(n->conn, &ev)) {
			if ((ev.type == NETCHAN_EV_CHAN_OPEN ||
			    ev.type == NETCHAN_EV_DATA) && ev.ch)
				n->rx = ev.ch;
			else if (ev.type == NETCHAN_EV_DISCONNECTED)
				n->closed = 1;
		}
		if (n->closed)
			return -1;
		if (n->rx) {
			while (alen < sizeof(abuf) && (rd = netchan_chan_read(
			    n->rx, abuf + alen, sizeof(abuf) - alen)) > 0)
				alen += (size_t)rd;
		}

		/* Feed each whole [u16 len][payload] frame to nc_auth. */
		while (alen >= 2) {
			size_t mlen = ((size_t)abuf[0] << 8) | abuf[1];

			if (mlen > NC_AUTH_MAX_MSG)
				return -1;
			if (alen < 2 + mlen)
				break;
			if (nc_auth_feed(&ua, abuf + 2, mlen) != 0)
				return -1;
			memmove(abuf, abuf + 2 + mlen, alen - (2 + mlen));
			alen -= 2 + mlen;
			if (ua_flush(n, &s) != 0)
				return -1;
			if (nc_auth_state(&ua) != NC_AUTH_PENDING)
				break;
		}
	}

	/* Bytes past the last auth frame are early TLV; keep them for recv. */
	if (alen && nct_rx_append(n, abuf, alen) != 0)
		return -1;

	return nc_auth_state(&ua) == NC_AUTH_OK ? 0 : -1;
}

int
ipc_transport_netchan_establish(struct ipc_transport *t, int timeout_ms)
{
	struct nct *n;
	uint32_t start, budget;

	if (!t || !t->ctx)
		return -1;
	n = t->ctx;
	if (timeout_ms <= 0)
		timeout_ms = NCT_ESTABLISH_MS;
	budget = (uint32_t)timeout_ms;
	start = nct_now_ms();

	/* complete the crypto handshake first, so every netchan datagram
	 * (including the connect SYN) can be sealed. */
	if (n->encrypted) {
		while (!nc_crypto_ready(&n->crypto)) {
			if (nct_now_ms() - start > budget)
				return -1;
			nct_service(n);		/* initiator repeats HELLO */
			if (nct_feed_one(n, NCT_TICK_MS) < 0)
				return -1;
			if (n->closed)
				return -1;
		}
	}

	if (!n->server) {
		if (netchan_connect(n->conn, &n->peer) != NETCHAN_OK)
			return -1;
		nct_flush_out(n);
	}

	/* run the handshake to CONNECTED. */
	while (netchan_state(n->conn) != NETCHAN_STATE_CONNECTED) {
		if (nct_now_ms() - start > budget)
			return -1;
		if (nct_step(n, NCT_TICK_MS) != 0)
			return -1;
		if (n->closed)
			return -1;
	}

	/* open our outbound reliable channel and flush the open. */
	n->tx = netchan_chan_open(n->conn, NETCHAN_RELIABLE,
	    NETCHAN_DIR_SEND, "ipc");
	if (!n->tx)
		return -1;
	nct_service(n);

	/* Authenticate the user over the established channel, before any TLV.
	 * Off unless a userauth config was supplied, so the default flow is
	 * untouched. */
	if (n->use_userauth && nct_run_userauth(n, start, budget) != 0)
		return -1;

	return 0;
}
