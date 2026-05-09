/* keys.c : key binding table and prefix key state machine */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "keys.h"
#include "cfg.h"
#include "xmalloc.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- toggle table ---- */

#define KEYS_MAX_TOGGLES	16
#define KEYS_TOGGLE_NAME_MAX	32

struct keys_toggle_entry {
	char	name[KEYS_TOGGLE_NAME_MAX];
	int	active;
};

/* ---- binding layer ---- */

#define KEYS_LAYER_NAME_MAX	32

struct keys_layer {
	char		name[KEYS_LAYER_NAME_MAX];
	regex_t		*title_re;	/* NULL = no title predicate */
	char		toggle[KEYS_TOGGLE_NAME_MAX]; /* "" = no toggle predicate */
	enum keys_action bindings[256];
};

/* ---- keys state ---- */

struct keys {
	uint8_t prefix;			/* the prefix key (default Ctrl-A = 0x01) */
	enum keys_state state;
	int timeout_pending;		/* 1 after entering PREFIX, 0 after fired/reset */
	keys_timeout_cb timeout_cb;
	void *timeout_arg;

	/* layer 0 is the default (always-active) layer.
	 * layers are searched top-to-bottom (nlayers-1 .. 0). */
	struct keys_layer layers[KEYS_MAX_LAYERS];
	int nlayers;

	/* current context for layer matching */
	char *window_title;		/* NULL when unset */

	/* named toggles */
	struct keys_toggle_entry toggles[KEYS_MAX_TOGGLES];
	int ntoggles;
};

/* ---- internal helpers ---- */

static int
layer_active(const struct keys *k, const struct keys_layer *lay)
{
	/* title regex predicate */
	if (lay->title_re) {
		if (!k->window_title)
			return 0;
		if (regexec(lay->title_re, k->window_title, 0, NULL, 0) != 0)
			return 0;
	}

	/* toggle predicate */
	if (lay->toggle[0] != '\0') {
		if (!keys_toggle_get(k, lay->toggle))
			return 0;
	}

	return 1;
}

static enum keys_action
lookup_binding(const struct keys *k, uint8_t key)
{
	int i;

	/* walk layers top-to-bottom */
	for (i = k->nlayers - 1; i >= 0; i--) {
		const struct keys_layer *lay = &k->layers[i];

		if (!layer_active(k, lay))
			continue;
		if (lay->bindings[key] != KEYS_ACTION_NONE)
			return lay->bindings[key];
	}
	return KEYS_ACTION_NONE;
}

static struct keys_layer *
find_layer(struct keys *k, const char *name)
{
	int i;

	for (i = 0; i < k->nlayers; i++) {
		if (strcmp(k->layers[i].name, name) == 0)
			return &k->layers[i];
	}
	return NULL;
}

/* ---- public API ---- */

struct keys *
keys_new(uint8_t prefix)
{
	struct keys *k;

	k = xcalloc(1, sizeof(*k));
	k->prefix = prefix;
	k->state = KEYS_STATE_NORMAL;

	/* layer 0 is the default (always-active) layer */
	snprintf(k->layers[0].name, KEYS_LAYER_NAME_MAX, "default");
	k->nlayers = 1;

	return k;
}

void
keys_free(struct keys *k)
{
	int i;

	for (i = 0; i < k->nlayers; i++) {
		if (k->layers[i].title_re) {
			regfree(k->layers[i].title_re);
			free(k->layers[i].title_re);
		}
	}
	free(k->window_title);
	free(k);
}

