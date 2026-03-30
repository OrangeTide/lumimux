/* tui_box.c : themed bordered frame drawing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_box.h"

#include <string.h>

void
tui_box_draw(struct tui_pad *p, const struct tui_box *box)
{
	const struct tui_theme *t = box->theme;
	int x = box->x, y = box->y;
	int w = box->w, h = box->h;
	int r;
	struct vt_color bfg = t->border_fg;
	struct vt_color bbg = t->border_bg;
	struct vt_color cfg = t->content_fg;
	struct vt_color cbg = t->content_bg;

	tui_pad_puts(p, y, x, t->border[TUI_BORDER_TL],
	    bfg, bbg, 0, TUI_OPAQUE);
	tui_pad_fill(p, y, x + 1, w - 2, ' ', bfg, bbg, 0, TUI_OPAQUE);
	{
		int c;

		for (c = x + 1; c < x + w - 1; c++)
			tui_pad_puts(p, y, c, t->border[TUI_BORDER_T],
			    bfg, bbg, 0, TUI_OPAQUE);
	}
	tui_pad_puts(p, y, x + w - 1, t->border[TUI_BORDER_TR],
	    bfg, bbg, 0, TUI_OPAQUE);

	if (box->title) {
		int tlen = (int)strlen(box->title);
		int total = tlen + 2; /* space + title + space */
		int start = x + (w - total) / 2;

		if (start < x + 1)
			start = x + 1;
		tui_pad_puts(p, y, start, t->title_l,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
		tui_pad_puts(p, y, start + 1, box->title,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
		tui_pad_puts(p, y, start + 1 + tlen, t->title_r,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
	}

	for (r = y + 1; r < y + h - 1; r++) {
		tui_pad_puts(p, r, x, t->border[TUI_BORDER_L],
		    bfg, bbg, 0, TUI_OPAQUE);
		tui_pad_fill(p, r, x + 1, w - 2, ' ',
		    cfg, cbg, 0, TUI_OPAQUE);
		tui_pad_puts(p, r, x + w - 1, t->border[TUI_BORDER_R],
		    bfg, bbg, 0, TUI_OPAQUE);
	}

	tui_pad_puts(p, y + h - 1, x, t->border[TUI_BORDER_BL],
	    bfg, bbg, 0, TUI_OPAQUE);
	{
		int c;

		for (c = x + 1; c < x + w - 1; c++)
			tui_pad_puts(p, y + h - 1, c,
			    t->border[TUI_BORDER_B],
			    bfg, bbg, 0, TUI_OPAQUE);
	}
	tui_pad_puts(p, y + h - 1, x + w - 1, t->border[TUI_BORDER_BR],
	    bfg, bbg, 0, TUI_OPAQUE);

	if (box->footer) {
		int flen = (int)strlen(box->footer);
		int total = flen + 2;
		int start = x + (w - total) / 2;

		if (start < x + 1)
			start = x + 1;
		tui_pad_puts(p, y + h - 1, start, t->title_l,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
		tui_pad_puts(p, y + h - 1, start + 1, box->footer,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
		tui_pad_puts(p, y + h - 1, start + 1 + flen, t->title_r,
		    t->title_fg, bbg, VT_ATTR_BOLD, TUI_OPAQUE);
	}

	if (t->shadow == TUI_SHADOW_HALF) {
		int c;

		/* right shadow column */
		for (r = y + 1; r < y + h; r++)
			tui_pad_put(p, r, x + w, 0x2584, /* ▄ */
			    t->shadow_fg, t->shadow_bg, 0, TUI_COLOR_BG);

		/* bottom shadow row */
		for (c = x + 1; c <= x + w; c++)
			tui_pad_put(p, y + h, c, 0x2580, /* ▀ */
			    t->shadow_fg, t->shadow_bg, 0, TUI_COLOR_BG);
	} else if (t->shadow == TUI_SHADOW_SHADE) {
		int c;

		for (r = y + 1; r < y + h; r++)
			tui_pad_put(p, r, x + w, 0x2591, /* ░ */
			    t->shadow_fg, t->shadow_bg, 0, TUI_COLOR_BG);

		for (c = x + 1; c <= x + w; c++)
			tui_pad_put(p, y + h, c, 0x2591, /* ░ */
			    t->shadow_fg, t->shadow_bg, 0, TUI_COLOR_BG);
	}
}

void
tui_box_center(struct tui_pad *p, struct tui_box *box,
    int screen_rows, int screen_cols)
{
	int sr, sc, pw, ph;

	sr = (screen_rows - box->h) / 2;
	sc = (screen_cols - box->w) / 2;
	if (sr < 0)
		sr = 0;
	if (sc < 0)
		sc = 0;

	pw = box->w;
	ph = box->h;
	if (box->theme->shadow != TUI_SHADOW_NONE) {
		pw++;
		ph++;
	}
	tui_pad_clear(p, pw, ph);
	p->screen_row = sr;
	p->screen_col = sc;

	box->x = 0;
	box->y = 0;
	tui_box_draw(p, box);
}
