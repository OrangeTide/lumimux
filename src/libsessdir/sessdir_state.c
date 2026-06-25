/* sessdir_state.c : session state file with flock() concurrency */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir_state.h"
#include "sessdir.h"
#include "xmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct sessdir_state {
	int fd;
	char path[PATH_MAX];
};

/* ---- dotenv parser ---- */

/* unescape a double-quoted value in-place.
 * recognizes: \\ \" \$ \`
 * input: points past the opening quote.
 * returns length of unescaped content (does not include quotes). */
static int
unescape_quoted(char *buf)
{
	char *src = buf, *dst = buf;

	while (*src && *src != '"') {
		if (*src == '\\' && src[1]) {
			switch (src[1]) {
			case '\\': case '"': case '$': case '`':
				*dst++ = src[1];
				src += 2;
				continue;
			default:
				break;
			}
		}
		*dst++ = *src++;
	}
	*dst = '\0';
	return (int)(dst - buf);
}

/* parse a dotenv file into key-value pairs.
 * callback receives key and value (unquoted/unescaped).
 * returns 0 on success, -1 on error. */
static int
parse_dotenv(const char *data,
    int (*cb)(const char *key, const char *val, void *arg), void *arg)
{
	const char *p = data;

	while (*p) {
		char key[256], val[4096];
		const char *eq, *eol;
		int klen, vlen;

		/* skip blank lines and comments */
		while (*p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			break;
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}

		/* find = */
		eq = strchr(p, '=');
		eol = strchr(p, '\n');
		if (!eol)
			eol = p + strlen(p);
		if (!eq || eq > eol) {
			p = (*eol) ? eol + 1 : eol;
			continue;
		}

		/* extract key */
		klen = (int)(eq - p);
		if (klen <= 0 || klen >= (int)sizeof(key)) {
			p = (*eol) ? eol + 1 : eol;
			continue;
		}
		memcpy(key, p, (size_t)klen);
		key[klen] = '\0';

		/* extract value */
		eq++; /* skip '=' */
		if (*eq == '"') {
			/* double-quoted value */
			const char *end;

			eq++; /* skip opening quote */
			end = strchr(eq, '"');
			if (!end)
				end = eol;
			vlen = (int)(end - eq);
			if (vlen >= (int)sizeof(val))
				vlen = (int)sizeof(val) - 1;
			memcpy(val, eq, (size_t)vlen);
			val[vlen] = '\0';
			unescape_quoted(val);
		} else {
			/* unquoted value */
			vlen = (int)(eol - eq);
			if (vlen >= (int)sizeof(val))
				vlen = (int)sizeof(val) - 1;
			memcpy(val, eq, (size_t)vlen);
			val[vlen] = '\0';
		}

		if (cb(key, val, arg) < 0)
			return -1;

		p = (*eol) ? eol + 1 : eol;
	}
	return 0;
}

/* ---- dotenv writer ---- */

/* check if a value needs quoting */
static int
needs_quoting(const char *val)
{
	const char *p;

	for (p = val; *p; p++) {
		if (*p == ' ' || *p == '"' || *p == '\\' ||
		    *p == '$' || *p == '`' || *p == '\t')
			return 1;
	}
	return 0;
}

/* write a key=value pair to a FILE, quoting if necessary */
static void
write_kv(FILE *f, const char *key, const char *val)
{
	if (needs_quoting(val)) {
		const char *p;

		fprintf(f, "%s=\"", key);
		for (p = val; *p; p++) {
			if (*p == '"' || *p == '\\' ||
			    *p == '$' || *p == '`')
				fputc('\\', f);
			fputc(*p, f);
		}
		fprintf(f, "\"\n");
	} else {
		fprintf(f, "%s=%s\n", key, val);
	}
}

/* ---- state file internals ---- */

/* read the entire state file into a malloc'd buffer. caller frees. */
static char *
state_read_all(struct sessdir_state *st)
{
	struct stat sb;
	char *buf;
	ssize_t n;

	if (fstat(st->fd, &sb) < 0)
		return xstrdup("");
	if (sb.st_size == 0)
		return xstrdup("");

	buf = xmalloc((size_t)sb.st_size + 1);
	lseek(st->fd, 0, SEEK_SET);
	n = read(st->fd, buf, (size_t)sb.st_size);
	if (n < 0) {
		free(buf);
		return xstrdup("");
	}
	buf[n] = '\0';
	return buf;
}

/* format a PID list into val[] as space-separated decimals.
 * returns the byte length written. */
static int
format_pid_list(char *val, size_t valsz, const pid_t *list, int n)
{
	int pos = 0, i;

	for (i = 0; i < n; i++) {
		int w;

		if (pos > 0 && pos < (int)valsz - 1)
			val[pos++] = ' ';
		w = snprintf(val + pos, valsz - (size_t)pos,
		    "%d", (int)list[i]);
		if (w > 0)
			pos += w;
	}
	val[pos] = '\0';
	return pos;
}

