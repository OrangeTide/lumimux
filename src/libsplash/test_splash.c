/* test_splash.c : unit tests for splash screen library */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash.h"
#include "splash_draw.h"
#include "vt_buf.h"
#include "vt_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count;
static int fail_count;

#define CHECK(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
		fail_count++; \
	} \
} while (0)

/* ---- splash_logo_width ---- */

static void
test_logo_width(void)
{
	int w;

	w = splash_logo_width("lumiMUX");
	/* l(3)+1+u(3)+1+m(3)+1+i(1)+1+M(3)+1+U(3)+1+X(3) = 25 */
	CHECK(w == 25, "lumiMUX logo width should be 25");

	w = splash_logo_width("lumi");
	/* l(3)+1+u(3)+1+m(3)+1+i(1) = 13 */
	CHECK(w == 13, "lumi logo width should be 13");

	w = splash_logo_width("mantle");
	/* m(3)+1+a(3)+1+n(3)+1+t(3)+1+l(3)+1+e(3) = 23 */
	CHECK(w == 23, "mantle logo width should be 23");

	w = splash_logo_width("");
	CHECK(w == 0, "empty name width should be 0");

	w = splash_logo_width("a");
	CHECK(w == 3, "single char 'a' width should be 3");

	w = splash_logo_width("i");
	CHECK(w == 1, "single char 'i' width should be 1");

	w = splash_logo_width("A");
	CHECK(w == 3, "single char 'A' width should be 3");
}

/* ---- splash_logo_height ---- */

static void
test_logo_height(void)
{
	int h;

	/* all lowercase: 3 glyph rows + 1 blank + 1 tagline = 5 */
	h = splash_logo_height("lumi");
	CHECK(h == 5, "all-lowercase height should be 5");

	/* mixed case: 4 glyph rows + 1 blank + 1 tagline = 6 */
	h = splash_logo_height("lumiMUX");
	CHECK(h == 6, "mixed-case height should be 6");

	/* all uppercase: 4 glyph rows + 1 blank + 1 tagline = 6 */
	h = splash_logo_height("ABC");
	CHECK(h == 6, "all-uppercase height should be 6");
}

/* ---- splash_create / splash_free ---- */

static void
test_create_free(void)
{
	struct vt_buf *buf;

	buf = splash_create(SPLASH_SPACE, "test");
	CHECK(buf != NULL, "space scene should create");
	if (buf) {
		CHECK(vt_buf_rows(buf) == 40, "space rows should be 40");
		CHECK(vt_buf_cols(buf) == 80, "space cols should be 80");
		splash_free(buf);
	}

	buf = splash_create(SPLASH_MOUNTAIN, "test");
	CHECK(buf != NULL, "mountain scene should create");
	if (buf) {
		CHECK(vt_buf_rows(buf) == 40, "mountain rows should be 40");
		CHECK(vt_buf_cols(buf) == 80, "mountain cols should be 80");
		splash_free(buf);
	}

	buf = splash_create(SPLASH_BEACH, "test");
	CHECK(buf != NULL, "beach scene should create");
	if (buf) {
		CHECK(vt_buf_rows(buf) == 65, "beach rows should be 65");
		CHECK(vt_buf_cols(buf) == 120, "beach cols should be 120");
		splash_free(buf);
	}
}

/* ---- splash_put / splash_put_fg ---- */

static void
test_put_cell(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	buf = vt_buf_new(10, 10, 0);

	splash_put(buf, 0, 0, 'A',
	    CLR_IDX(1), CLR_IDX(2), VT_ATTR_BOLD);
	c = vt_buf_cell(buf, 0, 0);
	CHECK(c->codepoint == 'A', "put: codepoint should be 'A'");
	CHECK(c->fg.type == VT_COLOR_INDEXED && c->fg.index == 1,
	    "put: fg should be idx 1");
	CHECK(c->bg.type == VT_COLOR_INDEXED && c->bg.index == 2,
	    "put: bg should be idx 2");
	CHECK(c->attrs == VT_ATTR_BOLD, "put: attrs should be BOLD");

	/* put_fg should preserve bg */
	splash_put_fg(buf, 0, 0, 'B', CLR_IDX(5), 0);
	c = vt_buf_cell(buf, 0, 0);
	CHECK(c->codepoint == 'B', "put_fg: codepoint should be 'B'");
	CHECK(c->fg.index == 5, "put_fg: fg should be idx 5");
	CHECK(c->bg.type == VT_COLOR_INDEXED && c->bg.index == 2,
	    "put_fg: bg should still be idx 2");

	/* out of bounds should not crash */
	splash_put(buf, 99, 99, 'X', CLR_DEF, CLR_DEF, 0);
	splash_put_fg(buf, -1, -1, 'X', CLR_DEF, 0);

	vt_buf_free(buf);
}

/* ---- splash_fill ---- */

static void
test_fill(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	buf = vt_buf_new(10, 10, 0);

	splash_fill(buf, 2, 3, 5, 7, '#',
	    CLR_IDX(9), CLR_IDX(0), 0);

	/* inside the fill region */
	c = vt_buf_cell(buf, 3, 4);
	CHECK(c->codepoint == '#', "fill: cell inside should be '#'");

	/* outside the fill region */
	c = vt_buf_cell(buf, 1, 1);
	CHECK(c->codepoint == ' ', "fill: cell outside should be ' '");

	vt_buf_free(buf);
}

/* ---- splash_rand determinism ---- */

static void
test_rand(void)
{
	uint32_t s1, s2;
	int i;

	s1 = 12345;
	s2 = 12345;

	for (i = 0; i < 100; i++)
		CHECK(splash_rand(&s1) == splash_rand(&s2),
		    "rand: same seed should produce same sequence");

	s2 = 99999;
	CHECK(splash_rand(&s1) != splash_rand(&s2),
	    "rand: different seeds should diverge");
}

/* ---- scene cells are populated ---- */

static void
test_scene_populated(void)
{
	struct vt_buf *buf;
	int r, c, non_space;

	buf = splash_create(SPLASH_SPACE, "test");
	CHECK(buf != NULL, "space scene created");
	if (!buf)
		return;

	/* verify at least some cells are non-default */
	non_space = 0;
	for (r = 0; r < vt_buf_rows(buf); r++) {
		for (c = 0; c < vt_buf_cols(buf); c++) {
			struct vt_cell *cell = vt_buf_cell(buf, r, c);

			if (cell->codepoint != ' ' ||
			    cell->fg.type != VT_COLOR_DEFAULT ||
			    cell->bg.type != VT_COLOR_DEFAULT)
				non_space++;
		}
	}
	CHECK(non_space > 100,
	    "space scene should have many non-default cells");
	splash_free(buf);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	test_logo_width();
	test_logo_height();
	test_create_free();
	test_put_cell();
	test_fill();
	test_rand();
	test_scene_populated();

	printf("test_splash: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count ? 1 : 0;
}
