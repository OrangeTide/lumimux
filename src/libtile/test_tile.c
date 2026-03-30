/* test_tile.c : tests for libtile */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tile.h"
#include "vt_cell.h"
#include "vt_state.h"
#include "vt_buf.h"

#include <stdio.h>
#include <string.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* ---- tests ---- */

static void
test_new_free(void)
{
	struct tile *t;

	TEST("new/free");
	t = tile_new(24, 80);
	ASSERT(t != NULL, "tile_new returned NULL");
	ASSERT(tile_rows(t) == 24, "wrong rows");
	ASSERT(tile_cols(t) == 80, "wrong cols");
	ASSERT(tile_pane_count(t) == 1, "count should be 1");
	ASSERT(tile_screen(t) != NULL, "screen should not be NULL");
	tile_free(t);
	PASS();
}

static void
test_root_leaf_geometry(void)
{
	struct tile *t;
	int x, y, w, h;

	TEST("root leaf geometry");
	t = tile_new(24, 80);

	/* root leaf has window_id 0 */
	ASSERT(tile_pane_geometry(t, 0, &x, &y, &w, &h) == 0,
	    "pane_geometry failed");
	ASSERT(x == 0, "x should be 0");
	ASSERT(y == 0, "y should be 0");
	ASSERT(w == 80, "w should be 80");
	ASSERT(h == 24, "h should be 24");
	tile_free(t);
	PASS();
}

static void
test_set_window(void)
{
	struct tile *t;

	TEST("set_window");
	t = tile_new(24, 80);
	ASSERT(tile_set_window(t, 0, 1, NULL) == 0, "set_window failed");
	ASSERT(tile_pane_geometry(t, 1, NULL, NULL, NULL, NULL) == 0,
	    "pane 1 not found after set_window");
	ASSERT(tile_pane_geometry(t, 0, NULL, NULL, NULL, NULL) == -1,
	    "pane 0 should not exist after set_window");
	tile_free(t);
	PASS();
}

static void
test_split_v(void)
{
	struct tile *t;
	int x, y, w, h;

	TEST("split vertical");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	ASSERT(tile_split(t, 1, TILE_SPLIT_V, 2, NULL) == 0,
	    "split failed");
	ASSERT(tile_pane_count(t) == 2, "count should be 2");

	/* left pane */
	ASSERT(tile_pane_geometry(t, 1, &x, &y, &w, &h) == 0,
	    "pane 1 geometry failed");
	ASSERT(x == 0, "pane 1 x should be 0");
	ASSERT(y == 0, "pane 1 y should be 0");
	ASSERT(h == 24, "pane 1 h should be 24");
	/* 80 * 128/256 = 40, so left gets 40, separator 1, right gets 39 */
	ASSERT(w == 40, "pane 1 w should be 40");

	/* right pane */
	ASSERT(tile_pane_geometry(t, 2, &x, &y, &w, &h) == 0,
	    "pane 2 geometry failed");
	ASSERT(x == 41, "pane 2 x should be 41");
	ASSERT(y == 0, "pane 2 y should be 0");
	ASSERT(w == 39, "pane 2 w should be 39");
	ASSERT(h == 24, "pane 2 h should be 24");

	tile_free(t);
	PASS();
}

static void
test_split_h(void)
{
	struct tile *t;
	int x, y, w, h;

	TEST("split horizontal");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	ASSERT(tile_split(t, 1, TILE_SPLIT_H, 2, NULL) == 0,
	    "split failed");
	ASSERT(tile_pane_count(t) == 2, "count should be 2");

	/* top pane */
	ASSERT(tile_pane_geometry(t, 1, &x, &y, &w, &h) == 0,
	    "pane 1 geometry failed");
	ASSERT(x == 0, "pane 1 x should be 0");
	ASSERT(y == 0, "pane 1 y should be 0");
	ASSERT(w == 80, "pane 1 w should be 80");
	/* 24 * 128/256 = 12, so top gets 12, separator 1, bottom gets 11 */
	ASSERT(h == 12, "pane 1 h should be 12");

	/* bottom pane */
	ASSERT(tile_pane_geometry(t, 2, &x, &y, &w, &h) == 0,
	    "pane 2 geometry failed");
	ASSERT(x == 0, "pane 2 x should be 0");
	ASSERT(y == 13, "pane 2 y should be 13");
	ASSERT(w == 80, "pane 2 w should be 80");
	ASSERT(h == 11, "pane 2 h should be 11");

	tile_free(t);
	PASS();
}

