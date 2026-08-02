/* sessdir_control.c : session write token and client roster */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "sessdir_control.h"

#include "sessdir.h"
#include "str.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- paths ---- */

static int
session_file(const char *session, const char *name, char *buf, size_t bufsz)
{
	char *dir;
	int n;

	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	n = snprintf(buf, bufsz, "%s/%s", dir, name);
	free(dir);
	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

static int
clients_dir(const char *session, char *buf, size_t bufsz)
{
	return session_file(session, "clients", buf, bufsz);
}

static int
client_dir(const char *session, unsigned long id, char *buf, size_t bufsz)
{
	char base[PATH_MAX];
	int n;

	if (clients_dir(session, base, sizeof(base)) < 0)
		return -1;
	n = snprintf(buf, bufsz, "%s/%lu", base, id);
	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

/* ---- write token ---- */

static int token_fd = -1;

int
sessdir_token_acquire(const char *session, unsigned long client_id,
    const char *name)
{
	char path[PATH_MAX];
	char line[160];
	int fd, n;

	if (token_fd >= 0)
		return 0;			/* already ours */
	if (session_file(session, "control.lock", path, sizeof(path)) < 0)
		return -1;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		close(fd);
		return -1;			/* someone else has it */
	}

	/* the contents are only ever read for display, and only believed
	 * while the recorded pid is alive */
	n = snprintf(line, sizeof(line), "%lu %ld %s\n", client_id,
	    (long)getpid(), name ? name : "");
	if (n > 0 && ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0) {
		ssize_t w = write(fd, line, (size_t)n);

		(void)w;	/* a lost name costs a message, not the lock */
	}

	token_fd = fd;
	return 0;
}

void
sessdir_token_release(void)
{
	if (token_fd < 0)
		return;
	/* blank the holder line before unlocking, so the next reader does
	 * not report us until someone else writes their own */
	{
		int rc = ftruncate(token_fd, 0);

		(void)rc;			/* display only */
	}
	close(token_fd);			/* releases the flock */
	token_fd = -1;
}

int
sessdir_token_held(void)
{
	return token_fd >= 0;
}

void
sessdir_token_forget(void)
{
	if (token_fd < 0)
		return;
	close(token_fd);	/* the lock lives on the shared description */
	token_fd = -1;
}

int
sessdir_token_holder(const char *session, char *name, size_t namesz,
    pid_t *pid)
{
	char path[PATH_MAX];
	char line[160];
	FILE *f;
	unsigned long id;
	long holder;
	char who[64];
	int got;

	if (name && namesz > 0)
		name[0] = '\0';
	if (session_file(session, "control.lock", path, sizeof(path)) < 0)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;			/* nobody has written one */
	}
	fclose(f);

	who[0] = '\0';
	got = sscanf(line, "%lu %ld %63[^\n]", &id, &holder, who);
	if (got < 2 || holder <= 0)
		return -1;
	/* a holder that died left this line behind; the flock is already
	 * gone, so the line means nothing */
	if (kill((pid_t)holder, 0) < 0 && errno == ESRCH)
		return -1;
	if (name && namesz > 0)
		snprintf(name, namesz, "%s", who);
	if (pid)
		*pid = (pid_t)holder;
	return 0;
}

/* ---- share control messages ---- */

static const char *
verb_name(int verb)
{
	switch (verb) {
	case SESSDIR_CTL_REQUEST:
		return "request";
	case SESSDIR_CTL_GRANT:
		return "grant";
	case SESSDIR_CTL_DENY:
		return "deny";
	case SESSDIR_CTL_KICK:
		return "kick";
	case SESSDIR_CTL_APPROVE:
		return "approve";
	case SESSDIR_CTL_REJECT:
		return "reject";
	default:
		return "none";
	}
}