/* write state from scratch: FOCUS + WINDOW_ORDER + WINDOW_NUMS.
 * uses atomic rename via temp file. */
static int
state_write(struct sessdir_state *st, pid_t focus,
    const pid_t *order, int norder, const pid_t *nums, int nnums)
{
	char tmp[PATH_MAX];
	FILE *f;
	char val[4096];

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", st->path) >= PATH_MAX)
		return -1;

	f = fopen(tmp, "w");
	if (!f)
		return -1;

	/* FOCUS */
	if (focus > 0) {
		char buf[32];

		snprintf(buf, sizeof(buf), "%d", (int)focus);
		write_kv(f, "FOCUS", buf);
	}

	/* WINDOW_ORDER (dense layout order) */
	if (format_pid_list(val, sizeof(val), order, norder) > 0)
		write_kv(f, "WINDOW_ORDER", val);

	/* WINDOW_NUMS (slot map; 0 marks a spare) */
	if (format_pid_list(val, sizeof(val), nums, nnums) > 0)
		write_kv(f, "WINDOW_NUMS", val);

	fclose(f);

	if (rename(tmp, st->path) < 0) {
		unlink(tmp);
		return -1;
	}

	/* reopen the fd to pick up the new inode */
	close(st->fd);
	st->fd = open(st->path, O_RDWR | O_CREAT, 0600);
	return st->fd < 0 ? -1 : 0;
}

/* ---- callback context for parsing ---- */

struct parse_ctx {
	pid_t focus;
	pid_t order[64];
	int norder;
	pid_t nums[64];		/* slot map: index = number, 0 = spare */
	int nnums;
};

/* parse a space-separated PID list into dst[]. when keep_zero is set,
 * a literal 0 is stored as a spare slot rather than dropped. */
static int
parse_pid_list(const char *val, pid_t *dst, int max, int keep_zero)
{
	const char *p = val;
	int n = 0;

	while (*p && n < max) {
		char *end;
		long v;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		v = strtol(p, &end, 10);
		if (end == p)
			break;
		if (v > 0 || keep_zero)
			dst[n++] = (pid_t)v;
		p = end;
	}
	return n;
}

static int
parse_cb(const char *key, const char *val, void *arg)
{
	struct parse_ctx *ctx = arg;

	if (strcmp(key, "FOCUS") == 0)
		ctx->focus = (pid_t)atol(val);
	else if (strcmp(key, "WINDOW_ORDER") == 0)
		ctx->norder = parse_pid_list(val, ctx->order, 64, 0);
	else if (strcmp(key, "WINDOW_NUMS") == 0)
		ctx->nnums = parse_pid_list(val, ctx->nums, 64, 1);
	return 0;
}

static void
state_parse(struct sessdir_state *st, struct parse_ctx *ctx)
{
	char *data;

	ctx->focus = 0;
	ctx->norder = 0;
	ctx->nnums = 0;

	data = state_read_all(st);
	parse_dotenv(data, parse_cb, ctx);
	free(data);

	/* migrate legacy state with no WINDOW_NUMS: seed numbers from the
	 * current order so existing windows keep a stable number. */
	if (ctx->nnums == 0 && ctx->norder > 0) {
		memcpy(ctx->nums, ctx->order,
		    (size_t)ctx->norder * sizeof(pid_t));
		ctx->nnums = ctx->norder;
	}
}

/* drop trailing spare slots so the map stays compact. */
static void
nums_trim(struct parse_ctx *ctx)
{
	while (ctx->nnums > 0 && ctx->nums[ctx->nnums - 1] == 0)
		ctx->nnums--;
}

/* ---- public API ---- */

struct sessdir_state *
sessdir_state_open(const char *session)
{
	struct sessdir_state *st;
	char *sess_path;

	sess_path = sessdir_session_path(session);
	if (!sess_path)
		return NULL;

	st = xcalloc(1, sizeof(*st));
	if (snprintf(st->path, sizeof(st->path), "%s/state",
	    sess_path) >= PATH_MAX) {
		free(sess_path);
		free(st);
		return NULL;
	}
	free(sess_path);

	st->fd = open(st->path, O_RDWR | O_CREAT, 0600);
	if (st->fd < 0) {
		free(st);
		return NULL;
	}

	return st;
}

void
sessdir_state_close(struct sessdir_state *st)
{
	if (!st)
		return;
	if (st->fd >= 0)
		close(st->fd);
	free(st);
}

pid_t
sessdir_state_focus(struct sessdir_state *st)
{
	struct parse_ctx ctx;

	flock(st->fd, LOCK_SH);
	state_parse(st, &ctx);
	flock(st->fd, LOCK_UN);
	return ctx.focus;
}