static void
test_close(void)
{
	struct tile *t;

	TEST("close pane");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	ASSERT(tile_pane_count(t) == 2, "count should be 2 after split");

	ASSERT(tile_close(t, 2) == 0, "close failed");
	ASSERT(tile_pane_count(t) == 1, "count should be 1 after close");

	/* remaining pane should fill the whole screen */
	{
		int x, y, w, h;

		tile_pane_geometry(t, 1, &x, &y, &w, &h);
		ASSERT(x == 0 && y == 0, "remaining pane origin wrong");
		ASSERT(w == 80 && h == 24, "remaining pane size wrong");
	}

	tile_free(t);
	PASS();
}

static void
test_close_last_fails(void)
{
	struct tile *t;

	TEST("close last pane fails");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	ASSERT(tile_close(t, 1) == -1, "should not close last pane");
	ASSERT(tile_pane_count(t) == 1, "count should still be 1");
	tile_free(t);
	PASS();
}

static void
test_focus_next_prev(void)
{
	struct tile *t;
	uint32_t id;

	TEST("focus next/prev");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_focus(t, 1);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_split(t, 2, TILE_SPLIT_H, 3, NULL);

	/* order: 1, 2, 3 (in-order traversal) */
	tile_focus(t, 1);
	ASSERT(tile_focused_id(t) == 1, "focused should be 1");

	id = tile_focus_next(t);
	ASSERT(id == 2, "next from 1 should be 2");

	id = tile_focus_next(t);
	ASSERT(id == 3, "next from 2 should be 3");

	id = tile_focus_next(t);
	ASSERT(id == 1, "next from 3 should wrap to 1");

	id = tile_focus_prev(t);
	ASSERT(id == 3, "prev from 1 should wrap to 3");

	id = tile_focus_prev(t);
	ASSERT(id == 2, "prev from 3 should be 2");

	tile_free(t);
	PASS();
}

static void
test_focus_close_updates(void)
{
	struct tile *t;

	TEST("focus updates on close");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_focus(t, 2);
	ASSERT(tile_focused_id(t) == 2, "focused should be 2");

	tile_close(t, 2);
	ASSERT(tile_focused_id(t) == 1,
	    "focus should move to sibling after close");

	tile_free(t);
	PASS();
}

static void
test_resize(void)
{
	struct tile *t;
	int x, y, w, h;

	TEST("resize redistributes panes");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);

	tile_resize(t, 24, 120);
	ASSERT(tile_rows(t) == 24, "rows should be 24");
	ASSERT(tile_cols(t) == 120, "cols should be 120");

	tile_pane_geometry(t, 1, &x, &y, &w, &h);
	/* 120 * 128/256 = 60 */
	ASSERT(w == 60, "pane 1 w should be 60 after resize");
	ASSERT(h == 24, "pane 1 h should be 24");

	tile_pane_geometry(t, 2, &x, &y, &w, &h);
	ASSERT(x == 61, "pane 2 x should be 61");
	ASSERT(w == 59, "pane 2 w should be 59 after resize");

	tile_free(t);
	PASS();
}

static void
test_split_too_small(void)
{
	struct tile *t;

	TEST("split too small fails");
	t = tile_new(2, 80);
	tile_set_window(t, 0, 1, NULL);
	ASSERT(tile_split(t, 1, TILE_SPLIT_H, 2, NULL) == -1,
	    "should fail with only 2 rows");

	tile_resize(t, 24, 2);
	ASSERT(tile_split(t, 1, TILE_SPLIT_V, 2, NULL) == -1,
	    "should fail with only 2 cols");

	tile_free(t);
	PASS();
}

static uint32_t each_seen_ids[8];
static int each_seen_count;

static void
each_pane_cb(uint32_t id, int w, int h, void *arg)
{
	(void)w;
	(void)h;
	(void)arg;
	if (each_seen_count < 8)
		each_seen_ids[each_seen_count++] = id;
}

static void
test_each_pane(void)
{
	struct tile *t;

	TEST("each_pane iteration");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_split(t, 2, TILE_SPLIT_H, 3, NULL);

	each_seen_count = 0;
	tile_each_pane(t, each_pane_cb, NULL);

	ASSERT(each_seen_count == 3, "should visit 3 panes");
	ASSERT(each_seen_ids[0] == 1, "first pane should be 1");
	ASSERT(each_seen_ids[1] == 2, "second pane should be 2");
	ASSERT(each_seen_ids[2] == 3, "third pane should be 3");

	tile_free(t);
	PASS();
}

