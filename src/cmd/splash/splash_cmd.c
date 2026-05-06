/* splash_cmd.c : display ANSI art splash screen */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash.h"
#include "multicall.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static const char *progname = "lumi-splash";

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [space|mountain|beach] [name]\n"
	    "\n"
	    "Display an ANSI art splash screen.\n"
	    "Press any key to dismiss.\n",
	    progname);
}

static int
get_term_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0)
		return -1;
	*rows = ws.ws_row;
	*cols = ws.ws_col;
	return 0;
}

int
cmd_splash_main(int argc, char **argv)
{
	enum splash_scene scene;
	const char *name;
	struct vt_buf *buf;
	int rows, cols;
	struct termios old, raw;
	char c;

	if (argv[0])
		progname = argv[0];

	scene = SPLASH_SPACE;
	name = "lumiMUX";

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			usage();
			return 0;
		} else if (strcmp(argv[1], "space") == 0) {
			scene = SPLASH_SPACE;
		} else if (strcmp(argv[1], "mountain") == 0) {
			scene = SPLASH_MOUNTAIN;
		} else if (strcmp(argv[1], "beach") == 0) {
			scene = SPLASH_BEACH;
		} else {
			fprintf(stderr, "%s: unknown scene '%s'\n",
			    progname, argv[1]);
			usage();
			return 1;
		}
	}

	if (argc > 2)
		name = argv[2];

	if (get_term_size(&rows, &cols) < 0) {
		rows = 24;
		cols = 80;
	}

	buf = splash_create(scene, name);
	if (!buf) {
		fprintf(stderr, "%s: splash_create failed\n", progname);
		return 1;
	}

	/* raw mode so we can wait for a single keypress */
	tcgetattr(STDIN_FILENO, &old);
	raw = old;
	raw.c_lflag &= (unsigned)~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);

	splash_show(buf, STDOUT_FILENO, rows, cols);

	/* wait for keypress */
	(void)read(STDIN_FILENO, &c, 1);

	/* restore terminal */
	tcsetattr(STDIN_FILENO, TCSANOW, &old);
	printf("\033[2J\033[H\033[?25h");
	fflush(stdout);

	splash_free(buf);
	return 0;
}
