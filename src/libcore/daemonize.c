/* daemonize.c : fork into background as a daemon */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "daemonize.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int
daemonize(void)
{
	pid_t pid;

	if (getuid() != geteuid()) {
		fprintf(stderr, "refusing to daemonize as setuid\n");
		return -1;
	}

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);	/* parent exits */

	if (setsid() < 0)
		return -1;

	/* not a session leader, cannot acquire a controlling terminal */
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);	/* first child exits */

	int fd = open("/dev/null", O_RDWR);
	if (fd < 0)
		return -1;
	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO)
		close(fd);

	/* ignore job control signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);

	return 0;
}