static void
test_nested_split(void)
{
	struct tile *t;
	int x, y, w, h;

	TEST("nested split (3 panes)");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);

	/* split vertically: left=1, right=2 */
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	/* split right pane horizontally: top=2, bottom=3 */
	tile_split(t, 2, TILE_SPLIT_H, 3, NULL);

	ASSERT(tile_pane_count(t) == 3, "count should be 3");

	/* pane 1: left half, full height */
	tile_pane_geometry(t, 1, &x, &y, &w, &h);
	ASSERT(x == 0 && y == 0, "pane 1 at origin");
	ASSERT(w == 40, "pane 1 w=40");
	ASSERT(h == 24, "pane 1 h=24");

	/* pane 2: right-top */
	tile_pane_geometry(t, 2, &x, &y, &w, &h);
	ASSERT(x == 41, "pane 2 x=41");
	ASSERT(y == 0, "pane 2 y=0");
	/* right side: 39 wide, split h at 128/256 = 50% of 24 = 12 */

	/* pane 3: right-bottom */
	tile_pane_geometry(t, 3, &x, &y, &w, &h);
	ASSERT(x == 41, "pane 3 x=41");
	ASSERT(y == 13, "pane 3 y=13");

	tile_free(t);
	PASS();
}

static void
test_focus_dir(void)
{
	struct tile *t;
	uint32_t id;

	TEST("focus directional");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_focus(t, 1);

	/* move right should go to pane 2 */
	id = tile_focus_dir(t, 0, 1);
	ASSERT(id == 2, "right from 1 should be 2");

	/* move left should go back to pane 1 */
	id = tile_focus_dir(t, 0, -1);
	ASSERT(id == 1, "left from 2 should be 1");

	/* move down with no pane below should stay */
	tile_focus(t, 1);
	id = tile_focus_dir(t, 1, 0);
	ASSERT(id == 1, "down from 1 with no pane below should stay");

	tile_free(t);
	PASS();
}

static void
test_close_middle(void)
{
	struct tile *t;

	TEST("close middle pane in nested split");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_split(t, 2, TILE_SPLIT_H, 3, NULL);
	ASSERT(tile_pane_count(t) == 3, "should have 3 panes");

	/* close pane 2 (top-right) -- its sibling 3 should replace parent */
	ASSERT(tile_close(t, 2) == 0, "close pane 2 should succeed");
	ASSERT(tile_pane_count(t) == 2, "should have 2 panes");
	ASSERT(tile_pane_geometry(t, 3, NULL, NULL, NULL, NULL) == 0,
	    "pane 3 should still exist");
	ASSERT(tile_pane_geometry(t, 1, NULL, NULL, NULL, NULL) == 0,
	    "pane 1 should still exist");

	tile_free(t);
	PASS();
}

static void
test_composite_single_pane(void)
{
	struct tile *t;
	struct vt_state *vt;
	const struct vt_cell *screen;

	TEST("composite single pane");
	vt = vt_state_new(24, 80, 0);
	/* write 'A' at (0,0) */
	{
		struct vt_cell *c = vt_buf_cell(vt->buf, 0, 0);

		c->codepoint = 'A';
		c->width = 1;
	}

	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, vt);
	tile_focus(t, 1);

	tile_composite(t);
	screen = tile_screen(t);
	ASSERT(screen[0].codepoint == 'A', "cell (0,0) should be 'A'");

	tile_free(t);
	vt_state_free(vt);
	PASS();
}

static void
test_composite_separator(void)
{
	struct tile *t;
	struct vt_state *vt1, *vt2;
	const struct vt_cell *screen;
	int sep_col;

	TEST("composite draws separator");
	vt1 = vt_state_new(24, 40, 0);
	vt2 = vt_state_new(24, 39, 0);

	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, vt1);
	tile_split(t, 1, TILE_SPLIT_V, 2, vt2);
	tile_focus(t, 1);

	tile_composite(t);
	screen = tile_screen(t);

	/* separator should be at column 40 (between pane 1 and 2) */
	sep_col = 40;
	ASSERT(screen[0 * 80 + sep_col].codepoint != 0 &&
	    screen[0 * 80 + sep_col].codepoint != ' ',
	    "separator column should have a glyph");

	/* content area should be clear (no vt content written) */
	ASSERT(screen[0 * 80 + 0].codepoint == 0 ||
	    screen[0 * 80 + 0].codepoint == ' ',
	    "pane 1 (0,0) should be blank");

	tile_free(t);
	vt_state_free(vt1);
	vt_state_free(vt2);
	PASS();
}

