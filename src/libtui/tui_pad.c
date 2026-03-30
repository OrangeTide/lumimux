/* tui_pad.c : overlay pad with blend modes and compositing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_pad.h"
#include "utf8.h"
#include "rune_width.h"

#include <string.h>

/* ---- pad primitives ---- */

void
tui_pad_clear(struct tui_pad *p, int w, int h)
{
	int r;

	if (h > TUI_PAD_MAX_ROWS)
		h = TUI_PAD_MAX_ROWS;
	if (w > TUI_PAD_MAX_COLS)
		w = TUI_PAD_MAX_COLS;

	for (r = 0; r < h; r++)
		memset(p->cells[r], 0, (size_t)w * sizeof(struct tui_cell));

	p->w = w;
	p->h = h;
}

void
tui_pad_put(struct tui_pad *p, int row, int col,
    uint32_t ch, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend)
{
	struct tui_cell *c;

	if (row < 0 || row >= p->h || col < 0 || col >= p->w)
		return;
	c = &p->cells[row][col];
	c->codepoint = ch;
	c->fg = fg;
	c->bg = bg;
	c->attrs = attrs;
	c->blend = (uint8_t)blend;
}

int
tui_pad_puts(struct tui_pad *p, int row, int col,
    const char *s, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend)
{
	while (*s && col < p->w) {
		uint32_t cp;
		int n, w;

		n = utf8_decode(&cp, (const unsigned char *)s, strlen(s));
		if (n <= 0)
			break;
		w = rune_width(cp);
		if (w < 0)
			w = 1;
		if (w == 0) {
			s += n;
			continue;	/* skip combining marks */
		}
		tui_pad_put(p, row, col, cp, fg, bg, attrs, blend);
		if (w == 2 && col + 1 < p->w)
			tui_pad_put(p, row, col + 1, TUI_CODEPOINT_CONT,
			    fg, bg, attrs, blend);
		col += w;
		s += n;
	}
	return col;
}

void
tui_pad_fill(struct tui_pad *p, int row, int col, int n,
    uint32_t ch, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend)
{
	int i;

	for (i = 0; i < n && col + i < p->w; i++)
		tui_pad_put(p, row, col + i, ch, fg, bg, attrs, blend);
}

/* ---- stack operations ---- */

void
tui_stack_init(struct tui_stack *s)
{
	s->depth = 0;
}

struct tui_pad *
tui_stack_push(struct tui_stack *s)
{
	if (s->depth >= TUI_STACK_MAX)
		return NULL;
	return &s->layers[s->depth++];
}

void
tui_stack_pop(struct tui_stack *s)
{
	if (s->depth > 0)
		s->depth--;
}

struct tui_pad *
tui_stack_top(struct tui_stack *s)
{
	if (s->depth <= 0)
		return NULL;
	return &s->layers[s->depth - 1];
}

int
tui_stack_depth(const struct tui_stack *s)
{
	return s->depth;
}

/* ---- compositing ---- */

/* resolve a single screen cell through the stack + base buffer */
static void
resolve_cell(const struct tui_stack *s, struct vt_buf *base,
    int sr, int sc, uint32_t *out_cp, struct vt_color *out_fg,
    struct vt_color *out_bg, uint16_t *out_attrs)
{
	int i;
	const struct tui_cell *color_cell = NULL;

	/* walk layers top to bottom */
	for (i = s->depth - 1; i >= 0; i--) {
		const struct tui_pad *p = &s->layers[i];
		int lr, lc;

		lr = sr - p->screen_row;
		lc = sc - p->screen_col;
		if (lr < 0 || lr >= p->h || lc < 0 || lc >= p->w)
			continue;

		const struct tui_cell *c = &p->cells[lr][lc];

		switch (c->blend) {
		case TUI_TRANSPARENT:
			continue;
		case TUI_OPAQUE:
			*out_cp = c->codepoint;
			*out_fg = c->fg;
			*out_bg = c->bg;
			*out_attrs = c->attrs;
			return;
		case TUI_COLOR_BG:
			/* save this cell's bg, keep looking for char */
			color_cell = c;
			continue;
		case TUI_COLOR_FG_BG:
			color_cell = c;
			continue;
		}
	}

	/* fell through all layers -- use base buffer */
	{
		struct vt_cell *vc = vt_buf_cell(base, sr, sc);

		if (vc) {
			*out_cp = vc->codepoint;
			*out_fg = vc->fg;
			*out_bg = vc->bg;
			*out_attrs = vc->attrs;
		} else {
			*out_cp = ' ';
			out_fg->type = VT_COLOR_DEFAULT;
			out_bg->type = VT_COLOR_DEFAULT;
			*out_attrs = 0;
		}
	}

	/* apply color overlay if one was found */
	if (color_cell) {
		if (color_cell->blend == TUI_COLOR_BG) {
			*out_bg = color_cell->bg;
		} else { /* TUI_COLOR_FG_BG */
			*out_fg = color_cell->fg;
			*out_bg = color_cell->bg;
		}
	}
}

void
tui_stack_render(const struct tui_stack *s,
    struct vt_buf *base, const struct tui_backend *be, void *ctx)
{
	int i, r, c;
	int min_r, min_c, max_r, max_c;

	if (s->depth == 0)
		return;

	/* compute bounding box of all layers */
	min_r = 9999;
	min_c = 9999;
	max_r = 0;
	max_c = 0;

	for (i = 0; i < s->depth; i++) {
		const struct tui_pad *p = &s->layers[i];
		int pr2, pc2;

		if (p->screen_row < min_r)
			min_r = p->screen_row;
		if (p->screen_col < min_c)
			min_c = p->screen_col;
		pr2 = p->screen_row + p->h;
		pc2 = p->screen_col + p->w;
		if (pr2 > max_r)
			max_r = pr2;
		if (pc2 > max_c)
			max_c = pc2;
	}

	/* render each cell in the bounding box */
	for (r = min_r; r < max_r; r++) {
		for (c = min_c; c < max_c; c++) {
			uint32_t cp;
			struct vt_color fg, bg;
			uint16_t attrs;

			resolve_cell(s, base, r, c, &cp, &fg, &bg, &attrs);
			if (cp == TUI_CODEPOINT_CONT)
				continue;
			be->cell(ctx, r, c, cp, &fg, &bg, attrs);
		}
	}

	be->flush(ctx);
}

void
tui_stack_erase(const struct tui_stack *s,
    struct vt_buf *base, const struct tui_backend *be, void *ctx,
    int row, int col, int h, int w)
{
	int r, c;

	for (r = row; r < row + h; r++) {
		for (c = col; c < col + w; c++) {
			uint32_t cp;
			struct vt_color fg, bg;
			uint16_t attrs;

			if (s->depth > 0) {
				resolve_cell(s, base, r, c,
				    &cp, &fg, &bg, &attrs);
			} else {
				struct vt_cell *vc;

				vc = vt_buf_cell(base, r, c);
				if (vc) {
					cp = vc->codepoint;
					fg = vc->fg;
					bg = vc->bg;
					attrs = vc->attrs;
				} else {
					cp = ' ';
					fg.type = VT_COLOR_DEFAULT;
					bg.type = VT_COLOR_DEFAULT;
					attrs = 0;
				}
			}
			if (cp != TUI_CODEPOINT_CONT)
				be->cell(ctx, r, c, cp, &fg, &bg, attrs);
		}
	}

	be->flush(ctx);
}
