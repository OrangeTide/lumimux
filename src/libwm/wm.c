/* wm.c : overlapping window manager compositor */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "wm.h"
#include "vt_buf.h"
#include "vt_state.h"
#include "tui_theme.h"
#include "rune_width.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wm {
	struct wm_window windows[WM_MAX_WINDOWS];
	int count;
	int rows, cols;
	struct vt_cell *screen;
	struct vt_cell *prev_screen;
	uint8_t *row_dirty;
	int next_z;
	int composite_needed;	/* set when state changes require recomposite */

	/* drag state */
	enum wm_drag drag;
	uint32_t drag_id;	/* window being dragged */
	int anchor_row, anchor_col;	/* mouse position at drag start */
	int orig_x, orig_y;	/* window position at drag start */
	int orig_w, orig_h;	/* window size at drag start (resize) */
	int edge_left, edge_top;	/* which edges are grabbed */
};

/* ---- helpers ---- */

static struct vt_cell *
screen_cell(struct wm *wm, int row, int col)
{
	if (row < 0 || row >= wm->rows || col < 0 || col >= wm->cols)
		return NULL;
	return &wm->screen[row * wm->cols + col];
}

static void
screen_put(struct wm *wm, int row, int col, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	struct vt_cell *c = screen_cell(wm, row, col);
	int w;

	if (!c)
		return;
	w = rune_width(cp);
	if (w < 1)
		w = 1;
	c->codepoint = cp;
	c->fg = fg;
	c->bg = bg;
	c->attrs = attrs;
	c->width = (uint8_t)w;
	if (w == 2) {
		struct vt_cell *c2 = screen_cell(wm, row, col + 1);

		if (c2) {
			c2->codepoint = 0;
			c2->fg = fg;
			c2->bg = bg;
			c2->attrs = attrs;
			c2->width = 0;
		}
	}
}

static void
screen_puts(struct wm *wm, int row, int col, const char *s,
    struct vt_color fg, struct vt_color bg, uint16_t attrs, int maxw)
{
	int c = col;
	int end = col + maxw;

	while (*s && c < end) {
		uint32_t cp;
		int len = utf8_decode(&cp, (const unsigned char *)s, 4);
		int w;

		if (len <= 0) {
			s++;
			continue;
		}
		w = rune_width(cp);
		if (w < 1)
			w = 1;
		if (c + w > end)
			break;
		screen_put(wm, row, c, cp, fg, bg, attrs);
		c += w;
		s += len;
	}
}

static int
str_display_width(const char *s, int maxcols)
{
	int cols = 0;

	while (*s && cols < maxcols) {
		uint32_t cp;
		int len = utf8_decode(&cp, (const unsigned char *)s, 4);
		int w;

		if (len <= 0) {
			s++;
			continue;
		}
		w = rune_width(cp);
		if (w < 1)
			w = 1;
		if (cols + w > maxcols)
			break;
		cols += w;
		s += len;
	}
	return cols;
}

static void
screen_fill(struct wm *wm, int row, int col, int n, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	int i;

	for (i = 0; i < n; i++)
		screen_put(wm, row, col + i, cp, fg, bg, attrs);
}

static void
screen_clear(struct wm *wm)
{
	int i, total;

	total = wm->rows * wm->cols;
	for (i = 0; i < total; i++)
		vt_cell_clear(&wm->screen[i]);
}

/* compare windows by z-order for qsort (lowest first) */
static int
z_compare(const void *a, const void *b)
{
	const int *ia = a;
	const int *ib = b;

	return ia[0] - ib[0];
}

/* ---- lifecycle ---- */

struct wm *
wm_new(int rows, int cols)
{
	struct wm *wm;

	wm = calloc(1, sizeof(*wm));
	if (!wm)
		return NULL;
	wm->rows = rows;
	wm->cols = cols;
	wm->screen = calloc((size_t)(rows * cols), sizeof(struct vt_cell));
	if (!wm->screen) {
		free(wm);
		return NULL;
	}
	wm->prev_screen = calloc((size_t)(rows * cols),
	    sizeof(struct vt_cell));
	if (!wm->prev_screen) {
		free(wm->screen);
		free(wm);
		return NULL;
	}
	wm->row_dirty = calloc((size_t)rows, 1);
	if (!wm->row_dirty) {
		free(wm->prev_screen);
		free(wm->screen);
		free(wm);
		return NULL;
	}
	wm->composite_needed = 1;
	return wm;
}