void
keys_default(struct keys *k)
{
	struct keys_layer *lay = &k->layers[0];

	memset(lay->bindings, 0, sizeof(lay->bindings));

	/* Ctrl-A Ctrl-A: send literal prefix */
	lay->bindings[k->prefix] = KEYS_ACTION_SEND_PREFIX;

	/* window creation/destruction */
	lay->bindings['c'] = KEYS_ACTION_NEW_WINDOW;
	lay->bindings['C'] = KEYS_ACTION_NEW_WINDOW;
	lay->bindings['k'] = KEYS_ACTION_KILL_WINDOW;
	lay->bindings['K'] = KEYS_ACTION_KILL_WINDOW;

	/* window navigation */
	lay->bindings['n'] = KEYS_ACTION_NEXT_WINDOW;
	lay->bindings[' '] = KEYS_ACTION_NEXT_WINDOW;
	lay->bindings[0x00] = KEYS_ACTION_NEXT_WINDOW;	/* Ctrl-Space */
	lay->bindings[0x0E] = KEYS_ACTION_NEXT_WINDOW;	/* Ctrl-N */
	lay->bindings['p'] = KEYS_ACTION_PREV_WINDOW;
	lay->bindings[0x10] = KEYS_ACTION_PREV_WINDOW;	/* Ctrl-P */

	/* select by number */
	lay->bindings['0'] = KEYS_ACTION_SELECT_0;
	lay->bindings['1'] = KEYS_ACTION_SELECT_1;
	lay->bindings['2'] = KEYS_ACTION_SELECT_2;
	lay->bindings['3'] = KEYS_ACTION_SELECT_3;
	lay->bindings['4'] = KEYS_ACTION_SELECT_4;
	lay->bindings['5'] = KEYS_ACTION_SELECT_5;
	lay->bindings['6'] = KEYS_ACTION_SELECT_6;
	lay->bindings['7'] = KEYS_ACTION_SELECT_7;
	lay->bindings['8'] = KEYS_ACTION_SELECT_8;
	lay->bindings['9'] = KEYS_ACTION_SELECT_9;

	/* session control */
	lay->bindings['d'] = KEYS_ACTION_DETACH;
	lay->bindings['D'] = KEYS_ACTION_DETACH;

	/* info */
	lay->bindings['"'] = KEYS_ACTION_WINDOW_LIST;
	lay->bindings['w'] = KEYS_ACTION_WINDOW_LIST;

	/* status toggle */
	lay->bindings['s'] = KEYS_ACTION_STATUS_TOGGLE;

	/* quick apps menu */
	lay->bindings['q'] = KEYS_ACTION_APPS_MENU;

	/* scrollback */
	lay->bindings['['] = KEYS_ACTION_SCROLLBACK;
	lay->bindings[0x1B] = KEYS_ACTION_SCROLLBACK;	/* Ctrl-A ESC */

	/* tiled splits */
	lay->bindings['h'] = KEYS_ACTION_SPLIT_H;
	lay->bindings['v'] = KEYS_ACTION_SPLIT_V;
	lay->bindings['\t'] = KEYS_ACTION_NEXT_PANE;	/* Tab */
	lay->bindings['X'] = KEYS_ACTION_CLOSE_PANE;
	lay->bindings['r'] = KEYS_ACTION_RESIZE_PANE;

	/* turbo window management */
	lay->bindings['m'] = KEYS_ACTION_MINIMIZE;
	lay->bindings['f'] = KEYS_ACTION_MAXIMIZE;

	/* per-window customization */
	lay->bindings['P'] = KEYS_ACTION_WINDOW_COLORS;

	/* per-window locks */
	lay->bindings['S'] = KEYS_ACTION_SCROLL_LOCK;
	lay->bindings['I'] = KEYS_ACTION_INPUT_LOCK;

	/* session management */
	lay->bindings['U'] = KEYS_ACTION_SESSION_LIST;

	/* clipboard */
	lay->bindings[']'] = KEYS_ACTION_PASTE;
	lay->bindings['y'] = KEYS_ACTION_CLIPBOARD_SYNC;
}

void
keys_bind(struct keys *k, uint8_t key, enum keys_action action)
{
	k->layers[0].bindings[key] = action;
}

void
keys_unbind(struct keys *k, uint8_t key)
{
	k->layers[0].bindings[key] = KEYS_ACTION_NONE;
}

enum keys_action
keys_feed(struct keys *k, uint8_t byte)
{
	enum keys_action action;

	switch (k->state) {
	case KEYS_STATE_NORMAL:
		if (byte == k->prefix) {
			k->state = KEYS_STATE_PREFIX;
			k->timeout_pending = 1;
			return KEYS_ACTION_CONSUMED;
		}
		return KEYS_ACTION_NONE;
	case KEYS_STATE_PREFIX:
		k->state = KEYS_STATE_NORMAL;
		k->timeout_pending = 0;
		action = lookup_binding(k, byte);
		if (action != KEYS_ACTION_NONE)
			return action;
		/* unknown binding after prefix -- consumed but no action */
		return KEYS_ACTION_CONSUMED;
	}

	return KEYS_ACTION_NONE;
}

void
keys_reset(struct keys *k)
{
	k->state = KEYS_STATE_NORMAL;
	k->timeout_pending = 0;
}

