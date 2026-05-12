/* render.c : differential screen renderer */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "render.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "tio_write.h"
#include "txl.h"
#include "utf8.h"
#include "xmalloc.h"

#include <stdlib.h>
#include <string.h>

struct render {
	int		rows;
	int		cols;
	struct vt_cell	*shadow;	/* rows * cols cells */
	struct txl	*txl;		/* terminal capabilities (not owned) */

	/* tracked output cursor position */
	int		cur_row;
	int		cur_col;

	/* tracked SGR state on the real terminal */
	uint16_t	cur_attrs;
	struct vt_color	cur_fg;
	struct vt_color	cur_bg;

	int		has_bce;	/* terminal supports BCE */
};

static struct vt_cell *
shadow_cell(struct render *r, int row, int col)
{
	return &r->shadow[row * r->cols + col];
}

static void
shadow_clear(struct render *r)
{
	int i, n;

	n = r->rows * r->cols;
	for (i = 0; i < n; i++)
		vt_cell_clear(&r->shadow[i]);
}

/* ---- escape sequence output helpers ---- */

/* mode 2026 synchronized output */
#define SYNC_BEGIN	"\033[?2026h"
#define SYNC_END	"\033[?2026l"

static int
emit_str(int fd, const char *s, size_t len)
{
	return tio_write(fd, s, len);
}

static int
emit_cstr(int fd, const char *s)
{
	return tio_write(fd, s, strlen(s));
}