void
wm_free(struct wm *wm)
{
	if (!wm)
		return;
	free(wm->screen);
	free(wm->prev_screen);
	free(wm->row_dirty);
	free(wm);
}

void
wm_resize(struct wm *wm, int rows, int cols)
{
	struct vt_cell *new_screen;

	new_screen = calloc((size_t)(rows * cols), sizeof(struct vt_cell));
	if (!new_screen)
		return;
	free(wm->screen);
	wm->screen = new_screen;
	free(wm->prev_screen);
	wm->prev_screen = calloc((size_t)(rows * cols),
	    sizeof(struct vt_cell));
	free(wm->row_dirty);
	wm->row_dirty = calloc((size_t)rows, 1);
	wm->rows = rows;
	wm->cols = cols;
	wm->composite_needed = 1;
}

/* ---- window management ---- */

int
wm_add(struct wm *wm, uint32_t id, struct vt_state *vt,
    int x, int y, int w, int h)
{
	struct wm_window *win;

	if (wm->count >= WM_MAX_WINDOWS)
		return -1;
	if (wm_find(wm, id))
		return -1;

	win = &wm->windows[wm->count++];
	memset(win, 0, sizeof(*win));
	win->id = id;
	win->vt = vt;
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;
	win->z = wm->next_z++;
	wm->composite_needed = 1;
	return 0;
}

void
wm_remove(struct wm *wm, uint32_t id)
{
	int i;

	for (i = 0; i < wm->count; i++) {
		if (wm->windows[i].id == id) {
			wm->windows[i] = wm->windows[--wm->count];
			wm->composite_needed = 1;
			return;
		}
	}
}

struct wm_window *
wm_find(struct wm *wm, uint32_t id)
{
	int i;

	for (i = 0; i < wm->count; i++) {
		if (wm->windows[i].id == id)
			return &wm->windows[i];
	}
	return NULL;
}

int
wm_count(const struct wm *wm)
{
	return wm->count;
}

struct wm_window *
wm_window_at(struct wm *wm, int index)
{
	if (index < 0 || index >= wm->count)
		return NULL;
	return &wm->windows[index];
}

/* ---- z-order and focus ---- */

void
wm_focus(struct wm *wm, uint32_t id)
{
	int i;

	for (i = 0; i < wm->count; i++)
		wm->windows[i].focused = (wm->windows[i].id == id) ? 1 : 0;
	wm_raise(wm, id);
	wm->composite_needed = 1;
}

void
wm_raise(struct wm *wm, uint32_t id)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->z = wm->next_z++;
		wm->composite_needed = 1;
	}
}

/* ---- window manipulation ---- */

void
wm_move(struct wm *wm, uint32_t id, int x, int y)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->x = x;
		win->y = y;
		wm->composite_needed = 1;
	}
}

void
wm_set_title(struct wm *wm, uint32_t id, const char *title)
{
	struct wm_window *win = wm_find(wm, id);
	size_t len;

	if (!win)
		return;
	len = utf8_trunc(title, WM_TITLE_MAX);
	memcpy(win->title, title, len);
	win->title[len] = '\0';
	wm->composite_needed = 1;
}

void
wm_set_flags(struct wm *wm, uint32_t id, uint32_t flags)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->flags = flags;
		wm->composite_needed = 1;
	}
}

void
wm_set_theme(struct wm *wm, uint32_t id, const struct tui_theme *t)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->win_theme = t;
		wm->composite_needed = 1;
	}
}

const struct tui_theme *
wm_get_theme(struct wm *wm, uint32_t id)
{
	struct wm_window *win = wm_find(wm, id);

	return win ? win->win_theme : NULL;
}

void
wm_set_scroll(struct wm *wm, uint32_t id, float pos, float len)
{
	struct wm_window *win = wm_find(wm, id);

	if (!win)
		return;
	if (pos < 0.0f) pos = 0.0f;
	if (pos > 1.0f) pos = 1.0f;
	if (len < 0.0f) len = 0.0f;
	if (len > 1.0f) len = 1.0f;
	win->scroll_pos = pos;
	win->scroll_len = len;
	wm->composite_needed = 1;
}