void
keys_set_timeout_cb(struct keys *k, keys_timeout_cb cb, void *arg)
{
	k->timeout_cb = cb;
	k->timeout_arg = arg;
}

int
keys_get_timeout(const struct keys *k)
{
	if (k->state == KEYS_STATE_PREFIX && k->timeout_pending)
		return KEYS_PREFIX_TIMEOUT_MS;
	return -1;
}

void
keys_timeout_expired(struct keys *k)
{
	if (k->state != KEYS_STATE_PREFIX || !k->timeout_pending)
		return;
	k->timeout_pending = 0;
	if (k->timeout_cb)
		k->timeout_cb(k, k->timeout_arg);
}

enum keys_state
keys_get_state(const struct keys *k)
{
	return k->state;
}

uint8_t
keys_get_prefix(const struct keys *k)
{
	return k->prefix;
}

enum keys_action
keys_get_binding(const struct keys *k, uint8_t key)
{
	return lookup_binding(k, key);
}

/* ---- title context ---- */

void
keys_set_title(struct keys *k, const char *title)
{
	free(k->window_title);
	k->window_title = title ? xstrdup(title) : NULL;
}

/* ---- toggle API ---- */

static struct keys_toggle_entry *
find_toggle(const struct keys *k, const char *name)
{
	int i;

	for (i = 0; i < k->ntoggles; i++) {
		if (strcmp(k->toggles[i].name, name) == 0)
			return (struct keys_toggle_entry *)&k->toggles[i];
	}
	return NULL;
}

int
keys_toggle(struct keys *k, const char *name)
{
	struct keys_toggle_entry *t;

	t = find_toggle(k, name);
	if (t) {
		t->active = !t->active;
		return t->active;
	}

	/* create new toggle, initially on */
	if (k->ntoggles >= KEYS_MAX_TOGGLES)
		return 0;
	t = &k->toggles[k->ntoggles++];
	snprintf(t->name, KEYS_TOGGLE_NAME_MAX, "%s", name);
	t->active = 1;
	return 1;
}

int
keys_toggle_get(const struct keys *k, const char *name)
{
	const struct keys_toggle_entry *t;

	t = find_toggle(k, name);
	return t ? t->active : 0;
}

/* ---- layer API ---- */

int
keys_layer_add(struct keys *k, const char *name,
    const char *title_re, const char *toggle_name)
{
	struct keys_layer *lay;
	regex_t *re = NULL;

	if (k->nlayers >= KEYS_MAX_LAYERS)
		return -1;

	/* compile title regex if provided */
	if (title_re && title_re[0]) {
		re = xcalloc(1, sizeof(*re));
		if (regcomp(re, title_re, REG_EXTENDED | REG_NOSUB) != 0) {
			free(re);
			return -1;
		}
	}

	lay = &k->layers[k->nlayers++];
	memset(lay, 0, sizeof(*lay));
	snprintf(lay->name, KEYS_LAYER_NAME_MAX, "%s", name);
	lay->title_re = re;
	if (toggle_name)
		snprintf(lay->toggle, KEYS_TOGGLE_NAME_MAX, "%s", toggle_name);

	return 0;
}

int
keys_layer_bind(struct keys *k, const char *layer_name,
    uint8_t key, enum keys_action action)
{
	struct keys_layer *lay;

	lay = find_layer(k, layer_name);
	if (!lay)
		return -1;
	lay->bindings[key] = action;
	return 0;
}

int
keys_layer_count(const struct keys *k)
{
	return k->nlayers;
}

/* ---- action name mapping ---- */

