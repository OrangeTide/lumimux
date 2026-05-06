/* tui_theme.h : themeable decoration for TUI widgets */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_THEME_H
#define TUI_THEME_H

#include "vt_cell.h"

/* Border glyph indices. */
enum {
	TUI_BORDER_TL,
	TUI_BORDER_T,
	TUI_BORDER_TR,
	TUI_BORDER_L,
	TUI_BORDER_R,
	TUI_BORDER_BL,
	TUI_BORDER_B,
	TUI_BORDER_BR,
};

/* Shadow rendering style. */
enum tui_shadow_style {
	TUI_SHADOW_NONE,
	TUI_SHADOW_HALF,	/* half-block drop shadow */
	TUI_SHADOW_SHADE,	/* block-shade (░) shadow */
};

struct tui_theme {
	const char	*name;

	/* 8 border glyphs as UTF-8 strings */
	const char	*border[8];

	/* title bracket glyphs */
	const char	*title_l;
	const char	*title_r;

	/* title bar status icons (UTF-8 strings, 1 display cell each) */
	const char	*icon_scroll_lock;
	const char	*icon_input_lock;
	const char	*icon_dead;

	/* scrollbar glyphs (1 display cell each) */
	const char	*scroll_up;
	const char	*scroll_down;
	const char	*scroll_track;
	const char	*scroll_thumb;

	/* horizontal separator glyphs */
	const char	*sep_l;
	const char	*sep_fill;
	const char	*sep_r;

	/* colors -- dialog / overlay widgets */
	struct vt_color	border_fg;
	struct vt_color	border_bg;
	struct vt_color	title_fg;
	struct vt_color	content_fg;
	struct vt_color	content_bg;
	struct vt_color	sel_fg;
	struct vt_color	sel_bg;
	struct vt_color	key_fg;
	struct vt_color	sel_key_fg;

	/* colors -- window manager frame */
	struct vt_color	focus_fg;
	struct vt_color	focus_bg;
	struct vt_color	unfocus_fg;
	struct vt_color	unfocus_bg;
	struct vt_color	title_focus_fg;
	struct vt_color	title_idle_fg;
	struct vt_color	close_fg;
	struct vt_color	tool_fg;
	struct vt_color	status_focus_fg;
	struct vt_color	status_idle_fg;
	struct vt_color	indicator_fg;
	struct vt_color	drag_fg;

	/* shadow */
	enum tui_shadow_style shadow;
	struct vt_color	shadow_fg;
	struct vt_color	shadow_bg;
};

/* "default", "0"-"255", "#rrggbb" -> vt_color. returns 0 or -1. */
int tui_theme_parse_color(const char *s, struct vt_color *out);

/* "none", "half", "shade" -> tui_shadow_style. returns 0 or -1. */
int tui_theme_parse_shadow(const char *s, enum tui_shadow_style *out);

const struct tui_theme *tui_theme_by_name(const char *name);
const struct tui_theme *tui_theme_default(void);
int tui_theme_count(void);
const struct tui_theme *tui_theme_at(int index);

#endif /* TUI_THEME_H */