float
wm_scroll_pos_from_row(const struct wm *wm, uint32_t id, int row)
{
	int i;

	for (i = 0; i < wm->count; i++) {
		const struct wm_window *win = &wm->windows[i];
		int track_top, track_inner;

		if (win->id != id)
			continue;

		/* scrollbar occupies right border column, rows fy+1..fy+fh-2 */
		track_top = win->y;		/* fy + 1 */
		if (win->h >= 4) {
			/* arrows at top and bottom */
			track_top++;
			track_inner = win->h - 2;
		} else {
			track_inner = win->h;
		}
		if (track_inner <= 0)
			return 0.0f;

		return (float)(row - track_top) / (float)(track_inner - 1);
	}
	return 0.0f;
}

void
wm_minimize(struct wm *wm, uint32_t id)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->minimized = 1;
		wm->composite_needed = 1;
	}
}

void
wm_unminimize(struct wm *wm, uint32_t id)
{
	struct wm_window *win = wm_find(wm, id);

	if (win) {
		win->minimized = 0;
		wm->composite_needed = 1;
	}
}

int
wm_toggle_maximize(struct wm *wm, uint32_t id)
{
	struct wm_window *win = wm_find(wm, id);

	if (!win)
		return 0;

	if (win->maximized) {
		/* restore saved geometry */
		win->x = win->saved_x;
		win->y = win->saved_y;
		win->w = win->saved_w;
		win->h = win->saved_h;
		win->maximized = 0;
		wm->composite_needed = 1;
		return 0;
	}

	/* save current geometry and fill screen */
	win->saved_x = win->x;
	win->saved_y = win->y;
	win->saved_w = win->w;
	win->saved_h = win->h;
	win->x = 1;
	win->y = 1;
	win->w = wm->cols - 2;
	win->h = wm->rows - 2;
	win->maximized = 1;
	wm->composite_needed = 1;
	return 1;
}

/* ---- compositor ---- */

static void
draw_shadow(struct wm *wm, const struct wm_window *win,
    const struct tui_theme *theme)
{
	int fx, fy, fw, fh;
	int r, c;

	if (theme->shadow == TUI_SHADOW_NONE)
		return;

	/* frame bounds (content + border) */
	fx = win->x - 1;
	fy = win->y - 1;
	fw = win->w + 2;
	fh = win->h + 2;

	if (theme->shadow == TUI_SHADOW_HALF) {
		for (r = fy + 1; r < fy + fh; r++)
			screen_put(wm, r, fx + fw, 0x2584,
			    theme->shadow_fg, theme->shadow_bg, 0);
		for (c = fx + 1; c <= fx + fw; c++)
			screen_put(wm, fy + fh, c, 0x2580,
			    theme->shadow_fg, theme->shadow_bg, 0);
	} else if (theme->shadow == TUI_SHADOW_SHADE) {
		for (r = fy + 1; r < fy + fh; r++)
			screen_put(wm, r, fx + fw, 0x2591,
			    theme->shadow_fg, theme->shadow_bg, 0);
		for (c = fx + 1; c <= fx + fw; c++)
			screen_put(wm, fy + fh, c, 0x2591,
			    theme->shadow_fg, theme->shadow_bg, 0);
	}
}

