/* tui_menu.c : vertical key+label menu widget */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_menu.h"
#include "tui_box.h"

#include <string.h>

void
tui_menu_measure(const struct tui_menu *m, int *w, int *h)
{
	int key_w = 0, lbl_w = 0;
	int i, bw, bh;

	for (i = 0; i < m->count; i++) {
		int kl = (int)strlen(m->items[i].keys);
		int ll = (int)strlen(m->items[i].label);

		if (kl > key_w)
			key_w = kl;
		if (ll > lbl_w)
			lbl_w = ll;
	}

	/* border + ' ' + keys + "  " + label + submenu? + ' ' + border */
	bw = 2 + 1 + key_w + 2 + lbl_w + 1;
	/* check if any item has submenu flag -- add space for '>' */
	for (i = 0; i < m->count; i++) {
		if (m->items[i].flags & TUI_MENU_SUBMENU) {
			bw += 2; /* " >" */
			break;
		}
	}
	if (bw < 16)
		bw = 16;
	bh = m->count + 2; /* top + bottom borders */

	*w = bw;
	*h = bh;
}

void
tui_menu_draw(struct tui_pad *p, const struct tui_menu *m,
    const struct tui_theme *theme, const char *title,
    const char *footer, int screen_rows, int screen_cols)
{
	int key_w = 0, lbl_w = 0;
	int bw, bh, i;
	int has_submenu = 0;
	struct tui_box box;

	/* compute column widths */
	for (i = 0; i < m->count; i++) {
		int kl = (int)strlen(m->items[i].keys);
		int ll = (int)strlen(m->items[i].label);

		if (kl > key_w)
			key_w = kl;
		if (ll > lbl_w)
			lbl_w = ll;
		if (m->items[i].flags & TUI_MENU_SUBMENU)
			has_submenu = 1;
	}

	/* border + ' ' + keys + "  " + label + [" >"] + ' ' + border */
	bw = 2 + 1 + key_w + 2 + lbl_w + (has_submenu ? 2 : 0) + 1;
	if (bw < 16)
		bw = 16;
	bh = m->count + 2;
	if (bw > TUI_PAD_MAX_COLS)
		bw = TUI_PAD_MAX_COLS;
	if (bh > TUI_PAD_MAX_ROWS)
		bh = TUI_PAD_MAX_ROWS;

	/* recompute lbl_w to fill the row when bw was clamped */
	lbl_w = bw - key_w - 6 - (has_submenu ? 2 : 0);

	box.theme = theme;
	box.title = title;
	box.footer = footer;
	box.w = bw;
	box.h = bh;
	tui_box_center(p, &box, screen_rows, screen_cols);

	/* content rows */
	for (i = 0; i < m->count && i + 1 < bh - 1; i++) {
		int row = i + 1;
		int col = 1; /* skip left border */
		int is_sel = (i == m->sel);
		struct vt_color fg = is_sel ? theme->sel_fg : theme->content_fg;
		struct vt_color bg = is_sel ? theme->sel_bg : theme->content_bg;
		struct vt_color kfg = is_sel ? theme->sel_key_fg : theme->key_fg;
		uint16_t attrs = is_sel ? VT_ATTR_BOLD : 0;
		int kl, ll, j;

		/* leading space */
		tui_pad_put(p, row, col++, ' ', fg, bg, attrs, TUI_OPAQUE);

		/* key column (left-aligned, padded) */
		kl = (int)strlen(m->items[i].keys);
		tui_pad_puts(p, row, col, m->items[i].keys,
		    kfg, bg, attrs, TUI_OPAQUE);
		col += kl;
		for (j = kl; j < key_w; j++)
			tui_pad_put(p, row, col++, ' ', fg, bg,
			    attrs, TUI_OPAQUE);

		/* gap */
		tui_pad_put(p, row, col++, ' ', fg, bg, attrs, TUI_OPAQUE);
		tui_pad_put(p, row, col++, ' ', fg, bg, attrs, TUI_OPAQUE);

		/* label column */
		ll = (int)strlen(m->items[i].label);
		tui_pad_puts(p, row, col, m->items[i].label,
		    fg, bg, attrs, TUI_OPAQUE);
		col += ll;
		for (j = ll; j < lbl_w; j++)
			tui_pad_put(p, row, col++, ' ', fg, bg,
			    attrs, TUI_OPAQUE);

		/* submenu indicator */
		if (has_submenu) {
			if (m->items[i].flags & TUI_MENU_SUBMENU)
				tui_pad_put(p, row, col++, ' ', fg, bg,
				    attrs, TUI_OPAQUE);
			else
				tui_pad_put(p, row, col++, ' ', fg, bg,
				    attrs, TUI_OPAQUE);
			if (m->items[i].flags & TUI_MENU_SUBMENU)
				tui_pad_put(p, row, col++, '>', fg, bg,
				    attrs, TUI_OPAQUE);
			else
				tui_pad_put(p, row, col++, ' ', fg, bg,
				    attrs, TUI_OPAQUE);
		}

		/* trailing space */
		tui_pad_put(p, row, col++, ' ', fg, bg, attrs, TUI_OPAQUE);
	}
}