int
sessdir_state_order(struct sessdir_state *st, pid_t *out, int max)
{
	struct parse_ctx ctx;
	int n;

	flock(st->fd, LOCK_SH);
	state_parse(st, &ctx);
	flock(st->fd, LOCK_UN);

	n = ctx.norder < max ? ctx.norder : max;
	memcpy(out, ctx.order, (size_t)n * sizeof(pid_t));
	return n;
}

int
sessdir_state_set_focus(struct sessdir_state *st, pid_t pid)
{
	struct parse_ctx ctx;
	int rc;

	flock(st->fd, LOCK_EX);
	state_parse(st, &ctx);
	ctx.focus = pid;
	rc = state_write(st, ctx.focus, ctx.order, ctx.norder,
	    ctx.nums, ctx.nnums);
	flock(st->fd, LOCK_UN);
	return rc;
}

int
sessdir_state_add_server(struct sessdir_state *st, pid_t pid)
{
	struct parse_ctx ctx;
	int i, slot, rc;

	flock(st->fd, LOCK_EX);
	state_parse(st, &ctx);

	/* check for duplicate */
	for (i = 0; i < ctx.norder; i++) {
		if (ctx.order[i] == pid) {
			flock(st->fd, LOCK_UN);
			return 0;
		}
	}

	if (ctx.norder >= 64) {
		flock(st->fd, LOCK_UN);
		return -1;
	}

	ctx.order[ctx.norder++] = pid;

	/* assign the lowest free window number: reuse the first spare
	 * slot, otherwise extend the map. */
	slot = -1;
	for (i = 0; i < ctx.nnums; i++) {
		if (ctx.nums[i] == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0 && ctx.nnums < 64)
		slot = ctx.nnums++;
	if (slot >= 0)
		ctx.nums[slot] = pid;

	if (ctx.focus == 0)
		ctx.focus = pid;
	rc = state_write(st, ctx.focus, ctx.order, ctx.norder,
	    ctx.nums, ctx.nnums);
	flock(st->fd, LOCK_UN);
	return rc;
}

int
sessdir_state_remove_server(struct sessdir_state *st, pid_t pid)
{
	struct parse_ctx ctx;
	int i, rc;

	flock(st->fd, LOCK_EX);
	state_parse(st, &ctx);

	/* remove from the dense layout order (shifts remaining entries) */
	for (i = 0; i < ctx.norder; i++) {
		if (ctx.order[i] == pid) {
			ctx.norder--;
			for (; i < ctx.norder; i++)
				ctx.order[i] = ctx.order[i + 1];
			break;
		}
	}

	/* free the stable window number: leave a spare, do not renumber
	 * the other windows. */
	for (i = 0; i < ctx.nnums; i++) {
		if (ctx.nums[i] == pid) {
			ctx.nums[i] = 0;
			break;
		}
	}
	nums_trim(&ctx);

	/* if focused server was removed, pick next or clear */
	if (ctx.focus == pid) {
		if (ctx.norder > 0)
			ctx.focus = ctx.order[0];
		else
			ctx.focus = 0;
	}

	rc = state_write(st, ctx.focus, ctx.order, ctx.norder,
	    ctx.nums, ctx.nnums);
	flock(st->fd, LOCK_UN);
	return rc;
}

int
sessdir_state_num(struct sessdir_state *st, pid_t pid)
{
	struct parse_ctx ctx;
	int i, num = -1;

	flock(st->fd, LOCK_SH);
	state_parse(st, &ctx);
	flock(st->fd, LOCK_UN);

	for (i = 0; i < ctx.nnums; i++) {
		if (ctx.nums[i] == pid) {
			num = i;
			break;
		}
	}
	return num;
}

int
sessdir_state_nums(struct sessdir_state *st, pid_t *out, int max)
{
	struct parse_ctx ctx;
	int n;

	flock(st->fd, LOCK_SH);
	state_parse(st, &ctx);
	flock(st->fd, LOCK_UN);

	n = ctx.nnums < max ? ctx.nnums : max;
	memcpy(out, ctx.nums, (size_t)n * sizeof(pid_t));
	return n;
}

int
sessdir_state_swap_num(struct sessdir_state *st, pid_t a, pid_t b)
{
	struct parse_ctx ctx;
	int i, sa = -1, sb = -1, rc;

	if (a == b)
		return 0;

	flock(st->fd, LOCK_EX);
	state_parse(st, &ctx);

	for (i = 0; i < ctx.nnums; i++) {
		if (ctx.nums[i] == a)
			sa = i;
		else if (ctx.nums[i] == b)
			sb = i;
	}
	if (sa < 0 || sb < 0) {
		flock(st->fd, LOCK_UN);
		return -1;
	}

	ctx.nums[sa] = b;
	ctx.nums[sb] = a;
	rc = state_write(st, ctx.focus, ctx.order, ctx.norder,
	    ctx.nums, ctx.nnums);
	flock(st->fd, LOCK_UN);
	return rc;
}