static void
draw_frame(struct wm *wm, const struct wm_window *win,
    const struct tui_theme *theme)
{
	int fx, fy, fw, fh, r, c;
	struct vt_color bfg = win->focused ? theme->focus_fg : theme->unfocus_fg;
	struct vt_color bbg = win->focused ? theme->focus_bg : theme->unfocus_bg;

	/* frame bounds */
	fx = win->x - 1;
	fy = win->y - 1;
	fw = win->w + 2;
	fh = win->h + 2;

	/* top border */
	screen_puts(wm, fy, fx, theme->border[TUI_BORDER_TL],
	    bfg, bbg, 0, 1);
	for (c = fx + 1; c < fx + fw - 1; c++)
		screen_puts(wm, fy, c, theme->border[TUI_BORDER_T],
		    bfg, bbg, 0, 1);
	screen_puts(wm, fy, fx + fw - 1, theme->border[TUI_BORDER_TR],
	    bfg, bbg, 0, 1);

	/* count status icons to reserve space on title bar */
	{
		int nicons = 0;
		int nbtns;	/* total button cells: [_][+][x] = 9, or [x] = 3 */
		int icol;

		if (win->flags & WM_WIN_DEAD)
			nicons++;
		if (win->flags & WM_WIN_SCROLL_LOCK)
			nicons++;
		if (win->flags & WM_WIN_INPUT_LOCK)
			nicons++;

		/* show all three buttons if there's room, else just close */
		nbtns = (fw >= 12) ? 9 : 3;

		/* focus indicator + title */
		{
			int start = fx + 1;

			/* focus indicator: ☼ for focused, space for unfocused */
			screen_put(wm, fy, start,
			    win->focused ? 0x263C : ' ',  /* ☼ */
			    theme->indicator_fg, bbg, VT_ATTR_BOLD);
			start++;

			if (win->title[0]) {
				struct vt_color tfg = win->focused ?
				    theme->title_focus_fg :
				    theme->title_idle_fg;
				/* indicator + brackets + buttons + icon slots */
				int avail = fw - nbtns - 2 - nicons;
				int twidth;

				if (avail < 0)
					avail = 0;
				twidth = str_display_width(win->title,
				    avail);
				screen_puts(wm, fy, start, theme->title_l,
				    tfg, bbg, VT_ATTR_BOLD, 1);
				screen_puts(wm, fy, start + 1, win->title,
				    tfg, bbg, VT_ATTR_BOLD, twidth);
				screen_puts(wm, fy, start + 1 + twidth,
				    theme->title_r,
				    tfg, bbg, VT_ATTR_BOLD, 1);
			}
		}

		/* buttons on top-right: [_][+][x] or just [x] */
		if (fw >= 12) {
			int mx = fx + fw - 10;

			/* minimize [_] */
			screen_put(wm, fy, mx, '[', bfg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 1, '_',
			    theme->tool_fg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 2, ']', bfg, bbg, VT_ATTR_BOLD);

			/* maximize [+] or restore [=] */
			screen_put(wm, fy, mx + 3, '[', bfg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 4,
			    win->maximized ? '=' : '+',
			    theme->tool_fg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 5, ']', bfg, bbg, VT_ATTR_BOLD);

			/* close [x] */
			screen_put(wm, fy, mx + 6, '[', bfg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 7, 'x',
			    theme->close_fg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, mx + 8, ']', bfg, bbg, VT_ATTR_BOLD);
		} else if (fw >= 6) {
			int cx = fx + fw - 4;

			screen_put(wm, fy, cx, '[', bfg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, cx + 1, 'x',
			    theme->close_fg, bbg, VT_ATTR_BOLD);
			screen_put(wm, fy, cx + 2, ']', bfg, bbg, VT_ATTR_BOLD);
		}

		/* status icons right before buttons */
		icol = fx + fw - nbtns - 1 - nicons;
		if (icol < fx + 1)
			icol = fx + 1;
		if (win->flags & WM_WIN_DEAD) {
			struct vt_color sfg = win->focused ?
			    theme->status_focus_fg :
			    theme->status_idle_fg;

			screen_puts(wm, fy, icol, theme->icon_dead,
			    sfg, bbg, VT_ATTR_BOLD, 1);
			icol++;
		}
		if (win->flags & WM_WIN_SCROLL_LOCK) {
			struct vt_color sfg = win->focused ?
			    theme->status_focus_fg :
			    theme->status_idle_fg;

			screen_puts(wm, fy, icol,
			    theme->icon_scroll_lock,
			    sfg, bbg, VT_ATTR_BOLD, 1);
			icol++;
		}
		if (win->flags & WM_WIN_INPUT_LOCK) {
			struct vt_color sfg = win->focused ?
			    theme->status_focus_fg :
			    theme->status_idle_fg;

			screen_puts(wm, fy, icol,
			    theme->icon_input_lock,
			    sfg, bbg, VT_ATTR_BOLD, 1);
			icol++;
		}
	}

	/* side borders */
	for (r = fy + 1; r < fy + fh - 1; r++) {
		screen_puts(wm, r, fx, theme->border[TUI_BORDER_L],
		    bfg, bbg, 0, 1);
		screen_puts(wm, r, fx + fw - 1, theme->border[TUI_BORDER_R],
		    bfg, bbg, 0, 1);
	}

	/* scrollbar overlays right border when active */
	if (win->scroll_len > 0.0f && win->h > 0 &&
	    theme->scroll_track && theme->scroll_track[0]) {
		int sb_col = fx + fw - 1;
		int track_top = fy + 1;
		int track_h = win->h;	/* total right-border rows */
		int has_arrows = (track_h >= 4);
		int inner_top, inner_h;
		int thumb_cells, thumb_start;
		float len = win->scroll_len;
		float pos = win->scroll_pos;
		struct vt_color tfg = win->focused ?
		    theme->title_focus_fg : theme->title_idle_fg;
		struct vt_color sbg = bbg;

		if (has_arrows) {
			screen_puts(wm, track_top, sb_col,
			    theme->scroll_up, tfg, sbg, 0, 1);
			screen_puts(wm, track_top + track_h - 1, sb_col,
			    theme->scroll_down, tfg, sbg, 0, 1);
			inner_top = track_top + 1;
			inner_h = track_h - 2;
		} else {
			inner_top = track_top;
			inner_h = track_h;
		}

		if (inner_h > 0) {
			/* compute thumb geometry */
			thumb_cells = (int)(len * (float)inner_h + 0.5f);
			if (thumb_cells < 1)
				thumb_cells = 1;
			if (thumb_cells > inner_h)
				thumb_cells = inner_h;
			thumb_start = (int)(pos *
			    (float)(inner_h - thumb_cells) + 0.5f);
			if (thumb_start < 0)
				thumb_start = 0;
			if (thumb_start + thumb_cells > inner_h)
				thumb_start = inner_h - thumb_cells;

			/* draw track and thumb */
			for (r = 0; r < inner_h; r++) {
				if (r >= thumb_start &&
				    r < thumb_start + thumb_cells)
					screen_puts(wm,
					    inner_top + r, sb_col,
					    theme->scroll_thumb,
					    theme->sel_fg, sbg,
					    VT_ATTR_BOLD, 1);
				else
					screen_puts(wm,
					    inner_top + r, sb_col,
					    theme->scroll_track,
					    bfg, sbg, 0, 1);
			}
		}
	}

	/* bottom border */
	screen_puts(wm, fy + fh - 1, fx, theme->border[TUI_BORDER_BL],
	    bfg, bbg, 0, 1);
	for (c = fx + 1; c < fx + fw - 1; c++)
		screen_puts(wm, fy + fh - 1, c, theme->border[TUI_BORDER_B],
		    bfg, bbg, 0, 1);
	screen_puts(wm, fy + fh - 1, fx + fw - 1,
	    theme->border[TUI_BORDER_BR], bfg, bbg, 0, 1);
}

