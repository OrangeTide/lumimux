/* tui_draw.c : draw-surface backend for the libtui pad stack */

#include "tui_draw.h"
#include "draw.h"

#include <stdlib.h>

struct tui_draw {
	struct tui_backend	be;	/* must stay first: ctx aliases the struct */
	struct draw		*d;
};

/* ---- backend callbacks ---- */

static void
draw_cell_cb(void *ctx, int row, int col, uint32_t cp,
    const struct vt_color *fg, const struct vt_color *bg,
    uint16_t attrs)
{
	struct tui_draw *t = ctx;

	draw_cell(t->d, row, col, cp, *fg, *bg, attrs);
}

static void
draw_flush_cb(void *ctx)
{
	struct tui_draw *t = ctx;

	draw_present(t->d);
}

/* ---- public API ---- */

struct tui_backend *
tui_draw_new(struct draw *d)
{
	struct tui_draw *t;

	t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;

	t->be.cell = draw_cell_cb;
	t->be.flush = draw_flush_cb;
	t->d = d;

	return &t->be;
}

void
tui_draw_free(struct tui_backend *be)
{
	if (be)
		free(be);	/* tui_draw starts with tui_backend */
}

void *
tui_draw_ctx(struct tui_backend *be)
{
	return be;		/* ctx is the struct; tui_backend is its head */
}
