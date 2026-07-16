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
 * It is a desktop backend. Browser transports (WebRTC, wss) already
 * encrypt, so this never needs to compile into a wasm build.
 *
 * A struct nc_crypto is single-session: call nc_crypto_init() once per
 * connection. A HELLO that arrives after the keys are established is
 * ignored (there is no mid-session re-key), and the same eph_sk_seed must
 * never be reused for two sessions, since a repeated key with a reset
 * counter would reuse the keystream.
 *
 * Wire framing (first byte is a type tag):
 *   HELLO: [0x01][ephemeral public key : 32]                 (33 bytes)
 *   DATA : [0x02][counter : 8 BE][mac : 16][ciphertext ...]  (+25 bytes)
 */

#define NC_CRYPTO_HELLO      0x01
#define NC_CRYPTO_DATA       0x02
#define NC_CRYPTO_HELLO_LEN  33
#define NC_CRYPTO_OVERHEAD   25       /* type + counter + mac */

struct nc_crypto {
    int      role;                    /* 0 = initiator, 1 = responder */
    int      have_key;                /* session keys derived yet? */
    uint8_t  eph_sk[32];              /* our ephemeral secret */
    uint8_t  eph_pk[32];              /* our ephemeral public */
    uint8_t  tx_key[32];              /* seal key (our -> peer) */
    uint8_t  rx_key[32];              /* open key (peer -> us) */
    uint8_t  psk[32];                 /* optional pre-shared key, else zeros */
    uint64_t tx_counter;             /* last sent counter (1-based) */
    uint64_t rx_max;                  /* highest accepted recv counter */
    uint64_t rx_window;               /* replay bitmask for the 64 below rx_max */
};

/*
 * Initialise. role is 0 for the connecting side, 1 for the accepting side.
 * eph_sk_seed, if non-NULL, supplies the 32-byte ephemeral secret (used by
 * tests for determinism); if NULL, a fresh secret is drawn from the OS.
 * psk, if non-NULL, is a 32-byte pre-shared key mixed into the KDF; if
 * NULL, no pre-shared key is used. Returns 0 on success, -1 if the OS RNG
 * was needed but unavailable.
 */
int nc_crypto_init(struct nc_crypto *c, int role,
                   const uint8_t *eph_sk_seed, const uint8_t *psk);

/* Write our HELLO handshake packet into out (>= NC_CRYPTO_HELLO_LEN).
 * Send this until the session is ready. Returns the byte count. */
size_t nc_crypto_handshake_packet(const struct nc_crypto *c,
                                  uint8_t *out, size_t cap);

/* Non-zero once the session keys are derived and DATA can flow. */
int nc_crypto_ready(const struct nc_crypto *c);

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
