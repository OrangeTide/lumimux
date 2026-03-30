/* tui_box.h : themed bordered frame */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_BOX_H
#define TUI_BOX_H

#include "tui_pad.h"
#include "tui_theme.h"

struct tui_box {
	const struct tui_theme	*theme;
	const char		*title;		/* NULL for no title */
	const char		*footer;	/* NULL for no footer */
	int			x, y;		/* position within pad */
	int			w, h;		/* outer dimensions */
};

void tui_box_draw(struct tui_pad *p, const struct tui_box *box);

/* center a box on screen, clear the pad with shadow room, and draw.
 * sets p->screen_row/screen_col. box->x and box->y are set to 0. */
void tui_box_center(struct tui_pad *p, struct tui_box *box,
    int screen_rows, int screen_cols);

#endif /* TUI_BOX_H */
