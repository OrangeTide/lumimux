/* draw.c : backend-neutral drawing surface */

#include "draw.h"
#include "draw_driver.h"

#include "utf8.h"
#include "rune_width.h"
#include "tkbd.h"

#include <stdlib.h>
#include <string.h>

struct draw {
	const struct draw_driver	*drv;
	void				*ctx;
	int				rows, cols;
	struct vt_cell			*cells;		/* rows*cols row-major */
	uint8_t				*row_dirty;	/* one byte per row */
	int				cur_row, cur_col;
	int				cur_vis;
	int				cur_shape;

	/* input event queue (GetMessage/PeekMessage model) */
	struct tkbd_seq			*evq;
	size_t				evcap, evlen, evpos;
};

static struct vt_cell *
cell_at(struct draw *d, int r, int c)
{
	return &d->cells[(size_t)r * d->cols + c];
}

static void
free_grid(struct draw *d)
{
	free(d->cells);
	free(d->row_dirty);
	d->cells = NULL;
	d->row_dirty = NULL;
	d->rows = d->cols = 0;
}

/* Allocate the grid at rows x cols, cleared to default cells and all dirty. */
static int
alloc_grid(struct draw *d, int rows, int cols)
{
	struct vt_cell *cells;
	uint8_t *dirty;
	size_t n = (size_t)rows * cols;
	size_t i;

	cells = malloc(n * sizeof(*cells));
	dirty = malloc((size_t)rows);
	if (!cells || !dirty) {
		free(cells);
		free(dirty);
		return -1;
	}
	for (i = 0; i < n; i++)
		vt_cell_clear(&cells[i]);
	memset(dirty, 1, (size_t)rows);
	d->cells = cells;
	d->row_dirty = dirty;
	d->rows = rows;
	d->cols = cols;
	return 0;
}

struct draw *
draw_new(const struct draw_driver *drv, void *ctx)
{
	struct draw *d;
	int rows = 0, cols = 0;

	if (!drv)
		return NULL;
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->drv = drv;
	d->ctx = ctx;
	if (!drv->size || drv->size(ctx, &rows, &cols) < 0 ||
	    rows <= 0 || cols <= 0) {
		rows = 24;
		cols = 80;
	}
	if (alloc_grid(d, rows, cols) < 0) {
		free(d);
		return NULL;
	}
	d->cur_vis = 1;
	d->cur_shape = DRAW_CURSOR_DEFAULT;
	return d;
}

void
draw_free(struct draw *d)
{
	if (!d)
		return;
	free_grid(d);
	free(d->evq);
	if (d->drv->free)
		d->drv->free(d->ctx);
	free(d);
}

/* Re-query the driver's size and reshape the grid to match. */
static void
draw_refresh_size(struct draw *d)
{
	int rows = 0, cols = 0;

	if (d->drv->size && d->drv->size(d->ctx, &rows, &cols) == 0 &&
	    rows > 0 && cols > 0)
		draw_resize(d, rows, cols);
}

void
draw_begin(struct draw *d)
{
	if (d->drv->begin)
		d->drv->begin(d->ctx);
	/* entering (or resuming) may find the terminal a different size */
	draw_refresh_size(d);
}

void
draw_end(struct draw *d)
{
	if (d->drv->end)
		d->drv->end(d->ctx);
}

void
draw_size(struct draw *d, int *rows, int *cols)
{
	if (rows)
		*rows = d->rows;
	if (cols)
		*cols = d->cols;
}

void
draw_resize(struct draw *d, int rows, int cols)
{
	if (rows <= 0 || cols <= 0)
		return;
	if (rows == d->rows && cols == d->cols)
		return;
	free_grid(d);
	if (alloc_grid(d, rows, cols) < 0)
		return;
	if (d->cur_row >= rows)
		d->cur_row = rows - 1;
	if (d->cur_col >= cols)
		d->cur_col = cols - 1;
}

void
draw_clear(struct draw *d)
{
	size_t n = (size_t)d->rows * d->cols;
	size_t i;

	for (i = 0; i < n; i++)
		vt_cell_clear(&d->cells[i]);
	memset(d->row_dirty, 1, (size_t)d->rows);
}

