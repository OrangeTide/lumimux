/* sessdir_watch.c : inotify watcher for session directory changes */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir_watch.h"
#include "sessdir.h"

#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

int
sessdir_watch_start(const char *session)
{
	char *path;
	int fd, wd;

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0)
		return -1;

	path = sessdir_session_path(session);
	if (!path) {
		close(fd);
		return -1;
	}

	wd = inotify_add_watch(fd, path,
	    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	free(path);
	if (wd < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

void
sessdir_watch_stop(int watch_fd)
{
	if (watch_fd >= 0)
		close(watch_fd);
}

int
sessdir_watch_read(int watch_fd)
{
	char buf[4096];
	ssize_t n;
	int flags = 0;

	for (;;) {
		n = read(watch_fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		/* any event means the directory changed */
		flags |= SESSDIR_WATCH_CHANGED;
	}
	return flags;
}
