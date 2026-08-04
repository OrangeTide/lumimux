/* detach.c : detach clients from a session */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "sessdir_control.h"
#include "log.h"
#include "multicall.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* How long the clients are given to go. Detaching is cooperative: a client
 * acts when it next reads the session directory, which is a watch event
 * away, or one poll interval on a system with no watch to give us. */
#define DETACH_WAIT_MS	1500
#define MAX_SERVERS	64

static void
usage(void)
{
	fprintf(stderr, "usage: lumi-detach [-s name] [-c id]\n");
	fprintf(stderr,
	    "  -s name     session name (default: 0)\n"
	    "  -c id       detach just this client (default: all of them)\n");
}

int
cmd_detach_main(int argc, char **argv)
{
	const char *name = "0";
	unsigned long one = 0;
	pid_t pids[MAX_SERVERS];
	int opt, left;

	while ((opt = getopt(argc, argv, "s:c:")) != -1) {
		switch (opt) {
		case 's':
			name = optarg;
			break;
		case 'c':
			one = strtoul(optarg, NULL, 10);
			if (one == 0) {
				fprintf(stderr,
				    "lumi-detach: not a client id\n");
				return 1;
			}
			break;
		default:
			usage();
			return 1;
		}
	}

	sessdir_cleanup_stale(name);
	if (sessdir_list_servers(name, pids, MAX_SERVERS) <= 0) {
		fprintf(stderr, "lumi-detach: session '%s' not found\n",
		    name);
		return 1;
	}

	if (one) {
		struct sessdir_client roster[SESSDIR_CLIENT_MAX];
		int n, i;

		/* say so rather than posting into the void: a mistyped id
		 * would otherwise look like it worked */
		n = sessdir_client_list(name, roster, SESSDIR_CLIENT_MAX);
		for (i = 0; i < n; i++)
			if (roster[i].client_id == one)
				break;
		if (n < 0 || i == n) {
			fprintf(stderr, "lumi-detach: no client %lu attached "
			    "to session '%s'\n", one, name);
			return 1;
		}
		if (sessdir_ctl_post(name, SESSDIR_CTL_KICK, one, 0,
		    "lumi detach") < 0) {
			fprintf(stderr, "lumi-detach: could not post to "
			    "session '%s'\n", name);
			return 1;
		}
		return 0;
	}

	/* No id: every client goes. This is the "give me my session back"
	 * case, and it is the reason the command exists at all. */
	left = sessdir_kick_others(name, 0, "lumi detach", DETACH_WAIT_MS);
	if (left < 0) {
		fprintf(stderr, "lumi-detach: could not read the clients of "
		    "session '%s'\n", name);
		return 1;
	}
	if (left > 0) {
		fprintf(stderr, "lumi-detach: %d client%s did not detach\n",
		    left, left == 1 ? "" : "s");
		return 1;
	}
	return 0;
}