/*
 * Blank a cell that has just been orphaned by an overwrite: when a write lands
 * on one half of a wide (width 2 plus width 0) pair, the other half is stale
 * and must return to a plain space so no fragment is drawn.
 */
static void
split_guard(struct draw *d, int r, int c)
{
	struct vt_cell *cur = cell_at(d, r, c);

	if (cur->width == 2 && c + 1 < d->cols)
		vt_cell_clear(cell_at(d, r, c + 1));
	if (cur->width == 0 && c > 0) {
		struct vt_cell *lead = cell_at(d, r, c - 1);

		if (lead->width == 2)
			vt_cell_clear(lead);
	}
}

/* Place one glyph of display width w (1 or 2) at (r, c). */
static void
put_glyph(struct draw *d, int r, int c, uint32_t cp, struct vt_color fg,
    struct vt_color bg, uint16_t attrs, int w)
{
	struct vt_cell *cell;

	if (r < 0 || r >= d->rows || c < 0 || c >= d->cols)
		return;
	if (w == 2 && c + 1 >= d->cols) {
		/* wide glyph would straddle the right edge: draw a blank */
		cp = ' ';
		w = 1;
	}
	split_guard(d, r, c);
	cell = cell_at(d, r, c);
	cell->codepoint = cp;
	cell->fg = fg;
	cell->bg = bg;
	cell->attrs = attrs;
	cell->width = (uint8_t)w;
	if (w == 2) {
		struct vt_cell *cont;

		split_guard(d, r, c + 1);
		cont = cell_at(d, r, c + 1);
		cont->codepoint = DRAW_CELL_CONT;
		cont->fg = fg;
		cont->bg = bg;
		cont->attrs = attrs;
		cont->width = 0;
	}
	d->row_dirty[r] = 1;
}

void
draw_cell(struct draw *d, int r, int c, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	int w = rune_width(cp);

	if (w != 2)
		w = 1;			/* controls and combining occupy one */
	put_glyph(d, r, c, cp, fg, bg, attrs, w);
}

int
draw_text(struct draw *d, int r, int c, const char *utf8,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	const unsigned char *p = (const unsigned char *)utf8;
	size_t rem;
	int col = c;

	if (!utf8 || r < 0 || r >= d->rows)
		return c;
	rem = strlen(utf8);
	while (rem > 0 && col < d->cols) {
		uint32_t rune;
		int n = utf8_decode(&rune, p, rem);
		int w;

		if (n <= 0)
			n = 1;
		if (rune < 0x20 || rune == 0x7f) {
			put_glyph(d, r, col, ' ', fg, bg, attrs, 1);
			col += 1;
			p += n;
			rem -= (size_t)n;
			continue;
		}
		w = rune_width(rune);
		if (w == 0) {
			/* zero-width (combining): skip, no advance */
			p += n;
			rem -= (size_t)n;
			continue;
		}
		if (w < 0)
			w = 1;
		if (col + w > d->cols)
			break;		/* wide rune will not fit */
		put_glyph(d, r, col, rune, fg, bg, attrs, w);
		col += w;
		p += n;
		rem -= (size_t)n;
	}
	return col;
}

void
draw_fill(struct draw *d, int r, int c, int n, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	int i;

	if (r < 0 || r >= d->rows)
		return;
	for (i = 0; i < n; i++) {
		int col = c + i;

		if (col < 0)
			continue;
		if (col >= d->cols)
			break;
		put_glyph(d, r, col, cp, fg, bg, attrs, 1);
	}
}

void
draw_cursor(struct draw *d, int r, int c)
{
	if (r < 0)
		r = 0;
	else if (r >= d->rows)
		r = d->rows - 1;
	if (c < 0)
		c = 0;
	else if (c >= d->cols)
		c = d->cols - 1;
	d->cur_row = r;
	d->cur_col = c;
}

void
draw_cursor_vis(struct draw *d, int on)
{
	d->cur_vis = on ? 1 : 0;
}

void
draw_cursor_shape(struct draw *d, enum draw_cursor_shape shape)
{
	d->cur_shape = shape;
}

