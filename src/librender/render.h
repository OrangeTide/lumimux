/* render.h : differential screen renderer */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

struct vt_state;
struct txl;

struct render;

/* Create a renderer. txl provides terminal capabilities (not owned,
 * must outlive the renderer). Pass NULL to use ANSI defaults. */
struct render *render_new(int rows, int cols, struct txl *txl);
void render_free(struct render *r);
int render_resize(struct render *r, int rows, int cols);

/* full redraw -- clears screen, redraws everything, updates shadow */
int render_full(struct render *r, int fd, struct vt_state *st);

/* differential update -- only emits changes since last render */
int render_diff(struct render *r, int fd, struct vt_state *st);

/* render from a flat cell array (compositor output).
 * cursor_row/col are screen coordinates; cursor_vis controls visibility. */
struct vt_cell;
int render_cells_full(struct render *r, int fd, const struct vt_cell *cells,
    int rows, int cols, int cursor_row, int cursor_col, int cursor_vis);
int render_cells_diff(struct render *r, int fd, const struct vt_cell *cells,
    int rows, int cols, int cursor_row, int cursor_col, int cursor_vis,
    const uint8_t *row_dirty);

#endif /* RENDER_H */