static int
verb_from_name(const char *s)
{
	if (strcmp(s, "request") == 0)
		return SESSDIR_CTL_REQUEST;
	if (strcmp(s, "grant") == 0)
		return SESSDIR_CTL_GRANT;
	if (strcmp(s, "deny") == 0)
		return SESSDIR_CTL_DENY;
	if (strcmp(s, "kick") == 0)
		return SESSDIR_CTL_KICK;
	if (strcmp(s, "approve") == 0)
		return SESSDIR_CTL_APPROVE;
	if (strcmp(s, "reject") == 0)
		return SESSDIR_CTL_REJECT;
	return SESSDIR_CTL_NONE;
}

int
sessdir_ctl_read(const char *session, struct sessdir_ctl *out)
{
	char path[PATH_MAX], line[256], verb[16];
	FILE *f;

	if (!out)
		return -1;
	if (session_file(session, "control.msg", path, sizeof(path)) < 0)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	memset(out, 0, sizeof(*out));
	verb[0] = '\0';
	if (sscanf(line, "%lu %15s %lu %lu %63[^\n]", &out->seq, verb,
	    &out->target, &out->actor, out->name) < 4)
		return -1;
	out->verb = verb_from_name(verb);
	return out->verb == SESSDIR_CTL_NONE ? -1 : 0;
}

int
sessdir_ctl_post(const char *session, int verb, unsigned long target,
    unsigned long actor, const char *name)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	struct sessdir_ctl prev;
	unsigned long seq = 1;
	FILE *f;

	if (session_file(session, "control.msg", path, sizeof(path)) < 0 ||
	    session_file(session, "control.msg.new", tmp, sizeof(tmp)) < 0)
		return -1;
	if (sessdir_ctl_read(session, &prev) == 0)
		seq = prev.seq + 1;

	/* write and rename: a reader either sees the old message or the new
	 * one, never a line torn between them */
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "%lu %s %lu %lu %s\n", seq, verb_name(verb), target,
	    actor, name ? name : "");
	fclose(f);
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* ---- attach lock ---- */

int
sessdir_share_set_locked(const char *session, int locked)
{
	char path[PATH_MAX];

	if (session_file(session, "control.locked", path, sizeof(path)) < 0)
		return -1;
	if (!locked) {
		unlink(path);
		return 0;
	}
	{
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (fd < 0)
			return -1;
		close(fd);
	}
	return 0;
}

int
sessdir_share_locked(const char *session)
{
	char path[PATH_MAX];
	struct stat st;

	if (session_file(session, "control.locked", path, sizeof(path)) < 0)
		return 0;
	return stat(path, &st) == 0;
}

/* ---- share mode and display (12B multi-writer) ----
 *
 * Both settings live in the same <session>/control file. A setter for one
 * has to read and preserve the other's current value, since the file is
 * always rewritten whole (temp-plus-rename, like everything else in this
 * file) rather than edited in place.
 */

static void
share_control_read(const char *session, int *mode, int *display)
{
	char path[PATH_MAX], line[64];
	FILE *f;

	*mode = SESSDIR_SHARE_SINGLE_WRITER;
	*display = SESSDIR_SHARE_DISPLAY_INDEPENDENT;
	if (session_file(session, "control", path, sizeof(path)) < 0)
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		char *val;

		if (!eq)
			continue;
		*eq = '\0';
		val = eq + 1;
		if (strcmp(line, "MODE") == 0) {
			if (strncmp(val, "multi-writer", 12) == 0)
				*mode = SESSDIR_SHARE_MULTI_WRITER;
		} else if (strcmp(line, "DISPLAY") == 0) {
			if (strncmp(val, "shared", 6) == 0)
				*display = SESSDIR_SHARE_DISPLAY_SHARED;
		}
	}
	fclose(f);
}