static void
draw_content(struct wm *wm, const struct wm_window *win)
{
	struct vt_buf *buf;
	int r, c;

	if (!win->vt)
		return;
	buf = win->vt->buf;
	if (!buf)
		return;

	for (r = 0; r < win->h; r++) {
		struct vt_row *vr = vt_buf_row(buf, r);
		int dst_row = win->y + r;

		if (!vr)
			continue;

		for (c = 0; c < win->w; c++) {
			struct vt_cell *dst;

			dst = screen_cell(wm, dst_row, win->x + c);
			if (!dst)
				continue;
			*dst = vr->cells[c];
			dst->attrs &= ~VT_ATTR_PREDICTED;
		}

		vr->flags &= ~VT_ROW_DIRTY;
	}
}

static int
any_vt_dirty(const struct wm *wm)
{
	int i;

	for (i = 0; i < wm->count; i++) {
		const struct wm_window *win = &wm->windows[i];
		struct vt_buf *buf;
		int r;

		if (win->minimized || !win->vt)
			continue;
		buf = win->vt->buf;
		if (!buf)
			continue;
		for (r = 0; r < win->h; r++) {
			struct vt_row *vr = vt_buf_row(buf, r);

			if (vr && (vr->flags & VT_ROW_DIRTY))
				return 1;
		}
	}
	return 0;
}

