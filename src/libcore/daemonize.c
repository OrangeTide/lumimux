/* daemonize.c : fork into background as a daemon */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "daemonize.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int
daemonize(void)
{
	pid_t pid, sid;

	/* refuse to run as setuid -- safety check */
	if (getuid() != geteuid()) {
		fprintf(stderr, "refusing to daemonize as setuid\n");
		return -1;
	}

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);	/* parent exits */

	sid = setsid();
	if (sid < 0)
		return -1;

	/* redirect stdio to /dev/null */
	if (!freopen("/dev/null", "r", stdin) ||
	    !freopen("/dev/null", "w", stdout) ||
	    !freopen("/dev/null", "w", stderr))
		return -1;

	/* ignore job control signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);

	return 0;
}