static const struct {
	const char *name;
	enum keys_action action;
} action_names[] = {
	{ "send-prefix",	KEYS_ACTION_SEND_PREFIX },
	{ "new-window",		KEYS_ACTION_NEW_WINDOW },
	{ "next-window",	KEYS_ACTION_NEXT_WINDOW },
	{ "prev-window",	KEYS_ACTION_PREV_WINDOW },
	{ "select-0",		KEYS_ACTION_SELECT_0 },
	{ "select-1",		KEYS_ACTION_SELECT_1 },
	{ "select-2",		KEYS_ACTION_SELECT_2 },
	{ "select-3",		KEYS_ACTION_SELECT_3 },
	{ "select-4",		KEYS_ACTION_SELECT_4 },
	{ "select-5",		KEYS_ACTION_SELECT_5 },
	{ "select-6",		KEYS_ACTION_SELECT_6 },
	{ "select-7",		KEYS_ACTION_SELECT_7 },
	{ "select-8",		KEYS_ACTION_SELECT_8 },
	{ "select-9",		KEYS_ACTION_SELECT_9 },
	{ "kill-window",	KEYS_ACTION_KILL_WINDOW },
	{ "detach",		KEYS_ACTION_DETACH },
	{ "window-list",	KEYS_ACTION_WINDOW_LIST },
	{ "status-toggle",	KEYS_ACTION_STATUS_TOGGLE },
	{ "apps-menu",		KEYS_ACTION_APPS_MENU },
	{ "split-h",		KEYS_ACTION_SPLIT_H },
	{ "split-v",		KEYS_ACTION_SPLIT_V },
	{ "next-pane",		KEYS_ACTION_NEXT_PANE },
	{ "prev-pane",		KEYS_ACTION_PREV_PANE },
	{ "close-pane",		KEYS_ACTION_CLOSE_PANE },
	{ "resize-pane",	KEYS_ACTION_RESIZE_PANE },
	{ "scrollback",		KEYS_ACTION_SCROLLBACK },
	{ "window-colors",	KEYS_ACTION_WINDOW_COLORS },
	{ "minimize",		KEYS_ACTION_MINIMIZE },
	{ "maximize",		KEYS_ACTION_MAXIMIZE },
	{ "scroll-lock",	KEYS_ACTION_SCROLL_LOCK },
	{ "input-lock",		KEYS_ACTION_INPUT_LOCK },
	{ "session-list",	KEYS_ACTION_SESSION_LIST },
	{ "paste",		KEYS_ACTION_PASTE },
	{ "clipboard-sync",	KEYS_ACTION_CLIPBOARD_SYNC },
	{ "none",		KEYS_ACTION_NONE },
};

#define ACTION_NAME_COUNT \
	(sizeof(action_names) / sizeof(action_names[0]))

enum keys_action
keys_action_from_name(const char *name)
{
	size_t i;

	if (!name)
		return KEYS_ACTION_NONE;
	for (i = 0; i < ACTION_NAME_COUNT; i++) {
		if (strcmp(name, action_names[i].name) == 0)
			return action_names[i].action;
	}
	return KEYS_ACTION_NONE;
}

const char *
keys_action_to_name(enum keys_action action)
{
	size_t i;

	if (action == KEYS_ACTION_NONE || action == KEYS_ACTION_CONSUMED)
		return NULL;
	for (i = 0; i < ACTION_NAME_COUNT; i++) {
		if (action_names[i].action == action)
			return action_names[i].name;
	}
	return NULL;
}

/* ---- key name parsing ---- */

int
keys_parse_keyname(const char *name)
{
	if (!name || !name[0])
		return -1;

	/* C-x or c-x: ctrl + letter */
	if ((name[0] == 'C' || name[0] == 'c') && name[1] == '-' &&
	    name[2] != '\0' && name[3] == '\0') {
		unsigned char ch = (unsigned char)name[2];

		if (ch >= 'a' && ch <= 'z')
			return ch & 0x1f;
		if (ch >= 'A' && ch <= 'Z')
			return ch & 0x1f;
		if (ch >= '@' && ch <= '_')
			return ch & 0x1f;
		return -1;
	}

	/* named keys */
	if (strcmp(name, "space") == 0)
		return ' ';
	if (strcmp(name, "quote") == 0)
		return '"';
	if (strcmp(name, "tab") == 0)
		return '\t';
	if (strcmp(name, "escape") == 0 || strcmp(name, "esc") == 0)
		return 0x1b;
	if (strcmp(name, "backspace") == 0)
		return 0x7f;

	/* single character */
	if (name[1] == '\0')
		return (unsigned char)name[0];

	return -1;
}

/* ---- config loading ---- */

/*
 * Config key format from cfg_each():
 *   [bind]           -> "bind.<key>"
 *   [bind "name"]    -> "bind.<name>.<key>"
 *
 * Special directives within a named layer:
 *   match-title = <regex>   -> set title predicate
 *   toggle = <name>         -> set toggle predicate
 */

struct load_cfg_ctx {
	struct keys *k;
	/* layer creation is deferred: we collect directives first, then
	 * create the layer, then apply bindings.  for simplicity we do
	 * two passes via cfg_each(). */
};