void
wm_composite(struct wm *wm, const struct tui_theme *theme)
{
	int order[WM_MAX_WINDOWS][2]; /* [z, index] */
	int i, n;

	memset(wm->row_dirty, 0, (size_t)wm->rows);
	if (!wm->composite_needed && !any_vt_dirty(wm))
		return;
	wm->composite_needed = 0;

	screen_clear(wm);

	/* build z-sorted order */
	n = 0;
	for (i = 0; i < wm->count; i++) {
		if (wm->windows[i].minimized)
			continue;
		order[n][0] = wm->windows[i].z;
		order[n][1] = i;
		n++;
	}
	qsort(order, (size_t)n, sizeof(order[0]), z_compare);

	/* paint back-to-front */
	for (i = 0; i < n; i++) {
		const struct wm_window *win = &wm->windows[order[i][1]];
		const struct tui_theme *wt = win->win_theme ?
		    win->win_theme : theme;

		draw_shadow(wm, win, wt);
		draw_frame(wm, win, wt);
		draw_content(wm, win);

		/* show cursor position in unfocused windows via
		 * reverse video so the user can see where each
		 * window's cursor sits without switching focus */
		if (!win->focused && win->vt &&
		    (win->vt->modes & VT_MODE_CURSOR_VIS)) {
			struct vt_cell *cell;

			cell = screen_cell(wm,
			    win->y + win->vt->cursor_row,
			    win->x + win->vt->cursor_col);
			if (cell)
				cell->attrs ^= VT_ATTR_REVERSE;
		}
	}

	/* overlay "WxH" size indicator centered in the window being resized */
	if (wm->drag == WM_DRAG_RESIZING) {
		struct wm_window *dw = wm_find(wm, wm->drag_id);

		if (dw && dw->w >= 5 && dw->h >= 1) {
			char label[32];
			int len, lx, ly;
			struct vt_color fg = { VT_COLOR_INDEXED, { .index = 15 } };
			struct vt_color bg = { VT_COLOR_INDEXED, { .index = 0 } };

			len = snprintf(label, sizeof(label), "%dx%d",
			    dw->w, dw->h);
			if (len > dw->w)
				len = dw->w;
			lx = dw->x + (dw->w - len) / 2;
			ly = dw->y + dw->h / 2;
			screen_puts(wm, ly, lx, label,
			    fg, bg, VT_ATTR_BOLD, len);
		}
	}
}

const struct vt_cell *
wm_screen(const struct wm *wm)
{
	return wm->screen;
}

struct vt_cell *
wm_screen_mut(struct wm *wm)
{
	return wm->screen;
}

const uint8_t *
wm_row_dirty(const struct wm *wm)
{
	return wm ? wm->row_dirty : NULL;
}

static int
color_eq(const struct vt_color *a, const struct vt_color *b)
{
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case VT_COLOR_DEFAULT:
		return 1;
	case VT_COLOR_INDEXED:
		return a->index == b->index;
	case VT_COLOR_RGB:
		return a->rgb.r == b->rgb.r &&
		    a->rgb.g == b->rgb.g &&
		    a->rgb.b == b->rgb.b;
	}
	return 0;
}

static int
cell_eq(const struct vt_cell *a, const struct vt_cell *b)
{
	return a->codepoint == b->codepoint &&
	    a->attrs == b->attrs &&
	    a->width == b->width &&
	    color_eq(&a->fg, &b->fg) &&
	    color_eq(&a->bg, &b->bg);
}

static int
row_changed(const struct vt_cell *a, const struct vt_cell *b, int cols)
{
	int c;

	for (c = 0; c < cols; c++) {
		if (!cell_eq(&a[c], &b[c]))
			return 1;
	}
	return 0;
}

void
wm_update_dirty(struct wm *wm)
{
	int r;
	size_t row_bytes;

	if (!wm || !wm->prev_screen)
		return;

	row_bytes = (size_t)wm->cols * sizeof(struct vt_cell);
	for (r = 0; r < wm->rows; r++) {
		size_t off = (size_t)r * (size_t)wm->cols;

		if (memcmp(&wm->screen[off], &wm->prev_screen[off],
		    row_bytes) != 0 &&
		    row_changed(&wm->screen[off], &wm->prev_screen[off],
		    wm->cols))
			wm->row_dirty[r] = 1;
	}
	memcpy(wm->prev_screen, wm->screen,
	    (size_t)(wm->rows * wm->cols) * sizeof(struct vt_cell));
}

int
wm_rows(const struct wm *wm)
{
	return wm->rows;
}

int
wm_cols(const struct wm *wm)
{
	return wm->cols;
}

/* ---- hit testing ---- */