static void
test_composite_cursor(void)
{
	struct tile *t;
	struct vt_state *vt1, *vt2;
	int crow, ccol, cvis;

	TEST("composite cursor position");
	vt1 = vt_state_new(24, 40, 0);
	vt2 = vt_state_new(24, 39, 0);

	/* place cursor at (5,10) in vt2 */
	vt2->cursor_row = 5;
	vt2->cursor_col = 10;

	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, vt1);
	tile_split(t, 1, TILE_SPLIT_V, 2, vt2);
	tile_focus(t, 2);

	tile_cursor(t, &crow, &ccol, &cvis);
	/* pane 2 starts at x=41, so cursor should be at col 41+10=51 */
	ASSERT(crow == 5, "cursor row should be 5");
	ASSERT(ccol == 51, "cursor col should be 51 (41+10)");
	ASSERT(cvis == 1, "cursor should be visible");

	tile_free(t);
	vt_state_free(vt1);
	vt_state_free(vt2);
	PASS();
}

static void
test_composite_intersection(void)
{
	struct tile *t;
	struct vt_state *vt1, *vt2, *vt3, *vt4;
	const struct vt_cell *screen;
	int sep_row, sep_col;

	TEST("composite intersection glyph");
	/* create a 2x2 grid of panes:
	 * split root vertically, then split each half horizontally */
	t = tile_new(25, 81);

	vt1 = vt_state_new(12, 40, 0);
	vt2 = vt_state_new(12, 40, 0);
	vt3 = vt_state_new(12, 40, 0);
	vt4 = vt_state_new(12, 40, 0);

	tile_set_window(t, 0, 1, vt1);
	tile_split(t, 1, TILE_SPLIT_V, 2, vt2);
	tile_split(t, 1, TILE_SPLIT_H, 3, vt3);
	tile_split(t, 2, TILE_SPLIT_H, 4, vt4);
	tile_focus(t, 1);

	tile_composite(t);
	screen = tile_screen(t);

	/* find the intersection point */
	{
		int x1, y1, w1, h1;
		int x2, y2, w2, h2;

		tile_pane_geometry(t, 1, &x1, &y1, &w1, &h1);
		tile_pane_geometry(t, 2, &x2, &y2, &w2, &h2);
		sep_col = x1 + w1; /* vertical separator */
		sep_row = y1 + h1; /* horizontal separator */
	}

	/* the intersection cell should be a cross glyph */
	{
		uint32_t cp = screen[sep_row * 81 + sep_col].codepoint;

		ASSERT(cp != 0 && cp != ' ',
		    "intersection should have a glyph");
	}

	tile_free(t);
	vt_state_free(vt1);
	vt_state_free(vt2);
	vt_state_free(vt3);
	vt_state_free(vt4);
	PASS();
}

static void
test_resize_pane(void)
{
	struct tile *t;
	int x, y, w, h;
	int orig_w1, orig_w2;

	TEST("resize pane adjusts split");
	t = tile_new(24, 80);
	tile_set_window(t, 0, 1, NULL);
	tile_split(t, 1, TILE_SPLIT_V, 2, NULL);
	tile_focus(t, 1);

	tile_pane_geometry(t, 1, &x, &y, &w, &h);
	orig_w1 = w;
	tile_pane_geometry(t, 2, &x, &y, &w, &h);
	orig_w2 = w;

	/* grow pane 1 rightward (move right edge right) */
	ASSERT(tile_resize_pane(t, 1, 0, 1, 5) == 0, "resize should succeed");

	tile_pane_geometry(t, 1, &x, &y, &w, &h);
	ASSERT(w > orig_w1, "pane 1 should be wider");

	tile_pane_geometry(t, 2, &x, &y, &w, &h);
	ASSERT(w < orig_w2, "pane 2 should be narrower");

	/* shrink pane 1 (move right edge left) */
	tile_pane_geometry(t, 1, NULL, NULL, &orig_w1, NULL);
	ASSERT(tile_resize_pane(t, 1, 0, 1, -5) == 0,
	    "shrink should succeed");

	tile_pane_geometry(t, 1, NULL, NULL, &w, NULL);
	ASSERT(w < orig_w1, "pane 1 should be narrower after shrink");

	/* no split in vertical direction for horizontal-only layout */
	ASSERT(tile_resize_pane(t, 1, 1, 0, 5) == -1,
	    "no h-split to resize should return -1");

	tile_free(t);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libtile tests:\n");

	test_new_free();
	test_root_leaf_geometry();
	test_set_window();
	test_split_v();
	test_split_h();
	test_close();
	test_close_last_fails();
	test_focus_next_prev();
	test_focus_close_updates();
	test_resize();
	test_split_too_small();
	test_nested_split();
	test_focus_dir();
	test_close_middle();
	test_resize_pane();
	test_composite_single_pane();
	test_composite_separator();
	test_composite_cursor();
	test_composite_intersection();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
