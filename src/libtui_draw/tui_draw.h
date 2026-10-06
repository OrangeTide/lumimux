/* tui_draw.h : draw-surface backend for the libtui pad stack */

#ifndef TUI_DRAW_H
#define TUI_DRAW_H

#include "tui_pad.h"

struct draw;

/*
 * A tui_backend that paints the pad stack into a libdraw surface instead of
 * emitting terminal escapes. cell writes into the surface grid with draw_cell,
 * and flush hands the frame to the driver with draw_present, so the SGR
 * emission and diffing live in the shared libdraw/librender path rather than in
 * a backend of their own. The surface must be full-screen sized: the pad stack
 * addresses absolute screen cells, and draw_present diffs the whole grid.
 */
struct tui_backend *tui_draw_new(struct draw *d);
void tui_draw_free(struct tui_backend *be);
void *tui_draw_ctx(struct tui_backend *be);

#endif /* TUI_DRAW_H */
