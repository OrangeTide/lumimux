/* tile.c : tiled split-pane tree operations and layout */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tile.h"
#include "vt_cell.h"
#include "xmalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct tile {
	struct tile_node *root;
	int rows, cols;
	struct vt_cell *screen;
	struct vt_cell *prev_screen;
	uint8_t *row_dirty;
	uint32_t focused_id;
	int pane_count;
};

/* ---- node helpers ---- */

static struct tile_node *
node_new_leaf(uint32_t window_id, struct vt_state *vt)
{
	struct tile_node *n;

	n = xcalloc(1, sizeof(*n));
	n->type = TILE_LEAF;
	n->window_id = window_id;
	n->vt = vt;
	return n;
}

static void
node_free(struct tile_node *n)
{
	if (!n)
		return;
	if (n->type != TILE_LEAF) {
		node_free(n->a);
		node_free(n->b);
	}
	free(n);
}

static int
node_leaf_count(const struct tile_node *n)
{
	if (!n)
		return 0;
	if (n->type == TILE_LEAF)
		return 1;
	return node_leaf_count(n->a) + node_leaf_count(n->b);
}

static struct tile_node *
node_find(struct tile_node *n, uint32_t window_id)
{
	struct tile_node *found;

	if (!n)
		return NULL;
	if (n->type == TILE_LEAF)
		return (n->window_id == window_id) ? n : NULL;
	found = node_find(n->a, window_id);
	if (found)
		return found;
	return node_find(n->b, window_id);
}

/* find the parent of the leaf containing window_id */
static struct tile_node *
node_find_leaf_parent(struct tile_node *n, uint32_t window_id)
{
	struct tile_node *found;

	if (!n || n->type == TILE_LEAF)
		return NULL;

	if (n->a->type == TILE_LEAF && n->a->window_id == window_id)
		return n;
	if (n->b->type == TILE_LEAF && n->b->window_id == window_id)
		return n;

	found = node_find_leaf_parent(n->a, window_id);
	if (found)
		return found;
	return node_find_leaf_parent(n->b, window_id);
}

/* find the parent of a given branch node */
static struct tile_node *
node_find_branch_parent(struct tile_node *n, struct tile_node *target)
{
	struct tile_node *found;

	if (!n || n->type == TILE_LEAF)
		return NULL;
	if (n->a == target || n->b == target)
		return n;

	found = node_find_branch_parent(n->a, target);
	if (found)
		return found;
	return node_find_branch_parent(n->b, target);
}

/* ---- layout ---- */

static void
tile_layout(struct tile_node *n, int x, int y, int w, int h)
{
	int mid;

	if (!n)
		return;

	n->x = x;
	n->y = y;
	n->w = w;
	n->h = h;

	if (n->type == TILE_LEAF)
		return;

	if (n->type == TILE_SPLIT_V) {
		mid = w * n->split_pos / 256;
		if (mid < 1)
			mid = 1;
		if (mid > w - 2)
			mid = w - 2;
		tile_layout(n->a, x, y, mid, h);
		tile_layout(n->b, x + mid + 1, y, w - mid - 1, h);
	} else { /* TILE_SPLIT_H */
		mid = h * n->split_pos / 256;
		if (mid < 1)
			mid = 1;
		if (mid > h - 2)
			mid = h - 2;
		tile_layout(n->a, x, y, w, mid);
		tile_layout(n->b, x, y + mid + 1, w, h - mid - 1);
	}
}

static void
relayout(struct tile *t)
{
	tile_layout(t->root, 0, 0, t->cols, t->rows);
}

/* ---- leaf traversal (in-order) ---- */

static void
leaves_collect(const struct tile_node *n, const struct tile_node **buf,
    int *count, int max)
{
	if (!n || *count >= max)
		return;
	if (n->type == TILE_LEAF) {
		buf[(*count)++] = n;
		return;
	}
	leaves_collect(n->a, buf, count, max);
	leaves_collect(n->b, buf, count, max);
}

#define MAX_PANES 64

/* ---- public API ---- */

struct tile *
tile_new(int rows, int cols)
{
	struct tile *t;

	t = xcalloc(1, sizeof(*t));
	t->rows = rows;
	t->cols = cols;
	t->root = node_new_leaf(0, NULL);
	t->pane_count = 1;
	t->screen = xcalloc((size_t)(rows * cols), sizeof(struct vt_cell));
	t->prev_screen = xcalloc((size_t)(rows * cols),
	    sizeof(struct vt_cell));
	t->row_dirty = xcalloc((size_t)rows, 1);
	relayout(t);
	return t;
}