/* emit a decimal number into buf, return length */
static int
fmt_int(char *buf, int n)
{
	int len = 0;
	char tmp[12];
	int i;

	if (n < 0)
		n = 0;
	if (n == 0) {
		buf[0] = '0';
		return 1;
	}
	while (n > 0) {
		tmp[len++] = '0' + (n % 10);
		n /= 10;
	}
	for (i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	return len;
}

static int
emit_cup(struct render *r, int fd, int row, int col)
{
	char buf[24];
	int len;

	if (r->txl) {
		len = txl_cup(r->txl, buf, sizeof(buf), row, col);
		if (len > 0)
			return emit_str(fd, buf, (size_t)len);
	}

	/* ANSI fallback */
	len = 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	len += fmt_int(buf + len, row + 1);
	buf[len++] = ';';
	len += fmt_int(buf + len, col + 1);
	buf[len++] = 'H';
	return emit_str(fd, buf, (size_t)len);
}

static int
emit_cap(struct render *r, int fd, int cap, const char *fallback)
{
	const char *s;

	if (r->txl) {
		s = txl_str(r->txl, cap);
		if (s)
			return emit_cstr(fd, s);
	}
	return emit_cstr(fd, fallback);
}

/* cursor visibility is managed by the attach event loop, not the
 * renderer.  these are no-ops so render functions don't emit stray
 * civis/cnorm sequences that conflict with the event loop policy. */

static int
emit_clear_screen(struct render *r, int fd)
{
	return emit_cap(r, fd, TXL_CLEAR, "\033[H\033[2J");
}

static int
emit_reset_sgr(struct render *r, int fd)
{
	return emit_cap(r, fd, TXL_SGR0, "\033[0m");
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

/* emit the minimal SGR sequence to transition from current to target */
static int
emit_sgr(struct render *r, int fd, uint16_t attrs,
    const struct vt_color *fg, const struct vt_color *bg)
{
	char buf[80];
	int len = 0;
	int need_sep = 0;

	if (attrs == r->cur_attrs &&
	    color_eq(fg, &r->cur_fg) &&
	    color_eq(bg, &r->cur_bg))
		return 0;

	buf[len++] = '\033';
	buf[len++] = '[';

	/* if attrs were removed, must reset first */
	if ((r->cur_attrs & ~attrs) != 0 ||
	    (r->cur_fg.type != VT_COLOR_DEFAULT && fg->type == VT_COLOR_DEFAULT) ||
	    (r->cur_bg.type != VT_COLOR_DEFAULT && bg->type == VT_COLOR_DEFAULT)) {
		buf[len++] = '0';
		need_sep = 1;
		r->cur_attrs = 0;
		r->cur_fg.type = VT_COLOR_DEFAULT;
		r->cur_bg.type = VT_COLOR_DEFAULT;
	}

#define SEP() do { if (need_sep) buf[len++] = ';'; need_sep = 1; } while (0)
#define ATTR(flag, code) \
	do { \
		if ((attrs & (flag)) && !(r->cur_attrs & (flag))) { \
			SEP(); \
			len += fmt_int(buf + len, (code)); \
		} \
	} while (0)

	ATTR(VT_ATTR_BOLD, 1);
	ATTR(VT_ATTR_DIM, 2);
	ATTR(VT_ATTR_ITALIC, 3);
	ATTR(VT_ATTR_UNDERLINE, 4);
	ATTR(VT_ATTR_BLINK, 5);
	ATTR(VT_ATTR_REVERSE, 7);
	ATTR(VT_ATTR_HIDDEN, 8);
	ATTR(VT_ATTR_STRIKE, 9);

#undef ATTR

	/* foreground */
	if (!color_eq(fg, &r->cur_fg)) {
		switch (fg->type) {
		case VT_COLOR_DEFAULT:
			SEP();
			buf[len++] = '3';
			buf[len++] = '9';
			break;
		case VT_COLOR_INDEXED:
			if (fg->index < 8) {
				SEP();
				len += fmt_int(buf + len, 30 + fg->index);
			} else if (fg->index < 16) {
				SEP();
				len += fmt_int(buf + len, 90 + fg->index - 8);
			} else {
				SEP();
				buf[len++] = '3';
				buf[len++] = '8';
				buf[len++] = ';';
				buf[len++] = '5';
				buf[len++] = ';';
				len += fmt_int(buf + len, fg->index);
			}
			break;
		case VT_COLOR_RGB:
			SEP();
			buf[len++] = '3';
			buf[len++] = '8';
			buf[len++] = ';';
			buf[len++] = '2';
			buf[len++] = ';';
			len += fmt_int(buf + len, fg->rgb.r);
			buf[len++] = ';';
			len += fmt_int(buf + len, fg->rgb.g);
			buf[len++] = ';';
			len += fmt_int(buf + len, fg->rgb.b);
			break;
		}
	}

	/* background */
	if (!color_eq(bg, &r->cur_bg)) {
		switch (bg->type) {
		case VT_COLOR_DEFAULT:
			SEP();
			buf[len++] = '4';
			buf[len++] = '9';
			break;
		case VT_COLOR_INDEXED:
			if (bg->index < 8) {
				SEP();
				len += fmt_int(buf + len, 40 + bg->index);
			} else if (bg->index < 16) {
				SEP();
				len += fmt_int(buf + len, 100 + bg->index - 8);
			} else {
				SEP();
				buf[len++] = '4';
				buf[len++] = '8';
				buf[len++] = ';';
				buf[len++] = '5';
				buf[len++] = ';';
				len += fmt_int(buf + len, bg->index);
			}
			break;
		case VT_COLOR_RGB:
			SEP();
			buf[len++] = '4';
			buf[len++] = '8';
			buf[len++] = ';';
			buf[len++] = '2';
			buf[len++] = ';';
			len += fmt_int(buf + len, bg->rgb.r);
			buf[len++] = ';';
			len += fmt_int(buf + len, bg->rgb.g);
			buf[len++] = ';';
			len += fmt_int(buf + len, bg->rgb.b);
			break;
		}
	}

#undef SEP

	buf[len++] = 'm';

	r->cur_attrs = attrs;
	r->cur_fg = *fg;
	r->cur_bg = *bg;

	return emit_str(fd, buf, (size_t)len);
}

static int
emit_cell(struct render *r, int fd, const struct vt_cell *c)
{
	unsigned char ubuf[4];
	int ulen;

	/* skip continuation cells (width == 0, part of wide char) */
	if (c->width == 0)
		return 0;

	{
		uint16_t a = c->attrs & ~VT_ATTR_PREDICTED;

		/* dim predicted (unconfirmed) cells for visual feedback */
		if (c->attrs & VT_ATTR_PREDICTED)
			a |= VT_ATTR_DIM;

		if (emit_sgr(r, fd, a, &c->fg, &c->bg) < 0)
			return -1;
	}

	ulen = utf8_encode(ubuf, c->codepoint);
	if (ulen <= 0) {
		/* fallback: emit space */
		ubuf[0] = ' ';
		ulen = 1;
	}

	if (emit_str(fd, (const char *)ubuf, (size_t)ulen) < 0)
		return -1;

	r->cur_col += c->width;
	return 0;
}

static int
cell_eq(const struct vt_cell *a, const struct vt_cell *b)
{
	return a->codepoint == b->codepoint &&
	    (a->attrs & ~VT_ATTR_PREDICTED) == (b->attrs & ~VT_ATTR_PREDICTED) &&
	    a->width == b->width &&
	    color_eq(&a->fg, &b->fg) &&
	    color_eq(&a->bg, &b->bg);
}

static int
emit_el(int fd)
{
	return emit_cstr(fd, "\033[K");
}

/*
 * Find the start of a trailing run of space cells with uniform
 * background color suitable for EL (erase line) optimization.
 * Returns the starting column, or cols if no useful run exists.
 * The run must have non-default bg and be at least 3 cells wide.
 */
static int
find_trail_el(const struct vt_cell *row_cells, int cols,
    struct vt_color *out_bg)
{
	int k;
	struct vt_color bg;
	int found = 0;

	for (k = cols - 1; k >= 0; k--) {
		const struct vt_cell *c = &row_cells[k];

		if ((c->codepoint != ' ' && c->codepoint != 0) ||
		    c->attrs != 0)
			break;

		if (!found) {
			bg = c->bg;
			found = 1;
		} else if (!color_eq(&c->bg, &bg)) {
			break;
		}
	}

	k++;

	if (!found || bg.type == VT_COLOR_DEFAULT || cols - k < 3)
		return cols;

	*out_bg = bg;
	return k;
}

/* update shadow cells to match EL result: space, no attrs, default fg */
static void
shadow_set_el(struct render *r, int row, int col_start, int col_end,
    const struct vt_color *bg)
{
	int col;

	for (col = col_start; col < col_end; col++) {
		struct vt_cell *s = shadow_cell(r, row, col);

		s->codepoint = ' ';
		s->width = 1;
		s->attrs = 0;
		s->fg.type = VT_COLOR_DEFAULT;
		s->bg = *bg;
	}
}

/*
 * Does this cell look the same as a cleared terminal cell?
 * After ED (erase display), every position is a default-attribute
 * space.  We can skip emitting cells that match this.
 */
static int
cell_is_blank(const struct vt_cell *c)
{
	return (c->codepoint == ' ' || c->codepoint == 0) &&
	    c->attrs == 0 &&
	    c->fg.type == VT_COLOR_DEFAULT &&
	    c->bg.type == VT_COLOR_DEFAULT;
}

/* ---- public API ---- */

struct render *
render_new(int rows, int cols, struct txl *txl)
{
	struct render *r;

	r = xcalloc(1, sizeof(*r));
	r->rows = rows;
	r->cols = cols;
	r->txl = txl;
	r->shadow = xmalloc((size_t)(rows * cols) * sizeof(r->shadow[0]));
	shadow_clear(r);
	r->has_bce = txl ? txl_has_bce(txl) : 1;
	return r;
}

void
render_free(struct render *r)
{
	if (!r)
		return;
	free(r->shadow);
	free(r);
}

int
render_resize(struct render *r, int rows, int cols)
{
	free(r->shadow);
	r->rows = rows;
	r->cols = cols;
	r->shadow = xmalloc((size_t)(rows * cols) * sizeof(r->shadow[0]));
	shadow_clear(r);
	return 0;
}

int
render_full(struct render *r, int fd, struct vt_state *st)
{
	int row, col;
	int rows, cols;

	rows = r->rows;
	cols = r->cols;

	emit_cstr(fd, SYNC_BEGIN);
	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;
	emit_clear_screen(r, fd);

	/* ED cleared the terminal -- shadow matches cleared state */
	shadow_clear(r);
	r->cur_row = 0;
	r->cur_col = 0;

	for (row = 0; row < rows; row++) {
		struct vt_row *vr = vt_buf_row(st->buf, row);

		if (!vr)
			continue;

		for (col = 0; col < cols; col++) {
			struct vt_cell *c = &vr->cells[col];
			struct vt_cell *s = shadow_cell(r, row, col);

			if (cell_is_blank(c))
				continue;

			if (r->cur_row != row || r->cur_col != col) {
				emit_cup(r, fd, row, col);
				r->cur_row = row;
				r->cur_col = col;
			}
			emit_cell(r, fd, c);
			*s = *c;
		}

		vr->flags &= ~VT_ROW_DIRTY;
	}

	/* position cursor */
	emit_cup(r, fd, st->cursor_row, st->cursor_col);
	r->cur_row = st->cursor_row;
	r->cur_col = st->cursor_col;

	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;

	emit_cstr(fd, SYNC_END);
	return tio_flush(fd);
}

int
render_diff(struct render *r, int fd, struct vt_state *st)
{
	int row, col;
	int rows, cols;
	int any_change = 0;

	rows = r->rows;
	cols = r->cols;

	emit_cstr(fd, SYNC_BEGIN);

	for (row = 0; row < rows; row++) {
		struct vt_row *vr = vt_buf_row(st->buf, row);

		if (!vr)
			continue;

		/* skip rows that aren't dirty */
		if (!(vr->flags & VT_ROW_DIRTY))
			continue;

		for (col = 0; col < cols; col++) {
			struct vt_cell *c = &vr->cells[col];
			struct vt_cell *s = shadow_cell(r, row, col);

			if (cell_eq(c, s))
				continue;

			/* move cursor if needed */
			if (r->cur_row != row || r->cur_col != col) {
				emit_cup(r, fd, row, col);
				r->cur_row = row;
				r->cur_col = col;
			}

			emit_cell(r, fd, c);
			*s = *c;
			any_change = 1;
		}

		vr->flags &= ~VT_ROW_DIRTY;
	}

	/* always reposition cursor (app may have moved it) */
	if (any_change || r->cur_row != st->cursor_row ||
	    r->cur_col != st->cursor_col) {
		emit_cup(r, fd, st->cursor_row, st->cursor_col);
		r->cur_row = st->cursor_row;
		r->cur_col = st->cursor_col;
	}

	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;

	emit_cstr(fd, SYNC_END);
	return tio_flush(fd);
}

/* ---- flat cell array rendering (for compositor output) ---- */

int
render_cells_full(struct render *r, int fd, const struct vt_cell *cells,
    int rows, int cols, int cursor_row, int cursor_col, int cursor_vis)
{
	int row, col;
	int rrows, rcols;

	rrows = rows < r->rows ? rows : r->rows;
	rcols = cols < r->cols ? cols : r->cols;

	emit_cstr(fd, SYNC_BEGIN);
	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;
	emit_clear_screen(r, fd);

	/* ED already cleared the terminal to default-attribute spaces.
	 * Set the shadow to match so we can skip emitting blank cells. */
	shadow_clear(r);
	r->cur_row = 0;
	r->cur_col = 0;

	for (row = 0; row < rrows; row++) {
		const struct vt_cell *rowp = &cells[row * cols];
		struct vt_color trail_bg;
		int trail_start;
		int emit_end;

		trail_start = r->has_bce ?
		    find_trail_el(rowp, rcols, &trail_bg) : rcols;
		emit_end = trail_start < rcols ? trail_start : rcols;

		for (col = 0; col < emit_end; col++) {
			const struct vt_cell *c = &rowp[col];
			struct vt_cell *s = shadow_cell(r, row, col);

			/* ED already put a default space here */
			if (cell_is_blank(c))
				continue;

			if (r->cur_row != row || r->cur_col != col) {
				emit_cup(r, fd, row, col);
				r->cur_row = row;
				r->cur_col = col;
			}
			emit_cell(r, fd, c);
			*s = *c;
		}

		if (trail_start < rcols) {
			static const struct vt_color dfg = { VT_COLOR_DEFAULT, };

			emit_sgr(r, fd, 0, &dfg, &trail_bg);
			if (r->cur_row != row || r->cur_col != trail_start) {
				emit_cup(r, fd, row, trail_start);
				r->cur_row = row;
			}
			emit_el(fd);
			shadow_set_el(r, row, trail_start, rcols, &trail_bg);
			r->cur_col = trail_start;
		}
	}

	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;

	emit_cstr(fd, SYNC_END);
	return tio_flush(fd);
}

/*
 * Overdraw threshold: if a gap of unchanged cells between two changed
 * cells is this many columns or fewer, overdraw (re-emit the unchanged
 * cells) rather than emitting a CUP sequence.  A CUP is typically 6-8
 * bytes; a default space is 1 byte (SGR already matches).  Overdraws
 * with non-default SGR cost more, so we use a conservative threshold.
 */
#define OVERDRAW_MAX	6

int
render_cells_diff(struct render *r, int fd, const struct vt_cell *cells,
    int rows, int cols, int cursor_row, int cursor_col, int cursor_vis,
    const uint8_t *row_dirty)
{
	int row, col;
	int rrows, rcols;
	int any_change = 0;
	int opened = 0;

	rrows = rows < r->rows ? rows : r->rows;
	rcols = cols < r->cols ? cols : r->cols;

	for (row = 0; row < rrows; row++) {
		int first_change, last_change;
		const struct vt_cell *rowp = &cells[row * cols];
		struct vt_color trail_bg;
		int trail_start;
		int use_el = 0;
		int scan_end;

		/* skip rows the compositor says are clean */
		if (row_dirty && !row_dirty[row])
			continue;

		/* check for trailing blank run usable with EL */
		trail_start = r->has_bce ?
		    find_trail_el(rowp, rcols, &trail_bg) : rcols;

		if (trail_start < rcols) {
			int k;

			for (k = trail_start; k < rcols; k++) {
				struct vt_cell *s = shadow_cell(r, row, k);

				if (s->codepoint != ' ' || s->attrs != 0 ||
				    s->fg.type != VT_COLOR_DEFAULT ||
				    !color_eq(&s->bg, &trail_bg)) {
					use_el = 1;
					break;
				}
			}
		}

		scan_end = use_el ? trail_start : rcols;

		/* find range of changed cells before trailing run */
		first_change = -1;
		last_change = -1;
		for (col = 0; col < scan_end; col++) {
			const struct vt_cell *c = &rowp[col];
			struct vt_cell *s = shadow_cell(r, row, col);

			if (!cell_eq(c, s)) {
				if (first_change < 0)
					first_change = col;
				last_change = col;
			}
		}

		if (first_change < 0 && !use_el)
			continue; /* row is dirty but no actual changes */

		if (!opened) {
			emit_cstr(fd, SYNC_BEGIN);
			opened = 1;
		}

		if (first_change >= 0) {
			/* position cursor at first change */
			if (r->cur_row != row ||
			    r->cur_col != first_change) {
				emit_cup(r, fd, row, first_change);
				r->cur_row = row;
				r->cur_col = first_change;
			}

			/* emit cells from first_change to last_change,
			 * overdriving small gaps of unchanged cells */
			for (col = first_change; col <= last_change; col++) {
				const struct vt_cell *c = &rowp[col];
				struct vt_cell *s =
				    shadow_cell(r, row, col);

				if (cell_eq(c, s)) {
					int gap_end;

					for (gap_end = col + 1;
					    gap_end <= last_change;
					    gap_end++) {
						if (!cell_eq(
						    &rowp[gap_end],
						    shadow_cell(r, row,
						    gap_end)))
							break;
					}

					if (gap_end <= last_change &&
					    gap_end - col <= OVERDRAW_MAX) {
						emit_cell(r, fd, c);
						continue;
					}

					col = gap_end - 1;
					continue;
				}

				if (r->cur_row != row ||
				    r->cur_col != col) {
					emit_cup(r, fd, row, col);
					r->cur_row = row;
					r->cur_col = col;
				}

				emit_cell(r, fd, c);
				*s = *c;
				any_change = 1;
			}
		}

		if (use_el) {
			static const struct vt_color dfg =
			    { VT_COLOR_DEFAULT, };

			emit_sgr(r, fd, 0, &dfg, &trail_bg);
			if (r->cur_row != row ||
			    r->cur_col != trail_start) {
				emit_cup(r, fd, row, trail_start);
				r->cur_row = row;
			}
			emit_el(fd);
			shadow_set_el(r, row, trail_start, rcols,
			    &trail_bg);
			r->cur_col = trail_start;
			any_change = 1;
		}
	}

	if (!opened)
		return 0;

	emit_reset_sgr(r, fd);
	r->cur_attrs = 0;
	r->cur_fg.type = VT_COLOR_DEFAULT;
	r->cur_bg.type = VT_COLOR_DEFAULT;
	emit_cstr(fd, SYNC_END);
	return tio_flush(fd);
}

void
render_move_cursor(struct render *r, int fd, int row, int col)
{
	if (r->cur_row == row && r->cur_col == col)
		return;
	emit_cup(r, fd, row, col);
	r->cur_row = row;
	r->cur_col = col;
}
