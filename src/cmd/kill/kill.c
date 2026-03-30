/* kill.c : terminate a named lumi session */
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
	fprintf(stderr, "usage: lumi-kill [-s name]\n");
}

int
cmd_kill_main(int argc, char **argv)
{
	const char *name = "0";
	pid_t pids[MAX_SERVERS];
	int n, i, opt;
	int killed = 0;

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
		fprintf(stderr, "lumi-kill: session '%s' not found\n", name);
		return 1;
	}

	for (i = 0; i < n; i++) {
		char *dir, path[PATH_MAX];
		int fd;
		uint32_t type, len;
		char buf[256];

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

		if (ipc_msg_send_empty(fd, IPC_MSG_KILL) < 0) {
			ipc_close(fd);
			continue;
		}

		/* wait for OK response */
		if (ipc_msg_recv(fd, &type, buf, sizeof(buf), &len) == 0 &&
		    type == IPC_MSG_OK)
			killed++;

		ipc_close(fd);
	}

	if (killed == 0) {
		fprintf(stderr, "lumi-kill: failed to kill session '%s'\n",
		    name);
		return 1;
	}

	return 0;
}