int
wm_hit_test(const struct wm *wm, int row, int col,
    uint32_t *out_id, enum wm_hit *out_area)
{
	int i, best = -1, best_z = -1;

	/* find topmost window containing this point */
	for (i = 0; i < wm->count; i++) {
		const struct wm_window *win = &wm->windows[i];
		int fx, fy, fw, fh;

		if (win->minimized)
			continue;

		/* frame bounds */
		fx = win->x - 1;
		fy = win->y - 1;
		fw = win->w + 2;
		fh = win->h + 2;

		if (row >= fy && row < fy + fh &&
		    col >= fx && col < fx + fw) {
			if (win->z > best_z) {
				best = i;
				best_z = win->z;
			}
		}
	}

	if (best < 0) {
		if (out_area)
			*out_area = WM_HIT_NONE;
		return 0;
	}

	{
		const struct wm_window *win = &wm->windows[best];
		int fx = win->x - 1;
		int fy = win->y - 1;
		int fw = win->w + 2;
		int fh = win->h + 2;

		if (out_id)
			*out_id = win->id;

		/* title bar buttons: [_][+][x] or just [x] */
		if (row == fy && fw >= 12) {
			int mx = fx + fw - 10;

			/* minimize button */
			if (col >= mx && col <= mx + 2) {
				if (out_area)
					*out_area = WM_HIT_MINIMIZE;
				return 1;
			}
			/* maximize button */
			if (col >= mx + 3 && col <= mx + 5) {
				if (out_area)
					*out_area = WM_HIT_MAXIMIZE;
				return 1;
			}
			/* close button */
			if (col >= mx + 6 && col <= mx + 8) {
				if (out_area)
					*out_area = WM_HIT_CLOSE;
				return 1;
			}
		} else if (row == fy && fw >= 6 &&
		    col >= fx + fw - 4 && col <= fx + fw - 2) {
			if (out_area)
				*out_area = WM_HIT_CLOSE;
			return 1;
		}

		/* title bar (top border row, excluding buttons) */
		if (row == fy) {
			if (out_area)
				*out_area = WM_HIT_TITLE;
			return 1;
		}

		/* scrollbar on right border column */
		if (win->scroll_len > 0.0f &&
		    col == fx + fw - 1 &&
		    row > fy && row < fy + fh - 1) {
			if (out_area)
				*out_area = WM_HIT_SCROLLBAR;
			return 1;
		}

		/* content area */
		if (row >= win->y && row < win->y + win->h &&
		    col >= win->x && col < win->x + win->w) {
			if (out_area)
				*out_area = WM_HIT_CONTENT;
			return 1;
		}

		/* anything else in the frame is border */
		if (out_area)
			*out_area = WM_HIT_BORDER;
		return 1;
	}
}

/* ---- mouse interaction ---- */

enum wm_drag
wm_drag_state(const struct wm *wm)
{
	return wm->drag;
}

uint32_t
wm_drag_id(const struct wm *wm)
{
	return wm->drag_id;
}

enum wm_drag
wm_mouse_press(struct wm *wm, int row, int col,
    uint32_t *out_id, enum wm_hit *out_area)
{
	uint32_t id;
	enum wm_hit area;
	struct wm_window *win;

	if (!wm_hit_test(wm, row, col, &id, &area)) {
		if (out_area)
			*out_area = WM_HIT_NONE;
		return WM_DRAG_IDLE;
	}

	if (out_id)
		*out_id = id;
	if (out_area)
		*out_area = area;

	/* always focus the clicked window */
	wm_focus(wm, id);

	switch (area) {
	case WM_HIT_TITLE:
		win = wm_find(wm, id);
		if (!win)
			break;
		wm->drag = WM_DRAG_MOVING;
		wm->drag_id = id;
		wm->anchor_row = row;
		wm->anchor_col = col;
		wm->orig_x = win->x;
		wm->orig_y = win->y;
		return WM_DRAG_MOVING;

	case WM_HIT_BORDER:
		win = wm_find(wm, id);
		if (!win)
			break;
		wm->drag = WM_DRAG_RESIZING;
		wm->drag_id = id;
		wm->anchor_row = row;
		wm->anchor_col = col;
		wm->orig_x = win->x;
		wm->orig_y = win->y;
		wm->orig_w = win->w;
		wm->orig_h = win->h;
		/* detect which edges are grabbed */
		wm->edge_left = (col < win->x);
		wm->edge_top = (row < win->y);
		return WM_DRAG_RESIZING;

	case WM_HIT_SCROLLBAR:
		win = wm_find(wm, id);
		if (!win)
			break;
		wm->drag = WM_DRAG_SCROLLING;
		wm->drag_id = id;
		wm->anchor_row = row;
		wm->anchor_col = col;
		return WM_DRAG_SCROLLING;

	case WM_HIT_CLOSE:
	case WM_HIT_MINIMIZE:
	case WM_HIT_MAXIMIZE:
	case WM_HIT_CONTENT:
	case WM_HIT_NONE:
		break;
	}

	return WM_DRAG_IDLE;
}

