/* sessdir.c : session directory management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "xmalloc.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *
sessdir_base(void)
{
	const char *xdg;
	char *dir;
	int rc;

	xdg = getenv("XDG_RUNTIME_DIR");
	dir = xmalloc(PATH_MAX);
	if (xdg && xdg[0])
		rc = snprintf(dir, PATH_MAX, "%s/lumi", xdg);
	else
		rc = snprintf(dir, PATH_MAX, "/tmp/lumi-%d",
		    (int)getuid());
	if (rc < 0 || rc >= PATH_MAX) {
		free(dir);
		return NULL;
	}

	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		free(dir);
		return NULL;
	}

	return dir;
}

char *
sessdir_session_path(const char *name)
{
	char *base, *path;
	int rc;

	base = sessdir_base();
	if (!base)
		return NULL;

	path = xmalloc(PATH_MAX);
	rc = snprintf(path, PATH_MAX, "%s/%s", base, name);
	free(base);
	if (rc < 0 || rc >= PATH_MAX) {
		free(path);
		return NULL;
	}
	return path;
}

char *
sessdir_server_path(const char *session, pid_t pid)
{
	char *sess_path, *path;
	int rc;

	sess_path = sessdir_session_path(session);
	if (!sess_path)
		return NULL;

	path = xmalloc(PATH_MAX);
	rc = snprintf(path, PATH_MAX, "%s/%d", sess_path, (int)pid);
	free(sess_path);
	if (rc < 0 || rc >= PATH_MAX) {
		free(path);
		return NULL;
	}
	return path;
}

int
sessdir_session_create(const char *name)
{
	char *path;

	path = sessdir_session_path(name);
	if (!path)
		return -1;

	if (mkdir(path, 0700) < 0 && errno != EEXIST) {
		free(path);
		return -1;
	}
	free(path);
	return 0;
}

int
sessdir_server_create(const char *session, pid_t pid)
{
	char *path;

	path = sessdir_server_path(session, pid);
	if (!path)
		return -1;

	if (mkdir(path, 0700) < 0 && errno != EEXIST) {
		free(path);
		return -1;
	}
	free(path);
	return 0;
}

/* remove a single file inside a directory (helper for destroy) */
static void
remove_file(const char *dir, const char *name)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) < PATH_MAX)
		unlink(path);
}

void
sessdir_server_destroy(const char *session, pid_t pid)
{
	char *path;
	DIR *d;
	struct dirent *ent;

	path = sessdir_server_path(session, pid);
	if (!path)
		return;

	/* remove all files in the PID directory */
	d = opendir(path);
	if (d) {
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.')
				continue;
			remove_file(path, ent->d_name);
		}
		closedir(d);
	}

	rmdir(path);
	free(path);
}

int
sessdir_write_file(const char *session, pid_t pid,
    const char *name, const char *content)
{
	char *dir, fpath[PATH_MAX];
	FILE *f;

	dir = sessdir_server_path(session, pid);
	if (!dir)
		return -1;

	if (snprintf(fpath, sizeof(fpath), "%s/%s", dir, name) >= PATH_MAX) {
		free(dir);
		return -1;
	}
	free(dir);

	f = fopen(fpath, "w");
	if (!f)
		return -1;
	fputs(content, f);
	fputc('\n', f);
	fclose(f);
	return 0;
}

char *
sessdir_read_file(const char *session, pid_t pid, const char *name)
{
	char *dir, fpath[PATH_MAX];
	FILE *f;
	char buf[4096];
	size_t len;

	dir = sessdir_server_path(session, pid);
	if (!dir)
		return NULL;

	if (snprintf(fpath, sizeof(fpath), "%s/%s", dir, name) >= PATH_MAX) {
		free(dir);
		return NULL;
	}
	free(dir);

	f = fopen(fpath, "r");
	if (!f)
		return NULL;
	len = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[len] = '\0';

	/* strip trailing newline */
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	return xstrdup(buf);
}

int
sessdir_list_sessions(char **names, int max)
{
	char *base;
	DIR *d;
	struct dirent *ent;
	int count = 0;

	base = sessdir_base();
	if (!base)
		return -1;

	d = opendir(base);
	free(base);
	if (!d)
		return -1;

	while ((ent = readdir(d)) != NULL && count < max) {
		if (ent->d_name[0] == '.')
			continue;
		/* only directories are sessions */
		if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN)
			continue;
		names[count++] = xstrdup(ent->d_name);
	}
	closedir(d);
	return count;
}

int
sessdir_list_servers(const char *session, pid_t *pids, int max)
{
	char *sess_path;
	DIR *d;
	struct dirent *ent;
	int count = 0;

	sess_path = sessdir_session_path(session);
	if (!sess_path)
		return -1;

	d = opendir(sess_path);
	free(sess_path);
	if (!d)
		return -1;

	while ((ent = readdir(d)) != NULL && count < max) {
		long val;
		char *end;

		if (ent->d_name[0] == '.')
			continue;
		/* PID directories are numeric */
		val = strtol(ent->d_name, &end, 10);
		if (*end != '\0' || val <= 0)
			continue;
		pids[count++] = (pid_t)val;
	}
	closedir(d);
	return count;
}

int
sessdir_server_alive(pid_t pid)
{
	if (pid <= 0)
		return 0;
	return kill(pid, 0) == 0 || errno == EPERM;
}

int
sessdir_cleanup_stale(const char *session)
{
	pid_t pids[64];
	int n, i, removed = 0;

	n = sessdir_list_servers(session, pids, 64);
	if (n < 0)
		return 0;

	for (i = 0; i < n; i++) {
		if (!sessdir_server_alive(pids[i])) {
			sessdir_server_destroy(session, pids[i]);
			removed++;
		}
	}
	return removed;
}
