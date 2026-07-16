/* nc_crypto.c : encrypted UDP transport decorator for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_crypto.h"
#include "monocypher.h"
#include <string.h>

/* OS entropy: desktop only, so getrandom()/urandom is fine. */
#include <sys/random.h>

static int
fill_random(uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = getrandom(buf + off, n - off, 0);
        if (r < 0)
            return -1;
        off += (size_t)r;
    }
    return 0;
}

static void
wr64be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}

static uint64_t
rd64be(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* One directional key = BLAKE2b(shared || pk_lo || pk_hi || psk || label).
 * Sorting the two public keys makes both peers hash the same transcript. */
static void
derive_key(uint8_t out[32], const uint8_t shared[32],
           const uint8_t pk_lo[32], const uint8_t pk_hi[32],
           const uint8_t psk[32], uint8_t label)
{
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    crypto_blake2b_update(&ctx, shared, 32);
    crypto_blake2b_update(&ctx, pk_lo, 32);
    crypto_blake2b_update(&ctx, pk_hi, 32);
    crypto_blake2b_update(&ctx, psk, 32);
    crypto_blake2b_update(&ctx, &label, 1);
    crypto_blake2b_final(&ctx, out);
}

/* Both sides compute the i2r and r2i keys, then pick tx/rx by role. */
static void
derive_session(struct nc_crypto *c, const uint8_t peer_pk[32])
{
    uint8_t shared[32];
    crypto_x25519(shared, c->eph_sk, peer_pk);

    const uint8_t *pk_lo = c->eph_pk, *pk_hi = peer_pk;
    if (memcmp(c->eph_pk, peer_pk, 32) > 0) { pk_lo = peer_pk; pk_hi = c->eph_pk; }

    uint8_t k_i2r[32], k_r2i[32];
    derive_key(k_i2r, shared, pk_lo, pk_hi, c->psk, 1);
    derive_key(k_r2i, shared, pk_lo, pk_hi, c->psk, 2);

    if (c->role == 0) {            /* initiator sends i2r, receives r2i */
        memcpy(c->tx_key, k_i2r, 32);
        memcpy(c->rx_key, k_r2i, 32);
    } else {                       /* responder sends r2i, receives i2r */
        memcpy(c->tx_key, k_r2i, 32);
        memcpy(c->rx_key, k_i2r, 32);
    }
    c->have_key = 1;

    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(k_i2r, sizeof(k_i2r));
    crypto_wipe(k_r2i, sizeof(k_r2i));
}

int
nc_crypto_init(struct nc_crypto *c, int role,
               const uint8_t *eph_sk_seed, const uint8_t *psk)
{
    memset(c, 0, sizeof(*c));
    c->role = role ? 1 : 0;
    if (eph_sk_seed) {
        memcpy(c->eph_sk, eph_sk_seed, 32);
    } else if (fill_random(c->eph_sk, 32) != 0) {
        return -1;
    }
    if (psk)
        memcpy(c->psk, psk, 32);
    crypto_x25519_public_key(c->eph_pk, c->eph_sk);
    return 0;
}

size_t
nc_crypto_handshake_packet(const struct nc_crypto *c, uint8_t *out, size_t cap)
{
    if (cap < NC_CRYPTO_HELLO_LEN)
        return 0;
    out[0] = NC_CRYPTO_HELLO;
    memcpy(out + 1, c->eph_pk, 32);
    return NC_CRYPTO_HELLO_LEN;
}

int
nc_crypto_ready(const struct nc_crypto *c)
{
    return c->have_key;
}

long
nc_crypto_seal(struct nc_crypto *c, const uint8_t *plain, size_t len,
               uint8_t *out, size_t cap)
{
    if (!c->have_key)
        return -1;
    if (cap < len + NC_CRYPTO_OVERHEAD)
        return -1;
    if (c->tx_counter == UINT64_MAX)
        return -1;                        /* refuse to wrap the nonce */

    uint64_t counter = ++c->tx_counter;   /* 1-based, never reused */
    uint8_t nonce[24];
    memset(nonce, 0, sizeof(nonce));
    wr64be(nonce, counter);

    out[0] = NC_CRYPTO_DATA;
    wr64be(out + 1, counter);
    /* out+9: 16-byte mac, out+25: ciphertext */
    crypto_aead_lock(out + NC_CRYPTO_OVERHEAD, out + 9, c->tx_key, nonce,
                     NULL, 0, plain, len);
    return (long)(len + NC_CRYPTO_OVERHEAD);
}

/* RFC 6479 style sliding replay window over a 64-bit mask. */
static int
replay_check_and_update(struct nc_crypto *c, uint64_t counter)
{
    if (counter == 0)
        return -1;                         /* counters are 1-based */
    if (counter > c->rx_max) {
        uint64_t shift = counter - c->rx_max;
        c->rx_window = (shift >= 64) ? 0 : (c->rx_window << shift);
        c->rx_window |= 1;                 /* bit 0 = rx_max (== counter now) */
        c->rx_max = counter;
        return 0;
    }
    uint64_t diff = c->rx_max - counter;
    if (diff >= 64)
        return -1;                         /* too old */
    uint64_t bit = (uint64_t)1 << diff;
    if (c->rx_window & bit)
        return -1;                         /* already seen: replay */
    c->rx_window |= bit;
    return 0;
}

long
nc_crypto_open(struct nc_crypto *c, const uint8_t *pkt, size_t len,
               uint8_t *out, size_t cap)
{
    if (len < 1)
        return -1;

    if (pkt[0] == NC_CRYPTO_HELLO) {
        if (len != NC_CRYPTO_HELLO_LEN)
            return -1;
        /* Only the first HELLO establishes keys. A later HELLO is ignored
         * on purpose: there is no mid-session re-key, so an attacker cannot
         * replay or inject one to reset the session to a chosen key. A new
         * session must use a fresh nc_crypto (see nc_crypto_init). */
        if (!c->have_key)
            derive_session(c, pkt + 1);
        return 0;                          /* nothing to deliver */
    }

    if (pkt[0] == NC_CRYPTO_DATA) {
        if (!c->have_key || len < NC_CRYPTO_OVERHEAD)
            return -1;
        size_t ct_len = len - NC_CRYPTO_OVERHEAD;
        if (ct_len > cap)
            return -1;

        uint64_t counter = rd64be(pkt + 1);
        uint8_t nonce[24];
        memset(nonce, 0, sizeof(nonce));
        wr64be(nonce, counter);

        if (crypto_aead_unlock(out, pkt + 9, c->rx_key, nonce,
                               NULL, 0, pkt + NC_CRYPTO_OVERHEAD, ct_len) != 0)
            return -1;                     /* forged or corrupt */

        if (replay_check_and_update(c, counter) != 0) {
            crypto_wipe(out, ct_len);
            return -1;                     /* replayed */
        }
        return (long)ct_len;
    }

    return -1;                             /* unknown type */
}
