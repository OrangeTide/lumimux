/* tile.h : tiled split-pane compositor */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TILE_H
#define TILE_H

#include <stdint.h>

struct vt_cell;
struct vt_state;

enum tile_split {
	TILE_LEAF,
	TILE_SPLIT_H,		/* top/bottom */
	TILE_SPLIT_V,		/* left/right */
};

struct tile_node {
	enum tile_split type;

	/* computed by tile_layout() */
	int x, y, w, h;

	/* TILE_LEAF fields */
	uint32_t window_id;	/* 0 = empty pane */
	struct vt_state *vt;	/* not owned */

	/* TILE_SPLIT_H / TILE_SPLIT_V fields */
	int split_pos;		/* fraction as numerator/256 (128 = 50%) */
	struct tile_node *a;	/* top or left child */
	struct tile_node *b;	/* bottom or right child */
};

struct tile;

/* lifecycle */
struct tile *tile_new(int rows, int cols);
void tile_free(struct tile *t);
void tile_resize(struct tile *t, int rows, int cols);

/* pane operations -- pane_id is the window_id of a leaf */
int tile_split(struct tile *t, uint32_t pane_id, enum tile_split dir,
    uint32_t new_id, struct vt_state *new_vt);
int tile_close(struct tile *t, uint32_t pane_id);
int tile_set_window(struct tile *t, uint32_t pane_id,
    uint32_t window_id, struct vt_state *vt);

/* focus */
void tile_focus(struct tile *t, uint32_t pane_id);
uint32_t tile_focus_next(struct tile *t);
uint32_t tile_focus_prev(struct tile *t);
uint32_t tile_focus_dir(struct tile *t, int dr, int dc);
uint32_t tile_focused_id(const struct tile *t);

/* query */
int tile_pane_count(const struct tile *t);
int tile_pane_geometry(const struct tile *t, uint32_t pane_id,
    int *x, int *y, int *w, int *h);
struct tile_node *tile_root(const struct tile *t);

/* resize the split adjacent to the focused pane.
 * delta is in cells (positive = grow in that direction).
 * dr/dc: -1=up/left, +1=down/right -- identifies which edge to move.
 * returns 0 on success, -1 if no split in that direction. */
int tile_resize_pane(struct tile *t, uint32_t pane_id, int dr, int dc,
    int delta);

/* compositing */
void tile_composite(struct tile *t);
/* compare screen against previous frame and set row_dirty flags.
 * call after tile_composite and any post-composite modifications. */
void tile_update_dirty(struct tile *t);
const struct vt_cell *tile_screen(const struct tile *t);
struct vt_cell *tile_screen_mut(struct tile *t);
const uint8_t *tile_row_dirty(const struct tile *t);
uint8_t *tile_row_dirty_mut(struct tile *t);
int tile_rows(const struct tile *t);
int tile_cols(const struct tile *t);
void tile_cursor(const struct tile *t, int *row, int *col, int *vis);

/* replace the root with a pre-built tile_node tree.
 * the tile takes ownership of root (will free on tile_free/next import).
 * caller must set window_id and vt on each leaf before calling.
 * triggers relayout. returns 0 on success, -1 on error. */
int tile_import_tree(struct tile *t, struct tile_node *root);

/* iterate leaves for resize callbacks */
typedef void (*tile_pane_cb)(uint32_t window_id, int w, int h, void *arg);
void tile_each_pane(const struct tile *t, tile_pane_cb cb, void *arg);

#endif /* TILE_H */
