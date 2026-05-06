/* new.c : create a new lumi session */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "multicall.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi-new [-Ad] [-f window] [-m mode] [-s name]"
	    " [shell]\n");
	fprintf(stderr,
	    "  -A          reattach if session already exists\n"
	    "  -d          create session but do not attach\n"
	    "  -f window   focus this window (by PID) after startup\n"
	    "  -m mode     UI mode: screen (default), turbo, minimal\n"
	    "  -s name     session name (default: 0)\n");
}

/* launch lumi-mserver in the background.
 * the child registers in sessdir, so attach discovers it. */
static int
launch_mserver(const char *name, const char *shell)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
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

	/* give mserver time to create sessdir + socket */
	usleep(100000);
	return 0;
}

int
cmd_new_main(int argc, char **argv)
{
	const char *name = "0";
	const char *shell = NULL;
	const char *mode = NULL;
	const char *focus_window = NULL;
	int opt;
	int detach = 0;
	int reattach = 0;
	int exists = 0;

	while ((opt = getopt(argc, argv, "Adf:m:s:")) != -1) {
		switch (opt) {
		case 'A':
			reattach = 1;
			break;
		case 'd':
			detach = 1;
			break;
		case 'f':
			focus_window = optarg;
			break;
		case 'm':
			mode = optarg;
			break;
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

	/* check if session already exists */
	{
		pid_t pids[1];

		if (sessdir_list_servers(name, pids, 1) > 0)
			exists = 1;
	}

	if (exists && !reattach) {
		fprintf(stderr,
		    "lumi-new: session '%s' already exists"
		    " (use -A to reattach)\n", name);
		return 1;
	}

	if (!exists) {
		sessdir_session_create(name);

		if (launch_mserver(name, shell) < 0)
			return 1;
	}

	if (detach)
		return 0;

	{
		char *attach_argv[10];
		int ac = 0;

		attach_argv[ac++] = "lumi-attach";
		if (focus_window) {
			attach_argv[ac++] = "-f";
			attach_argv[ac++] = (char *)focus_window;
		}
		if (mode) {
			attach_argv[ac++] = "-m";
			attach_argv[ac++] = (char *)mode;
		}
		attach_argv[ac++] = "-s";
		attach_argv[ac++] = (char *)name;
		attach_argv[ac] = NULL;
		return multicall_exec_cmd("attach", ac, attach_argv);
	}
}
