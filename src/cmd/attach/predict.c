/* predict.c : speculative local echo for low-latency typing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "predict.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "xmalloc.h"

#include <stdlib.h>

#define PREDICT_RING_SIZE 128

/* one predicted character and its grid position */
struct pred_entry {
	uint32_t	cp;
	int		row;
	int		col;
	int		width;
};

struct predict {
	struct pred_entry ring[PREDICT_RING_SIZE];
	int		head;	/* next write slot */
	int		tail;	/* oldest unconfirmed */
	int		echo_off; /* PTY echo disabled (password entry) */
};

struct predict *
predict_new(void)
{
	return xcalloc(1, sizeof(struct predict));
}

void
predict_free(struct predict *pr)
{
	free(pr);
}

static int
ring_count(const struct predict *pr)
{
	int n = pr->head - pr->tail;

	if (n < 0)
		n += PREDICT_RING_SIZE;
	return n;
}

int
predict_pending(const struct predict *pr)
{
	return ring_count(pr);
}

/* check whether the terminal is in a state suitable for prediction */
static int
can_predict(const struct predict *pr, const struct vt_state *vt)
{
	/* skip if PTY echo is off (password prompts) */
	if (pr->echo_off)
		return 0;
	/* skip if alt screen (full-screen apps like vim) */
	if (vt->modes & VT_MODE_ALTSCREEN)
		return 0;
	/* skip if cursor is hidden (program is drawing) */
	if (!(vt->modes & VT_MODE_CURSOR_VIS))
		return 0;
	/* skip if insert mode (shifts cells -- too complex to predict) */
	if (vt->modes & VT_MODE_INSERT)
		return 0;
	return 1;
}

void
predict_set_echo(struct predict *pr, int on)
{
	pr->echo_off = !on;
}

/* compute the column where the next prediction should be placed,
 * accounting for the width of all pending (unconfirmed) predictions. */
static int
predict_col(const struct predict *pr, const struct vt_state *vt)
{
	int col = vt->cursor_col;
	int i;

	for (i = pr->tail; i != pr->head; i = (i + 1) % PREDICT_RING_SIZE)
		col += pr->ring[i].width;
	return col;
}

int
predict_key(struct predict *pr, struct vt_state *vt,
    uint32_t cp, int width)
{
	struct pred_entry *ent;
	struct vt_cell *c;
	struct vt_row *r;
	int cols, col;

	if (!can_predict(pr, vt))
		return 0;

	/* only predict printable characters (not control codes) */
	if (cp < 0x20 || cp == 0x7F)
		return 0;

	/* ring full -- drop oldest silently */
	if (ring_count(pr) >= PREDICT_RING_SIZE - 1)
		return 0;

	cols = vt_buf_cols(vt->buf);
	col = predict_col(pr, vt);

	/* would this wrap?  skip prediction at line boundaries to
	 * avoid getting out of sync with scroll. */
	if (col + width > cols)
		return 0;

	/* record the prediction */
	ent = &pr->ring[pr->head];
	ent->cp = cp;
	ent->row = vt->cursor_row;
	ent->col = col;
	ent->width = width;
	pr->head = (pr->head + 1) % PREDICT_RING_SIZE;

	/* write into the grid with VT_ATTR_PREDICTED flag */
	c = vt_buf_cell(vt->buf, vt->cursor_row, col);
	if (!c)
		return 0;

	c->codepoint = cp;
	c->attrs = vt->attrs | VT_ATTR_PREDICTED;
	c->fg = vt->fg;
	c->bg = vt->bg;
	c->width = (uint8_t)width;

	/* blank trailing cell for wide characters */
	if (width == 2 && col + 1 < cols) {
		struct vt_cell *c2;

		c2 = vt_buf_cell(vt->buf, vt->cursor_row, col + 1);
		if (c2) {
			vt_cell_clear(c2);
			c2->width = 0;
		}
	}

	/* mark row dirty so renderer picks it up */
	r = vt_buf_row(vt->buf, vt->cursor_row);
	if (r)
		r->flags |= VT_ROW_DIRTY;

	/* do NOT advance vt->cursor_col -- the real VT parser will
	 * do that when the server echo arrives, preventing double echo */

	return 1;
}

/* roll back all pending predictions from the grid.
 * removes VT_ATTR_PREDICTED but preserves cell content -- the VT
 * parser will overwrite with real data in the same render cycle. */
static void
rollback_all(struct predict *pr, struct vt_state *vt)
{
	while (pr->tail != pr->head) {
		struct pred_entry *ent = &pr->ring[pr->tail];
		struct vt_cell *c;

		c = vt_buf_cell(vt->buf, ent->row, ent->col);
		if (c && (c->attrs & VT_ATTR_PREDICTED))
			c->attrs &= ~VT_ATTR_PREDICTED;
		pr->tail = (pr->tail + 1) % PREDICT_RING_SIZE;
	}
}

void
predict_confirm(struct predict *pr, struct vt_state *vt,
    const char *data, size_t len)
{
	size_t i;

	if (pr->tail == pr->head)
		return;

	/* walk incoming bytes looking for printable characters that
	 * match predictions.  any mismatch triggers a full rollback
	 * so the real server output takes precedence. */
	for (i = 0; i < len && pr->tail != pr->head; i++) {
		unsigned char c = (unsigned char)data[i];
		struct pred_entry *ent;
		struct vt_cell *cell;

		/* control characters (BS, CR, LF, ESC, etc.) are part of
		 * normal terminal echo and don't correspond to predicted
		 * printable input -- skip without rolling back.  the VT
		 * parser handles cursor movement in the same render cycle. */
		if (c < 0x20 || c == 0x7F)
			continue;

		/* UTF-8 continuation bytes don't start new characters */
		if ((c & 0xC0) == 0x80)
			continue;

		ent = &pr->ring[pr->tail];

		/* for ASCII, simple comparison */
		if (c < 0x80) {
			if (ent->cp == (uint32_t)c) {
				/* confirmed -- clear the predicted flag */
				cell = vt_buf_cell(vt->buf,
				    ent->row, ent->col);
				if (cell)
					cell->attrs &= ~VT_ATTR_PREDICTED;
				pr->tail = (pr->tail + 1) %
				    PREDICT_RING_SIZE;
				continue;
			}
			/* mismatch -- roll back everything */
			rollback_all(pr, vt);
			return;
		}

		/* UTF-8 lead byte -- decode the codepoint.
		 * simplified: just compare the first byte of what
		 * would encode ent->cp.  a full rollback on any
		 * ambiguity is safe. */
		rollback_all(pr, vt);
		return;
	}
}

void
predict_reset(struct predict *pr, struct vt_state *vt)
{
	rollback_all(pr, vt);
}
