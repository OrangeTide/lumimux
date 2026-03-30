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
	fprintf(stderr, "usage: lumi-new-window [-s name] [shell]\n");
}

int
cmd_new_window_main(int argc, char **argv)
{
	const char *name = "0";
	const char *shell = NULL;
	pid_t pid;
	int opt;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		switch (opt) {
		case 's':
			name = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}
	if (optind < argc)
		shell = argv[optind];

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		setsid();
		if (shell) {
			char *child_argv[] = {
			    "lumi-mserver", "-s", (char *)name,
			    (char *)shell, NULL,
			};
			_exit(multicall_exec_cmd("mserver", 4, child_argv));
		} else {
			char *child_argv[] = {
			    "lumi-mserver", "-s", (char *)name, NULL,
			};
			_exit(multicall_exec_cmd("mserver", 3, child_argv));
		}
	}
	return 0;
}
