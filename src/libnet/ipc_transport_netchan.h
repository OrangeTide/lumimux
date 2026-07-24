/* ipc_transport_netchan.h : netchan-backed ipc_transport */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef IPC_TRANSPORT_NETCHAN_H
#define IPC_TRANSPORT_NETCHAN_H

#include "ipc_transport.h"
#include "nc_addr.h"
#include "nc_crypto.h"

/*
 * A netchan-backed ipc_transport.  It carries the same TLV messages as
 * the Unix-socket transport (ipc_transport_unix_new), but over a netchan
 * reliable channel on a UDP socket.  The reliable channel is treated as
 * an in-order byte pipe: each TLV frame (ipc_msg framing) is chunked to
 * fit netchan's per-message limit on send and reassembled on receive, so
 * a caller sees the exact same message boundaries as over a stream fd.
 *
 * The transport owns the UDP socket and drives netchan's feed / service /
 * send_next / poll cycle internally; send() and recv() block like their
 * ipc_msg counterparts.  get_fd() returns the UDP socket so an event loop
 * can wake on readability, but one readable datagram does not imply a
 * full frame (fragmentation, acks), so event-loop use must drain with a
 * non-blocking path wired in a later step.
 */

/*
 * Create a netchan transport over an already-created UDP socket `udp_fd`
 * (ownership transferred; closed by ipc_transport_free).
 *   server != 0 : passive end.  Learns the peer address from the first
 *                 datagram; `peer` is ignored.
 *   server == 0 : active end.  Sends to `peer` (required).
 * The connection is not yet established; call
 * ipc_transport_netchan_establish() next.  Returns NULL on failure (the
 * caller retains ownership of udp_fd on NULL).
 */
struct ipc_transport *ipc_transport_netchan_new(int udp_fd, int server,
    const struct nc_addr *peer);

/*
 * Like ipc_transport_netchan_new, but wraps every datagram in the
 * nc_crypto AEAD layer (X25519 ephemeral handshake, XChaCha20-Poly1305 per
 * packet, sliding replay window).  `psk`, if non-NULL, is a 32-byte
 * pre-shared key mixed into the key derivation so both ends mutually
 * authenticate; NULL gives encryption without authentication.  The crypto
 * handshake runs inside ipc_transport_netchan_establish() before the
 * netchan handshake.  Returns NULL on failure (caller retains udp_fd).
 */
struct ipc_transport *ipc_transport_netchan_new_crypto(int udp_fd,
    int server, const struct nc_addr *peer, const uint8_t *psk);

/*
 * Optional authentication for a crypto transport, layered on the PSK.  Any
 * field may be NULL/zero, which selects the plain PSK behaviour above.
 *
 *   psk                 32-byte pre-shared key mixed into the KDF, or NULL.
 *   static_sk           server: its 32-byte long-term identity secret, so a
 *                       client can pin it across restarts (ssh host-key TOFU).
 *                       NULL for an anonymous server.
 *   verify_peer         client: called once with the server's presented
 *                       identity key (or NULL if none) before any key material
 *                       is derived; return 0 to accept, non-zero to refuse the
 *                       session permanently.  This is where known_hosts
 *                       comparison lives.
 *   verify_ctx          opaque pointer passed to verify_peer.
 *   require_peer_static client: refuse a server that presents no identity key.
 */
/*
 * Optional ssh-shaped user authentication run over the encrypted channel
 * after the crypto handshake, before any TLV.  This authenticates the
 * connecting *user* (nc_crypto authenticates the connection).  It is off
 * unless a non-NULL ipc_netchan_userauth is supplied, so the default flow is
 * unchanged.  The callbacks run synchronously inside
 * ipc_transport_netchan_establish(), where blocking (and prompting) is fine.
 *
 * Client side supplies `user` and credential fetchers; server side supplies
 * the policy callbacks.  Each side ignores the other side's fields.
 */
struct ipc_netchan_userauth {
	/* Client: the login name to claim, and where to get credentials.
	 * A fetcher fills its output and returns 0 to supply that credential,
	 * non-zero to skip the method.  Either fetcher may be NULL. */
	const char *user;
	int (*get_key)(void *ctx, uint8_t sk[64], uint8_t pk[32]);
	int (*get_password)(void *ctx, char *buf, size_t sz);
	void *cred_ctx;

	/* Server: which methods to offer a name, and how to check each.
	 * A NULL check rejects that method.  methods() returning 0 refuses
	 * the name. */
	unsigned (*methods)(void *ctx, const char *user);
	int (*check_key)(void *ctx, const char *user, const uint8_t pk[32]);
	int (*check_password)(void *ctx, const char *user, const char *password);
	void *server_ctx;
};