static int
share_control_write(const char *session, int mode, int display)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	FILE *f;

	if (session_file(session, "control", path, sizeof(path)) < 0 ||
	    session_file(session, "control.new", tmp, sizeof(tmp)) < 0)
		return -1;

	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "MODE=%s\n", mode == SESSDIR_SHARE_MULTI_WRITER ?
	    "multi-writer" : "single-writer");
	fprintf(f, "DISPLAY=%s\n", display == SESSDIR_SHARE_DISPLAY_SHARED ?
	    "shared" : "independent");
	fclose(f);

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int
sessdir_share_mode_get(const char *session)
{
	int mode, display;

	share_control_read(session, &mode, &display);
	return mode;
}

int
sessdir_share_mode_set(const char *session, int mode)
{
	int old_mode, display;

	share_control_read(session, &old_mode, &display);
	return share_control_write(session, mode, display);
}

int
sessdir_share_display_get(const char *session)
{
	int mode, display;

	share_control_read(session, &mode, &display);
	return display;
}

int
sessdir_share_display_set(const char *session, int display)
{
	int mode, old_display;

	share_control_read(session, &mode, &old_display);
	return share_control_write(session, mode, display);
}

/* ---- roster ---- */

int
sessdir_client_register(const char *session, const struct sessdir_client *c)
{
	char base[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	FILE *f;

	if (!c)
		return -1;
	if (clients_dir(session, base, sizeof(base)) < 0 ||
	    client_dir(session, c->client_id, dir, sizeof(dir)) < 0)
		return -1;
	if (mkdir(base, 0700) < 0 && errno != EEXIST)
		return -1;
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		return -1;
	if (snprintf(path, sizeof(path), "%s/info", dir) >= (int)sizeof(path) ||
	    snprintf(tmp, sizeof(tmp), "%s/info.new", dir) >= (int)sizeof(tmp))
		return -1;

	/* write and rename, so a reader never sees half an entry */
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "CLIENT_ID=%lu\n", c->client_id);
	fprintf(f, "PID=%ld\n", (long)c->pid);
	fprintf(f, "NAME=%s\n", c->name);
	fprintf(f, "WHO=%s\n", c->who);
	fprintf(f, "ROLE=%s\n", c->role);
	fprintf(f, "MODE=%s\n", c->mode);
	fprintf(f, "COUPLING=%s\n", c->coupling);
	fprintf(f, "ROWS=%d\n", c->rows);
	fprintf(f, "COLS=%d\n", c->cols);
	fprintf(f, "SINCE=%ld\n", c->since);
	fprintf(f, "PENDING=%d\n", c->pending);
	fclose(f);

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

void
sessdir_client_unregister(const char *session, unsigned long client_id)
{
	char dir[PATH_MAX], path[PATH_MAX];

	if (client_dir(session, client_id, dir, sizeof(dir)) < 0)
		return;
	if (snprintf(path, sizeof(path), "%s/info", dir) < (int)sizeof(path))
		unlink(path);
	rmdir(dir);
}

/* Copy a value into dst, stopping at the newline.
 *
 * Roster entries are written by other clients and end up drawn on this
 * one's screen, so nothing a terminal would act on survives the trip. A
 * client that named itself with an escape sequence would otherwise be
 * painting on everyone else's display. */
static void
set_field(char *dst, size_t dstsz, const char *val)
{
	char raw[256];
	size_t n = 0;

	while (val[n] && val[n] != '\n' && n + 1 < sizeof(raw)) {
		raw[n] = val[n];
		n++;
	}
	raw[n] = '\0';
	str_sanitize(dst, raw, dstsz);
}

static int
client_read(const char *session, unsigned long id, struct sessdir_client *c)
{
	char dir[PATH_MAX], path[PATH_MAX], line[256];
	FILE *f;

	if (client_dir(session, id, dir, sizeof(dir)) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "%s/info", dir) >= (int)sizeof(path))
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;

	memset(c, 0, sizeof(*c));
	c->client_id = id;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		const char *val;

		if (!eq)
			continue;
		*eq = '\0';
		val = eq + 1;
		if (strcmp(line, "PID") == 0)
			c->pid = (pid_t)strtol(val, NULL, 10);
		else if (strcmp(line, "NAME") == 0)
			set_field(c->name, sizeof(c->name), val);
		else if (strcmp(line, "WHO") == 0)
			set_field(c->who, sizeof(c->who), val);
		else if (strcmp(line, "ROLE") == 0)
			set_field(c->role, sizeof(c->role), val);
		else if (strcmp(line, "MODE") == 0)
			set_field(c->mode, sizeof(c->mode), val);
		else if (strcmp(line, "COUPLING") == 0)
			set_field(c->coupling, sizeof(c->coupling), val);
		else if (strcmp(line, "ROWS") == 0)
			c->rows = (int)strtol(val, NULL, 10);
		else if (strcmp(line, "COLS") == 0)
			c->cols = (int)strtol(val, NULL, 10);
		else if (strcmp(line, "SINCE") == 0)
			c->since = strtol(val, NULL, 10);
		else if (strcmp(line, "PENDING") == 0)
			c->pending = (int)strtol(val, NULL, 10);
	}
	fclose(f);
	return 0;
}

