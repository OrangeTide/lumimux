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
	    "usage: lumi-new [-Ad] [-m mode] [-s name] [shell]\n");
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
	int opt;
	int detach = 0;
	int reattach = 0;
	int exists = 0;

	while ((opt = getopt(argc, argv, "Adm:s:")) != -1) {
		switch (opt) {
		case 'A':
			reattach = 1;
			break;
		case 'd':
			detach = 1;
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

	if (mode) {
		char *attach_argv[] = {
		    "lumi-attach", "-m", (char *)mode,
		    "-s", (char *)name, NULL,
		};
		return multicall_exec_cmd("attach", 5, attach_argv);
	} else {
		char *attach_argv[] = {
		    "lumi-attach", "-s", (char *)name, NULL,
		};
		return multicall_exec_cmd("attach", 3, attach_argv);
	}
}
