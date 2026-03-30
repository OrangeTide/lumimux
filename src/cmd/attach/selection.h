/* selection.h : mouse text selection, copy, and paste */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SELECTION_H
#define SELECTION_H

#include "vt_cell.h"

#include <stddef.h>
#include <stdint.h>

/* start a new selection at screen coordinate (row, col).
 * win_id identifies the window (used for context, 0 for tiled mode). */
void sel_begin(uint32_t win_id, int row, int col);

/* extend the selection to (row, col) during drag. */
void sel_update(int row, int col);

/* finalize selection on mouse release.
 * extracts text and copies to clipboard via OSC 52. */
void sel_finish(const struct vt_cell *screen, int rows, int cols);

/* cancel or dismiss a visible selection. */
void sel_clear(void);

/* returns 1 if a selection is in progress or visible. */
int sel_active(void);

/* toggle VT_ATTR_REVERSE on selected cells in the composited screen buffer.
 * call after composite, before render. */
void sel_highlight(struct vt_cell *screen, int rows, int cols);

/* return pointer and length of the last copied text (for paste). */
const char *sel_copy_buf(void);
size_t sel_copy_len(void);

#endif /* SELECTION_H */
