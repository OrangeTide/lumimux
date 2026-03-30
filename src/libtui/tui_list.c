/* tui_list.c : scrollable selection list widget */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_list.h"
#include "tui_box.h"

void
tui_list_draw(struct tui_pad *p, struct tui_list *l,
    const struct tui_theme *theme, tui_list_draw_fn draw_row,
    void *ctx, const char *title, int screen_rows, int screen_cols)
{
	int bw, bh, vis;
	int content_w;
	int i, row;
	struct tui_box box;

	/* compute box height: count + 2 borders, clamped to screen */
	bh = l->count + 2;
	if (bh > screen_rows - 2)
		bh = screen_rows - 2;
	if (bh < 3)
		bh = 3;
	if (bh > TUI_PAD_MAX_ROWS)
		bh = TUI_PAD_MAX_ROWS;

	vis = bh - 2; /* visible content rows */
	l->visible = vis;

	/* default width: 40 columns, clamped */
	bw = 40;
	if (bw > screen_cols - 2)
		bw = screen_cols - 2;
	if (bw < 16)
		bw = 16;
	if (bw > TUI_PAD_MAX_COLS)
		bw = TUI_PAD_MAX_COLS;

	content_w = bw - 2; /* inside borders */

	/* adjust scroll to keep selection visible */
	if (l->sel < l->scroll)
		l->scroll = l->sel;
	if (l->sel >= l->scroll + vis)
		l->scroll = l->sel - vis + 1;
	if (l->scroll < 0)
		l->scroll = 0;

	box.theme = theme;
	box.title = title;
	box.footer = NULL;
	box.w = bw;
	box.h = bh;
	tui_box_center(p, &box, screen_rows, screen_cols);

	/* content rows via callback */
	for (i = 0; i < vis && l->scroll + i < l->count; i++) {
		int idx = l->scroll + i;
		int selected = (idx == l->sel);

		row = i + 1; /* skip top border */

		/* fill row background */
		{
			struct vt_color fg = selected ?
			    theme->sel_fg : theme->content_fg;
			struct vt_color bg = selected ?
			    theme->sel_bg : theme->content_bg;
			uint16_t attrs = selected ? VT_ATTR_BOLD : 0;

			tui_pad_fill(p, row, 1, content_w, ' ',
			    fg, bg, attrs, TUI_OPAQUE);
		}

		draw_row(p, row, 1, content_w, idx, selected, theme, ctx);
	}

	/* scroll indicators */
	if (l->scroll > 0) {
		/* up arrow in top-right inside border */
		tui_pad_put(p, 1, bw - 2, '^',
		    theme->border_fg, theme->content_bg, 0, TUI_OPAQUE);
	}
	if (l->scroll + vis < l->count) {
		/* down arrow in bottom-right inside border */
		tui_pad_put(p, bh - 2, bw - 2, 'v',
		    theme->border_fg, theme->content_bg, 0, TUI_OPAQUE);
	}
}