struct ipc_netchan_auth {
	const uint8_t *psk;
	const uint8_t *static_sk;
	nc_crypto_verify_cb verify_peer;
	void *verify_ctx;
	int require_peer_static;
	/* NULL for no user-authentication phase (the default). */
	const struct ipc_netchan_userauth *userauth;
};

/*
 * Like ipc_transport_netchan_new_crypto, but takes the full auth config so a
 * server can present a long-term identity key and a client can pin it.  `auth`
 * may be NULL, which is identical to passing a NULL psk.  Returns NULL on
 * failure (caller retains udp_fd).
 */
struct ipc_transport *ipc_transport_netchan_new_crypto_auth(int udp_fd,
    int server, const struct nc_addr *peer, const struct ipc_netchan_auth *auth);

/*
 * Drive the handshake and open the local reliable channel, blocking up to
 * timeout_ms (<= 0 selects a default).  Returns 0 once connected with the
 * send channel open, -1 on timeout or error.
 */
int ipc_transport_netchan_establish(struct ipc_transport *t, int timeout_ms);

/*
 * Non-blocking event-loop interface.  The blocking send()/recv() vtable
 * ops are convenient for handshakes and simple clients, but a server that
 * multiplexes many fds must not block in recv().  These three calls let a
 * poll()/iox loop service the netchan link without blocking:
 *
 *   - call ipc_transport_netchan_timeout() to get the poll timeout so
 *     retransmit and keepalive timers still fire while idle;
 *   - poll on get_fd() for readability with that timeout;
 *   - call ipc_transport_netchan_pump() on wake-up (readable or timeout)
 *     to read all ready datagrams, service timers, and flush output;
 *   - drain ipc_transport_netchan_try_recv() until it reports nothing
 *     ready, handling each complete frame.
 */

/*
 * Read every datagram currently ready on the socket, service netchan
 * timers, and flush queued output.  Never blocks.  Returns 0 on success,
 * -1 on a socket or allocation error.  A closed peer is not an error here;
 * it surfaces through try_recv.
 */
int ipc_transport_netchan_pump(struct ipc_transport *t);

/*
 * Take one reassembled TLV frame if a whole one is buffered.  Never
 * blocks.  Returns 0 and fills the caller buffer when a frame is ready,
 * 1 when none is ready yet, -1 when the connection has ended (peer gone
 * with nothing left to read, or a fatal frame error).
 */
int ipc_transport_netchan_try_recv(struct ipc_transport *t, uint32_t *type,
    void *buf, size_t bufsz, uint32_t *len);

/*
 * Milliseconds until netchan next needs servicing, for use as the poll
 * timeout; -1 when idle (block until the socket is readable).  Also
 * flushes any packets the service produced.
 */
int ipc_transport_netchan_timeout(struct ipc_transport *t);

/*
 * Roaming: replace the underlying UDP socket with `new_udp_fd` (ownership
 * transferred; the old socket is closed) while keeping the netchan and
 * crypto session intact.  A client whose local address changed calls this
 * with a fresh socket; its next packets carry a new source address, and
 * the peer's netchan migrates to it automatically (validated against the
 * live channels, so an unrelated sender cannot hijack the session).  The
 * crypto keys are unchanged, so no re-handshake occurs.  Callers using the
 * event-loop interface must re-register get_fd() afterward, since the
 * pollable fd has changed.  Returns 0 on success, -1 on error.
 */
int ipc_transport_netchan_rebind(struct ipc_transport *t, int new_udp_fd);

/*
 * Roaming detection.  A client whose local address or route disappeared
 * fails to send; the transport records that.  Returns 1 (and clears the
 * flag) if such a failure has occurred since the last call, 0 otherwise.
 * Always 0 for a server.  Poll this from the event loop to drive automatic
 * roaming.
 */
int ipc_transport_netchan_roam_pending(struct ipc_transport *t);

/*
 * Roam now: open a fresh UDP socket in the peer's address family and rebind
 * to it (see ipc_transport_netchan_rebind), so the next packets carry a new
 * source address and the peer migrates.  Convenience wrapper for the common
 * case where the caller has no socket of its own to supply.  Returns the new
 * fd on success (equal to get_fd afterward), -1 on error or on a server.
 */
int ipc_transport_netchan_roam(struct ipc_transport *t);

#endif /* IPC_TRANSPORT_NETCHAN_H */
