/* tile_composite.c : tiled pane compositor and border renderer */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tile.h"
#include "vt_cell.h"
#include "vt_state.h"
#include "vt_buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- border glyphs ---- */

/* focused (heavy box-drawing) */
#define GLYPH_F_H	0x2501	/* ━ horizontal */
#define GLYPH_F_V	0x2503	/* ┃ vertical */
#define GLYPH_F_TL	0x250F	/* ┏ top-left */
#define GLYPH_F_TR	0x2513	/* ┓ top-right */
#define GLYPH_F_BL	0x2517	/* ┗ bottom-left */
#define GLYPH_F_BR	0x251B	/* ┛ bottom-right */
#define GLYPH_F_LT	0x2523	/* ┣ left tee */
#define GLYPH_F_RT	0x252B	/* ┫ right tee */
#define GLYPH_F_TT	0x2533	/* ┳ top tee */
#define GLYPH_F_BT	0x253B	/* ┻ bottom tee */
#define GLYPH_F_X	0x254B	/* ╋ cross */

/* unfocused (light dashed + arc corners) */
#define GLYPH_U_H	0x2504	/* ┄ horizontal */
#define GLYPH_U_V	0x250A	/* ┊ vertical */
#define GLYPH_U_TL	0x256D	/* ╭ top-left */
#define GLYPH_U_TR	0x256E	/* ╮ top-right */
#define GLYPH_U_BL	0x2570	/* ╰ bottom-left */
#define GLYPH_U_BR	0x256F	/* ╯ bottom-right */
#define GLYPH_U_LT	0x251C	/* ├ left tee */
#define GLYPH_U_RT	0x2524	/* ┤ right tee */
#define GLYPH_U_TT	0x252C	/* ┬ top tee */
#define GLYPH_U_BT	0x2534	/* ┴ bottom tee */
#define GLYPH_U_X	0x253C	/* ┼ cross */

/* ---- internal tile struct access ---- */

/* tile.c keeps struct tile opaque; we access it via public API */

/* ---- separator bitmap ---- */

/*
 * We use a 2D bitmap to track where separators are drawn
 * and whether they're horizontal, vertical, or intersections.
 * Bits:
 *   0x01 = horizontal separator here
 *   0x02 = vertical separator here
 *   0x04 = adjacent to focused pane
 */
#define SEP_H		0x01
#define SEP_V		0x02
#define SEP_FOCUSED	0x04

/* ---- helpers ---- */

static struct vt_cell *
screen_cell(struct vt_cell *screen, int rows, int cols, int r, int c)
{
	if (r < 0 || r >= rows || c < 0 || c >= cols)
		return NULL;
	return &screen[r * cols + c];
}

static void
screen_put_glyph(struct vt_cell *screen, int rows, int cols,
    int r, int c, uint32_t cp, int focused)
{
	struct vt_cell *cell;

	cell = screen_cell(screen, rows, cols, r, c);
	if (!cell)
		return;
	cell->codepoint = cp;
	cell->width = 1;
	cell->attrs = 0;
	if (focused) {
		cell->fg.type = VT_COLOR_INDEXED;
		cell->fg.index = 7;	/* white */
		cell->bg.type = VT_COLOR_DEFAULT;
	} else {
		cell->fg.type = VT_COLOR_INDEXED;
		cell->fg.index = 8;	/* dark grey */
		cell->bg.type = VT_COLOR_DEFAULT;
	}
}

/* ---- content blitting ---- */

static void
blit_pane(struct vt_cell *screen, uint8_t *row_dirty, int srows, int scols,
    const struct tile_node *n)
{
	struct vt_buf *buf;
	int r, c;

	if (!n->vt)
		return;
	buf = n->vt->buf;
	if (!buf)
		return;

	for (r = 0; r < n->h; r++) {
		struct vt_row *vr = vt_buf_row(buf, r);
		int dst_row = n->y + r;
		int dirty;

		if (!vr)
			continue;

		dirty = (vr->flags & VT_ROW_DIRTY) != 0;

		for (c = 0; c < n->w; c++) {
			struct vt_cell *src, *dst;

			src = &vr->cells[c];
			dst = screen_cell(screen, srows, scols,
			    dst_row, n->x + c);
			if (!dst)
				continue;
			*dst = *src;
		}

		if (dirty && dst_row >= 0 && dst_row < srows)
			row_dirty[dst_row] = 1;

		vr->flags &= ~VT_ROW_DIRTY;
	}
}

