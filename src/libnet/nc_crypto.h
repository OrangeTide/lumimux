/* nc_crypto.h : encrypted UDP transport decorator for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_CRYPTO_H
#define NC_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/*
 * A transport decorator that sits between netchan and the socket:
 * netchan_send_next produces a plaintext datagram, nc_crypto_seal() wraps
 * it, and the socket sends the wrapped bytes; incoming datagrams pass
 * through nc_crypto_open() before netchan_feed(). The netchan core never
 * knows it is encrypted, exactly like adding any other transport.
 *
 * This is the WireGuard/Noise shape, deliberately not QUIC:
 *   - one X25519 ephemeral handshake per connection, no certificates,
 *     no PKI, optional pre-shared key for closed LAN games;
 *   - directional keys, so each side's packet counter starts at 1 with no
 *     risk of nonce reuse;
 *   - XChaCha20-Poly1305 AEAD per packet, 64-bit counter nonce;
 *   - a sliding replay window on receive.
 *
 * STATIC IDENTITY KEYS
 *
 * A side may also hold a long-term X25519 identity key. When it does, its
 * public half travels in the HELLO and a second Diffie-Hellman is folded
 * into the key derivation. Authentication then falls out for free: only a
 * peer holding the matching secret can derive the same keys, so the first
 * sealed packet that opens is proof of possession. There is no signature on
 * the wire and no extra round trip.
 *
 * In Noise's naming this is NX when only the responder has a static key: the
 * identity travels in the handshake rather than being known in advance. A
 * client that has seen this server before, and compares the key against what
 * it recorded, is applying NK's verification to NX's message flow, which is
 * precisely what trust on first use means. When both sides carry a static
 * key the shape is closer to XX, though without XX's encryption of the
 * identity keys themselves. With no static key at all it stays NN, which is
 * encryption without authentication.
 *
 * Deciding *whether to trust* a peer's static key is not this file's job.
 * Supply a verify_peer callback and answer the question however the
 * application wants: pin a known key, look it up in a known-hosts file and
 * warn on change (the ssh model), or accept anything. nc_crypto only
 * guarantees that a peer which passes verification is the one that holds the
 * secret. Note that forward secrecy survives: the ephemeral-ephemeral shared
 * secret is still in the transcript, so stealing a static key later does not
 * decrypt recorded sessions, it only permits impersonation from then on.
 *
 * A struct nc_crypto is single-session: call nc_crypto_init() once per
 * connection. A HELLO that arrives after the keys are established is
 * ignored (there is no mid-session re-key), and the same eph_sk_seed must
 * never be reused for two sessions, since a repeated key with a reset
 * counter would reuse the keystream.
 *
 * Wire framing (first byte is a type tag):
 *   HELLO: [0x01][ephemeral pk : 32][static pk : 32]        (65 bytes)
 *   DATA : [0x02][counter : 8 BE][mac : 16][ciphertext ...] (+25 bytes)
 *
 * The static-pk field is all zeros when the sender has no identity key. It
 * is always present so both HELLOs are the same size, which leaves a spoofed
 * source no amplification to abuse.
 */

#define NC_CRYPTO_HELLO      0x01
#define NC_CRYPTO_DATA       0x02
#define NC_CRYPTO_HELLO_LEN  65
#define NC_CRYPTO_OVERHEAD   25       /* type + counter + mac */
#define NC_CRYPTO_SID_LEN    32       /* exported session id */

/*
 * Verdict on a peer's static public key, called once, after the peer's HELLO
 * is parsed and before any key material is derived from it.
 *
 * peer_static_pk is the 32-byte key the peer presented, or NULL if it
 * presented none. Return 0 to accept and continue, non-zero to abort the
 * session permanently (see nc_crypto_failed).
 */
typedef int (*nc_crypto_verify_cb)(void *ctx, const uint8_t *peer_static_pk);