/* call fn for each client id in the roster directory */
static int
clients_walk(const char *session,
    void (*fn)(const char *session, unsigned long id, void *arg), void *arg)
{
	char base[PATH_MAX];
	DIR *d;
	struct dirent *ent;
	int n = 0;

	if (clients_dir(session, base, sizeof(base)) < 0)
		return -1;
	d = opendir(base);
	if (!d)
		return -1;
	while ((ent = readdir(d)) != NULL) {
		unsigned long id;
		char *end;

		if (ent->d_name[0] == '.')
			continue;
		id = strtoul(ent->d_name, &end, 10);
		if (*end != '\0')
			continue;
		fn(session, id, arg);
		n++;
	}
	closedir(d);
	return n;
}

struct list_ctx {
	struct sessdir_client *out;
	int max;
	int n;
};

static void
list_one(const char *session, unsigned long id, void *arg)
{
	struct list_ctx *ctx = arg;

	if (ctx->n >= ctx->max)
		return;
	if (client_read(session, id, &ctx->out[ctx->n]) == 0)
		ctx->n++;
}

int
sessdir_client_list(const char *session, struct sessdir_client *out, int max)
{
	struct list_ctx ctx;

	ctx.out = out;
	ctx.max = max;
	ctx.n = 0;
	if (clients_walk(session, list_one, &ctx) < 0)
		return -1;
	return ctx.n;
}

struct prune_ctx {
	int removed;
};

static void
prune_one(const char *session, unsigned long id, void *arg)
{
	struct prune_ctx *ctx = arg;
	struct sessdir_client c;

	if (client_read(session, id, &c) < 0) {
		/* A directory that exists but has no readable info file yet
		 * is either genuine debris (a process that died between its
		 * mkdir() and the write+rename that populates info) or a
		 * registration race: sessdir_client_register() creates the
		 * directory before it writes and renames info.new into
		 * place, and sessdir_cleanup_stale() runs on every attach
		 * and rescan, including from sibling processes registering
		 * their own, unrelated clients at the same moment. Deleting
		 * on the very first unreadable read cost a real client its
		 * roster entry mid-registration; give a young directory a
		 * grace window instead and only treat it as debris once it
		 * has had time to finish registering. */
		char dir[PATH_MAX];
		struct stat st;

		if (client_dir(session, id, dir, sizeof(dir)) == 0 &&
		    stat(dir, &st) == 0 &&
		    difftime(time(NULL), st.st_mtime) < 5)
			return;
		sessdir_client_unregister(session, id);
		ctx->removed++;
		return;
	}
	if (c.pid <= 0 || (kill(c.pid, 0) < 0 && errno == ESRCH)) {
		sessdir_client_unregister(session, id);
		ctx->removed++;
	}
}

int
sessdir_client_prune(const char *session)
{
	struct prune_ctx ctx;

	ctx.removed = 0;
	if (clients_walk(session, prune_one, &ctx) < 0)
		return 0;
	return ctx.removed;
}
