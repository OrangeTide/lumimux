/* sessdir_layout.c : per-session window layout persistence */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir_layout.h"
#include "sessdir.h"
#include "xmalloc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tree node helpers ---- */

void
sessdir_tree_free(struct sessdir_tree_node *n)
{
	if (!n)
		return;
	sessdir_tree_free(n->a);
	sessdir_tree_free(n->b);
	free(n);
}

static struct sessdir_tree_node *
tree_new_leaf(int win_index)
{
	struct sessdir_tree_node *n;

	n = xcalloc(1, sizeof(*n));
	n->type = SESSDIR_TREE_LEAF;
	n->win_index = win_index;
	return n;
}

static struct sessdir_tree_node *
tree_new_split(enum sessdir_tree_type type, int split_pos,
    struct sessdir_tree_node *a, struct sessdir_tree_node *b)
{
	struct sessdir_tree_node *n;

	n = xcalloc(1, sizeof(*n));
	n->type = type;
	n->split_pos = split_pos;
	n->a = a;
	n->b = b;
	return n;
}

/* ---- dotenv helpers (subset of sessdir_state parser) ---- */

/* unescape a double-quoted value in-place */
static void
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
}

struct kv_entry {
	char	key[256];
	char	val[4096];
};

#define MAX_KV 64

struct kv_table {
	struct kv_entry	entries[MAX_KV];
	int		count;
};

static int
kv_parse_cb(const char *line, struct kv_table *kv)
{
	const char *eq, *eol;
	int klen, vlen;

	/* skip blank lines and comments */
	while (*line == '\n' || *line == '\r')
		line++;
	if (*line == '\0' || *line == '#' || *line == ';')
		return 0;

	eq = strchr(line, '=');
	eol = strchr(line, '\n');
	if (!eol)
		eol = line + strlen(line);
	if (!eq || eq > eol)
		return 0;

	klen = (int)(eq - line);
	if (klen <= 0 || klen >= (int)sizeof(kv->entries[0].key))
		return 0;
	if (kv->count >= MAX_KV)
		return 0;

	memcpy(kv->entries[kv->count].key, line, (size_t)klen);
	kv->entries[kv->count].key[klen] = '\0';

	eq++; /* skip '=' */
	if (*eq == '"') {
		const char *end;

		eq++;
		end = strchr(eq, '"');
		if (!end)
			end = eol;
		vlen = (int)(end - eq);
		if (vlen >= (int)sizeof(kv->entries[0].val))
			vlen = (int)sizeof(kv->entries[0].val) - 1;
		memcpy(kv->entries[kv->count].val, eq, (size_t)vlen);
		kv->entries[kv->count].val[vlen] = '\0';
		unescape_quoted(kv->entries[kv->count].val);
	} else {
		vlen = (int)(eol - eq);
		if (vlen >= (int)sizeof(kv->entries[0].val))
			vlen = (int)sizeof(kv->entries[0].val) - 1;
		memcpy(kv->entries[kv->count].val, eq, (size_t)vlen);
		kv->entries[kv->count].val[vlen] = '\0';
	}

	kv->count++;
	return 0;
}

static void
kv_parse(const char *data, struct kv_table *kv)
{
	const char *p = data;

	kv->count = 0;
	while (*p) {
		kv_parse_cb(p, kv);
		/* advance to next line */
		{
			const char *nl = strchr(p, '\n');

			if (!nl)
				break;
			p = nl + 1;
		}
	}
}

static const char *
kv_get(const struct kv_table *kv, const char *key)
{
	int i;

	for (i = 0; i < kv->count; i++) {
		if (strcmp(kv->entries[i].key, key) == 0)
			return kv->entries[i].val;
	}
	return NULL;
}

/* ---- layout file I/O ---- */

static char *
layout_path(const char *session)
{
	char *sess_path, *path;
	int len;

	sess_path = sessdir_session_path(session);
	if (!sess_path)
		return NULL;

	path = xmalloc(PATH_MAX);
	len = snprintf(path, PATH_MAX, "%s/layout", sess_path);
	free(sess_path);
	if (len >= PATH_MAX) {
		free(path);
		return NULL;
	}
	return path;
}

static char *
layout_read(const char *session)
{
	char *path, *buf;
	FILE *f;
	long sz;

	path = layout_path(session);
	if (!path)
		return NULL;

	f = fopen(path, "r");
	free(path);
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return xstrdup("");
	}

	buf = xmalloc((size_t)sz + 1);
	sz = (long)fread(buf, 1, (size_t)sz, f);
	buf[sz] = '\0';
	fclose(f);
	return buf;
}

/* ---- tree serialization ---- */

/*
 * Preorder text format:
 *   split:  "v<pos>" or "h<pos>"  followed by two children
 *   leaf:   "<index>"
 *
 * Example: "v128 h128 0 1 2"
 *   = vertical split(128) of [horizontal split(128) of [leaf 0, leaf 1], leaf 2]
 */

static struct sessdir_tree_node *
tree_parse(const char **pp)
{
	const char *p = *pp;
	char *end;
	long val;

	while (*p == ' ')
		p++;
	if (*p == '\0')
		return NULL;

	if (*p == 'v' || *p == 'h') {
		enum sessdir_tree_type type;
		int pos;
		struct sessdir_tree_node *a, *b;

		type = (*p == 'v') ? SESSDIR_TREE_SPLIT_V
		    : SESSDIR_TREE_SPLIT_H;
		p++;
		pos = (int)strtol(p, &end, 10);
		if (end == p)
			return NULL;
		p = end;

		*pp = p;
		a = tree_parse(pp);
		if (!a)
			return NULL;
		b = tree_parse(pp);
		if (!b) {
			sessdir_tree_free(a);
			return NULL;
		}
		return tree_new_split(type, pos, a, b);
	}

	/* leaf: plain integer */
	val = strtol(p, &end, 10);
	if (end == p)
		return NULL;
	*pp = end;
	return tree_new_leaf((int)val);
}

