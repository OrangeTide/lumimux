/* test_wm.c : tests for libwm */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "wm.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "tui_theme.h"

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
	struct wm *wm;

	TEST("new/free");
	wm = wm_new(24, 80);
	ASSERT(wm != NULL, "wm_new returned NULL");
	ASSERT(wm_rows(wm) == 24, "wrong rows");
	ASSERT(wm_cols(wm) == 80, "wrong cols");
	ASSERT(wm_count(wm) == 0, "count should be 0");
	wm_free(wm);
	PASS();
}

static void
test_add_remove(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;

	TEST("add/remove windows");
	wm = wm_new(24, 80);
	vt = vt_state_new(10, 40, 0);
	ASSERT(wm && vt, "alloc failed");

	ASSERT(wm_add(wm, 1, vt, 2, 2, 40, 10) == 0, "add failed");
	ASSERT(wm_count(wm) == 1, "count should be 1");

	win = wm_find(wm, 1);
	ASSERT(win != NULL, "find returned NULL");
	ASSERT(win->id == 1, "wrong id");
	ASSERT(win->x == 2 && win->y == 2, "wrong position");
	ASSERT(win->w == 40 && win->h == 10, "wrong size");

	/* duplicate add should fail */
	ASSERT(wm_add(wm, 1, vt, 0, 0, 10, 10) == -1, "dup add should fail");

	wm_remove(wm, 1);
	ASSERT(wm_count(wm) == 0, "count should be 0 after remove");
	ASSERT(wm_find(wm, 1) == NULL, "find should return NULL");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_z_order(void)
{
	struct wm *wm;
	struct vt_state *vt1, *vt2, *vt3;
	struct wm_window *w1, *w2, *w3;

	TEST("z-order and focus");
	wm = wm_new(24, 80);
	vt1 = vt_state_new(5, 20, 0);
	vt2 = vt_state_new(5, 20, 0);
	vt3 = vt_state_new(5, 20, 0);

	wm_add(wm, 1, vt1, 2, 2, 20, 5);
	wm_add(wm, 2, vt2, 5, 5, 20, 5);
	wm_add(wm, 3, vt3, 8, 8, 20, 5);

	w1 = wm_find(wm, 1);
	w2 = wm_find(wm, 2);
	w3 = wm_find(wm, 3);
	ASSERT(w1->z < w2->z, "w1 should be behind w2");
	ASSERT(w2->z < w3->z, "w2 should be behind w3");

	/* raise w1 to top */
	wm_raise(wm, 1);
	ASSERT(w1->z > w3->z, "w1 should now be on top");

	/* focus w2 */
	wm_focus(wm, 2);
	ASSERT(w2->focused == 1, "w2 should be focused");
	ASSERT(w1->focused == 0, "w1 should not be focused");
	ASSERT(w2->z > w1->z, "focused w2 should be on top");

	vt_state_free(vt1);
	vt_state_free(vt2);
	vt_state_free(vt3);
	wm_free(wm);
	PASS();
}

static void
test_composite_empty(void)
{
	struct wm *wm;
	const struct vt_cell *scr;
	const struct tui_theme *theme;

	TEST("composite empty screen");
	wm = wm_new(5, 10);
	theme = tui_theme_default();
	wm_composite(wm, theme);
	scr = wm_screen(wm);
	ASSERT(scr != NULL, "screen should not be NULL");
	ASSERT(scr[0].codepoint == ' ', "empty screen should have spaces");
	wm_free(wm);
	PASS();
}

static void
test_composite_window(void)
{
	struct wm *wm;
	struct vt_state *vt;
	const struct vt_cell *scr;
	const struct tui_theme *theme;
	struct vt_cell *cell;

	TEST("composite window with content");
	wm = wm_new(20, 60);
	vt = vt_state_new(5, 20, 0);
	theme = tui_theme_default();

	/* put a character in the vt buffer */
	cell = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(cell != NULL, "vt_buf_cell returned NULL");
	cell->codepoint = 'A';
	cell->width = 1;

	/* window at (2,2) with 20x5 content */
	wm_add(wm, 1, vt, 2, 2, 20, 5);
	wm_composite(wm, theme);
	scr = wm_screen(wm);

	/* content at (2,2) should have 'A' */
	ASSERT(scr[2 * 60 + 2].codepoint == 'A',
	    "content cell should have 'A'");

	/* border at (1,1) should have a glyph (top-left corner) */
	ASSERT(scr[1 * 60 + 1].codepoint != ' ',
	    "top-left corner should be a border glyph");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_hit_test(void)
{
	struct wm *wm;
	struct vt_state *vt;
	uint32_t hit_id;
	enum wm_hit area;

	TEST("hit testing");
	wm = wm_new(20, 60);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 5);

	/* content area */
	ASSERT(wm_hit_test(wm, 5, 5, &hit_id, &area) == 1, "should hit");
	ASSERT(hit_id == 1, "wrong window id");
	ASSERT(area == WM_HIT_CONTENT, "should be content");

	/* title bar (top border, row 4) */
	ASSERT(wm_hit_test(wm, 4, 5, &hit_id, &area) == 1, "should hit title");
	ASSERT(area == WM_HIT_TITLE, "should be title");

	/* border (left side, row 6) */
	ASSERT(wm_hit_test(wm, 6, 4, &hit_id, &area) == 1, "should hit border");
	ASSERT(area == WM_HIT_BORDER, "should be border");

	/* background (0,0) */
	ASSERT(wm_hit_test(wm, 0, 0, &hit_id, &area) == 0, "should miss");
	ASSERT(area == WM_HIT_NONE, "should be none");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_move(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;

	TEST("move window");
	wm = wm_new(24, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 2, 2, 20, 5);

	wm_move(wm, 1, 10, 8);
	win = wm_find(wm, 1);
	ASSERT(win->x == 10 && win->y == 8, "position should be updated");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_set_title(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;

	TEST("set title");
	wm = wm_new(24, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 2, 2, 20, 5);

	wm_set_title(wm, 1, "bash");
	win = wm_find(wm, 1);
	ASSERT(strcmp(win->title, "bash") == 0, "title should be 'bash'");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_resize(void)
{
	struct wm *wm;

	TEST("resize screen");
	wm = wm_new(24, 80);
	wm_resize(wm, 40, 120);
	ASSERT(wm_rows(wm) == 40, "rows should be 40");
	ASSERT(wm_cols(wm) == 120, "cols should be 120");
	wm_free(wm);
	PASS();
}

static void
test_z_overlap(void)
{
	struct wm *wm;
	struct vt_state *vt1, *vt2;
	const struct vt_cell *scr;
	const struct tui_theme *theme;
	struct vt_cell *c;

	TEST("z-order overlap compositing");
	wm = wm_new(20, 40);
	vt1 = vt_state_new(3, 10, 0);
	vt2 = vt_state_new(3, 10, 0);
	theme = tui_theme_default();

	/* put distinct chars in each vt */
	c = vt_buf_cell(vt1->buf, 0, 0);
	c->codepoint = '1';
	c->width = 1;
	c = vt_buf_cell(vt2->buf, 0, 0);
	c->codepoint = '2';
	c->width = 1;

	/* overlapping windows: w1 at (2,2), w2 at (2,2) on top */
	wm_add(wm, 1, vt1, 2, 2, 10, 3);
	wm_add(wm, 2, vt2, 2, 2, 10, 3);
	wm_composite(wm, theme);
	scr = wm_screen(wm);

	/* w2 is on top (added later, higher z), so we see '2' */
	ASSERT(scr[2 * 40 + 2].codepoint == '2',
	    "topmost window content should be visible");

	/* raise w1 to top and recomposite */
	wm_raise(wm, 1);
	wm_composite(wm, theme);
	scr = wm_screen(wm);
	ASSERT(scr[2 * 40 + 2].codepoint == '1',
	    "raised window content should be visible");

	vt_state_free(vt1);
	vt_state_free(vt2);
	wm_free(wm);
	PASS();
}

static void
test_mouse_press_focus(void)
{
	struct wm *wm;
	struct vt_state *vt1, *vt2;
	struct wm_window *w1, *w2;
	uint32_t hit_id;
	enum wm_hit area;

	TEST("mouse press focuses window");
	wm = wm_new(20, 60);
	vt1 = vt_state_new(5, 20, 0);
	vt2 = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt1, 5, 5, 20, 5);
	wm_add(wm, 2, vt2, 10, 10, 20, 5);

	w1 = wm_find(wm, 1);
	w2 = wm_find(wm, 2);
	ASSERT(w2->z > w1->z, "w2 should be on top initially");

	/* click on w1's content */
	wm_mouse_press(wm, 5, 5, &hit_id, &area);
	ASSERT(hit_id == 1, "should hit window 1");
	ASSERT(area == WM_HIT_CONTENT, "should be content");
	ASSERT(w1->focused == 1, "w1 should be focused");
	ASSERT(w2->focused == 0, "w2 should not be focused");
	ASSERT(w1->z > w2->z, "w1 should be raised to top");

	vt_state_free(vt1);
	vt_state_free(vt2);
	wm_free(wm);
	PASS();
}

static void
test_drag_move(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;
	uint32_t hit_id;
	enum wm_hit area;
	enum wm_drag drag;
	int changed, resized;
	uint32_t released_id;

	TEST("drag move window via title bar");
	wm = wm_new(30, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 10, 10, 20, 5);

	/* title bar is at row 9 (y-1), cols 9..30 */
	drag = wm_mouse_press(wm, 9, 15, &hit_id, &area);
	ASSERT(drag == WM_DRAG_MOVING, "should start moving");
	ASSERT(hit_id == 1, "should be window 1");
	ASSERT(area == WM_HIT_TITLE, "should be title");
	ASSERT(wm_drag_state(wm) == WM_DRAG_MOVING, "state should be moving");

	/* drag 5 cols right, 3 rows down */
	changed = wm_mouse_drag(wm, 12, 20);
	ASSERT(changed == 1, "position should change");
	win = wm_find(wm, 1);
	ASSERT(win->x == 15, "x should be 10 + 5");
	ASSERT(win->y == 13, "y should be 10 + 3");

	/* no-op drag to same position */
	changed = wm_mouse_drag(wm, 12, 20);
	ASSERT(changed == 0, "no change on same position");

	/* release */
	released_id = wm_mouse_release(wm, &resized);
	ASSERT(released_id == 1, "should return window 1");
	ASSERT(resized == 0, "should not be resize");
	ASSERT(wm_drag_state(wm) == WM_DRAG_IDLE, "should be idle");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_drag_resize(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;
	uint32_t hit_id;
	enum wm_hit area;
	enum wm_drag drag;
	int changed, resized;
	uint32_t released_id;

	TEST("drag resize window via border");
	wm = wm_new(30, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 10, 10, 20, 5);

	/* right border is at col 30 (x + w), rows 10..14 */
	drag = wm_mouse_press(wm, 12, 30, &hit_id, &area);
	ASSERT(drag == WM_DRAG_RESIZING, "should start resizing");
	ASSERT(hit_id == 1, "should be window 1");
	ASSERT(area == WM_HIT_BORDER, "should be border");

	/* drag 8 cols right, 4 rows down */
	changed = wm_mouse_drag(wm, 16, 38);
	ASSERT(changed == 1, "size should change");
	win = wm_find(wm, 1);
	ASSERT(win->w == 28, "width should be 20 + 8");
	ASSERT(win->h == 9, "height should be 5 + 4");
	/* position should not change during resize */
	ASSERT(win->x == 10, "x should stay 10");
	ASSERT(win->y == 10, "y should stay 10");

	/* release */
	released_id = wm_mouse_release(wm, &resized);
	ASSERT(released_id == 1, "should return window 1");
	ASSERT(resized == 1, "should be resize");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_resize_min_clamp(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;
	uint32_t hit_id;
	enum wm_hit area;

	TEST("resize clamps to minimum dimensions");
	wm = wm_new(30, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 10, 10, 20, 5);

	/* start resize from right border */
	wm_mouse_press(wm, 12, 30, &hit_id, &area);

	/* drag far left to shrink below minimum */
	wm_mouse_drag(wm, 0, 0);
	win = wm_find(wm, 1);
	ASSERT(win->w == WM_MIN_WIDTH, "width should clamp to minimum");
	ASSERT(win->h == WM_MIN_HEIGHT, "height should clamp to minimum");

	wm_mouse_release(wm, NULL);

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_close_button(void)
{
	struct wm *wm;
	struct vt_state *vt;
	uint32_t hit_id;
	enum wm_hit area;
	enum wm_drag drag;

	TEST("click close button returns WM_HIT_CLOSE");
	wm = wm_new(20, 60);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 5);

	/* close button is at row 4 (y-1), cols fw-4..fw-2
	 * frame x = 4, fw = 22, so close at cols 22..24 -> 4+22-4=22 */
	drag = wm_mouse_press(wm, 4, 23, &hit_id, &area);
	ASSERT(area == WM_HIT_CLOSE, "should be close button");
	ASSERT(hit_id == 1, "should be window 1");
	ASSERT(drag == WM_DRAG_IDLE, "close should not start drag");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_background_click(void)
{
	struct wm *wm;
	uint32_t hit_id;
	enum wm_hit area;
	enum wm_drag drag;

	TEST("click on background returns NONE");
	wm = wm_new(20, 60);

	drag = wm_mouse_press(wm, 0, 0, &hit_id, &area);
	ASSERT(area == WM_HIT_NONE, "should be none");
	ASSERT(drag == WM_DRAG_IDLE, "should not start drag");

	wm_free(wm);
	PASS();
}

static void
test_release_no_drag(void)
{
	struct wm *wm;
	int resized;
	uint32_t id;

	TEST("release with no active drag");
	wm = wm_new(20, 60);

	id = wm_mouse_release(wm, &resized);
	ASSERT(id == 0, "should return 0");
	ASSERT(resized == 0, "should not be resize");

	wm_free(wm);
	PASS();
}

static void
test_drag_removed_window(void)
{
	struct wm *wm;
	struct vt_state *vt;
	uint32_t hit_id;
	enum wm_hit area;
	int changed;

	TEST("drag after window removed returns 0");
	wm = wm_new(30, 80);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 10, 10, 20, 5);

	wm_mouse_press(wm, 9, 15, &hit_id, &area);
	ASSERT(wm_drag_state(wm) == WM_DRAG_MOVING, "should be moving");

	/* remove the window mid-drag */
	wm_remove(wm, 1);
	changed = wm_mouse_drag(wm, 15, 20);
	ASSERT(changed == 0, "should return 0");
	ASSERT(wm_drag_state(wm) == WM_DRAG_IDLE, "should reset to idle");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_scrollbar_hidden(void)
{
	struct wm *wm;
	struct vt_state *vt;
	uint32_t hit_id;
	enum wm_hit area;

	TEST("scrollbar hidden: right border -> WM_HIT_BORDER");
	wm = wm_new(20, 60);
	vt = vt_state_new(5, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 5);

	/* scroll_len defaults to 0 -> scrollbar hidden */
	/* right border col = x + w = 5 + 20 = 25, content row = 6 */
	ASSERT(wm_hit_test(wm, 6, 25, &hit_id, &area) == 1, "should hit");
	ASSERT(area == WM_HIT_BORDER, "should be border, not scrollbar");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_scrollbar_visible(void)
{
	struct wm *wm;
	struct vt_state *vt;
	uint32_t hit_id;
	enum wm_hit area;

	TEST("scrollbar visible: right border -> WM_HIT_SCROLLBAR");
	wm = wm_new(20, 60);
	vt = vt_state_new(8, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 8);

	/* enable scrollbar */
	wm_set_scroll(wm, 1, 0.5f, 0.3f);

	/* right border col = 25, content row = 6 (between title and bottom) */
	ASSERT(wm_hit_test(wm, 6, 25, &hit_id, &area) == 1, "should hit");
	ASSERT(area == WM_HIT_SCROLLBAR, "should be scrollbar");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_scrollbar_drag(void)
{
	struct wm *wm;
	struct vt_state *vt;
	struct wm_window *win;
	uint32_t hit_id;
	enum wm_hit area;
	enum wm_drag drag;
	int changed;

	TEST("scrollbar drag updates scroll_pos");
	wm = wm_new(20, 60);
	vt = vt_state_new(8, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 8);
	wm_set_scroll(wm, 1, 0.0f, 0.3f);

	/* right border col = 25, click on scrollbar track area */
	drag = wm_mouse_press(wm, 7, 25, &hit_id, &area);
	ASSERT(area == WM_HIT_SCROLLBAR, "should be scrollbar");
	ASSERT(drag == WM_DRAG_SCROLLING, "should start scrolling");
	ASSERT(wm_drag_state(wm) == WM_DRAG_SCROLLING, "state = scrolling");

	/* drag down a few rows */
	changed = wm_mouse_drag(wm, 10, 25);
	ASSERT(changed == 1, "scroll_pos should change");

	win = wm_find(wm, 1);
	ASSERT(win->scroll_pos > 0.0f, "pos should have increased");
	ASSERT(win->scroll_pos <= 1.0f, "pos should be <= 1");

	wm_mouse_release(wm, NULL);
	ASSERT(wm_drag_state(wm) == WM_DRAG_IDLE, "should be idle");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_scroll_pos_from_row(void)
{
	struct wm *wm;
	struct vt_state *vt;
	float pos;

	TEST("scroll_pos_from_row conversion");
	wm = wm_new(20, 60);
	vt = vt_state_new(8, 20, 0);
	wm_add(wm, 1, vt, 5, 5, 20, 8);
	wm_set_scroll(wm, 1, 0.5f, 0.3f);

	/* h=8 >= 4, so arrows present. track_top = y+1 = 6,
	 * track_inner = h-2 = 6, rows 6..11 */
	pos = wm_scroll_pos_from_row(wm, 1, 6);
	ASSERT(pos >= -0.01f && pos <= 0.01f, "top of track -> ~0.0");

	pos = wm_scroll_pos_from_row(wm, 1, 11);
	ASSERT(pos >= 0.99f && pos <= 1.01f, "bottom of track -> ~1.0");

	/* middle of track */
	pos = wm_scroll_pos_from_row(wm, 1, 8);
	ASSERT(pos > 0.2f && pos < 0.6f, "mid track -> mid range");

	vt_state_free(vt);
	wm_free(wm);
	PASS();
}

static void
test_unfocused_cursor(void)
{
	struct wm *wm;
	struct vt_state *vt1, *vt2;
	const struct vt_cell *scr;
	const struct tui_theme *theme;
	int cur_row, cur_col;

	TEST("unfocused window cursor shown with reverse video");
	wm = wm_new(30, 80);
	vt1 = vt_state_new(5, 20, 0);
	vt2 = vt_state_new(5, 20, 0);
	theme = tui_theme_default();

	/* two windows: win 1 focused, win 2 unfocused */
	wm_add(wm, 1, vt1, 2, 2, 20, 5);
	wm_add(wm, 2, vt2, 30, 2, 20, 5);
	wm_focus(wm, 1);

	/* ensure cursor visible on both */
	vt1->modes |= VT_MODE_CURSOR_VIS;
	vt2->modes |= VT_MODE_CURSOR_VIS;

	/* place cursors at known positions */
	vt1->cursor_row = 0;
	vt1->cursor_col = 0;
	vt2->cursor_row = 1;
	vt2->cursor_col = 3;

	wm_composite(wm, theme);
	scr = wm_screen(wm);

	/* focused window cursor cell should NOT have reverse added */
	ASSERT(!(scr[2 * 80 + 2].attrs & VT_ATTR_REVERSE),
	    "focused cursor cell should not be reversed");

	/* unfocused window cursor cell should have reverse */
	cur_row = 2 + vt2->cursor_row; /* win y + cursor row */
	cur_col = 30 + vt2->cursor_col; /* win x + cursor col */
	ASSERT(scr[cur_row * 80 + cur_col].attrs & VT_ATTR_REVERSE,
	    "unfocused cursor cell should have REVERSE attr");

	vt_state_free(vt1);
	vt_state_free(vt2);
	wm_free(wm);
	PASS();
}

int
main(void)
{
	printf("libwm tests:\n");

	test_new_free();
	test_add_remove();
	test_z_order();
	test_composite_empty();
	test_composite_window();
	test_hit_test();
	test_move();
	test_set_title();
	test_resize();
	test_z_overlap();
	test_mouse_press_focus();
	test_drag_move();
	test_drag_resize();
	test_resize_min_clamp();
	test_close_button();
	test_background_click();
	test_release_no_drag();
	test_drag_removed_window();
	test_scrollbar_hidden();
	test_scrollbar_visible();
	test_scrollbar_drag();
	test_scroll_pos_from_row();
	test_unfocused_cursor();

	printf("test_wm: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