static void
blit_leaves(struct vt_cell *screen, uint8_t *row_dirty, int srows, int scols,
    const struct tile_node *n)
{
	if (!n)
		return;
	if (n->type == TILE_LEAF) {
		blit_pane(screen, row_dirty, srows, scols, n);
		return;
	}
	blit_leaves(screen, row_dirty, srows, scols, n->a);
	blit_leaves(screen, row_dirty, srows, scols, n->b);
}

/* ---- separator rendering ---- */

/*
 * Walk the tree and mark separator lines in a bitmap.
 * Also track which pane is focused so we can determine
 * whether a separator is adjacent to the focused pane.
 */

static int
subtree_has_focused(const struct tile_node *n, uint32_t focused_id)
{
	if (!n)
		return 0;
	if (n->type == TILE_LEAF)
		return n->window_id == focused_id;
	return subtree_has_focused(n->a, focused_id) ||
	    subtree_has_focused(n->b, focused_id);
}

static void
mark_separators(uint8_t *smap, int srows, int scols,
    const struct tile_node *n, uint32_t focused_id)
{
	int i, pos;
	int focused;

	if (!n || n->type == TILE_LEAF)
		return;

	/* is this separator adjacent to the focused pane? */
	focused = subtree_has_focused(n->a, focused_id) &&
	    subtree_has_focused(n->b, focused_id);
	if (!focused) {
		focused = subtree_has_focused(n->a, focused_id) ||
		    subtree_has_focused(n->b, focused_id);
	}

	if (n->type == TILE_SPLIT_V) {
		/* vertical separator at column between a and b */
		pos = n->a->x + n->a->w;
		if (pos >= 0 && pos < scols) {
			for (i = n->y; i < n->y + n->h && i < srows; i++) {
				smap[i * scols + pos] |= SEP_V;
				if (focused)
					smap[i * scols + pos] |= SEP_FOCUSED;
			}
		}
	} else { /* TILE_SPLIT_H */
		/* horizontal separator at row between a and b */
		pos = n->a->y + n->a->h;
		if (pos >= 0 && pos < srows) {
			for (i = n->x; i < n->x + n->w && i < scols; i++) {
				smap[pos * scols + i] |= SEP_H;
				if (focused)
					smap[pos * scols + i] |= SEP_FOCUSED;
			}
		}
	}

	mark_separators(smap, srows, scols, n->a, focused_id);
	mark_separators(smap, srows, scols, n->b, focused_id);
}

static uint32_t
pick_glyph(uint8_t flags)
{
	int h = flags & SEP_H;
	int v = flags & SEP_V;
	int f = flags & SEP_FOCUSED;

	if (h && v) {
		/* intersection */
		return f ? GLYPH_F_X : GLYPH_U_X;
	}
	if (h) {
		return f ? GLYPH_F_H : GLYPH_U_H;
	}
	if (v) {
		return f ? GLYPH_F_V : GLYPH_U_V;
	}
	return ' ';
}

static void
draw_separators(struct vt_cell *screen, int srows, int scols,
    const uint8_t *smap)
{
	int r, c;

	for (r = 0; r < srows; r++) {
		for (c = 0; c < scols; c++) {
			uint8_t flags = smap[r * scols + c];
			uint32_t glyph;

			if (!flags)
				continue;
			glyph = pick_glyph(flags);
			screen_put_glyph(screen, srows, scols, r, c,
			    glyph, flags & SEP_FOCUSED);
		}
	}
}

/*
 * Check for tee and corner glyphs at separator endpoints.
 * Walk the smap looking for cells where a separator line starts
 * or ends at a perpendicular separator.
 */