void
tile_free(struct tile *t)
{
	if (!t)
		return;
	node_free(t->root);
	free(t->screen);
	free(t->prev_screen);
	free(t->row_dirty);
	free(t);
}

void
tile_resize(struct tile *t, int rows, int cols)
{
	if (!t)
		return;
	t->rows = rows;
	t->cols = cols;
	free(t->screen);
	free(t->prev_screen);
	free(t->row_dirty);
	t->screen = xcalloc((size_t)(rows * cols), sizeof(struct vt_cell));
	t->prev_screen = xcalloc((size_t)(rows * cols),
	    sizeof(struct vt_cell));
	t->row_dirty = xcalloc((size_t)rows, 1);
	relayout(t);
}

int
tile_import_tree(struct tile *t, struct tile_node *root)
{
	if (!t || !root)
		return -1;
	node_free(t->root);
	t->root = root;
	t->pane_count = node_leaf_count(root);
	relayout(t);
	return 0;
}

int
tile_split(struct tile *t, uint32_t pane_id, enum tile_split dir,
    uint32_t new_id, struct vt_state *new_vt)
{
	struct tile_node *target, *parent;
	struct tile_node *new_branch, *new_leaf;

	if (!t || (dir != TILE_SPLIT_H && dir != TILE_SPLIT_V))
		return -1;

	target = node_find(t->root, pane_id);
	if (!target || target->type != TILE_LEAF)
		return -1;

	/* minimum size check: need at least 3 cells in split direction
	 * (1 for each child + 1 for separator) */
	if (dir == TILE_SPLIT_H && target->h < 3)
		return -1;
	if (dir == TILE_SPLIT_V && target->w < 3)
		return -1;

	new_leaf = node_new_leaf(new_id, new_vt);

	/* create a branch node that replaces target in the tree */
	new_branch = xcalloc(1, sizeof(*new_branch));
	new_branch->type = dir;
	new_branch->split_pos = 128; /* 50% */
	new_branch->a = target;
	new_branch->b = new_leaf;

	/* splice into tree */
	parent = node_find_leaf_parent(t->root, pane_id);
	if (parent) {
		if (parent->a == target)
			parent->a = new_branch;
		else
			parent->b = new_branch;
	} else {
		/* splitting the root */
		t->root = new_branch;
	}

	t->pane_count++;
	relayout(t);
	return 0;
}

int
tile_close(struct tile *t, uint32_t pane_id)
{
	struct tile_node *parent, *leaf, *sibling;
	struct tile_node *grandparent;

	if (!t || t->pane_count <= 1)
		return -1;

	/* find the branch node whose child is the target leaf */
	parent = node_find_leaf_parent(t->root, pane_id);
	if (!parent)
		return -1;

	/* identify which child is the leaf and which is the sibling */
	if (parent->a->type == TILE_LEAF &&
	    parent->a->window_id == pane_id) {
		leaf = parent->a;
		sibling = parent->b;
	} else {
		leaf = parent->b;
		sibling = parent->a;
	}

	/* replace parent branch with sibling in grandparent */
	if (t->root == parent) {
		t->root = sibling;
	} else {
		grandparent = node_find_branch_parent(t->root, parent);
		if (!grandparent)
			return -1;
		if (grandparent->a == parent)
			grandparent->a = sibling;
		else
			grandparent->b = sibling;
	}

	/* update focus if the closed pane was focused */
	if (t->focused_id == pane_id) {
		if (sibling->type == TILE_LEAF) {
			t->focused_id = sibling->window_id;
		} else {
			const struct tile_node *leaves[MAX_PANES];
			int lcount = 0;

			leaves_collect(sibling, leaves, &lcount, MAX_PANES);
			if (lcount > 0)
				t->focused_id = leaves[0]->window_id;
		}
	}

	node_free(leaf);
	parent->a = NULL;
	parent->b = NULL;
	free(parent);
	t->pane_count--;
	relayout(t);
	return 0;
}

int
tile_set_window(struct tile *t, uint32_t pane_id,
    uint32_t window_id, struct vt_state *vt)
{
	struct tile_node *n;

	if (!t)
		return -1;
	n = node_find(t->root, pane_id);
	if (!n || n->type != TILE_LEAF)
		return -1;
	n->window_id = window_id;
	n->vt = vt;
	return 0;
}

