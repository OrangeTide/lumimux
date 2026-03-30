/* ipc.c : Unix domain socket helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc.h"
#include "ipc_msg.h"
#include "xmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <limits.h>

static void
set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

char *
ipc_socket_dir(void)
{
	const char *xdg;
	char *dir;
	int rc;

	xdg = getenv("XDG_RUNTIME_DIR");
	dir = xmalloc(PATH_MAX);
	if (xdg && xdg[0])
		rc = snprintf(dir, PATH_MAX, "%s/lumi", xdg);
	else
		rc = snprintf(dir, PATH_MAX, "/tmp/lumi-%d", (int)getuid());
	if (rc < 0 || rc > PATH_MAX) {
		free(dir);
		return NULL;
	}

	/* create directory if it doesn't exist */
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		free(dir);
		return NULL;
	}

	return dir;
}

char *
ipc_socket_path(const char *name)
{
	char *dir, *path;
	int rc;

	dir = ipc_socket_dir();
	if (!dir)
		return NULL;

	path = xmalloc(PATH_MAX);
	rc = snprintf(path, PATH_MAX, "%s/%s.sock", dir, name);
	free(dir);
	if (rc < 0 || rc > PATH_MAX) {
		free(path);
		return NULL;
	}
	return path;
}

int
ipc_listen(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	set_cloexec(fd);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	/* remove stale socket */
	unlink(path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	if (listen(fd, 4) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}

	return fd;
}

int
ipc_accept(int listen_fd)
{
	int fd;

	fd = accept(listen_fd, NULL, NULL);
	if (fd < 0)
		return -1;

	set_cloexec(fd);
	return fd;
}

int
ipc_connect(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	set_cloexec(fd);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

void
ipc_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

void
ipc_unlink(const char *path)
{
	if (path)
		unlink(path);
}

int
ipc_command(const char *name, uint32_t msg_type, const char *cmd_name)
{
	char *path;
	int fd;

	path = ipc_socket_path(name);
	if (!path)
		return -1;

	fd = ipc_connect(path);
	if (fd < 0) {
		fprintf(stderr, "%s: session '%s' not found\n",
		    cmd_name, name);
		free(path);
		return -1;
	}
	free(path);

	if (ipc_msg_send_empty(fd, msg_type) < 0) {
		ipc_close(fd);
		return -1;
	}

	ipc_close(fd);
	return 0;
}
