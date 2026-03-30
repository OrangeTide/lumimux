/* list.c : list active lumi sessions */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "multicall.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_SESSIONS 64

int
cmd_list_main(int argc, char **argv)
{
	char *names[MAX_SESSIONS];
	int n, i;

	(void)argc;
	(void)argv;

	n = sessdir_list_sessions(names, MAX_SESSIONS);
	if (n < 0)
		return 1;

	for (i = 0; i < n; i++) {
		pid_t pids[1];

		sessdir_cleanup_stale(names[i]);
		if (sessdir_list_servers(names[i], pids, 1) > 0)
			printf("%s\n", names[i]);
		free(names[i]);
	}

	return 0;
}
