/* pty.c : pseudo-terminal allocation and management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "pty.h"
#include "log.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

int
pty_open(int *child_pid, const char *shell)
{
	int master;
	pid_t pid;

	pid = forkpty(&master, NULL, NULL, NULL);
	if (pid < 0) {
		log_err("forkpty: %m");
		return -1;
	}

	if (pid == 0) {
		/* child -- reset signals and exec shell */
		signal(SIGCHLD, SIG_DFL);
		signal(SIGWINCH, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGHUP, SIG_DFL);

		/* set TERM so child shell uses the right terminfo entry */
		setenv("TERM", "screen-256color", 1);

		if (!shell)
			shell = getenv("SHELL");
		if (!shell)
			shell = "/bin/sh";

		execlp(shell, shell, (char *)NULL);
		_exit(127);
	}

	/* parent */
	*child_pid = (int)pid;

	/* set master non-blocking */
	{
		int flags = fcntl(master, F_GETFL);

		if (flags >= 0)
			fcntl(master, F_SETFL, flags | O_NONBLOCK);
	}

	return master;
}

int
pty_resize(int master_fd, int rows, int cols)
{
	struct winsize ws = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
	};

	if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0) {
		log_err("TIOCSWINSZ: %m");
		return -1;
	}
	return 0;
}

void
pty_close(int master_fd)
{
	close(master_fd);
}