static int
tree_serialize(const struct sessdir_tree_node *n, char *buf, int buflen)
{
	int pos = 0, rc;

	if (!n)
		return 0;

	if (n->type == SESSDIR_TREE_LEAF) {
		rc = snprintf(buf + pos, (size_t)(buflen - pos),
		    "%d", n->win_index);
		if (rc > 0)
			pos += rc;
		return pos;
	}

	/* split node */
	rc = snprintf(buf + pos, (size_t)(buflen - pos), "%c%d ",
	    n->type == SESSDIR_TREE_SPLIT_V ? 'v' : 'h',
	    n->split_pos);
	if (rc > 0)
		pos += rc;

	rc = tree_serialize(n->a, buf + pos, buflen - pos);
	pos += rc;

	if (pos < buflen)
		buf[pos++] = ' ';

	rc = tree_serialize(n->b, buf + pos, buflen - pos);
	pos += rc;

	return pos;
}

/* ---- public API: mode probe ---- */

enum sessdir_layout_mode
sessdir_layout_mode(const char *session)
{
	char *data;
	struct kv_table kv;
	const char *mode;
	enum sessdir_layout_mode result = SESSDIR_LAYOUT_NONE;

	data = layout_read(session);
	if (!data)
		return SESSDIR_LAYOUT_NONE;

	kv_parse(data, &kv);
	mode = kv_get(&kv, "MODE");
	if (mode) {
		if (strcmp(mode, "turbo") == 0)
			result = SESSDIR_LAYOUT_TURBO;
		else if (strcmp(mode, "screen") == 0)
			result = SESSDIR_LAYOUT_SCREEN;
	}

	free(data);
	return result;
}

/* ---- public API: load ---- */

int
sessdir_layout_load_turbo(const char *session,
    struct sessdir_turbo_layout *out)
{
	char *data;
	struct kv_table kv;
	const char *mode, *val;
	int i;

	memset(out, 0, sizeof(*out));
	out->focus = -1;

	data = layout_read(session);
	if (!data)
		return -1;

	kv_parse(data, &kv);
	mode = kv_get(&kv, "MODE");
	if (!mode || strcmp(mode, "turbo") != 0) {
		free(data);
		return -1;
	}

	val = kv_get(&kv, "FOCUS");
	if (val)
		out->focus = (int)strtol(val, NULL, 10);

	for (i = 0; i < SESSDIR_LAYOUT_MAX_WINS; i++) {
		char key[32];

		snprintf(key, sizeof(key), "WIN_%d", i);
		val = kv_get(&kv, key);
		if (!val)
			continue;
		if (sscanf(val, "%d %d %d %d",
		    &out->wins[i].x, &out->wins[i].y,
		    &out->wins[i].w, &out->wins[i].h) == 4) {
			out->wins[i].valid = 1;
			if (i >= out->nwins)
				out->nwins = i + 1;
		}
	}

	free(data);
	return 0;
}

int
sessdir_layout_load_screen(const char *session,
    struct sessdir_screen_layout *out)
{
	char *data;
	struct kv_table kv;
	const char *mode, *val;

	memset(out, 0, sizeof(*out));
	out->focus = -1;

	data = layout_read(session);
	if (!data)
		return -1;

	kv_parse(data, &kv);
	mode = kv_get(&kv, "MODE");
	if (!mode || strcmp(mode, "screen") != 0) {
		free(data);
		return -1;
	}

	val = kv_get(&kv, "FOCUS");
	if (val)
		out->focus = (int)strtol(val, NULL, 10);

	val = kv_get(&kv, "TREE");
	if (val) {
		const char *p = val;

		out->root = tree_parse(&p);
	}

	free(data);
	return (out->root) ? 0 : -1;
}

/* ---- public API: save ---- */

int
sessdir_layout_save_turbo(const char *session,
    const struct sessdir_turbo_layout *layout)
{
	char *path;
	FILE *f;
	int i;

	path = layout_path(session);
	if (!path)
		return -1;

	f = fopen(path, "w");
	free(path);
	if (!f)
		return -1;

	fprintf(f, "MODE=turbo\n");
	if (layout->focus >= 0)
		fprintf(f, "FOCUS=%d\n", layout->focus);

	for (i = 0; i < layout->nwins; i++) {
		if (!layout->wins[i].valid)
			continue;
		fprintf(f, "WIN_%d=\"%d %d %d %d\"\n", i,
		    layout->wins[i].x, layout->wins[i].y,
		    layout->wins[i].w, layout->wins[i].h);
	}

	fclose(f);
	return 0;
}

int
sessdir_layout_save_screen(const char *session,
    const struct sessdir_screen_layout *layout)
{
	char *path;
	FILE *f;
	char tree_buf[4096];
	int len;

	path = layout_path(session);
	if (!path)
		return -1;

	f = fopen(path, "w");
	free(path);
	if (!f)
		return -1;

	fprintf(f, "MODE=screen\n");
	if (layout->focus >= 0)
		fprintf(f, "FOCUS=%d\n", layout->focus);

	if (layout->root) {
		len = tree_serialize(layout->root, tree_buf,
		    (int)sizeof(tree_buf) - 1);
		tree_buf[len] = '\0';
		fprintf(f, "TREE=\"%s\"\n", tree_buf);
	}

	fclose(f);
	return 0;
}
