/* net_passwd.c : enroll a password for netchan direct-connect userauth */
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
	    "usage: lumi net-passwd [-f passwdfile] <user>\n"
	    "  Adds <user> to the server's passwd file with an Argon2id hash\n"
	    "  of a password read from the terminal.  -f overrides the path\n"
	    "  (default: ~/.config/lumi/passwd).\n");
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
cmd_net_passwd_main(int argc, char **argv)
{
	const char *path = NULL;
	const char *user;
	char pathbuf[256], pw[256], again[256];
	int opt, rc = 1;

	while ((opt = getopt(argc, argv, "f:")) != -1) {
		switch (opt) {
		case 'f':
			path = optarg;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind >= argc) {
		usage();
		return 2;
	}
	user = argv[optind];

	if (!path) {
		if (lumi_config_path(pathbuf, sizeof(pathbuf), "passwd") != 0) {
			fprintf(stderr, "lumi-net-passwd: cannot resolve "
			    "config directory\n");
			return 1;
		}
		path = pathbuf;
	}

	/* ks_passwd_add appends, so refuse a name that already has a line
	 * rather than silently creating a duplicate. */
	if (ks_user_exists(path, user)) {
		fprintf(stderr, "lumi-net-passwd: user '%s' already in %s; "
		    "remove its line first to change it\n", user, path);
		return 1;
	}

	if (prompt_hidden("Password: ", pw, sizeof(pw)) != 0)
		return 1;
	if (pw[0] == '\0') {
		fprintf(stderr, "lumi-net-passwd: empty password refused\n");
		goto out;
	}
	if (prompt_hidden("Same again: ", again, sizeof(again)) != 0)
		goto out;
	if (strcmp(pw, again) != 0) {
		fprintf(stderr, "lumi-net-passwd: passwords differ\n");
		goto out;
	}

	if (ks_passwd_add(path, user, pw) != 0) {
		fprintf(stderr, "lumi-net-passwd: cannot write %s\n", path);
		goto out;
	}
	printf("added '%s' to %s\n", user, path);
	rc = 0;

out:
	memset(pw, 0, sizeof(pw));
	memset(again, 0, sizeof(again));
	return rc;
}
