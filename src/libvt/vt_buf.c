/* vt_buf.c : terminal grid buffer and scrollback */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_buf.h"
#include "xmalloc.h"

#include <stdlib.h>
#include <string.h>

struct vt_buf {
	int		rows;		/* visible rows */
	int		cols;		/* visible columns */
	struct vt_row	**grid;		/* row pointers for visible area */
	struct vt_row	**ring;		/* scrollback ring buffer */
	int		ring_cap;	/* total ring capacity */
	int		ring_len;	/* current lines in ring */
	int		ring_head;	/* next write position */
};

static struct vt_row *
row_alloc(int cols)
{
	struct vt_row *r;
	int i;

	r = xmalloc(sizeof(*r));
	r->cells = xmalloc((size_t)cols * sizeof(r->cells[0]));
	r->flags = 0;
	for (i = 0; i < cols; i++)
		vt_cell_clear(&r->cells[i]);
	return r;
}

static void
row_free(struct vt_row *r)
{
	if (!r)
		return;
	free(r->cells);
	free(r);
}

static void
row_resize(struct vt_row *r, int old_cols, int new_cols)
{
	int i;

	r->cells = xrealloc(r->cells, (size_t)new_cols * sizeof(r->cells[0]));
	for (i = old_cols; i < new_cols; i++)
		vt_cell_clear(&r->cells[i]);
	r->flags |= VT_ROW_DIRTY;
}

static void
row_clear(struct vt_row *r, int cols)
{
	int i;

	for (i = 0; i < cols; i++)
		vt_cell_clear(&r->cells[i]);
	r->flags = VT_ROW_DIRTY;
}

struct vt_buf *
vt_buf_new(int rows, int cols, int scrollback)
{
	struct vt_buf *buf;
	int i;

	buf = xcalloc(1, sizeof(*buf));
	buf->rows = rows;
	buf->cols = cols;

	buf->grid = xmalloc((size_t)rows * sizeof(buf->grid[0]));
	for (i = 0; i < rows; i++)
		buf->grid[i] = row_alloc(cols);

	buf->ring_cap = scrollback;
	if (scrollback > 0)
		buf->ring = xcalloc((size_t)scrollback, sizeof(buf->ring[0]));
	buf->ring_len = 0;
	buf->ring_head = 0;

	return buf;
}

void
vt_buf_free(struct vt_buf *buf)
{
	int i;

	if (!buf)
		return;

	for (i = 0; i < buf->rows; i++)
		row_free(buf->grid[i]);
	free(buf->grid);

	for (i = 0; i < buf->ring_cap; i++)
		row_free(buf->ring[i]);
	free(buf->ring);

	free(buf);
}

int
vt_buf_resize(struct vt_buf *buf, int rows, int cols)
{
	int i;
	int old_rows = buf->rows;
	int old_cols = buf->cols;

	/* resize existing rows' cell arrays */
	if (cols != old_cols) {
		for (i = 0; i < old_rows && i < rows; i++)
			row_resize(buf->grid[i], old_cols, cols);

		/* resize scrollback rows too */
		for (i = 0; i < buf->ring_cap; i++) {
			if (buf->ring[i])
				row_resize(buf->ring[i], old_cols, cols);
		}
	}

	if (rows > old_rows) {
		/* grow: add new rows */
		buf->grid = xrealloc(buf->grid,
		    (size_t)rows * sizeof(buf->grid[0]));
		for (i = old_rows; i < rows; i++)
			buf->grid[i] = row_alloc(cols);
	} else if (rows < old_rows) {
		/* shrink: free excess rows */
		for (i = rows; i < old_rows; i++)
			row_free(buf->grid[i]);
		buf->grid = xrealloc(buf->grid,
		    (size_t)rows * sizeof(buf->grid[0]));
	}

	buf->rows = rows;
	buf->cols = cols;
	return 0;
}

int
vt_buf_rows(const struct vt_buf *buf)
{
	return buf->rows;
}

int
vt_buf_cols(const struct vt_buf *buf)
{
	return buf->cols;
}

struct vt_row *
vt_buf_row(struct vt_buf *buf, int row)
{
	if (row < 0 || row >= buf->rows)
		return NULL;
	return buf->grid[row];
}

struct vt_cell *
vt_buf_cell(struct vt_buf *buf, int row, int col)
{
	if (row < 0 || row >= buf->rows)
		return NULL;
	if (col < 0 || col >= buf->cols)
		return NULL;
	return &buf->grid[row]->cells[col];
}

