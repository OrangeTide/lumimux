/* nc_crypto.c : encrypted UDP transport decorator for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_crypto.h"
#include "monocypher.h"
#include <string.h>

/*
 * OS entropy. Four backends, because a library that expects to be copied
 * into somebody else's tree does not get to choose the platform:
 *
 *   BCryptGenRandom  Windows. Link bcrypt.
 *   arc4random_buf   macOS and the BSDs. It cannot fail, so there is no
 *                    error path here to get wrong.
 *   getrandom(2)     Linux, glibc 2.25 and later. A short read is normal for
 *                    a large request, so the loop is not optional.
 *   /dev/urandom     everything else, and Linux kernels too old for the
 *                    syscall, where getrandom fails with ENOSYS.
 *
 * auth/keystore.c carries the same helper. The duplication is deliberate:
 * the two layers are vendored independently, and neither should drag in the
 * other for twenty lines of platform selection.
 */
#if defined(_WIN32)
#  define NC_RANDOM_BCRYPT 1
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#  define NC_RANDOM_ARC4 1
#  include <stdlib.h>
#else
#  if defined(__linux__)
#    define NC_RANDOM_GETRANDOM 1
#    include <sys/random.h>
#    include <errno.h>
#  endif
#  include <stdio.h>
#endif

/* Transcript labels. The session id uses its own label so publishing it
 * says nothing about either directional key. */
#define LABEL_SID  0
#define LABEL_I2R  1
#define LABEL_R2I  2

static const uint8_t zero32[32];

static int
fill_random(uint8_t *buf, size_t n)
{
#if defined(NC_RANDOM_BCRYPT)
    /* STATUS_SUCCESS is 0. */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#elif defined(NC_RANDOM_ARC4)
    arc4random_buf(buf, n);
    return 0;
#else
    FILE *f;
    size_t got;

#  if defined(NC_RANDOM_GETRANDOM)
    size_t off = 0;

    while (off < n) {
        ssize_t r = getrandom(buf + off, n - off, 0);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;              /* ENOSYS on an old kernel: use the file */
        }
        off += (size_t)r;
    }
    if (off == n)
        return 0;
#  endif

    f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
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

static int
is_zero32(const uint8_t p[32])
{
    return crypto_verify32(p, zero32) == 0;
}

/*
 * One transcript output = BLAKE2b over every secret and public value the
 * handshake produced, then the label.
 *
 *   dh_ee   ephemeral-ephemeral, always present
 *   dh_is   initiator ephemeral <-> responder static, zeros if absent
 *   dh_si   responder ephemeral <-> initiator static, zeros if absent
 *   eph_lo, eph_hi     the two ephemeral public keys, sorted so both peers
 *                      hash the same bytes regardless of role
 *   s_init, s_resp     the identity public keys in role order, zeros if absent
 *   psk                the pre-shared key, zeros if unused
 *
 * Every value that could distinguish this session from another one is in
 * here, which is what makes the session id safe for a higher layer to bind
 * a signature to.
 */
static void
derive_out(uint8_t out[32], const uint8_t dh_ee[32], const uint8_t dh_is[32],
           const uint8_t dh_si[32], const uint8_t eph_lo[32],
           const uint8_t eph_hi[32], const uint8_t s_init[32],
           const uint8_t s_resp[32], const uint8_t psk[32], uint8_t label)
{
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    crypto_blake2b_update(&ctx, dh_ee, 32);
    crypto_blake2b_update(&ctx, dh_is, 32);
    crypto_blake2b_update(&ctx, dh_si, 32);
    crypto_blake2b_update(&ctx, eph_lo, 32);
    crypto_blake2b_update(&ctx, eph_hi, 32);
    crypto_blake2b_update(&ctx, s_init, 32);
    crypto_blake2b_update(&ctx, s_resp, 32);
    crypto_blake2b_update(&ctx, psk, 32);
    crypto_blake2b_update(&ctx, &label, 1);
    crypto_blake2b_final(&ctx, out);
}

/*
 * Both sides compute the same three Diffie-Hellman results from opposite
 * ends, then the i2r and r2i keys, then pick tx/rx by role.
 *
 * The static-key half is the whole authentication story. The initiator
 * computes dh_is from its own ephemeral secret and the responder's identity
 * public key; the responder computes the identical value from its identity
 * secret and the initiator's ephemeral public key. Only the true holder of
 * the identity secret can arrive at it, so an impostor derives different
 * keys and every packet it seals fails to open. No signature, no round trip.
 */
