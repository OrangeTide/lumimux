/* keystore.c : on-disk keys and credentials for the auth demo */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "keystore.h"
#include "monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * OS entropy, the same selection crypto/nc_crypto.c makes. See the comment
 * there for why it is spelled out twice rather than shared.
 */
#if defined(_WIN32)
#  define KS_RANDOM_BCRYPT 1
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#  define KS_RANDOM_ARC4 1
#else
#  if defined(__linux__)
#    define KS_RANDOM_GETRANDOM 1
#    include <sys/random.h>
#    include <errno.h>
#  endif
#endif

/*
 * Argon2id parameters. 8 MiB and three passes is far below what a password
 * vault should use, and chosen so the demo's tests stay quick. A real
 * deployment turns nb_blocks up until a login costs a noticeable fraction of
 * a second, which is the entire defence against an offline guessing attack
 * on a stolen file.
 */
#define KS_ARGON_BLOCKS 8192
#define KS_ARGON_PASSES 3
#define KS_SALT_LEN     16

static const char KEYFILE_MAGIC[] = "netchan-key-v1";

/****************************************************************
 * Small helpers
 ****************************************************************/

static int
ks_random(uint8_t *buf, size_t n)
{
#if defined(KS_RANDOM_BCRYPT)
    /* STATUS_SUCCESS is 0. */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#elif defined(KS_RANDOM_ARC4)
    arc4random_buf(buf, n);
    return 0;
#else
    FILE *f;
    size_t got;

#  if defined(KS_RANDOM_GETRANDOM)
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

void
ks_hex_encode(char *out, const uint8_t *in, size_t n)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int
hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int
ks_hex_decode(uint8_t *out, size_t n, const char *in)
{
    if (strlen(in) != n * 2)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(in[i * 2]);
        int lo = hex_nibble(in[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/*
 * Open for append or create, always mode 0600: these files hold secrets or
 * trust decisions and none of them is anyone else's business.
 *
 * Two things differ on Windows and neither is fixed here. The 0600 is
 * ignored, because permissions are ACLs and the CRT has nowhere to put a
 * POSIX mode, so a key file is left at whatever the containing directory
 * grants. And the streams are opened in text mode, so every "\n" written
 * becomes "\r\n" on disk and is translated back on the way in. Every format
 * in this file is line-oriented hex, so it round-trips either way, and a
 * file written on one platform still reads on the other. Anything added
 * here that is position-sensitive or genuinely binary would need "b" in the
 * mode string and a second look at both points.
 */
static FILE *
open_private(const char *path, const char *mode, int flags)
{
    int fd = open(path, flags, 0600);
    FILE *f;

    if (fd < 0)
        return NULL;
    f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

static void
stretch(uint8_t out[32], const char *passphrase, const uint8_t salt[KS_SALT_LEN])
{
    crypto_argon2_config config;
    crypto_argon2_inputs inputs;
    void *work;

    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = KS_ARGON_BLOCKS;
    config.nb_passes = KS_ARGON_PASSES;
    config.nb_lanes  = 1;

    inputs.pass      = (const uint8_t *)passphrase;
    inputs.pass_size = (uint32_t)strlen(passphrase);
    inputs.salt      = salt;
    inputs.salt_size = KS_SALT_LEN;

    work = malloc((size_t)KS_ARGON_BLOCKS * 1024);
    if (!work) {
        /* Failing closed beats deriving a guessable key. */
        memset(out, 0, 32);
        return;
    }
    crypto_argon2(out, 32, work, config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work, (size_t)KS_ARGON_BLOCKS * 1024);
    free(work);
}

/****************************************************************
 * known_hosts
 ****************************************************************/

int
ks_known_host(const char *path, const char *host, const uint8_t pk[32],
              uint8_t stored[32])
{
    char line[KS_LINE], name[KS_LINE], hex[KS_LINE];
    FILE *f = fopen(path, "r");
    int result = KS_HOST_UNKNOWN;

    if (!f)
        return KS_HOST_UNKNOWN;

    while (fgets(line, sizeof(line), f)) {
        uint8_t on_file[32];

        if (line[0] == '#' || sscanf(line, "%511s %511s", name, hex) != 2)
            continue;
        if (strcmp(name, host) != 0)
            continue;
        if (ks_hex_decode(on_file, 32, hex) != 0)
            continue;
        if (crypto_verify32(on_file, pk) == 0) {
            result = KS_HOST_MATCH;
        } else {
            result = KS_HOST_CHANGED;
            if (stored)
                memcpy(stored, on_file, 32);
        }
        break;
    }
    fclose(f);
    return result;
}

int
ks_known_host_add(const char *path, const char *host, const uint8_t pk[32])
{
    char hex[65];
    FILE *f = open_private(path, "a", O_WRONLY | O_CREAT | O_APPEND);

    if (!f)
        return -1;
    ks_hex_encode(hex, pk, 32);
    fprintf(f, "%s %s\n", host, hex);
    fclose(f);
    return 0;
}

/****************************************************************
 * Server host key
 ****************************************************************/

int
ks_host_key(const char *path, uint8_t sk[32])
{
    char hex[KS_LINE];
    FILE *f = fopen(path, "r");

    if (f) {
        int ok = fscanf(f, "%511s", hex) == 1 && ks_hex_decode(sk, 32, hex) == 0;

        fclose(f);
        return ok ? 0 : -1;
    }

    if (ks_random(sk, 32) != 0)
        return -1;
    f = open_private(path, "w", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return -1;
    ks_hex_encode(hex, sk, 32);
    fprintf(f, "%s\n", hex);
    fclose(f);
    return 0;
}

/****************************************************************
 * authorized_keys and passwd
 ****************************************************************/

int
ks_authorized_key(const char *path, const char *user, const uint8_t pk[32])
{
    char line[KS_LINE], name[KS_LINE], hex[KS_LINE];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        uint8_t on_file[32];

        if (line[0] == '#' || sscanf(line, "%511s %511s", name, hex) != 2)
            continue;
        if (strcmp(name, user) != 0)
            continue;
        if (ks_hex_decode(on_file, 32, hex) != 0)
            continue;
        if (crypto_verify32(on_file, pk) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int
ks_user_exists(const char *path, const char *user)
{
    char line[KS_LINE], name[KS_LINE];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%511s", name) != 1)
            continue;
        if (strcmp(name, user) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int
ks_check_password(const char *path, const char *user, const char *password)
{
    char line[KS_LINE], name[KS_LINE], salt_hex[KS_LINE], hash_hex[KS_LINE];
    uint8_t salt[KS_SALT_LEN], want[32], got[32];
    FILE *f = fopen(path, "r");
    int have_entry = 0, ok = 0;

    memset(salt, 0, sizeof(salt));
    memset(want, 0, sizeof(want));

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' ||
                sscanf(line, "%511s %511s %511s", name, salt_hex, hash_hex) != 3)
                continue;
            if (strcmp(name, user) != 0)
                continue;
            if (ks_hex_decode(salt, sizeof(salt), salt_hex) != 0 ||
                ks_hex_decode(want, sizeof(want), hash_hex) != 0)
                continue;
            have_entry = 1;
            break;
        }
        fclose(f);
    }

    /*
     * Stretch even when the account does not exist. Skipping the work would
     * make an unknown name answer far faster than a known one, which is a
     * free account-enumeration oracle over an otherwise perfect channel.
     */
    stretch(got, password, salt);
    if (have_entry)
        ok = crypto_verify32(got, want) == 0;

    crypto_wipe(got, sizeof(got));
    crypto_wipe(want, sizeof(want));
    return ok;
}

int
ks_passwd_add(const char *path, const char *user, const char *password)
{
    uint8_t salt[KS_SALT_LEN], hash[32];
    char salt_hex[KS_SALT_LEN * 2 + 1], hash_hex[65];
    FILE *f;

    if (ks_random(salt, sizeof(salt)) != 0)
        return -1;
    stretch(hash, password, salt);

    f = open_private(path, "a", O_WRONLY | O_CREAT | O_APPEND);
    if (!f) {
        crypto_wipe(hash, sizeof(hash));
        return -1;
    }
    ks_hex_encode(salt_hex, salt, sizeof(salt));
    ks_hex_encode(hash_hex, hash, sizeof(hash));
    fprintf(f, "%s %s %s\n", user, salt_hex, hash_hex);
    fclose(f);
    crypto_wipe(hash, sizeof(hash));
    return 0;
}

/****************************************************************
 * Client identity key file
 ****************************************************************/

int
ks_keyfile_generate(const char *path, const char *passphrase, uint8_t pk_out[32])
{
    uint8_t seed[32], sk[64], pk[32], salt[KS_SALT_LEN], key[32];
    uint8_t sealed[64 + 16];
    char hex[161];
    int encrypted = passphrase && passphrase[0] != '\0';
    FILE *f;

    if (ks_random(seed, sizeof(seed)) != 0)
        return -1;
    crypto_eddsa_key_pair(sk, pk, seed);   /* wipes seed */

    f = open_private(path, "w", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        crypto_wipe(sk, sizeof(sk));
        return -1;
    }

    fprintf(f, "%s\n", KEYFILE_MAGIC);
    ks_hex_encode(hex, pk, 32);
    fprintf(f, "pk %s\n", hex);

    if (encrypted) {
        uint8_t nonce[24];

        if (ks_random(salt, sizeof(salt)) != 0) {
            fclose(f);
            crypto_wipe(sk, sizeof(sk));
            return -1;
        }
        stretch(key, passphrase, salt);
        /* The salt is fresh per file and the key is used once, so a fixed
         * all-zero nonce is safe here and one less thing to store. */
        memset(nonce, 0, sizeof(nonce));
        crypto_aead_lock(sealed + 16, sealed, key, nonce, NULL, 0, sk, 64);
        crypto_wipe(key, sizeof(key));

        fprintf(f, "kdf argon2id\n");
        ks_hex_encode(hex, salt, sizeof(salt));
        fprintf(f, "salt %s\n", hex);
        ks_hex_encode(hex, sealed, sizeof(sealed));
        fprintf(f, "sk %s\n", hex);
    } else {
        fprintf(f, "kdf none\n");
        ks_hex_encode(hex, sk, 64);
        fprintf(f, "sk %s\n", hex);
    }

    fclose(f);
    if (pk_out)
        memcpy(pk_out, pk, 32);
    crypto_wipe(sk, sizeof(sk));
    return 0;
}

/* Pull the "sk" and "kdf" lines out of a key file without committing to
 * what they mean yet; both loaders below want the same scan. */
static int
keyfile_scan(const char *path, char *kdf, size_t kdf_cap,
             char *sk_hex, size_t sk_cap, char *salt_hex, size_t salt_cap,
             uint8_t pk[32])
{
    char line[KS_LINE], key[KS_LINE], val[KS_LINE];
    FILE *f = fopen(path, "r");
    int have_pk = 0, have_sk = 0;

    if (!f)
        return -1;

    kdf[0] = salt_hex[0] = sk_hex[0] = '\0';
    if (!fgets(line, sizeof(line), f) ||
        strncmp(line, KEYFILE_MAGIC, strlen(KEYFILE_MAGIC)) != 0) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%511s %511s", key, val) != 2)
            continue;
        if (strcmp(key, "pk") == 0 && ks_hex_decode(pk, 32, val) == 0)
            have_pk = 1;
        else if (strcmp(key, "kdf") == 0)
            snprintf(kdf, kdf_cap, "%s", val);
        else if (strcmp(key, "salt") == 0)
            snprintf(salt_hex, salt_cap, "%s", val);
        else if (strcmp(key, "sk") == 0) {
            snprintf(sk_hex, sk_cap, "%s", val);
            have_sk = 1;
        }
    }
    fclose(f);
    return (have_pk && have_sk) ? 0 : -1;
}

int
ks_keyfile_encrypted(const char *path)
{
    char kdf[KS_LINE], sk_hex[KS_LINE], salt_hex[KS_LINE];
    uint8_t pk[32];

    if (keyfile_scan(path, kdf, sizeof(kdf), sk_hex, sizeof(sk_hex),
                     salt_hex, sizeof(salt_hex), pk) != 0)
        return 0;
    return strcmp(kdf, "argon2id") == 0;
}

int
ks_keyfile_load(const char *path, const char *passphrase,
                uint8_t sk[64], uint8_t pk[32])
{
    char kdf[KS_LINE], sk_hex[KS_LINE], salt_hex[KS_LINE];
    uint8_t salt[KS_SALT_LEN], key[32], sealed[64 + 16], nonce[24];
    int rc;

    if (keyfile_scan(path, kdf, sizeof(kdf), sk_hex, sizeof(sk_hex),
                     salt_hex, sizeof(salt_hex), pk) != 0)
        return -1;

    if (strcmp(kdf, "none") == 0)
        return ks_hex_decode(sk, 64, sk_hex) == 0 ? 0 : -1;

    if (strcmp(kdf, "argon2id") != 0 ||
        ks_hex_decode(salt, sizeof(salt), salt_hex) != 0 ||
        ks_hex_decode(sealed, sizeof(sealed), sk_hex) != 0)
        return -1;

    if (!passphrase)
        return -2;

    stretch(key, passphrase, salt);
    memset(nonce, 0, sizeof(nonce));
    /* A wrong passphrase yields a wrong key, and the Poly1305 tag is what
     * turns that into a clean "bad passphrase" instead of 64 bytes of
     * garbage that would later fail as an unexplained signature error. */
    rc = crypto_aead_unlock(sk, sealed, key, nonce, NULL, 0, sealed + 16, 64);
    crypto_wipe(key, sizeof(key));
    return rc == 0 ? 0 : -2;
}
