/* tui_list.h : scrollable selection list widget */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_LIST_H
#define TUI_LIST_H

#include "tui_pad.h"
#include "tui_theme.h"

typedef void (*tui_list_draw_fn)(struct tui_pad *p, int row, int col,
    int width, int index, int selected,
    const struct tui_theme *theme, void *ctx);

struct tui_list {
	int	count;
	int	sel;
	int	scroll;
	int	visible;	/* set by tui_list_draw */
};

void tui_list_draw(struct tui_pad *p, struct tui_list *l,
    const struct tui_theme *theme, tui_list_draw_fn draw_row,
    void *ctx, const char *title, int screen_rows, int screen_cols);

#endif /* TUI_LIST_H */