static void
fix_endpoints(struct vt_cell *screen, int srows, int scols,
    const uint8_t *smap)
{
	int r, c;

	for (r = 0; r < srows; r++) {
		for (c = 0; c < scols; c++) {
			uint8_t flags = smap[r * scols + c];
			int up, down, left, right;
			int f;
			uint32_t glyph;

			if (!(flags & (SEP_H | SEP_V)))
				continue;
			if ((flags & SEP_H) && (flags & SEP_V))
				continue; /* already handled as cross */

			/* check neighbors for perpendicular separators */
			up = (r > 0 && (smap[(r - 1) * scols + c] & SEP_V));
			down = (r < srows - 1 &&
			    (smap[(r + 1) * scols + c] & SEP_V));
			left = (c > 0 && (smap[r * scols + c - 1] & SEP_H));
			right = (c < scols - 1 &&
			    (smap[r * scols + c + 1] & SEP_H));

			f = flags & SEP_FOCUSED;

			if (flags & SEP_V) {
				/* vertical line -- check for H at this cell */
				if (left && right) {
					glyph = f ? GLYPH_F_X : GLYPH_U_X;
				} else if (left) {
					glyph = f ? GLYPH_F_RT : GLYPH_U_RT;
				} else if (right) {
					glyph = f ? GLYPH_F_LT : GLYPH_U_LT;
				} else {
					continue;
				}
			} else {
				/* horizontal line -- check for V at this cell */
				if (up && down) {
					glyph = f ? GLYPH_F_X : GLYPH_U_X;
				} else if (up) {
					glyph = f ? GLYPH_F_BT : GLYPH_U_BT;
				} else if (down) {
					glyph = f ? GLYPH_F_TT : GLYPH_U_TT;
				} else {
					continue;
				}
			}
			screen_put_glyph(screen, srows, scols, r, c,
			    glyph, f);
		}
	}
}

/* ---- public compositing API ---- */

void
tile_composite(struct tile *t)
{
	struct tile_node *root;
	struct vt_cell *screen;
	uint8_t *row_dirty;
	uint8_t *smap;
	int rows, cols, total;

	if (!t)
		return;

	root = tile_root(t);
	rows = tile_rows(t);
	cols = tile_cols(t);
	screen = (struct vt_cell *)tile_screen(t);
	row_dirty = tile_row_dirty_mut(t);
	if (!screen || !root)
		return;

	total = rows * cols;

	/* clear dirty flags -- blit_pane sets them per source VT row */
	memset(row_dirty, 0, (size_t)rows);

	/* clear screen */
	{
		int i;

		for (i = 0; i < total; i++)
			vt_cell_clear(&screen[i]);
	}

	/* blit pane contents, tracking which rows had VT changes */
	blit_leaves(screen, row_dirty, rows, cols, root);

	/* draw separators if there are any splits */
	if (root->type != TILE_LEAF) {
		smap = calloc((size_t)total, 1);
		if (smap) {
			mark_separators(smap, rows, cols, root,
			    tile_focused_id(t));
			draw_separators(screen, rows, cols, smap);
			fix_endpoints(screen, rows, cols, smap);
			free(smap);
		}
	}
}

void
tile_cursor(const struct tile *t, int *row, int *col, int *vis)
{
	const struct tile_node *leaves[64];
	int count = 0;
	int i;

	if (!t) {
		if (row) *row = 0;
		if (col) *col = 0;
		if (vis) *vis = 0;
		return;
	}

	/* find the focused leaf */
	{
		struct tile_node *root = tile_root(t);

		/* use the internal traversal helper -- declared in tile.c
		 * but we need it here. Instead, walk manually. */
		/* inline leaf collection */
		struct tile_node *stack[128];
		int sp = 0;

		if (root)
			stack[sp++] = root;
		while (sp > 0 && count < 64) {
			struct tile_node *n = stack[--sp];

			if (n->type == TILE_LEAF) {
				leaves[count++] = n;
			} else {
				/* push b first so a comes out first */
				if (n->b)
					stack[sp++] = n->b;
				if (n->a)
					stack[sp++] = n->a;
			}
		}
	}

	for (i = 0; i < count; i++) {
		if (leaves[i]->window_id == tile_focused_id(t)) {
			const struct tile_node *leaf = leaves[i];

			if (leaf->vt) {
				if (row)
					*row = leaf->y + leaf->vt->cursor_row;
				if (col)
					*col = leaf->x + leaf->vt->cursor_col;
				if (vis)
					*vis = 1;
			} else {
				if (row) *row = leaf->y;
				if (col) *col = leaf->x;
				if (vis) *vis = 0;
			}
			return;
		}
	}

	/* focused pane not found */
	if (row) *row = 0;
	if (col) *col = 0;
	if (vis) *vis = 0;
}