struct nc_crypto {
    int      role;                    /* 0 = initiator, 1 = responder */
    int      have_key;                /* session keys derived yet? */
    int      failed;                  /* peer verification refused the session */
    uint8_t  eph_sk[32];              /* our ephemeral secret */
    uint8_t  eph_pk[32];              /* our ephemeral public */
    int      have_static;             /* we hold a long-term identity key */
    uint8_t  static_sk[32];           /* our identity secret */
    uint8_t  static_pk[32];           /* our identity public */
    int      peer_has_static;         /* the peer presented one */
    uint8_t  peer_static_pk[32];      /* the peer's identity public */
    uint8_t  tx_key[32];              /* seal key (our -> peer) */
    uint8_t  rx_key[32];              /* open key (peer -> us) */
    uint8_t  psk[32];                 /* optional pre-shared key, else zeros */
    uint8_t  sid[NC_CRYPTO_SID_LEN];  /* session id bound to the transcript */
    uint64_t tx_counter;              /* last sent counter (1-based) */
    uint64_t rx_max;                  /* highest accepted recv counter */
    uint64_t rx_window;               /* replay bitmask for the 64 below rx_max */
    int      require_peer_static;     /* refuse a peer with no identity key */
    nc_crypto_verify_cb verify_peer;
    void    *verify_ctx;
};

/*
 * Everything optional about a session, so the common case stays a zeroed
 * struct and new knobs do not keep changing nc_crypto_init's signature.
 */
struct nc_crypto_cfg {
    const uint8_t *eph_sk_seed;       /* 32 bytes, NULL to draw from the OS */
    const uint8_t *psk;               /* 32 bytes, NULL for none */
    const uint8_t *static_sk;         /* 32 bytes, NULL for no identity */
    int            require_peer_static;
    nc_crypto_verify_cb verify_peer;
    void          *verify_ctx;
};

/*
 * Initialise. role is 0 for the connecting side, 1 for the accepting side.
 * cfg may be NULL, which selects a fresh ephemeral key from the OS RNG, no
 * pre-shared key, and no identity key: the unauthenticated NN handshake.
 * Returns 0 on success, -1 if the OS RNG was needed but unavailable.
 */
int nc_crypto_init(struct nc_crypto *c, int role,
                   const struct nc_crypto_cfg *cfg);

/* Derive the public half of a 32-byte X25519 identity secret, so a program
 * can print or store its own key without pulling in monocypher itself. */
void nc_crypto_identity_public(uint8_t out[32], const uint8_t static_sk[32]);

/* Write our HELLO handshake packet into out (>= NC_CRYPTO_HELLO_LEN).
 * Send this until the session is ready. Returns the byte count. */
size_t nc_crypto_handshake_packet(const struct nc_crypto *c,
                                  uint8_t *out, size_t cap);

/* Non-zero once the session keys are derived and DATA can flow. */
int nc_crypto_ready(const struct nc_crypto *c);

/* Non-zero if verify_peer refused this peer, or a required identity key was
 * missing. The session is dead and will never become ready. */
int nc_crypto_failed(const struct nc_crypto *c);

/*
 * A 32-byte value derived from the same handshake transcript as the session
 * keys, under a different label, so it can be published without revealing
 * anything about them. It is unique per session and it commits to both
 * ephemeral keys, both identity keys, and the pre-shared key.
 *
 * Higher layers sign it to bind their own authentication to this exact
 * session, which is what stops a signature captured by one peer from being
 * replayed to another. Returns NULL until the session is ready.
 */
const uint8_t *nc_crypto_session_id(const struct nc_crypto *c);

/* Seal a plaintext netchan datagram. Returns the wrapped length
 * (len + NC_CRYPTO_OVERHEAD), or -1 on error / not ready / no room. */
long nc_crypto_seal(struct nc_crypto *c, const uint8_t *plain, size_t len,
                    uint8_t *out, size_t cap);

/* Process one incoming datagram.
 *   HELLO packet: consumes it (may make the session ready); returns 0.
 *   DATA packet : writes plaintext to out; returns its length (> 0).
 *   bad auth / replay / malformed: returns -1.
 * A return of 0 means "nothing to deliver to netchan_feed". */
long nc_crypto_open(struct nc_crypto *c, const uint8_t *pkt, size_t len,
                    uint8_t *out, size_t cap);

#endif /* NC_CRYPTO_H */
