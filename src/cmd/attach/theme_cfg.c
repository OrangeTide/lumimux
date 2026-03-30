/* theme_cfg.c : user-defined theme loading from config */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "theme_cfg.h"
#include "attach_ui.h"

#include "cfg.h"
#include "tui_theme.h"

#include <stddef.h>
#include <string.h>

#define USER_THEME_MAX 8
#define THEME_STR_BUF  256

struct user_theme {
	struct tui_theme theme;
	char strbuf[THEME_STR_BUF];
	int strbuf_pos;
	int used;
};

static struct user_theme user_themes[USER_THEME_MAX];
static int user_theme_count;

static const char *
theme_strpack(struct user_theme *ut, const char *s)
{
	int len = (int)strlen(s) + 1;

	if (ut->strbuf_pos + len > THEME_STR_BUF)
		return NULL;
	memcpy(ut->strbuf + ut->strbuf_pos, s, (size_t)len);
	{
		const char *p = ut->strbuf + ut->strbuf_pos;

		ut->strbuf_pos += len;
		return p;
	}
}

static struct user_theme *
user_theme_slot(const char *name)
{
	int i;

	for (i = 0; i < user_theme_count; i++) {
		if (user_themes[i].used &&
		    strcmp(user_themes[i].theme.name, name) == 0)
			return &user_themes[i];
	}

	if (user_theme_count >= USER_THEME_MAX)
		return NULL;

	{
		struct user_theme *ut = &user_themes[user_theme_count++];
		const struct tui_theme *base;

		memset(ut, 0, sizeof(*ut));
		ut->used = 1;

		base = tui_theme_default();
		ut->theme = *base;
		ut->theme.name = theme_strpack(ut, name);
		return ut;
	}
}

static int
border_name_to_index(const char *name)
{
	static const struct {
		const char *name;
		int index;
	} map[] = {
		{ "tl", 0 }, { "t", 1 }, { "tr", 2 }, { "l", 3 },
		{ "r", 4 }, { "bl", 5 }, { "b", 6 }, { "br", 7 },
	};
	int i;

	for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
		if (strcmp(name, map[i].name) == 0)
			return map[i].index;
	}
	return -1;
}

static int
load_theme_entry(const char *key, const char *value, void *arg)
{
	const char *rest;
	const char *dot;
	char name[64];
	struct user_theme *ut;
	size_t nlen;

	(void)arg;

	if (strncmp(key, "theme.", 6) != 0)
		return 0;
	rest = key + 6;

	dot = strchr(rest, '.');
	if (!dot)
		return 0;

	nlen = (size_t)(dot - rest);
	if (nlen == 0 || nlen >= sizeof(name))
		return 0;
	memcpy(name, rest, nlen);
	name[nlen] = '\0';

	ut = user_theme_slot(name);
	if (!ut)
		return 0;

	rest = dot + 1;

	if (strcmp(rest, "base") == 0) {
		const struct tui_theme *base = tui_theme_by_name(value);

		if (base) {
			const char *saved_name = ut->theme.name;

			ut->theme = *base;
			ut->theme.name = saved_name;
		}
		return 0;
	}

	if (strncmp(rest, "border.", 7) == 0) {
		int idx = border_name_to_index(rest + 7);

		if (idx >= 0) {
			const char *s = theme_strpack(ut, value);

			if (s)
				ut->theme.border[idx] = s;
		}
		return 0;
	}

	if (strcmp(rest, "title_l") == 0) {
		const char *s = theme_strpack(ut, value);

		if (s) ut->theme.title_l = s;
		return 0;
	}
	if (strcmp(rest, "title_r") == 0) {
		const char *s = theme_strpack(ut, value);

		if (s) ut->theme.title_r = s;
		return 0;
	}
	if (strcmp(rest, "sep_l") == 0) {
		const char *s = theme_strpack(ut, value);

		if (s) ut->theme.sep_l = s;
		return 0;
	}
	if (strcmp(rest, "sep_fill") == 0) {
		const char *s = theme_strpack(ut, value);

		if (s) ut->theme.sep_fill = s;
		return 0;
	}
	if (strcmp(rest, "sep_r") == 0) {
		const char *s = theme_strpack(ut, value);

		if (s) ut->theme.sep_r = s;
		return 0;
	}

	if (strcmp(rest, "shadow") == 0) {
		tui_theme_parse_shadow(value, &ut->theme.shadow);
		return 0;
	}

	{
		static const struct {
			const char *name;
			size_t offset;
		} colors[] = {
			{ "border_fg", offsetof(struct tui_theme, border_fg) },
			{ "border_bg", offsetof(struct tui_theme, border_bg) },
			{ "title_fg", offsetof(struct tui_theme, title_fg) },
			{ "content_fg", offsetof(struct tui_theme, content_fg) },
			{ "content_bg", offsetof(struct tui_theme, content_bg) },
			{ "sel_fg", offsetof(struct tui_theme, sel_fg) },
			{ "sel_bg", offsetof(struct tui_theme, sel_bg) },
			{ "key_fg", offsetof(struct tui_theme, key_fg) },
			{ "sel_key_fg", offsetof(struct tui_theme, sel_key_fg) },
			{ "shadow_fg", offsetof(struct tui_theme, shadow_fg) },
			{ "shadow_bg", offsetof(struct tui_theme, shadow_bg) },
		};
		int i;

		for (i = 0; i < (int)(sizeof(colors) / sizeof(colors[0])); i++) {
			if (strcmp(rest, colors[i].name) == 0) {
				struct vt_color *cp = (struct vt_color *)
				    ((char *)&ut->theme + colors[i].offset);

				tui_theme_parse_color(value, cp);
				return 0;
			}
		}
	}

	return 0;
}

const struct tui_theme *
theme_find(const char *name)
{
	int i;

	for (i = 0; i < user_theme_count; i++) {
		if (user_themes[i].used &&
		    strcmp(user_themes[i].theme.name, name) == 0)
			return &user_themes[i].theme;
	}
	return tui_theme_by_name(name);
}

void
theme_load_cfg(const struct cfg *c)
{
	const char *val;

	user_theme_count = 0;
	memset(user_themes, 0, sizeof(user_themes));
	cfg_each(c, load_theme_entry, NULL);

	val = cfg_get(c, "ui.theme");
	if (val) {
		const struct tui_theme *t = theme_find(val);

		if (t)
			theme = t;
	}
}
