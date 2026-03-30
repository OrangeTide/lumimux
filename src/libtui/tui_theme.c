/* tui_theme.c : built-in theme definitions */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_theme.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* color convenience macros (same as splash_draw.h) */
#define CI(n)  { .type = VT_COLOR_INDEXED, { .index = (n) } }
#define CD     { .type = VT_COLOR_DEFAULT }

static const struct tui_theme themes[] = {
	{
		.name = "ascii",
		.border = { "+", "-", "+", "|", "|", "+", "-", "+" },
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "S",
		.icon_input_lock = "I",
		.icon_dead = "X",
		.scroll_up = "^", .scroll_down = "v",
		.scroll_track = ".", .scroll_thumb = "#",
		.sep_l = "+", .sep_fill = "-", .sep_r = "+",
		.border_fg = CI(7), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(0), .sel_bg = CI(15),
		.key_fg = CI(10), .sel_key_fg = CI(2),
		.shadow = TUI_SHADOW_NONE,
		.shadow_fg = CD, .shadow_bg = CD,
	},
	{
		.name = "thin",
		.border = {
			"\xe2\x94\x8c", "\xe2\x94\x80",
			"\xe2\x94\x90", "\xe2\x94\x82",
			"\xe2\x94\x82", "\xe2\x94\x94",
			"\xe2\x94\x80", "\xe2\x94\x98",
		}, /* ┌ ─ ┐ │ │ └ ─ ┘ */
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x94\x9c",  /* ├ */
		.sep_fill = "\xe2\x94\x80", /* ─ */
		.sep_r = "\xe2\x94\xa4",  /* ┤ */
		.border_fg = CI(7), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(0), .sel_bg = CI(15),
		.key_fg = CI(10), .sel_key_fg = CI(2),
		.shadow = TUI_SHADOW_HALF,
		.shadow_fg = CI(0), .shadow_bg = CI(0),
	},
	{
		.name = "double",
		.border = {
			"\xe2\x95\x94", "\xe2\x95\x90",
			"\xe2\x95\x97", "\xe2\x95\x91",
			"\xe2\x95\x91", "\xe2\x95\x9a",
			"\xe2\x95\x90", "\xe2\x95\x9d",
		}, /* ╔ ═ ╗ ║ ║ ╚ ═ ╝ */
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x95\xa0",  /* ╠ */
		.sep_fill = "\xe2\x95\x90", /* ═ */
		.sep_r = "\xe2\x95\xa3",  /* ╣ */
		.border_fg = CI(15), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(0), .sel_bg = CI(15),
		.key_fg = CI(10), .sel_key_fg = CI(2),
		.shadow = TUI_SHADOW_NONE,
		.shadow_fg = CD, .shadow_bg = CD,
	},
	{
		.name = "rounded",
		.border = {
			"\xe2\x95\xad", "\xe2\x94\x80",
			"\xe2\x95\xae", "\xe2\x94\x82",
			"\xe2\x94\x82", "\xe2\x95\xb0",
			"\xe2\x94\x80", "\xe2\x95\xaf",
		}, /* ╭ ─ ╮ │ │ ╰ ─ ╯ */
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x94\x9c",  /* ├ */
		.sep_fill = "\xe2\x94\x80", /* ─ */
		.sep_r = "\xe2\x94\xa4",  /* ┤ */
		.border_fg = CI(7), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(0), .sel_bg = CI(15),
		.key_fg = CI(10), .sel_key_fg = CI(2),
		.shadow = TUI_SHADOW_HALF,
		.shadow_fg = CI(0), .shadow_bg = CI(0),
	},
	{
		.name = "turbo",
		.border = {
			"\xe2\x95\x94", "\xe2\x95\x90",
			"\xe2\x95\x97", "\xe2\x95\x91",
			"\xe2\x95\x91", "\xe2\x95\x9a",
			"\xe2\x95\x90", "\xe2\x95\x9d",
		}, /* ╔ ═ ╗ ║ ║ ╚ ═ ╝ */
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x95\xa0",  /* ╠ */
		.sep_fill = "\xe2\x95\x90", /* ═ */
		.sep_r = "\xe2\x95\xa3",  /* ╣ */
		.border_fg = CI(15), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(15), .sel_bg = CI(2),
		.key_fg = CI(14), .sel_key_fg = CI(15),
		.shadow = TUI_SHADOW_SHADE,
		.shadow_fg = CI(8), .shadow_bg = CI(0),
	},
	{
		.name = "crimson",
		.border = {
			"\xe2\x95\x92", "\xe2\x95\x90",
			"\xe2\x95\x95", "\xe2\x94\x82",
			"\xe2\x94\x82", "\xe2\x95\x98",
			"\xe2\x95\x90", "\xe2\x95\x9b",
		}, /* ╒ ═ ╕ │ │ ╘ ═ ╛ */
		.title_l = "\xe2\x95\xa1", /* ╡ */
		.title_r = "\xe2\x95\x9e", /* ╞ */
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x95\x9e",  /* ╞ */
		.sep_fill = "\xe2\x95\x90", /* ═ */
		.sep_r = "\xe2\x95\xa1",  /* ╡ */
		.border_fg = CI(13), .border_bg = CI(1),
		.title_fg = CI(15),
		.content_fg = CI(15), .content_bg = CI(1),
		.sel_fg = CI(11), .sel_bg = CI(6),
		.key_fg = CI(15), .sel_key_fg = CI(11),
		.shadow = TUI_SHADOW_SHADE,
		.shadow_fg = CI(8), .shadow_bg = CI(0),
	},
	{
		.name = "acid",
		.border = {
			"\xe2\x96\x84", "\xe2\x96\x84",
			"\xe2\x96\x84", "\xe2\x96\x8c",
			"\xe2\x96\x90", "\xe2\x96\x80",
			"\xe2\x96\x80", "\xe2\x96\x80",
		}, /* ▄ ▄ ▄ ▌ ▐ ▀ ▀ ▀ */
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x96\x8c",  /* ▌ */
		.sep_fill = "\xe2\x94\x80", /* ─ */
		.sep_r = "\xe2\x96\x90",  /* ▐ */
		.border_fg = CI(13), .border_bg = CI(0),
		.title_fg = CI(15),
		.content_fg = CI(15), .content_bg = CI(0),
		.sel_fg = CI(0), .sel_bg = CI(13),
		.key_fg = CI(11), .sel_key_fg = CI(0),
		.shadow = TUI_SHADOW_HALF,
		.shadow_fg = CI(8), .shadow_bg = CI(0),
	},
	{
		.name = "shade",
		.border = {
			"\xe2\x96\x93", "\xe2\x96\x92",
			"\xe2\x96\x93", "\xe2\x96\x93",
			"\xe2\x96\x93", "\xe2\x96\x93",
			"\xe2\x96\x92", "\xe2\x96\x93",
		}, /* ▓ ▒ ▓ ▓ ▓ ▓ ▒ ▓ */
		.title_l = "\xe2\x96\x91", /* ░ */
		.title_r = "\xe2\x96\x91", /* ░ */
		.icon_scroll_lock = "\xe2\x8f\xb8", /* ⏸ */
		.icon_input_lock = "\xe2\x8a\x98",  /* ⊘ */
		.icon_dead = "\xe2\x9c\x95",        /* ✕ */
		.scroll_up = "\xe2\x96\xb2",    /* ▲ */
		.scroll_down = "\xe2\x96\xbc",  /* ▼ */
		.scroll_track = "\xe2\x96\x91", /* ░ */
		.scroll_thumb = "\xe2\x96\x88", /* █ */
		.sep_l = "\xe2\x96\x93",  /* ▓ */
		.sep_fill = "\xe2\x96\x91", /* ░ */
		.sep_r = "\xe2\x96\x93",  /* ▓ */
		.border_fg = CI(6), .border_bg = CI(0),
		.title_fg = CI(14),
		.content_fg = CI(7), .content_bg = CI(0),
		.sel_fg = CI(0), .sel_bg = CI(6),
		.key_fg = CI(14), .sel_key_fg = CI(0),
		.shadow = TUI_SHADOW_SHADE,
		.shadow_fg = CI(8), .shadow_bg = CI(0),
	},
	{
		.name = "borderless",
		.border = { " ", " ", " ", " ", " ", " ", " ", " " },
		.title_l = " ", .title_r = " ",
		.icon_scroll_lock = "S",
		.icon_input_lock = "I",
		.icon_dead = "X",
		.scroll_up = " ", .scroll_down = " ",
		.scroll_track = " ", .scroll_thumb = " ",
		.sep_l = " ", .sep_fill = " ", .sep_r = " ",
		.border_fg = CI(7), .border_bg = CI(4),
		.title_fg = CI(15),
		.content_fg = CI(7), .content_bg = CI(4),
		.sel_fg = CI(0), .sel_bg = CI(15),
		.key_fg = CI(10), .sel_key_fg = CI(2),
		.shadow = TUI_SHADOW_NONE,
		.shadow_fg = CD, .shadow_bg = CD,
	},
};