int
vt_buf_scrollback_lines(const struct vt_buf *buf)
{
	return buf->ring_len;
}

struct vt_row *
vt_buf_scrollback_row(struct vt_buf *buf, int offset)
{
	int idx;

	if (offset >= 0 || -offset > buf->ring_len)
		return NULL;

	/* offset is negative: -1 = most recent */
	idx = (buf->ring_head + offset + buf->ring_cap) % buf->ring_cap;
	return buf->ring[idx];
}

static void
ring_push(struct vt_buf *buf, struct vt_row *r)
{
	if (buf->ring_cap == 0) {
		row_free(r);
		return;
	}

	row_free(buf->ring[buf->ring_head]);
	buf->ring[buf->ring_head] = r;
	buf->ring_head = (buf->ring_head + 1) % buf->ring_cap;
	if (buf->ring_len < buf->ring_cap)
		buf->ring_len++;
}

void
vt_buf_scroll(struct vt_buf *buf, int top, int bottom, int count)
{
	int i;

	if (top < 0)
		top = 0;
	if (bottom > buf->rows)
		bottom = buf->rows;
	if (top >= bottom || count == 0)
		return;

	if (count > 0) {
		/* scroll up: lines at top go to scrollback */
		if (count > bottom - top)
			count = bottom - top;

		/* push departing rows into scrollback */
		for (i = top; i < top + count; i++) {
			if (top == 0)
				ring_push(buf, buf->grid[i]);
			else
				row_free(buf->grid[i]);
		}

		/* shift remaining rows up */
		for (i = top; i < bottom - count; i++)
			buf->grid[i] = buf->grid[i + count];

		/* fill bottom with blank rows */
		for (i = bottom - count; i < bottom; i++)
			buf->grid[i] = row_alloc(buf->cols);
	} else {
		/* scroll down: lines at bottom are lost */
		count = -count;
		if (count > bottom - top)
			count = bottom - top;

		/* free departing rows at bottom */
		for (i = bottom - count; i < bottom; i++)
			row_free(buf->grid[i]);

		/* shift remaining rows down */
		for (i = bottom - 1; i >= top + count; i--)
			buf->grid[i] = buf->grid[i - count];

		/* fill top with blank rows */
		for (i = top; i < top + count; i++)
			buf->grid[i] = row_alloc(buf->cols);
	}

	/* mark all rows in scroll region dirty for renderer */
	for (i = top; i < bottom; i++)
		buf->grid[i]->flags |= VT_ROW_DIRTY;
}

void
vt_buf_clear_rows(struct vt_buf *buf, int from, int to)
{
	int i;

	if (from < 0)
		from = 0;
	if (to > buf->rows)
		to = buf->rows;
	for (i = from; i < to; i++)
		row_clear(buf->grid[i], buf->cols);
}

void
vt_buf_dirty_all(struct vt_buf *buf)
{
	int i;

	for (i = 0; i < buf->rows; i++)
		buf->grid[i]->flags |= VT_ROW_DIRTY;
}

/* resolve a line index in the virtual [scrollback][visible] history */
static const struct vt_row *
history_row(const struct vt_buf *buf, int line)
{
	int sb = buf->ring_len;

	if (line < 0)
		return NULL;

	if (line < sb) {
		/* scrollback region: oldest = 0, newest = sb-1 */
		int off = -(sb - line); /* negative offset for ring */
		int idx;

		idx = (buf->ring_head + off + buf->ring_cap) %
		    buf->ring_cap;
		return buf->ring[idx];
	}

	line -= sb;
	if (line < buf->rows)
		return buf->grid[line];

	return NULL;
}

void
vt_buf_copy_scrollback(struct vt_buf *dst, const struct vt_buf *src,
    int dst_row, int src_offset, int count)
{
	int i, j;
	int dst_cols = dst->cols;

	for (i = 0; i < count; i++) {
		const struct vt_row *sr;
		struct vt_row *dr;
		int src_cols;

		dr = vt_buf_row(dst, dst_row + i);
		if (!dr)
			break;

		sr = history_row(src, src_offset + i);

		/* clear destination row first */
		for (j = 0; j < dst_cols; j++)
			vt_cell_clear(&dr->cells[j]);

		if (!sr || !sr->cells)
			goto dirty;

		/* copy cells, clipping to destination width */
		src_cols = src->cols;
		if (src_cols > dst_cols)
			src_cols = dst_cols;
		for (j = 0; j < src_cols; j++)
			dr->cells[j] = sr->cells[j];
dirty:
		dr->flags |= VT_ROW_DIRTY;
	}
}
