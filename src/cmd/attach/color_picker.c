/* color_picker.c : per-window color customization UI */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "color_picker.h"
#include "attach_ui.h"

#include "tui_box.h"
#include "tui_list.h"
#include "tkbd.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- themable element descriptors ---- */

#define ELEM_COUNT 7

struct color_elem {
	const char	*label;
	int		offset;		/* byte offset into struct tui_theme */
};

static const struct color_elem elements[ELEM_COUNT] = {
	{ "Border fg",   offsetof(struct tui_theme, border_fg)  },
	{ "Border bg",   offsetof(struct tui_theme, border_bg)  },
	{ "Title fg",    offsetof(struct tui_theme, title_fg)   },
	{ "Content fg",  offsetof(struct tui_theme, content_fg) },
	{ "Content bg",  offsetof(struct tui_theme, content_bg) },
	{ "Select fg",   offsetof(struct tui_theme, sel_fg)     },
	{ "Select bg",   offsetof(struct tui_theme, sel_bg)     },
};

/* ---- persistent per-window theme storage ---- */

#define MAX_OVERRIDES 32

struct theme_slot {
	uint32_t	id;
	int		used;
	struct tui_theme theme;
};

static struct theme_slot overrides[MAX_OVERRIDES];

static struct tui_theme *
override_alloc(uint32_t id)
{
	int i, free_slot = -1;

	for (i = 0; i < MAX_OVERRIDES; i++) {
		if (overrides[i].used && overrides[i].id == id)
			return &overrides[i].theme;
		if (!overrides[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL;
	overrides[free_slot].id = id;
	overrides[free_slot].used = 1;
	return &overrides[free_slot].theme;
}

/* ---- state ---- */

static uint32_t target_id;		/* window being customized */
static struct tui_theme working;	/* mutable copy */
static int changed;			/* any element confirmed? */
static int had_override;		/* window already had custom colors */

/* 0 = element list, 1 = palette grid */
static int screen;

/* element list */
static struct tui_list elem_list;

/* palette grid cursor */
static int pal_sel;			/* 0..255 */
static int pal_elem;			/* which element we're editing */
static struct vt_color saved_color;	/* for revert on ESC */

/* ---- helpers ---- */

static struct vt_color *
elem_color(struct tui_theme *t, int idx)
{
	return (struct vt_color *)((char *)t + elements[idx].offset);
}

static void
apply_theme(void)
{
	color_picker_set_theme(target_id, &working);
}

/* ---- element list screen ---- */

static void
elem_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	struct vt_color swatch_bg;
	int c, j, llen;
	const char *label;

	(void)ctx;
	c = col;

	/* leading space */
	tui_pad_put(p, row, c++, ' ', fg, bg, 0, TUI_OPAQUE);

	/* 2-cell color swatch */
	swatch_bg = *elem_color(&working, index);
	tui_pad_put(p, row, c++, 0x2588, swatch_bg, bg, 0, TUI_OPAQUE);
	tui_pad_put(p, row, c++, 0x2588, swatch_bg, bg, 0, TUI_OPAQUE);

	/* space + label */
	tui_pad_put(p, row, c++, ' ', fg, bg, 0, TUI_OPAQUE);
	label = elements[index].label;
	llen = (int)strlen(label);
	for (j = 0; j < llen && c < col + width - 1; j++)
		tui_pad_put(p, row, c++,
		    (uint32_t)(unsigned char)label[j],
		    fg, bg, 0, TUI_OPAQUE);

	/* fill remainder */
	while (c < col + width)
		tui_pad_put(p, row, c++, ' ', fg, bg, 0, TUI_OPAQUE);
}

static void
elem_draw(void)
{
	struct tui_pad *p;

	elem_list.count = ELEM_COUNT;
	elem_list.sel = elem_list.sel;

	p = tui_stack_top(&overlay);
	if (!p) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	}

	tui_list_draw(p, &elem_list, theme, elem_row_cb,
	    NULL, "Window Colors", content_rows, content_cols);
	overlay_visible = 1;
	overlay_render();
}

/* ---- palette grid screen ---- */

#define PAL_COLS 16
#define PAL_ROWS 16
#define SWATCH_W 2
#define PAL_GRID_W (PAL_COLS * SWATCH_W)	/* 32 */
#define BOX_W (PAL_GRID_W + 2)			/* 34 */
#define BOX_H (PAL_ROWS + 4)			/* 20: border + title + 16 rows + footer + border */

static void
palette_draw(void)
{
	struct tui_pad *p;
	struct tui_box box;
	int r, c, idx;
	struct vt_color bfg, bbg, cfg, cbg;
	char footer[24];
	int sel_row = pal_sel / PAL_COLS;
	int sel_col = pal_sel % PAL_COLS;

	p = tui_stack_top(&overlay);
	if (!p)
		return;

	snprintf(footer, sizeof(footer), "Color: %d", pal_sel);

	box.theme = theme;
	box.title = elements[pal_elem].label;
	box.footer = footer;
	box.w = BOX_W;
	box.h = BOX_H;
	tui_box_center(p, &box, content_rows, content_cols);

	bfg = theme->border_fg;
	bbg = theme->border_bg;
	cfg = theme->content_fg;
	cbg = theme->content_bg;

	/* draw the 16x16 palette grid inside the box content area */
	for (r = 0; r < PAL_ROWS; r++) {
		for (c = 0; c < PAL_COLS; c++) {
			struct vt_color swatch;
			int px, py;
			int selected;

			idx = r * PAL_COLS + c;
			swatch.type = VT_COLOR_INDEXED;
			swatch.index = (uint8_t)idx;

			selected = (r == sel_row && c == sel_col);
			py = 1 + r;		/* row 0 is title border */
			px = 1 + c * SWATCH_W;	/* col 0 is left border */

			if (selected) {
				/* highlight: reverse video brackets */
				tui_pad_put(p, py, px,
				    '[', cfg, swatch, VT_ATTR_BOLD,
				    TUI_OPAQUE);
				tui_pad_put(p, py, px + 1,
				    ']', cfg, swatch, VT_ATTR_BOLD,
				    TUI_OPAQUE);
			} else {
				tui_pad_put(p, py, px,
				    ' ', cfg, swatch, 0, TUI_OPAQUE);
				tui_pad_put(p, py, px + 1,
				    ' ', cfg, swatch, 0, TUI_OPAQUE);
			}
		}
	}

	overlay_visible = 1;
	overlay_render();
}

static void
palette_preview(void)
{
	struct vt_color c;

	c.type = VT_COLOR_INDEXED;
	c.index = (uint8_t)pal_sel;
	*elem_color(&working, pal_elem) = c;
	apply_theme();
	palette_draw();
}

/* ---- show / hide ---- */

static void
color_picker_hide(int back_to_menu)
{
	color_picker_visible = 0;

	if (changed) {
		/* commit working copy to persistent slot */
		struct tui_theme *slot = override_alloc(target_id);

		if (slot) {
			*slot = working;
			color_picker_set_theme(target_id, slot);
		}
	} else if (!had_override) {
		/* no changes and no prior override -- clear */
		color_picker_set_theme(target_id, NULL);
	}

	if (back_to_menu && overlay_visible) {
		overlay_pop();
		if (overlay_visible) {
			menu_visible = 1;
			overlay_render();
		}
	} else {
		menu_visible = 0;
		overlay_erase_all();
	}
}

void
color_picker_show(uint32_t window_id)
{
	const struct tui_theme *base;
	struct tui_pad *p;

	target_id = window_id;
	base = color_picker_get_theme(window_id);
	had_override = (base != NULL);
	if (!base)
		base = theme;
	working = *base;
	changed = 0;
	screen = 0;
	pal_sel = 0;

	memset(&elem_list, 0, sizeof(elem_list));
	elem_list.count = ELEM_COUNT;
	elem_list.sel = 0;

	color_picker_visible = 1;

	p = tui_stack_push(&overlay);
	if (!p)
		return;
	overlay_visible = 1;
	elem_draw();
}

/* ---- input handling ---- */

static void
palette_input(const struct tkbd_seq *seq)
{
	int row = pal_sel / PAL_COLS;
	int col = pal_sel % PAL_COLS;

	switch (seq->key) {
	case TKBD_KEY_UP:
		row = (row > 0) ? row - 1 : PAL_ROWS - 1;
		pal_sel = row * PAL_COLS + col;
		palette_preview();
		return;
	case TKBD_KEY_DOWN:
		row = (row < PAL_ROWS - 1) ? row + 1 : 0;
		pal_sel = row * PAL_COLS + col;
		palette_preview();
		return;
	case TKBD_KEY_LEFT:
		col = (col > 0) ? col - 1 : PAL_COLS - 1;
		pal_sel = row * PAL_COLS + col;
		palette_preview();
		return;
	case TKBD_KEY_RIGHT:
		col = (col < PAL_COLS - 1) ? col + 1 : 0;
		pal_sel = row * PAL_COLS + col;
		palette_preview();
		return;
	case TKBD_KEY_ENTER:
		/* confirm -- keep the color */
		changed = 1;
		screen = 0;
		elem_draw();
		return;
	case TKBD_KEY_ESC:
		/* revert to saved color */
		*elem_color(&working, pal_elem) = saved_color;
		apply_theme();
		screen = 0;
		elem_draw();
		return;
	default:
		break;
	}
}

void
color_picker_input(const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	if (screen == 1) {
		palette_input(seq);
		return;
	}

	/* element list input */
	switch (seq->key) {
	case TKBD_KEY_UP:
		if (elem_list.sel > 0)
			elem_list.sel--;
		else
			elem_list.sel = ELEM_COUNT - 1;
		elem_draw();
		return;
	case TKBD_KEY_DOWN:
		if (elem_list.sel < ELEM_COUNT - 1)
			elem_list.sel++;
		else
			elem_list.sel = 0;
		elem_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		/* open palette for selected element */
		pal_elem = elem_list.sel;
		saved_color = *elem_color(&working, pal_elem);
		if (saved_color.type == VT_COLOR_INDEXED)
			pal_sel = saved_color.index;
		else
			pal_sel = 0;
		screen = 1;
		/* apply current theme so preview is active */
		apply_theme();
		palette_draw();
		return;
	case TKBD_KEY_LEFT:
		color_picker_hide(1);
		return;
	case TKBD_KEY_ESC:
		color_picker_hide(0);
		return;
	default:
		break;
	}
}
