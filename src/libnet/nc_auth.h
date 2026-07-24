/* nc_auth.h : SSH-shaped client authentication over an established session */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_AUTH_H
#define NC_AUTH_H

#include <stdint.h>
#include <stddef.h>

/*
 * nc_crypto authenticates the *server* to the client, because the client
 * knows the server's identity key in advance. It says nothing about who the
 * client is. That question belongs to the application, and this file is one
 * answer to it: a small conversation modelled on ssh's userauth, carried as
 * ordinary reliable messages over the encrypted channel.
 *
 * The flow is ssh's, minus the parts a game does not need:
 *
 *   client -> HELLO    "I claim to be <user>"
 *   server -> METHODS  "for that name I will accept publickey, password"
 *   client -> PUBKEY   a public key and a signature, tried first
 *   server -> OK, or FAIL carrying the methods still worth trying
 *   client -> PASSWORD only if publickey was refused or unavailable
 *   server -> OK or FAIL
 *
 * Public-key authentication is what makes a login instant: the client's
 * secret key sits on disk, encrypted under a local passphrase, and unlocking
 * it costs nothing on the network. Password entry is the fallback for a
 * client with no key enrolled yet, exactly as with ssh.
 *
 * THE SIGNATURE MUST BE BOUND TO THE SESSION
 *
 * The signed message is a digest over a fixed label, the nc_crypto session
 * id, the username, and the public key. Binding to the session id is not
 * decoration, it is the whole security of the scheme. A signature over a
 * bare challenge could be collected by a malicious server and replayed to a
 * real one to log in as the client. The session id commits to both ephemeral
 * keys and both identity keys, so a signature produced for one session is
 * worthless everywhere else. ssh does the same thing with its session
 * identifier, and for the same reason.
 *
 * This file has no idea what a socket or a netchan channel is. It is handed
 * a send callback and fed whole messages, so it can be driven by a test with
 * two state machines in one process and no transport at all.
 *
 * ASKING FOR A CREDENTIAL WITHOUT BLOCKING
 *
 * A client cannot answer "what is your password" from inside a function call,
 * because the answer comes from a human. A callback that returns the password
 * would have to sit and wait for one, and a program driven by an event loop
 * cannot afford to sit anywhere: its timers stop, and on this protocol that
 * means the connection times out while the user is still typing.
 *
 * So the conversation suspends instead. When it needs a credential it sets
 * nc_auth_needs() and returns, sending nothing. The application collects the
 * answer however it likes, taking as long as it likes, and calls
 * nc_auth_supply_key() or nc_auth_supply_password() to resume. Passing NULL
 * means "I have nothing", and the conversation moves on to the next method.
 *
 * This costs the client side its callbacks entirely, which is a fair trade:
 * the state machine no longer calls out into code that might block, so there
 * is nowhere left for a blocking read to hide.
 */

/* Method bits, as carried in METHODS and FAIL. */
#define NC_AUTH_M_PUBKEY    0x01
#define NC_AUTH_M_PASSWORD  0x02

#define NC_AUTH_MAX_USER    64
#define NC_AUTH_MAX_PASS    128
#define NC_AUTH_MAX_MSG     256
#define NC_AUTH_MAX_TRIES   6

enum {
    NC_AUTH_PENDING,        /* conversation still in progress */
    NC_AUTH_OK,             /* the client is authenticated */
    NC_AUTH_DENIED,         /* no method left, or too many attempts */
};

/* What the client conversation is waiting for. See nc_auth_needs. */
enum {
    NC_AUTH_NEED_NOTHING = 0,
    NC_AUTH_NEED_KEY,       /* an Ed25519 key pair, or NULL to skip */
    NC_AUTH_NEED_PASSWORD,  /* a password, or NULL to skip */
};

/* Hand a complete auth message to the transport. */
typedef void (*nc_auth_send_cb)(void *ctx, const void *msg, size_t len);

struct nc_auth_server_cb {
    /*
     * Which methods to offer for this name, as a bitmask. Returning 0
     * refuses the name outright.
     *
     * Offering the same methods for an unknown user as for a known one is
     * deliberate in the demo's store: answering differently turns the
     * handshake into a way to enumerate accounts.
     */
    unsigned (*methods)(void *ctx, const char *user);
    /* Non-zero if pk is an authorised key for user. The signature over it
     * has already been checked by the time this is called. */
    int (*check_key)(void *ctx, const char *user, const uint8_t pk[32]);
    /* Non-zero if the password is correct for user. */
    int (*check_password)(void *ctx, const char *user, const char *password);
    void *ctx;
};

struct nc_auth {
    int      server;                    /* which side of the conversation */
    int      state;
    int      need;                      /* client: NC_AUTH_NEED_* */
    int      greeted;                   /* server: HELLO seen */
    unsigned avail;                     /* methods still worth trying */
    int      tries;
    uint8_t  sid[32];                   /* nc_crypto session id */
    char     user[NC_AUTH_MAX_USER + 1];
    nc_auth_send_cb send;
    void    *send_ctx;
    struct nc_auth_server_cb scb;
};

void nc_auth_client_init(struct nc_auth *a, const uint8_t sid[32],
                         const char *user,
                         nc_auth_send_cb send, void *send_ctx);

void nc_auth_server_init(struct nc_auth *a, const uint8_t sid[32],
                         const struct nc_auth_server_cb *cb,
                         nc_auth_send_cb send, void *send_ctx);

/* Client only: emit the opening HELLO. Harmless on the server. */
void nc_auth_start(struct nc_auth *a);

/* Feed one complete message. Returns 0, or -1 if the peer sent something
 * malformed or out of order, which also ends the conversation as denied. */
int nc_auth_feed(struct nc_auth *a, const void *msg, size_t len);

/*
 * What the conversation is waiting for, or NC_AUTH_NEED_NOTHING. Check this
 * after every nc_auth_feed. While it is non-zero the client has sent nothing
 * and is waiting; nothing times out inside nc_auth, so the application may
 * take as long as it needs.
 */
int nc_auth_needs(const struct nc_auth *a);

/*
 * Resume a suspended conversation. Pass NULL to say the credential is not
 * available, which drops that method and moves on to the next one. Calling
 * either function when nc_auth_needs() does not match is a no-op.
 *
 * nc_auth does not retain the secret: the key is used to sign immediately and
 * the password is copied into the outgoing message and wiped.
 */
void nc_auth_supply_key(struct nc_auth *a, const uint8_t sk[64],
                        const uint8_t pk[32]);
void nc_auth_supply_password(struct nc_auth *a, const char *password);

int nc_auth_state(const struct nc_auth *a);

/* The authenticated name. Only meaningful once the state is NC_AUTH_OK. */
const char *nc_auth_user(const struct nc_auth *a);

/*
 * The digest a client signs and a server checks:
 *   BLAKE2b-256("netchan-auth-v1" || sid || ulen || user || pk)
 * Exposed so a key-management tool can produce the same bytes.
 */
void nc_auth_signed_digest(uint8_t out[32], const uint8_t sid[32],
                           const char *user, const uint8_t pk[32]);

#endif /* NC_AUTH_H */
