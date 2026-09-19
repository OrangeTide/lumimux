/* send_keys.c : send keystrokes to a session */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *progname = "lumi-send-keys";

/* Named keys and the bytes a terminal sends for them. Arrow, navigation, and
 * function keys use the common xterm forms. */
static const struct {
	const char	*name;
	const char	*seq;
} keynames[] = {
	{ "enter",	"\r" },
	{ "return",	"\r" },
	{ "tab",	"\t" },
	{ "escape",	"\033" },
	{ "esc",	"\033" },
	{ "space",	" " },
	{ "bspace",	"\177" },
	{ "backspace",	"\177" },
	{ "up",		"\033[A" },
	{ "down",	"\033[B" },
	{ "right",	"\033[C" },
	{ "left",	"\033[D" },
	{ "home",	"\033[H" },
	{ "end",	"\033[F" },
	{ "ppage",	"\033[5~" },
	{ "pageup",	"\033[5~" },
	{ "npage",	"\033[6~" },
	{ "pagedown",	"\033[6~" },
	{ "ic",		"\033[2~" },
	{ "insert",	"\033[2~" },
	{ "dc",		"\033[3~" },
	{ "delete",	"\033[3~" },
	{ "f1",		"\033OP" },
	{ "f2",		"\033OQ" },
	{ "f3",		"\033OR" },
	{ "f4",		"\033OS" },
	{ "f5",		"\033[15~" },
	{ "f6",		"\033[17~" },
	{ "f7",		"\033[18~" },
	{ "f8",		"\033[19~" },
	{ "f9",		"\033[20~" },
	{ "f10",	"\033[21~" },
	{ "f11",	"\033[23~" },
	{ "f12",	"\033[24~" },
};

/* A growable byte buffer for the assembled key sequence. */
struct buf {
	char	*p;
	size_t	len;
	size_t	cap;
};

static int
buf_add(struct buf *b, const char *s, size_t n)
{
	if (b->len + n > b->cap) {
		size_t nc = b->cap ? b->cap : 64;
		char *np;

		while (nc < b->len + n)
			nc *= 2;
		np = realloc(b->p, nc);
		if (!np)
			return -1;
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	return 0;
}

static const char *
lookup_key(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(keynames) / sizeof(keynames[0]); i++)
		if (strcasecmp(name, keynames[i].name) == 0)
			return keynames[i].seq;
	return NULL;
}

/* Translate one argument and append its bytes. A name matching the key table
 * (optionally prefixed with C- for Ctrl and M- for Alt) emits that key; a
 * single character with modifiers emits the control byte or an Esc prefix.
 * Anything else, including an unresolvable modifier combination, is sent
 * verbatim, as tmux does. With `literal`, the argument is always verbatim. */
static int
add_arg(struct buf *b, const char *arg, int literal)
{
	const char *p = arg, *seq;
	int ctrl = 0, alt = 0;

	if (literal)
		return buf_add(b, arg, strlen(arg));

	for (;;) {
		if ((p[0] == 'C' || p[0] == 'c') && p[1] == '-') {
			ctrl = 1;
			p += 2;
		} else if ((p[0] == 'M' || p[0] == 'm') && p[1] == '-') {
			alt = 1;
			p += 2;
		} else {
			break;
		}
	}

	seq = lookup_key(p);
	if (seq && !ctrl) {		/* named key, no Ctrl form here */
		if (alt && buf_add(b, "\033", 1) < 0)
			return -1;
		return buf_add(b, seq, strlen(seq));
	}
	if (!seq && strlen(p) == 1) {	/* a single character */
		unsigned char c = (unsigned char)p[0];

		if (ctrl)
			c &= 0x1f;
		if (alt && buf_add(b, "\033", 1) < 0)
			return -1;
		return buf_add(b, (char *)&c, 1);
	}
	if (!seq && alt && !ctrl)	/* Alt + a literal string */
		return buf_add(b, "\033", 1) < 0 ? -1 :
		    buf_add(b, p, strlen(p));

	/* unrecognized (or Ctrl on a multi-byte key): send the arg verbatim */
	return buf_add(b, arg, strlen(arg));
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-s session] [-w pid | -i index | -o] [-l] key...\n"
	    "\n"
	    "Send keystrokes to a session window. Each argument is a key name\n"
	    "(Enter, Tab, Escape, Space, Up/Down/Left/Right, Home, End, PPage,\n"
	    "NPage, IC, DC, F1-F12), a modified key (C-a for Ctrl, M-x for Alt),\n"
	    "or literal text. -l sends every argument literally.\n"
	    "\n"
	    "  -s session  target session (default $LUMI_SESSION or 0)\n"
	    "  -w pid      target the window with this server pid\n"
	    "  -i index    target the window with this number (as in the tab bar)\n"
	    "  -o          target a window other than the focused one\n"
	    "  -l          treat all arguments as literal text\n",
	    progname);
}

int
cmd_send_keys_main(int argc, char **argv)
{
	const char *session = NULL;
	struct buf b = { NULL, 0, 0 };
	pid_t target = 0;
	int opt, literal = 0, i, rc = 0, index = -1;

	if (argv[0])
		progname = argv[0];

	while ((opt = getopt(argc, argv, "s:w:i:ol")) != -1) {
		switch (opt) {
		case 's':
			session = optarg;
			break;
		case 'w':
			target = (pid_t)strtol(optarg, NULL, 10);
			if (target <= 0) {
				fprintf(stderr, "%s: bad window pid '%s'\n",
				    progname, optarg);
				return 1;
			}
			break;
		case 'i': {
			char *end;

			index = (int)strtol(optarg, &end, 10);
			if (*end != '\0' || index < 0) {
				fprintf(stderr, "%s: bad window index '%s'\n",
				    progname, optarg);
				return 1;
			}
			break;
		}
		case 'o':
			target = -1;
			break;
		case 'l':
			literal = 1;
			break;
		default:
			usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return 1;
	}

	if (!session) {
		session = getenv("LUMI_SESSION");
		if (!session)
			session = "0";
	}

	if (index >= 0) {			/* -i resolves to a server pid */
		target = lu_window_pid(session, index);
		if (target == 0) {
			fprintf(stderr, "%s: no window numbered %d in session "
			    "'%s'\n", progname, index, session);
			return 1;
		}
	}

	for (i = 0; i < argc; i++) {
		if (add_arg(&b, argv[i], literal) < 0) {
			fprintf(stderr, "%s: out of memory\n", progname);
			free(b.p);
			return 1;
		}
	}
	if (b.len == 0) {
		free(b.p);
		return 0;
	}

	switch (lu_send_input(session, target, b.p, b.len)) {
	case LU_SEND_OK:
		break;
	case LU_SEND_NO_SESSION:
		fprintf(stderr, "%s: session '%s' not found\n", progname,
		    session);
		rc = 1;
		break;
	case LU_SEND_NO_TARGET:
		fprintf(stderr, "%s: no target window\n", progname);
		rc = 1;
		break;
	case LU_SEND_READONLY:
		fprintf(stderr, "%s: window is read-only; another client holds "
		    "the keyboard\n", progname);
		rc = 1;
		break;
	case LU_SEND_ERROR:
		fprintf(stderr, "%s: sending keys failed\n", progname);
		rc = 1;
		break;
	}

	free(b.p);
	return rc;
}