#define THEME_COUNT ((int)(sizeof(themes) / sizeof(themes[0])))

int
tui_theme_parse_color(const char *s, struct vt_color *out)
{
	char *end;
	long v;
	unsigned r, g, b;

	if (!s || !out)
		return -1;

	if (strcmp(s, "default") == 0) {
		out->type = VT_COLOR_DEFAULT;
		return 0;
	}

	if (s[0] == '#' && strlen(s) == 7) {
		if (sscanf(s, "#%2x%2x%2x", &r, &g, &b) == 3) {
			out->type = VT_COLOR_RGB;
			out->rgb.r = (uint8_t)r;
			out->rgb.g = (uint8_t)g;
			out->rgb.b = (uint8_t)b;
			return 0;
		}
		return -1;
	}

	v = strtol(s, &end, 10);
	if (end != s && *end == '\0' && v >= 0 && v <= 255) {
		out->type = VT_COLOR_INDEXED;
		out->index = (uint8_t)v;
		return 0;
	}

	return -1;
}

int
tui_theme_parse_shadow(const char *s, enum tui_shadow_style *out)
{
	if (!s || !out)
		return -1;
	if (strcmp(s, "none") == 0) {
		*out = TUI_SHADOW_NONE;
		return 0;
	}
	if (strcmp(s, "half") == 0) {
		*out = TUI_SHADOW_HALF;
		return 0;
	}
	if (strcmp(s, "shade") == 0) {
		*out = TUI_SHADOW_SHADE;
		return 0;
	}
	return -1;
}

const struct tui_theme *
tui_theme_by_name(const char *name)
{
	int i;

	for (i = 0; i < THEME_COUNT; i++) {
		if (strcmp(themes[i].name, name) == 0)
			return &themes[i];
	}
	return NULL;
}

const struct tui_theme *
tui_theme_default(void)
{
	const char *lang;

	/* check if locale suggests UTF-8 */
	lang = getenv("LANG");
	if (lang && (strstr(lang, "UTF-8") || strstr(lang, "utf-8") ||
	    strstr(lang, "utf8")))
		return tui_theme_by_name("thin");

	lang = getenv("LC_ALL");
	if (lang && (strstr(lang, "UTF-8") || strstr(lang, "utf-8") ||
	    strstr(lang, "utf8")))
		return tui_theme_by_name("thin");

	return tui_theme_by_name("ascii");
}

int
tui_theme_count(void)
{
	return THEME_COUNT;
}

const struct tui_theme *
tui_theme_at(int index)
{
	if (index < 0 || index >= THEME_COUNT)
		return NULL;
	return &themes[index];
}
