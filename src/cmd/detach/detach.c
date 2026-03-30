/* detach.c : tell server to detach a client */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "sessdir.h"
#include "log.h"
#include "multicall.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_SERVERS 64

static void
usage(void)
{
	fprintf(stderr, "usage: lumi-detach [-s name]\n");
}

int
cmd_detach_main(int argc, char **argv)
{
	const char *name = "0";
	pid_t pids[MAX_SERVERS];
	int n, i, opt;

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

	sessdir_cleanup_stale(name);
	n = sessdir_list_servers(name, pids, MAX_SERVERS);
	if (n <= 0) {
		fprintf(stderr, "lumi-detach: session '%s' not found\n",
		    name);
		return 1;
	}

	for (i = 0; i < n; i++) {
		char *dir, path[PATH_MAX];
		int fd;

		dir = sessdir_server_path(name, pids[i]);
		if (!dir)
			continue;
		if (snprintf(path, sizeof(path), "%s/socket", dir)
		    >= (int)sizeof(path)) {
			free(dir);
			continue;
		}
		free(dir);

		fd = ipc_connect(path);
		if (fd < 0)
			continue;

		ipc_msg_send_empty(fd, IPC_MSG_DETACH);
		ipc_close(fd);
	}

	return 0;
}
