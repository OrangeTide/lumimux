/* keystore.h : on-disk keys and credentials for the auth demo */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <stdint.h>
#include <stddef.h>

/*
 * Five small text files, all deliberately in the shape ssh uses, because the
 * shape is the point: the trust decisions live in files an administrator can
 * read and edit, not inside the protocol.
 *
 *   known_hosts      client: host -> server identity key, written on first
 *                    contact and compared forever after
 *   host_key         server: its X25519 identity secret
 *   authorized_keys  server: user -> Ed25519 public keys allowed to log in
 *   passwd           server: user -> Argon2id hash of a password
 *   id_netchan       client: its Ed25519 key pair, optionally sealed under a
 *                    passphrase
 *
 * Every field is hex, so parsing needs nothing but fgets and sscanf, and a
 * file stays diffable and pasteable.
 */

#define KS_LINE 512

/****************************************************************
 * Hex
 ****************************************************************/

void ks_hex_encode(char *out, const uint8_t *in, size_t n);   /* out: 2n+1 */
int  ks_hex_decode(uint8_t *out, size_t n, const char *in);   /* 0 on success */

/****************************************************************
 * Client: known_hosts, the trust-on-first-use store
 ****************************************************************/

enum {
    KS_HOST_UNKNOWN = 0,    /* no entry: first contact */
    KS_HOST_MATCH   = 1,    /* entry present and identical */
    KS_HOST_CHANGED = 2,    /* entry present and different: refuse loudly */
};

/* Look up host. On KS_HOST_CHANGED, stored (if non-NULL) receives the key
 * that was on file, for the warning message. */
int ks_known_host(const char *path, const char *host,
                  const uint8_t pk[32], uint8_t stored[32]);

/* Append host -> pk. Returns 0 on success. */
int ks_known_host_add(const char *path, const char *host, const uint8_t pk[32]);

/****************************************************************
 * Server: identity key, authorised keys, passwords
 ****************************************************************/

/* Read a 32-byte X25519 secret from path, generating and saving one with
 * mode 0600 if the file does not exist. Returns 0 on success. */
int ks_host_key(const char *path, uint8_t sk[32]);

/* Non-zero if pk appears on a line for user. */
int ks_authorized_key(const char *path, const char *user, const uint8_t pk[32]);

/* Non-zero if password matches the stored Argon2id hash for user. Always
 * spends the same work for an unknown user as for a known one, so timing
 * does not reveal which names exist. */
int ks_check_password(const char *path, const char *user, const char *password);

/* Append user with an Argon2id hash of password. Returns 0 on success. */
int ks_passwd_add(const char *path, const char *user, const char *password);

/* Non-zero if the file has any line for user. */
int ks_user_exists(const char *path, const char *user);

/****************************************************************
 * Client identity key file, optionally encrypted under a passphrase
 ****************************************************************/

/*
 * Generate an Ed25519 key pair and write it to path with mode 0600. If
 * passphrase is non-NULL and non-empty, the 64-byte secret is sealed with
 * XChaCha20-Poly1305 under a key stretched from the passphrase by Argon2id,
 * which is why unlocking it later costs a moment of CPU and nothing on the
 * network.
 */
int ks_keyfile_generate(const char *path, const char *passphrase,
                        uint8_t pk_out[32]);

/* Load a key file. Returns 0 on success, -1 if the file is unreadable or
 * malformed, -2 if the passphrase is wrong. */
int ks_keyfile_load(const char *path, const char *passphrase,
                    uint8_t sk[64], uint8_t pk[32]);

/* Non-zero if the key file at path is passphrase-protected. */
int ks_keyfile_encrypted(const char *path);

#endif /* KEYSTORE_H */
