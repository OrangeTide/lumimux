/* draw_driver.h : output-target seam for the draw surface */

#ifndef DRAW_DRIVER_H
#define DRAW_DRIVER_H

#include <stddef.h>
#include <stdint.h>

struct vt_cell;
struct tkbd_seq;

/* Bits returned by draw_driver.signals. */
#define DRAW_SIG_RESIZE		0x01	/* the output size changed */
#define DRAW_SIG_SUSPEND	0x02	/* a stop was requested (SIGTSTP) */

/*
 * A frame handed to the driver by draw_present. The surface owns the cells,
 * so present forwards the pointer without a copy. row_dirty is one byte per
 * row (nonzero means changed); NULL means treat every row as dirty.
 */
struct draw_frame {
	const struct vt_cell	*cells;		/* rows*cols, row-major */
	int			rows, cols;
	const uint8_t		*row_dirty;	/* one byte per row, or NULL */
	int			cur_row, cur_col;
	int			cur_vis;
	int			cur_shape;	/* enum draw_cursor_shape */
};

/*
 * The single seam a backend implements. The first six methods are required;
 * begin and end may run more than once (suspend and resume). The optional
 * methods may be NULL, and the surface treats a NULL slot as a no-op.
 */
struct draw_driver {
	void (*begin)(void *ctx);	/* raw mode + alt screen, or open window */
	void (*end)(void *ctx);		/* undo begin */
	int  (*size)(void *ctx, int *rows, int *cols);
	void (*present)(void *ctx, const struct draw_frame *f);
	int  (*poll)(void *ctx, int timeout_ms,
	    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u);
					/* events dispatched, or -1 on error/EOF */
	void (*free)(void *ctx);

	/* Optional. */
	void (*set_clipboard)(void *ctx, const char *utf8, size_t n);
	void (*set_title)(void *ctx, const char *utf8);
	void (*bell)(void *ctx);

	/* Optional lifecycle signals. A terminal driver reports a resize or a
	 * stop request here so the surface can deliver them as events, and
	 * performs the actual process stop in suspend. NULL when the backend
	 * has no such notion (the surface then never yields those events). */
	int  (*signals)(void *ctx);	/* pending DRAW_SIG_* bits, cleared on read */
	void (*suspend)(void *ctx);	/* stop now; returns once resumed */
};

#endif /* DRAW_DRIVER_H */