void
draw_present(struct draw *d)
{
	struct draw_frame f;

	f.cells = d->cells;
	f.rows = d->rows;
	f.cols = d->cols;
	f.row_dirty = d->row_dirty;
	f.cur_row = d->cur_row;
	f.cur_col = d->cur_col;
	f.cur_vis = d->cur_vis;
	f.cur_shape = d->cur_shape;
	if (d->drv->present)
		d->drv->present(d->ctx, &f);
	memset(d->row_dirty, 0, (size_t)d->rows);
}

int
draw_poll(struct draw *d, int timeout_ms,
    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u)
{
	if (!d->drv->poll)
		return 0;
	return d->drv->poll(d->ctx, timeout_ms, on_event, u);
}

/* Append one event to the queue. Silently drops on allocation failure. */
static void
evq_push(void *u, const struct tkbd_seq *seq)
{
	struct draw *d = u;

	if (d->evlen == d->evcap) {
		size_t ncap = d->evcap ? d->evcap * 2 : 64;
		struct tkbd_seq *p = realloc(d->evq, ncap * sizeof(*p));

		if (!p)
			return;
		d->evq = p;
		d->evcap = ncap;
	}
	d->evq[d->evlen++] = *seq;
}

/* Remove the head into out; reset the queue to empty once drained. */
static int
evq_pop(struct draw *d, struct tkbd_seq *out)
{
	if (d->evpos >= d->evlen)
		return 0;
	*out = d->evq[d->evpos++];
	if (d->evpos >= d->evlen)
		d->evpos = d->evlen = 0;
	return 1;
}

int
draw_next_event(struct draw *d, int timeout_ms, struct tkbd_seq *out)
{
	int pr;

	if (evq_pop(d, out))
		return 1;
	if (!d->drv->poll)
		return 0;
	pr = d->drv->poll(d->ctx, timeout_ms, evq_push, d);
	if (pr < 0)
		return -1;
	return evq_pop(d, out) ? 1 : 0;
}

int
draw_peek_event(struct draw *d, struct tkbd_seq *out)
{
	int pr;

	if (d->evpos < d->evlen) {
		*out = d->evq[d->evpos];
		return 1;
	}
	if (!d->drv->poll)
		return 0;
	pr = d->drv->poll(d->ctx, 0, evq_push, d);
	if (pr < 0)
		return -1;
	if (d->evpos < d->evlen) {
		*out = d->evq[d->evpos];
		return 1;
	}
	return 0;
}

/* Poll cadence for draw_wait: bounds how long a pending resize or suspend
 * waits behind an idle poll, and lets a lone Esc flush. */
#define DRAW_TICK_MS 100

int
draw_wait(struct draw *d, struct draw_event *ev)
{
	for (;;) {
		int sig = d->drv->signals ? d->drv->signals(d->ctx) : 0;

		if (sig & DRAW_SIG_SUSPEND) {
			draw_end(d);
			if (d->drv->suspend)
				d->drv->suspend(d->ctx);
			draw_begin(d);		/* re-enters and re-syncs size */
			ev->type = DRAW_EVENT_RESUME;
			return ev->type;
		}
		if (sig & DRAW_SIG_RESIZE) {
			draw_refresh_size(d);
			ev->type = DRAW_EVENT_RESIZE;
			return ev->type;
		}
		if (evq_pop(d, &ev->key)) {
			ev->type = DRAW_EVENT_KEY;
			return ev->type;
		}
		if (!d->drv->poll) {
			ev->type = DRAW_EVENT_EOF;
			return ev->type;
		}
		if (d->drv->poll(d->ctx, DRAW_TICK_MS, evq_push, d) < 0) {
			ev->type = DRAW_EVENT_EOF;
			return ev->type;
		}
		/* loop: re-check signals and the refilled queue */
	}
}

void
draw_set_clipboard(struct draw *d, const char *utf8, size_t n)
{
	if (d->drv->set_clipboard)
		d->drv->set_clipboard(d->ctx, utf8, n);
}

void
draw_set_title(struct draw *d, const char *utf8)
{
	if (d->drv->set_title)
		d->drv->set_title(d->ctx, utf8);
}

void
draw_bell(struct draw *d)
{
	if (d->drv->bell)
		d->drv->bell(d->ctx);
}
