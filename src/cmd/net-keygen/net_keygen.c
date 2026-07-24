/* net_keygen.c : make a netchan client identity key for attach userauth */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "multicall.h"
#include "keystore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi net-keygen [-f keyfile] [-N passphrase] [-q]\n"
	    "  -f keyfile   key path (default: ~/.config/lumi/id_netchan)\n"
	    "  -N phrase    passphrase to seal the key; empty leaves it\n"
	    "               unencrypted.  Without -N the tool prompts.\n"
	    "  -q           print only the public key hex\n");
}

/* Build ~/.config/lumi/<name> (honoring XDG_CONFIG_HOME) into buf, creating
 * the directory if needed.  Returns 0 on success, -1 on no home dir,
 * truncation, or a directory that cannot be created. */
static int
lumi_config_path(char *buf, size_t sz, const char *name)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char dir[256];
	int n;

	if (xdg && *xdg)
		n = snprintf(dir, sizeof(dir), "%s/lumi", xdg);
	else if (home && *home)
		n = snprintf(dir, sizeof(dir), "%s/.config/lumi", home);
	else
		return -1;
	if (n <= 0 || (size_t)n >= sizeof(dir))
		return -1;

	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -1;

	n = snprintf(buf, sz, "%s/%s", dir, name);
	return (n > 0 && (size_t)n < sz) ? 0 : -1;
}

/* Read a line from the terminal with echo suppressed.  Returns 0 on success,
 * -1 on error.  The trailing newline is stripped. */
static int
prompt_hidden(const char *msg, char *out, size_t sz)
{
	struct termios old, raw;
	int have_tty;

	fputs(msg, stderr);
	fflush(stderr);

	have_tty = isatty(STDIN_FILENO);
	if (have_tty) {
		if (tcgetattr(STDIN_FILENO, &old) != 0) {
			have_tty = 0;
		} else {
			raw = old;
			raw.c_lflag &= ~(tcflag_t)ECHO;
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
		}
	}

	if (!fgets(out, (int)sz, stdin)) {
		if (have_tty) {
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
			fputc('\n', stderr);
		}
		return -1;
	}

	if (have_tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

int
cmd_net_keygen_main(int argc, char **argv)
{
	const char *path = NULL;
	const char *pass = NULL;
	char pathbuf[256], buf[256], again[256], hex[65];
	uint8_t pk[32];
	int quiet = 0, opt;

	while ((opt = getopt(argc, argv, "f:N:q")) != -1) {
		switch (opt) {
		case 'f':
			path = optarg;
			break;
		case 'N':
			pass = optarg;
			break;
		case 'q':
			quiet = 1;
			break;
		default:
			usage();
			return 2;
		}
	}

	if (!path) {
		if (lumi_config_path(pathbuf, sizeof(pathbuf),
		    "id_netchan") != 0) {
			fprintf(stderr, "lumi-net-keygen: cannot resolve "
			    "config directory\n");
			return 1;
		}
		path = pathbuf;
	}

	/* Refuse to clobber an existing key: overwriting locks the user out
	 * of any server that already trusts the old public key. */
	if (access(path, F_OK) == 0) {
		fprintf(stderr, "lumi-net-keygen: %s already exists; remove it "
		    "first to replace it\n", path);
		return 1;
	}

	if (!pass) {
		if (prompt_hidden("Passphrase (empty for none): ", buf,
		    sizeof(buf)) != 0)
			return 1;
		if (buf[0] != '\0') {
			if (prompt_hidden("Same again: ", again,
			    sizeof(again)) != 0)
				return 1;
			if (strcmp(buf, again) != 0) {
				fprintf(stderr,
				    "lumi-net-keygen: passphrases differ\n");
				return 1;
			}
		}
		pass = buf;
	}

	if (ks_keyfile_generate(path, pass, pk) != 0) {
		fprintf(stderr, "lumi-net-keygen: cannot write %s\n", path);
		return 1;
	}

	ks_hex_encode(hex, pk, sizeof(pk));
	if (quiet) {
		printf("%s\n", hex);
	} else {
		printf("wrote %s (%s)\n", path,
		    pass[0] ? "passphrase protected" : "unencrypted");
		printf("add this line to the server's authorized_keys, with "
		    "your login name:\n\n    <username> %s\n\n", hex);
	}
	return 0;
}