/* ---- focus ---- */

void
tile_focus(struct tile *t, uint32_t pane_id)
{
	if (!t)
		return;
	if (node_find(t->root, pane_id))
		t->focused_id = pane_id;
}

uint32_t
tile_focus_next(struct tile *t)
{
	const struct tile_node *leaves[MAX_PANES];
	int count = 0;
	int i;

	if (!t)
		return 0;

	leaves_collect(t->root, leaves, &count, MAX_PANES);
	for (i = 0; i < count; i++) {
		if (leaves[i]->window_id == t->focused_id) {
			int next = (i + 1) % count;

			t->focused_id = leaves[next]->window_id;
			return t->focused_id;
		}
	}
	if (count > 0) {
		t->focused_id = leaves[0]->window_id;
		return t->focused_id;
	}
	return 0;
}

uint32_t
tile_focus_prev(struct tile *t)
{
	const struct tile_node *leaves[MAX_PANES];
	int count = 0;
	int i;

	if (!t)
		return 0;

	leaves_collect(t->root, leaves, &count, MAX_PANES);
	for (i = 0; i < count; i++) {
		if (leaves[i]->window_id == t->focused_id) {
			int prev = (i - 1 + count) % count;

			t->focused_id = leaves[prev]->window_id;
			return t->focused_id;
		}
	}
	if (count > 0) {
		t->focused_id = leaves[count - 1]->window_id;
		return t->focused_id;
	}
	return 0;
}

uint32_t
tile_focus_dir(struct tile *t, int dr, int dc)
{
	const struct tile_node *leaves[MAX_PANES];
	const struct tile_node *focused = NULL;
	int count = 0;
	int best = -1;
	int best_dist = INT_MAX;
	int fx, fy, i;

	if (!t)
		return 0;

	leaves_collect(t->root, leaves, &count, MAX_PANES);

	/* find focused pane center */
	for (i = 0; i < count; i++) {
		if (leaves[i]->window_id == t->focused_id) {
			focused = leaves[i];
			break;
		}
	}
	if (!focused)
		return t->focused_id;

	fx = focused->x + focused->w / 2;
	fy = focused->y + focused->h / 2;

	/* find nearest pane in the requested direction */
	for (i = 0; i < count; i++) {
		int cx, cy, dx, dy, dist;

		if (leaves[i] == focused)
			continue;

		cx = leaves[i]->x + leaves[i]->w / 2;
		cy = leaves[i]->y + leaves[i]->h / 2;
		dx = cx - fx;
		dy = cy - fy;

		/* check direction constraint */
		if (dr < 0 && dy >= 0)
			continue;
		if (dr > 0 && dy <= 0)
			continue;
		if (dc < 0 && dx >= 0)
			continue;
		if (dc > 0 && dx <= 0)
			continue;

		dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = i;
		}
	}

	if (best >= 0) {
		t->focused_id = leaves[best]->window_id;
		return t->focused_id;
	}
	return t->focused_id;
}

uint32_t
tile_focused_id(const struct tile *t)
{
	if (!t)
		return 0;
	return t->focused_id;
}

/* ---- query ---- */

int
tile_pane_count(const struct tile *t)
{
	if (!t)
		return 0;
	return t->pane_count;
}

int
tile_pane_geometry(const struct tile *t, uint32_t pane_id,
    int *x, int *y, int *w, int *h)
{
	struct tile_node *n;

	if (!t)
		return -1;
	n = node_find((struct tile_node *)t->root, pane_id);
	if (!n || n->type != TILE_LEAF)
		return -1;
	if (x) *x = n->x;
	if (y) *y = n->y;
	if (w) *w = n->w;
	if (h) *h = n->h;
	return 0;
}

struct tile_node *
tile_root(const struct tile *t)
{
	if (!t)
		return NULL;
	return t->root;
}

/* ---- pane resize ---- */

/* walk up from a leaf to find the nearest ancestor branch whose split
 * direction matches the requested axis and where the pane is on the
 * correct side of the split for the requested direction.
 *
 * dr/dc: -1 = up/left edge, +1 = down/right edge.
 * delta > 0 means move that edge outward (grow), < 0 means inward (shrink).
 *
 * For a vertical split (left|right):
 *   dc=-1 (left edge): pane must be child b (right side) -- moving the
 *         separator left grows the right pane.
 *   dc=+1 (right edge): pane must be child a (left side) -- moving the
 *         separator right grows the left pane.
 *
 * For a horizontal split (top/bottom):
 *   dr=-1 (top edge): pane must be child b (bottom side).
 *   dr=+1 (bottom edge): pane must be child a (top side).
 */