/* pass 1: create named layers from directives */
static int
load_layer_directives(const char *key, const char *value, void *arg)
{
	struct load_cfg_ctx *ctx = arg;
	const char *rest;
	char layer_name[KEYS_LAYER_NAME_MAX];
	const char *dot;
	size_t nlen;

	/* only process bind.*.* (three-part keys = named layers) */
	if (strncmp(key, "bind.", 5) != 0)
		return 0;
	rest = key + 5;
	dot = strchr(rest, '.');
	if (!dot)
		return 0; /* bind.<key> = default layer, handled in pass 2 */

	/* extract layer name */
	nlen = (size_t)(dot - rest);
	if (nlen == 0 || nlen >= KEYS_LAYER_NAME_MAX)
		return 0;
	memcpy(layer_name, rest, nlen);
	layer_name[nlen] = '\0';

	/* skip if layer already exists */
	if (find_layer(ctx->k, layer_name))
		return 0;

	/* create layer with no predicates (will be set in pass 2) */
	keys_layer_add(ctx->k, layer_name, NULL, NULL);

	return 0;
}

/* pass 2: set directives and bindings */
static int
load_bind_entry(const char *key, const char *value, void *arg)
{
	struct load_cfg_ctx *ctx = arg;
	const char *rest;
	const char *dot;
	char layer_name[KEYS_LAYER_NAME_MAX];
	size_t nlen;
	const char *keyname;
	int byte;
	enum keys_action action;
	struct keys_layer *lay;

	if (strncmp(key, "bind.", 5) != 0)
		return 0;
	rest = key + 5;
	dot = strchr(rest, '.');

	if (!dot) {
		/* bind.<key> = default layer */
		keyname = rest;
		byte = keys_parse_keyname(keyname);
		if (byte < 0)
			return 0;
		action = keys_action_from_name(value);
		if (action == KEYS_ACTION_NONE && strcmp(value, "none") == 0)
			keys_unbind(ctx->k, (uint8_t)byte);
		else
			keys_bind(ctx->k, (uint8_t)byte, action);
		return 0;
	}

	/* bind.<name>.<key> = named layer */
	nlen = (size_t)(dot - rest);
	if (nlen == 0 || nlen >= KEYS_LAYER_NAME_MAX)
		return 0;
	memcpy(layer_name, rest, nlen);
	layer_name[nlen] = '\0';

	keyname = dot + 1;

	lay = find_layer(ctx->k, layer_name);
	if (!lay)
		return 0;

	/* handle special directives */
	if (strcmp(keyname, "match-title") == 0) {
		regex_t *re;

		/* free any existing regex */
		if (lay->title_re) {
			regfree(lay->title_re);
			free(lay->title_re);
			lay->title_re = NULL;
		}
		if (value[0]) {
			re = xcalloc(1, sizeof(*re));
			if (regcomp(re, value,
			    REG_EXTENDED | REG_NOSUB) == 0) {
				lay->title_re = re;
			} else {
				free(re);
			}
		}
		return 0;
	}

	if (strcmp(keyname, "toggle") == 0) {
		snprintf(lay->toggle, KEYS_TOGGLE_NAME_MAX, "%s", value);
		return 0;
	}

	/* regular binding */
	byte = keys_parse_keyname(keyname);
	if (byte < 0)
		return 0;
	action = keys_action_from_name(value);
	lay->bindings[(uint8_t)byte] = action;

	return 0;
}

int
keys_load_cfg(struct keys *k, const struct cfg *c)
{
	const char *val;
	struct load_cfg_ctx ctx;

	if (!k || !c)
		return -1;

	/* [keys] prefix */
	val = cfg_get(c, "keys.prefix");
	if (val) {
		int byte;

		byte = keys_parse_keyname(val);
		if (byte >= 0) {
			/* clear old prefix-sends-prefix binding */
			k->layers[0].bindings[k->prefix] = KEYS_ACTION_NONE;
			k->prefix = (uint8_t)byte;
			/* set new prefix-sends-prefix binding */
			k->layers[0].bindings[k->prefix] =
			    KEYS_ACTION_SEND_PREFIX;
		}
	}

	/* pass 1: discover and create named layers */
	ctx.k = k;
	cfg_each(c, load_layer_directives, &ctx);

	/* pass 2: apply directives and bindings */
	cfg_each(c, load_bind_entry, &ctx);

	return 0;
}