static int
derive_session(struct nc_crypto *c, const uint8_t peer_eph[32])
{
    uint8_t dh_ee[32], dh_is[32], dh_si[32];
    uint8_t k_i2r[32], k_r2i[32];
    const uint8_t *eph_lo, *eph_hi, *s_init, *s_resp;
    int rc = 0;

    memset(dh_is, 0, sizeof(dh_is));
    memset(dh_si, 0, sizeof(dh_si));
    crypto_x25519(dh_ee, c->eph_sk, peer_eph);

    /* A low-order peer key drives the shared secret to zero, which would
     * make every session with that peer derive the same keys. Refuse. */
    if (is_zero32(dh_ee)) {
        rc = -1;
        goto out;
    }

    if (c->role == 0) {                       /* we are the initiator */
        if (c->peer_has_static)
            crypto_x25519(dh_is, c->eph_sk, c->peer_static_pk);
        if (c->have_static)
            crypto_x25519(dh_si, c->static_sk, peer_eph);
    } else {                                  /* we are the responder */
        if (c->have_static)
            crypto_x25519(dh_is, c->static_sk, peer_eph);
        if (c->peer_has_static)
            crypto_x25519(dh_si, c->eph_sk, c->peer_static_pk);
    }

    eph_lo = c->eph_pk;
    eph_hi = peer_eph;
    if (memcmp(c->eph_pk, peer_eph, 32) > 0) { eph_lo = peer_eph; eph_hi = c->eph_pk; }

    s_init = c->role == 0 ? c->static_pk : c->peer_static_pk;
    s_resp = c->role == 0 ? c->peer_static_pk : c->static_pk;

    derive_out(c->sid, dh_ee, dh_is, dh_si, eph_lo, eph_hi,
               s_init, s_resp, c->psk, LABEL_SID);
    derive_out(k_i2r, dh_ee, dh_is, dh_si, eph_lo, eph_hi,
               s_init, s_resp, c->psk, LABEL_I2R);
    derive_out(k_r2i, dh_ee, dh_is, dh_si, eph_lo, eph_hi,
               s_init, s_resp, c->psk, LABEL_R2I);

    if (c->role == 0) {            /* initiator sends i2r, receives r2i */
        memcpy(c->tx_key, k_i2r, 32);
        memcpy(c->rx_key, k_r2i, 32);
    } else {                       /* responder sends r2i, receives i2r */
        memcpy(c->tx_key, k_r2i, 32);
        memcpy(c->rx_key, k_i2r, 32);
    }
    c->have_key = 1;

out:
    crypto_wipe(dh_ee, sizeof(dh_ee));
    crypto_wipe(dh_is, sizeof(dh_is));
    crypto_wipe(dh_si, sizeof(dh_si));
    crypto_wipe(k_i2r, sizeof(k_i2r));
    crypto_wipe(k_r2i, sizeof(k_r2i));
    return rc;
}

int
nc_crypto_init(struct nc_crypto *c, int role, const struct nc_crypto_cfg *cfg)
{
    memset(c, 0, sizeof(*c));
    c->role = role ? 1 : 0;

    if (cfg && cfg->eph_sk_seed) {
        memcpy(c->eph_sk, cfg->eph_sk_seed, 32);
    } else if (fill_random(c->eph_sk, 32) != 0) {
        return -1;
    }
    crypto_x25519_public_key(c->eph_pk, c->eph_sk);

    if (!cfg)
        return 0;

    if (cfg->psk)
        memcpy(c->psk, cfg->psk, 32);
    if (cfg->static_sk) {
        memcpy(c->static_sk, cfg->static_sk, 32);
        crypto_x25519_public_key(c->static_pk, c->static_sk);
        c->have_static = 1;
    }
    c->require_peer_static = cfg->require_peer_static;
    c->verify_peer = cfg->verify_peer;
    c->verify_ctx = cfg->verify_ctx;
    return 0;
}

void
nc_crypto_identity_public(uint8_t out[32], const uint8_t static_sk[32])
{
    crypto_x25519_public_key(out, static_sk);
}

size_t
nc_crypto_handshake_packet(const struct nc_crypto *c, uint8_t *out, size_t cap)
{
    if (cap < NC_CRYPTO_HELLO_LEN)
        return 0;
    out[0] = NC_CRYPTO_HELLO;
    memcpy(out + 1, c->eph_pk, 32);
    /* Zeros here mean "no identity key", and keep both HELLOs the same size. */
    memcpy(out + 33, c->have_static ? c->static_pk : zero32, 32);
    return NC_CRYPTO_HELLO_LEN;
}

int
nc_crypto_ready(const struct nc_crypto *c)
{
    return c->have_key;
}

int
nc_crypto_failed(const struct nc_crypto *c)
{
    return c->failed;
}

const uint8_t *
nc_crypto_session_id(const struct nc_crypto *c)
{
    return c->have_key ? c->sid : NULL;
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

/* A HELLO arrived. Decide whether to trust who sent it, then derive. */
static void
consume_hello(struct nc_crypto *c, const uint8_t *body)
{
    const uint8_t *peer_static = body + 32;

    c->peer_has_static = !is_zero32(peer_static);
    if (c->peer_has_static)
        memcpy(c->peer_static_pk, peer_static, 32);

    if (c->require_peer_static && !c->peer_has_static) {
        c->failed = 1;                     /* anonymous peer, not allowed */
        return;
    }
    if (c->verify_peer &&
        c->verify_peer(c->verify_ctx,
                       c->peer_has_static ? c->peer_static_pk : NULL) != 0) {
        c->failed = 1;                     /* the application said no */
        return;
    }
    if (derive_session(c, body) != 0)
        c->failed = 1;                     /* degenerate ephemeral key */
}

long
nc_crypto_open(struct nc_crypto *c, const uint8_t *pkt, size_t len,
               uint8_t *out, size_t cap)
{
    if (len < 1 || c->failed)
        return -1;

    if (pkt[0] == NC_CRYPTO_HELLO) {
        if (len != NC_CRYPTO_HELLO_LEN)
            return -1;
        /* Only the first HELLO establishes keys. A later HELLO is ignored
         * on purpose: there is no mid-session re-key, so an attacker cannot
         * replay or inject one to reset the session to a chosen key, nor
         * re-run verification with a different identity. A new session must
         * use a fresh nc_crypto (see nc_crypto_init). */
        if (!c->have_key)
            consume_hello(c, pkt + 1);
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
