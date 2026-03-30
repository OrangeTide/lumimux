/* vt_buf.h : terminal grid buffer and scrollback */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef VT_BUF_H
#define VT_BUF_H

#include "vt_cell.h"

/* per-row metadata */
#define VT_ROW_WRAPPED	(1u << 0)
#define VT_ROW_DIRTY	(1u << 1)

struct vt_row {
	struct vt_cell	*cells;
	unsigned	flags;
};

struct vt_buf;

struct vt_buf *vt_buf_new(int rows, int cols, int scrollback);
void vt_buf_free(struct vt_buf *buf);
int vt_buf_resize(struct vt_buf *buf, int rows, int cols);

int vt_buf_rows(const struct vt_buf *buf);
int vt_buf_cols(const struct vt_buf *buf);

/* access visible grid (0 = top of screen) */
struct vt_row *vt_buf_row(struct vt_buf *buf, int row);
struct vt_cell *vt_buf_cell(struct vt_buf *buf, int row, int col);

/* scrollback access (-1 = most recent scrollback line, etc.) */
int vt_buf_scrollback_lines(const struct vt_buf *buf);
struct vt_row *vt_buf_scrollback_row(struct vt_buf *buf, int offset);

/* scroll visible region: positive = scroll up (new blank at bottom) */
void vt_buf_scroll(struct vt_buf *buf, int top, int bottom, int count);

/* clear a range of rows */
void vt_buf_clear_rows(struct vt_buf *buf, int from, int to);

/* mark all rows dirty */
void vt_buf_dirty_all(struct vt_buf *buf);

/* copy rows from the scrollback+visible history into dst.
 * src_offset is the line index into the virtual concatenation of
 * [scrollback(0..sb_lines-1)][visible(0..vis_rows-1)].
 * count rows are copied starting at dst row dst_row.
 * lines are clipped to the destination width. */
void vt_buf_copy_scrollback(struct vt_buf *dst, const struct vt_buf *src,
    int dst_row, int src_offset, int count);

#endif /* VT_BUF_H */