int
wm_mouse_drag(struct wm *wm, int row, int col)
{
	struct wm_window *win;
	int dr, dc;

	if (wm->drag == WM_DRAG_IDLE)
		return 0;

	win = wm_find(wm, wm->drag_id);
	if (!win) {
		wm->drag = WM_DRAG_IDLE;
		return 0;
	}

	dr = row - wm->anchor_row;
	dc = col - wm->anchor_col;

	if (wm->drag == WM_DRAG_MOVING) {
		int new_x = wm->orig_x + dc;
		int new_y = wm->orig_y + dr;

		if (new_x == win->x && new_y == win->y)
			return 0;
		win->x = new_x;
		win->y = new_y;
		wm->composite_needed = 1;
		return 1;
	}

	if (wm->drag == WM_DRAG_RESIZING) {
		int new_w, new_h, new_x, new_y;

		/* left edge: moving left grows, moving right shrinks */
		if (wm->edge_left) {
			new_w = wm->orig_w - dc;
			new_x = wm->orig_x + dc;
		} else {
			new_w = wm->orig_w + dc;
			new_x = win->x;
		}

		/* top edge: moving up grows, moving down shrinks */
		if (wm->edge_top) {
			new_h = wm->orig_h - dr;
			new_y = wm->orig_y + dr;
		} else {
			new_h = wm->orig_h + dr;
			new_y = win->y;
		}

		/* clamp to minimum size */
		if (new_w < WM_MIN_WIDTH) {
			if (wm->edge_left)
				new_x -= WM_MIN_WIDTH - new_w;
			new_w = WM_MIN_WIDTH;
		}
		if (new_h < WM_MIN_HEIGHT) {
			if (wm->edge_top)
				new_y -= WM_MIN_HEIGHT - new_h;
			new_h = WM_MIN_HEIGHT;
		}

		if (new_w == win->w && new_h == win->h &&
		    new_x == win->x && new_y == win->y)
			return 0;
		win->w = new_w;
		win->h = new_h;
		win->x = new_x;
		win->y = new_y;
		wm->composite_needed = 1;
		return 1;
	}

	if (wm->drag == WM_DRAG_SCROLLING) {
		float new_pos;

		new_pos = wm_scroll_pos_from_row(wm, wm->drag_id, row);
		if (new_pos < 0.0f) new_pos = 0.0f;
		if (new_pos > 1.0f) new_pos = 1.0f;
		if (new_pos == win->scroll_pos)
			return 0;
		win->scroll_pos = new_pos;
		wm->composite_needed = 1;
		return 1;
	}

	return 0;
}

int
wm_arrange_grid(struct wm *wm)
{
	struct wm_window *wins[WM_MAX_WINDOWS];
	int n, i, gcols, grows;
	int cell_w, cell_h, extra_w, extra_h;

	n = 0;
	for (i = 0; i < wm->count; i++) {
		if (!wm->windows[i].minimized)
			wins[n++] = &wm->windows[i];
	}
	if (n == 0)
		return -1;

	/* integer ceiling of sqrt(n) for column count */
	gcols = 1;
	while (gcols * gcols < n)
		gcols++;
	grows = (n + gcols - 1) / gcols;

	cell_w = wm->cols / gcols;
	cell_h = wm->rows / grows;
	extra_w = wm->cols - cell_w * gcols;
	extra_h = wm->rows - cell_h * grows;

	for (i = 0; i < n; i++) {
		int gr = i / gcols;
		int gc = i % gcols;
		int x, y, w, h;

		x = gc * cell_w + (gc < extra_w ? gc : extra_w) + 1;
		y = gr * cell_h + (gr < extra_h ? gr : extra_h) + 1;
		w = cell_w + (gc < extra_w ? 1 : 0) - 2;
		h = cell_h + (gr < extra_h ? 1 : 0) - 2;

		if (w < WM_MIN_WIDTH)
			w = WM_MIN_WIDTH;
		if (h < WM_MIN_HEIGHT)
			h = WM_MIN_HEIGHT;

		wins[i]->x = x;
		wins[i]->y = y;
		wins[i]->w = w;
		wins[i]->h = h;
		wins[i]->maximized = 0;
	}

	return 0;
}

uint32_t
wm_mouse_release(struct wm *wm, int *resized)
{
	uint32_t id;
	int was_resize;

	if (wm->drag == WM_DRAG_IDLE) {
		if (resized)
			*resized = 0;
		return 0;
	}

	id = wm->drag_id;
	was_resize = (wm->drag == WM_DRAG_RESIZING);
	wm->drag = WM_DRAG_IDLE;

	if (resized)
		*resized = was_resize;
	return id;
}
