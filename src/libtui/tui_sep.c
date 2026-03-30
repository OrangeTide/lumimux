/* tui_sep.c : horizontal separator */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_sep.h"

void
tui_sep_draw(struct tui_pad *p, const struct tui_theme *theme,
    int row, int x, int w)
{
	struct vt_color fg = theme->border_fg;
	struct vt_color bg = theme->border_bg;
	int c;

	if (w < 2)
		return;

	tui_pad_puts(p, row, x, theme->sep_l, fg, bg, 0, TUI_OPAQUE);
	for (c = x + 1; c < x + w - 1; c++)
		tui_pad_puts(p, row, c, theme->sep_fill, fg, bg,
		    0, TUI_OPAQUE);
	tui_pad_puts(p, row, x + w - 1, theme->sep_r, fg, bg,
	    0, TUI_OPAQUE);
}