int
tile_resize_pane(struct tile *t, uint32_t pane_id, int dr, int dc,
    int delta)
{
	struct tile_node *n, *parent;
	int want_h, want_a;
	int new_pos, dim;

	if (!t || delta == 0)
		return -1;

	/* determine which split type and which child side we need */
	want_h = (dr != 0); /* horizontal split if moving top/bottom edge */
	/* for dc: -1 means left edge -> pane is child b (right side)
	 *         +1 means right edge -> pane is child a (left side)
	 * for dr: -1 means top edge -> pane is child b (bottom side)
	 *         +1 means bottom edge -> pane is child a (top side) */
	want_a = (dr > 0 || dc > 0);

	/* start from the leaf and walk up looking for a matching split */
	n = node_find(t->root, pane_id);
	if (!n)
		return -1;

	for (;;) {
		if (n == t->root) {
			parent = NULL;
		} else if (n->type == TILE_LEAF) {
			parent = node_find_leaf_parent(t->root, n->window_id);
		} else {
			parent = node_find_branch_parent(t->root, n);
		}
		if (!parent)
			return -1; /* no matching split found */

		if ((want_h && parent->type == TILE_SPLIT_H) ||
		    (!want_h && parent->type == TILE_SPLIT_V)) {
			/* check we're on the correct side */
			if ((want_a && parent->a == n) ||
			    (!want_a && parent->b == n)) {
				/* found the right split */
				break;
			}
		}
		n = parent;
	}

	/* adjust split_pos: delta is in cells, convert to fraction/256 */
	dim = want_h ? parent->h : parent->w;
	if (dim <= 0)
		return -1;

	/* positive delta with want_a means grow child a -> increase split_pos
	 * positive delta with !want_a means grow child b -> decrease split_pos */
	if (want_a)
		new_pos = parent->split_pos + (delta * 256 / dim);
	else
		new_pos = parent->split_pos - (delta * 256 / dim);

	/* clamp: minimum 1 cell for each child */
	if (new_pos < 256 / dim)
		new_pos = 256 / dim;
	if (new_pos > 256 - 256 / dim)
		new_pos = 256 - 256 / dim;
	/* hard clamp */
	if (new_pos < 1)
		new_pos = 1;
	if (new_pos > 254)
		new_pos = 254;

	parent->split_pos = new_pos;
	relayout(t);
	return 0;
}

/* ---- compositing stubs (implemented in tile_composite.c) ---- */

/* ---- iteration ---- */

static void
each_pane_walk(const struct tile_node *n, tile_pane_cb cb, void *arg)
{
	if (!n)
		return;
	if (n->type == TILE_LEAF) {
		if (n->window_id != 0)
			cb(n->window_id, n->w, n->h, arg);
		return;
	}
	each_pane_walk(n->a, cb, arg);
	each_pane_walk(n->b, cb, arg);
}

void
tile_each_pane(const struct tile *t, tile_pane_cb cb, void *arg)
{
	if (!t || !cb)
		return;
	each_pane_walk(t->root, cb, arg);
}

int
tile_rows(const struct tile *t)
{
	return t ? t->rows : 0;
}

int
tile_cols(const struct tile *t)
{
	return t ? t->cols : 0;
}

const struct vt_cell *
tile_screen(const struct tile *t)
{
	return t ? t->screen : NULL;
}

struct vt_cell *
tile_screen_mut(struct tile *t)
{
	return t ? t->screen : NULL;
}

const uint8_t *
tile_row_dirty(const struct tile *t)
{
	return t ? t->row_dirty : NULL;
}

uint8_t *
tile_row_dirty_mut(struct tile *t)
{
	return t ? t->row_dirty : NULL;
}

void
tile_update_dirty(struct tile *t)
{
	int r;
	size_t row_bytes;

	if (!t || !t->prev_screen)
		return;

	row_bytes = (size_t)t->cols * sizeof(struct vt_cell);
	for (r = 0; r < t->rows; r++) {
		size_t off = (size_t)r * (size_t)t->cols;

		if (memcmp(&t->screen[off], &t->prev_screen[off],
		    row_bytes) != 0)
			t->row_dirty[r] = 1;
	}
	memcpy(t->prev_screen, t->screen,
	    (size_t)(t->rows * t->cols) * sizeof(struct vt_cell));
}
