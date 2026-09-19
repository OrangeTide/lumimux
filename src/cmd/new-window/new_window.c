/* new_window.c : create a new window in an existing session */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi-new-window [-s name] [command [args...]]\n");
}

int
cmd_new_window_main(int argc, char **argv)
{
	const char *name = "0";
	pid_t pid;
	int opt;

	while ((opt = getopt(argc, argv, "+s:")) != -1) {
		switch (opt) {
		case 's':
			name = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		int extra = argc - optind;	/* command words, may be 0 */
		int n = 3 + extra;
		char **cargv = malloc((size_t)(n + 1) * sizeof(*cargv));
		int i;

		if (!cargv)
			_exit(1);
		setsid();
		cargv[0] = "lumi-mserver";
		cargv[1] = "-s";
		cargv[2] = (char *)name;
		for (i = 0; i < extra; i++)
			cargv[3 + i] = argv[optind + i];
		cargv[n] = NULL;
		_exit(multicall_exec_cmd("mserver", n, cargv));
	}
	return 0;
}
