/* test_tui.c : tests for libtui */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_pad.h"
#include "tui_theme.h"
#include "tui_box.h"
#include "tui_menu.h"
#include "tui_list.h"
#include "tui_sep.h"

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

/* ---- mock backend ---- */

#define MOCK_MAX 256

struct mock_cell {
	int		row, col;
	uint32_t	cp;
	struct vt_color	fg, bg;
	uint16_t	attrs;
};

static struct mock_cell mock_cells[MOCK_MAX];
static int mock_count;
static int mock_flushed;

static void
mock_cell_cb(void *ctx, int row, int col, uint32_t cp,
    const struct vt_color *fg, const struct vt_color *bg,
    uint16_t attrs)
{
	(void)ctx;
	if (mock_count < MOCK_MAX) {
		struct mock_cell *m = &mock_cells[mock_count++];

		m->row = row;
		m->col = col;
		m->cp = cp;
		m->fg = *fg;
		m->bg = *bg;
		m->attrs = attrs;
	}
}

static void
mock_flush_cb(void *ctx)
{
	(void)ctx;
	mock_flushed = 1;
}

static const struct tui_backend mock_backend = {
	.cell = mock_cell_cb,
	.flush = mock_flush_cb,
};

static void
mock_reset(void)
{
	mock_count = 0;
	mock_flushed = 0;
}

/* ---- pad primitive tests ---- */

static void
test_pad_clear(void)
{
	struct tui_pad p;

	TEST("pad_clear zeroes cells");
	tui_pad_clear(&p, 10, 5);
	ASSERT(p.w == 10, "width mismatch");
	ASSERT(p.h == 5, "height mismatch");
	ASSERT(p.cells[0][0].blend == TUI_TRANSPARENT, "not transparent");
	ASSERT(p.cells[0][0].codepoint == 0, "not zero");
	ASSERT(p.cells[4][9].blend == TUI_TRANSPARENT, "last cell not transparent");
	PASS();
}

static void
test_pad_clear_clamp(void)
{
	struct tui_pad p;

	TEST("pad_clear clamps to max");
	tui_pad_clear(&p, 999, 999);
	ASSERT(p.w == TUI_PAD_MAX_COLS, "width not clamped");
	ASSERT(p.h == TUI_PAD_MAX_ROWS, "height not clamped");
	PASS();
}

static void
test_pad_put(void)
{
	struct tui_pad p;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 7 } };
	struct vt_color bg = { .type = VT_COLOR_INDEXED, { .index = 4 } };

	TEST("pad_put writes cell");
	tui_pad_clear(&p, 20, 10);
	tui_pad_put(&p, 3, 5, 'X', fg, bg, VT_ATTR_BOLD, TUI_OPAQUE);
	ASSERT(p.cells[3][5].codepoint == 'X', "codepoint mismatch");
	ASSERT(p.cells[3][5].fg.index == 7, "fg mismatch");
	ASSERT(p.cells[3][5].bg.index == 4, "bg mismatch");
	ASSERT(p.cells[3][5].attrs == VT_ATTR_BOLD, "attrs mismatch");
	ASSERT(p.cells[3][5].blend == TUI_OPAQUE, "blend mismatch");
	PASS();
}

static void
test_pad_put_oob(void)
{
	struct tui_pad p;
	struct vt_color c = { .type = VT_COLOR_DEFAULT };

	TEST("pad_put out of bounds is no-op");
	tui_pad_clear(&p, 10, 5);
	tui_pad_put(&p, -1, 0, 'X', c, c, 0, TUI_OPAQUE);
	tui_pad_put(&p, 5, 0, 'X', c, c, 0, TUI_OPAQUE);
	tui_pad_put(&p, 0, 10, 'X', c, c, 0, TUI_OPAQUE);
	tui_pad_put(&p, 0, -1, 'X', c, c, 0, TUI_OPAQUE);
	/* all cells should still be zero */
	ASSERT(p.cells[0][0].codepoint == 0, "cell was modified");
	ASSERT(p.cells[4][9].codepoint == 0, "cell was modified");
	PASS();
}

static void
test_pad_puts_ascii(void)
{
	struct tui_pad p;
	struct vt_color c = { .type = VT_COLOR_DEFAULT };
	int end;

	TEST("pad_puts ASCII string");
	tui_pad_clear(&p, 20, 5);
	end = tui_pad_puts(&p, 0, 2, "hello", c, c, 0, TUI_OPAQUE);
	ASSERT(end == 7, "end column mismatch");
	ASSERT(p.cells[0][2].codepoint == 'h', "char 0 mismatch");
	ASSERT(p.cells[0][3].codepoint == 'e', "char 1 mismatch");
	ASSERT(p.cells[0][6].codepoint == 'o', "char 4 mismatch");
	PASS();
}

static void
test_pad_puts_utf8(void)
{
	struct tui_pad p;
	struct vt_color c = { .type = VT_COLOR_DEFAULT };
	int end;

	TEST("pad_puts UTF-8 box-drawing");
	tui_pad_clear(&p, 20, 5);
	/* ┌─┐ = 3 codepoints, each 3 bytes */
	end = tui_pad_puts(&p, 0, 0,
	    "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",
	    c, c, 0, TUI_OPAQUE);
	ASSERT(end == 3, "end column mismatch");
	ASSERT(p.cells[0][0].codepoint == 0x250C, "TL mismatch"); /* ┌ */
	ASSERT(p.cells[0][1].codepoint == 0x2500, "H mismatch");  /* ─ */
	ASSERT(p.cells[0][2].codepoint == 0x2510, "TR mismatch"); /* ┐ */
	PASS();
}

static void
test_pad_fill(void)
{
	struct tui_pad p;
	struct vt_color c = { .type = VT_COLOR_INDEXED, { .index = 2 } };
	int i;

	TEST("pad_fill fills N cells");
	tui_pad_clear(&p, 20, 5);
	tui_pad_fill(&p, 1, 3, 5, '#', c, c, 0, TUI_OPAQUE);
	for (i = 0; i < 5; i++)
		ASSERT(p.cells[1][3 + i].codepoint == '#', "fill mismatch");
	ASSERT(p.cells[1][2].codepoint == 0, "before fill modified");
	ASSERT(p.cells[1][8].codepoint == 0, "after fill modified");
	PASS();
}

/* ---- theme tests ---- */

static void
test_theme_by_name(void)
{
	static const char *names[] = {
		"ascii", "thin", "double", "rounded",
		"turbo", "crimson", "acid", "shade", "borderless",
	};
	const struct tui_theme *t;
	int i;

	TEST("theme_by_name lookup");
	for (i = 0; i < (int)(sizeof(names) / sizeof(*names)); i++) {
		t = tui_theme_by_name(names[i]);
		ASSERT(t != NULL, "theme not found");
		ASSERT(strcmp(t->name, names[i]) == 0, "name mismatch");
	}

	t = tui_theme_by_name("bogus");
	ASSERT(t == NULL, "bogus should be NULL");
	PASS();
}

static void
test_theme_default(void)
{
	const struct tui_theme *t;

	TEST("theme_default returns non-NULL");
	t = tui_theme_default();
	ASSERT(t != NULL, "default is NULL");
	ASSERT(strcmp(t->name, "ascii") == 0 ||
	    strcmp(t->name, "thin") == 0, "unexpected default theme");
	ASSERT(tui_theme_count() == 9, "expected 9 themes");
	ASSERT(tui_theme_at(0) != NULL, "index 0 is NULL");
	ASSERT(tui_theme_at(-1) == NULL, "index -1 should be NULL");
	ASSERT(tui_theme_at(tui_theme_count()) == NULL, "past end");
	PASS();
}

/* ---- parse helper tests ---- */

static void
test_parse_color(void)
{
	struct vt_color c;

	TEST("parse_color valid inputs");
	ASSERT(tui_theme_parse_color("default", &c) == 0, "parse failed");
	ASSERT(c.type == VT_COLOR_DEFAULT, "not DEFAULT");

	ASSERT(tui_theme_parse_color("0", &c) == 0, "parse 0 failed");
	ASSERT(c.type == VT_COLOR_INDEXED && c.index == 0, "0 mismatch");
	ASSERT(tui_theme_parse_color("255", &c) == 0, "parse 255 failed");
	ASSERT(c.type == VT_COLOR_INDEXED && c.index == 255, "255 mismatch");

	ASSERT(tui_theme_parse_color("#1a2b3c", &c) == 0, "parse failed");
	ASSERT(c.type == VT_COLOR_RGB, "not RGB");
	ASSERT(c.rgb.r == 0x1a && c.rgb.g == 0x2b && c.rgb.b == 0x3c,
	    "rgb mismatch");
	PASS();
}

static void
test_parse_color_invalid(void)
{
	struct vt_color c;

	TEST("parse_color rejects invalid");
	ASSERT(tui_theme_parse_color("", &c) == -1, "empty accepted");
	ASSERT(tui_theme_parse_color("256", &c) == -1, "256 accepted");
	ASSERT(tui_theme_parse_color("-1", &c) == -1, "-1 accepted");
	ASSERT(tui_theme_parse_color("#xyz", &c) == -1, "#xyz accepted");
	ASSERT(tui_theme_parse_color("bogus", &c) == -1, "bogus accepted");
	ASSERT(tui_theme_parse_color(NULL, &c) == -1, "NULL accepted");
	PASS();
}

static void
test_parse_shadow(void)
{
	enum tui_shadow_style s;

	TEST("parse_shadow");
	ASSERT(tui_theme_parse_shadow("none", &s) == 0, "none failed");
	ASSERT(s == TUI_SHADOW_NONE, "not NONE");
	ASSERT(tui_theme_parse_shadow("half", &s) == 0, "half failed");
	ASSERT(s == TUI_SHADOW_HALF, "not HALF");
	ASSERT(tui_theme_parse_shadow("shade", &s) == 0, "shade failed");
	ASSERT(s == TUI_SHADOW_SHADE, "not SHADE");
	ASSERT(tui_theme_parse_shadow("bogus", &s) == -1, "bogus accepted");
	ASSERT(tui_theme_parse_shadow(NULL, &s) == -1, "NULL accepted");
	PASS();
}

/* ---- box tests ---- */

static void
test_box_draw_corners(void)
{
	struct tui_pad p;
	const struct tui_theme *t;
	struct tui_box box;

	TEST("box_draw places border corners");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	tui_pad_clear(&p, 20, 10);
	box.theme = t;
	box.title = NULL;
	box.footer = NULL;
	box.x = 0;
	box.y = 0;
	box.w = 10;
	box.h = 5;
	tui_box_draw(&p, &box);

	ASSERT(p.cells[0][0].codepoint == '+', "TL not +");
	ASSERT(p.cells[0][9].codepoint == '+', "TR not +");
	ASSERT(p.cells[4][0].codepoint == '+', "BL not +");
	ASSERT(p.cells[4][9].codepoint == '+', "BR not +");
	ASSERT(p.cells[0][5].codepoint == '-', "top edge not -");
	ASSERT(p.cells[2][0].codepoint == '|', "left edge not |");
	ASSERT(p.cells[2][9].codepoint == '|', "right edge not |");
	/* interior should be space with content colors */
	ASSERT(p.cells[2][5].codepoint == ' ', "interior not space");
	ASSERT(p.cells[2][5].blend == TUI_OPAQUE, "interior not opaque");
	PASS();
}

static void
test_box_draw_title(void)
{
	struct tui_pad p;
	const struct tui_theme *t;
	struct tui_box box;

	TEST("box_draw with title");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	tui_pad_clear(&p, 20, 10);
	box.theme = t;
	box.title = "hi";
	box.footer = NULL;
	box.x = 0;
	box.y = 0;
	box.w = 10;
	box.h = 5;
	tui_box_draw(&p, &box);

	/* title "hi" centered in 10-wide box: space at col 3, h at 4, i at 5 */
	ASSERT(p.cells[0][4].codepoint == 'h', "title h missing");
	ASSERT(p.cells[0][5].codepoint == 'i', "title i missing");
	PASS();
}

/* ---- stack tests ---- */

static void
test_stack_push_pop(void)
{
	struct tui_stack s;
	struct tui_pad *p;
	int i;

	TEST("stack push/pop");
	tui_stack_init(&s);
	ASSERT(tui_stack_depth(&s) == 0, "not empty");
	ASSERT(tui_stack_top(&s) == NULL, "top of empty not NULL");

	for (i = 0; i < TUI_STACK_MAX; i++) {
		p = tui_stack_push(&s);
		ASSERT(p != NULL, "push returned NULL");
	}
	ASSERT(tui_stack_depth(&s) == TUI_STACK_MAX, "depth wrong");

	/* push beyond max */
	p = tui_stack_push(&s);
	ASSERT(p == NULL, "push past max should be NULL");

	/* pop all */
	for (i = 0; i < TUI_STACK_MAX; i++)
		tui_stack_pop(&s);
	ASSERT(tui_stack_depth(&s) == 0, "not empty after pop all");

	/* pop empty is no-op */
	tui_stack_pop(&s);
	ASSERT(tui_stack_depth(&s) == 0, "pop below zero");
	PASS();
}

/* ---- compositing tests ---- */

static void
test_render_opaque(void)
{
	struct tui_stack s;
	struct tui_pad *p;
	struct vt_buf *buf;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 7 } };
	struct vt_color bg = { .type = VT_COLOR_INDEXED, { .index = 4 } };

	TEST("render opaque cell");
	buf = vt_buf_new(24, 80, 0);
	ASSERT(buf != NULL, "vt_buf_new failed");

	tui_stack_init(&s);
	p = tui_stack_push(&s);
	tui_pad_clear(p, 3, 2);
	p->screen_row = 0;
	p->screen_col = 0;
	tui_pad_put(p, 0, 0, 'A', fg, bg, 0, TUI_OPAQUE);

	mock_reset();
	tui_stack_render(&s, buf, &mock_backend, NULL);
	ASSERT(mock_flushed == 1, "not flushed");
	ASSERT(mock_count > 0, "no cells emitted");

	/* find cell at (0,0) */
	ASSERT(mock_cells[0].row == 0 && mock_cells[0].col == 0,
	    "first cell not at 0,0");
	ASSERT(mock_cells[0].cp == 'A', "codepoint not A");
	ASSERT(mock_cells[0].fg.index == 7, "fg mismatch");

	vt_buf_free(buf);
	PASS();
}

static void
test_render_transparent_falls_through(void)
{
	struct tui_stack s;
	struct tui_pad *p;
	struct vt_buf *buf;
	struct vt_cell *vc;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 3 } };
	struct vt_color bg = { .type = VT_COLOR_INDEXED, { .index = 1 } };

	TEST("render transparent falls to vt_buf");
	buf = vt_buf_new(24, 80, 0);
	ASSERT(buf != NULL, "vt_buf_new failed");

	/* write something into the base buffer */
	vc = vt_buf_cell(buf, 0, 0);
	ASSERT(vc != NULL, "vt_buf_cell failed");
	vc->codepoint = 'Z';
	vc->fg = fg;
	vc->bg = bg;

	tui_stack_init(&s);
	p = tui_stack_push(&s);
	tui_pad_clear(p, 1, 1);
	p->screen_row = 0;
	p->screen_col = 0;
	/* cell is transparent (default after clear) */

	mock_reset();
	tui_stack_render(&s, buf, &mock_backend, NULL);
	ASSERT(mock_count == 1, "expected 1 cell");
	ASSERT(mock_cells[0].cp == 'Z', "should show base buffer Z");
	ASSERT(mock_cells[0].fg.index == 3, "fg from base");

	vt_buf_free(buf);
	PASS();
}

static void
test_render_color_bg_blend(void)
{
	struct tui_stack s;
	struct tui_pad *p;
	struct vt_buf *buf;
	struct vt_cell *vc;
	struct vt_color base_fg = { .type = VT_COLOR_INDEXED, { .index = 7 } };
	struct vt_color base_bg = { .type = VT_COLOR_INDEXED, { .index = 0 } };
	struct vt_color shadow_bg = { .type = VT_COLOR_INDEXED, { .index = 8 } };
	struct vt_color dummy = { .type = VT_COLOR_DEFAULT };

	TEST("render COLOR_BG keeps char+fg, replaces bg");
	buf = vt_buf_new(24, 80, 0);
	ASSERT(buf != NULL, "vt_buf_new failed");

	vc = vt_buf_cell(buf, 0, 0);
	vc->codepoint = 'Q';
	vc->fg = base_fg;
	vc->bg = base_bg;

	tui_stack_init(&s);
	p = tui_stack_push(&s);
	tui_pad_clear(p, 1, 1);
	p->screen_row = 0;
	p->screen_col = 0;
	tui_pad_put(p, 0, 0, 0, dummy, shadow_bg, 0, TUI_COLOR_BG);

	mock_reset();
	tui_stack_render(&s, buf, &mock_backend, NULL);
	ASSERT(mock_count == 1, "expected 1 cell");
	ASSERT(mock_cells[0].cp == 'Q', "should keep base char Q");
	ASSERT(mock_cells[0].fg.index == 7, "should keep base fg");
	ASSERT(mock_cells[0].bg.index == 8, "should use overlay bg");

	vt_buf_free(buf);
	PASS();
}

static void
test_render_two_layers(void)
{
	struct tui_stack s;
	struct tui_pad *p0, *p1;
	struct vt_buf *buf;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 1 } };
	struct vt_color bg = { .type = VT_COLOR_INDEXED, { .index = 2 } };
	struct vt_color fg2 = { .type = VT_COLOR_INDEXED, { .index = 3 } };
	struct vt_color bg2 = { .type = VT_COLOR_INDEXED, { .index = 4 } };

	TEST("render two layers, top wins");
	buf = vt_buf_new(24, 80, 0);

	tui_stack_init(&s);

	/* layer 0: 'A' at screen (0,0) */
	p0 = tui_stack_push(&s);
	tui_pad_clear(p0, 2, 2);
	p0->screen_row = 0;
	p0->screen_col = 0;
	tui_pad_put(p0, 0, 0, 'A', fg, bg, 0, TUI_OPAQUE);
	tui_pad_put(p0, 0, 1, 'B', fg, bg, 0, TUI_OPAQUE);

	/* layer 1: 'X' at screen (0,0), transparent at (0,1) */
	p1 = tui_stack_push(&s);
	tui_pad_clear(p1, 2, 1);
	p1->screen_row = 0;
	p1->screen_col = 0;
	tui_pad_put(p1, 0, 0, 'X', fg2, bg2, 0, TUI_OPAQUE);
	/* (0,1) is transparent -- should fall through to layer 0 */

	mock_reset();
	tui_stack_render(&s, buf, &mock_backend, NULL);

	/* find the two cells */
	{
		int found_x = 0, found_b = 0, i;

		for (i = 0; i < mock_count; i++) {
			if (mock_cells[i].row == 0 &&
			    mock_cells[i].col == 0) {
				ASSERT(mock_cells[i].cp == 'X',
				    "top layer should win");
				found_x = 1;
			}
			if (mock_cells[i].row == 0 &&
			    mock_cells[i].col == 1) {
				ASSERT(mock_cells[i].cp == 'B',
				    "should fall through to layer 0");
				found_b = 1;
			}
		}
		ASSERT(found_x, "cell (0,0) not found");
		ASSERT(found_b, "cell (0,1) not found");
	}

	vt_buf_free(buf);
	PASS();
}

/* ---- erase test ---- */

static void
test_erase_restores_base(void)
{
	struct tui_stack s;
	struct vt_buf *buf;
	struct vt_cell *vc;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 5 } };
	struct vt_color bg = { .type = VT_COLOR_INDEXED, { .index = 6 } };

	TEST("erase restores from vt_buf");
	buf = vt_buf_new(24, 80, 0);
	vc = vt_buf_cell(buf, 2, 3);
	vc->codepoint = 'R';
	vc->fg = fg;
	vc->bg = bg;

	tui_stack_init(&s);
	mock_reset();
	tui_stack_erase(&s, buf, &mock_backend, NULL, 2, 3, 1, 1);
	ASSERT(mock_count == 1, "expected 1 cell");
	ASSERT(mock_cells[0].cp == 'R', "should restore R");
	ASSERT(mock_cells[0].fg.index == 5, "fg mismatch");

	vt_buf_free(buf);
	PASS();
}

/* ---- menu tests ---- */

static void
test_menu_measure(void)
{
	struct tui_menu m;
	int w, h;

	TEST("menu_measure returns expected dimensions");
	memset(&m, 0, sizeof(m));
	m.count = 2;
	snprintf(m.items[0].keys, sizeof(m.items[0].keys), "c/C");
	snprintf(m.items[0].label, sizeof(m.items[0].label), "New window");
	snprintf(m.items[1].keys, sizeof(m.items[1].keys), "d");
	snprintf(m.items[1].label, sizeof(m.items[1].label), "Detach");

	tui_menu_measure(&m, &w, &h);
	/* border + ' ' + 3(key) + "  " + 10(label) + ' ' + border = 19 */
	ASSERT(w == 19, "width mismatch");
	ASSERT(h == 4, "height mismatch (2 items + 2 borders)");
	PASS();
}

static void
test_menu_measure_submenu(void)
{
	struct tui_menu m;
	int w, h;

	TEST("menu_measure adds space for submenu indicator");
	memset(&m, 0, sizeof(m));
	m.count = 1;
	snprintf(m.items[0].keys, sizeof(m.items[0].keys), "w");
	snprintf(m.items[0].label, sizeof(m.items[0].label), "Windows");
	m.items[0].flags = TUI_MENU_SUBMENU;

	tui_menu_measure(&m, &w, &h);
	/* border + ' ' + 1(key) + "  " + 7(label) + " >" + ' ' + border = 16 */
	ASSERT(w == 16, "width with submenu mismatch");
	PASS();
}

static void
test_menu_draw_content(void)
{
	struct tui_pad p;
	struct tui_menu m;
	const struct tui_theme *t;

	TEST("menu_draw fills content rows");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	memset(&m, 0, sizeof(m));
	m.count = 2;
	m.sel = 1;
	snprintf(m.items[0].keys, sizeof(m.items[0].keys), "c");
	snprintf(m.items[0].label, sizeof(m.items[0].label), "New");
	snprintf(m.items[1].keys, sizeof(m.items[1].keys), "d");
	snprintf(m.items[1].label, sizeof(m.items[1].label), "Det");

	tui_menu_draw(&p, &m, t, "test", "ESC", 24, 80);

	/* row 0 is top border, row 1 is first item, row 2 is second (selected) */
	/* first char after border+space should be the key */
	ASSERT(p.cells[1][1].codepoint == ' ', "leading space");
	ASSERT(p.cells[1][2].codepoint == 'c', "key 'c'");
	/* selected row should have bold attrs */
	ASSERT(p.cells[2][2].attrs & VT_ATTR_BOLD, "selected should be bold");
	PASS();
}

static void
test_menu_draw_submenu_indicator(void)
{
	struct tui_pad p;
	struct tui_menu m;
	const struct tui_theme *t;
	int c, found;

	TEST("menu_draw shows > for submenu items");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	memset(&m, 0, sizeof(m));
	m.count = 1;
	m.sel = 0;
	snprintf(m.items[0].keys, sizeof(m.items[0].keys), "w");
	snprintf(m.items[0].label, sizeof(m.items[0].label), "Win");
	m.items[0].flags = TUI_MENU_SUBMENU;

	tui_menu_draw(&p, &m, t, NULL, NULL, 24, 80);

	/* scan row 1 for '>' */
	found = 0;
	for (c = 0; c < p.w; c++) {
		if (p.cells[1][c].codepoint == '>') {
			found = 1;
			break;
		}
	}
	ASSERT(found, "submenu '>' indicator not found");
	PASS();
}

/* ---- list tests ---- */

static void
test_list_draw_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *theme, void *ctx)
{
	const char **labels = ctx;
	struct vt_color fg = selected ? theme->sel_fg : theme->content_fg;
	struct vt_color bg = selected ? theme->sel_bg : theme->content_bg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;

	(void)width;
	tui_pad_puts(p, row, col, labels[index], fg, bg, attrs, TUI_OPAQUE);
}

static void
test_list_draw(void)
{
	struct tui_pad p;
	struct tui_list l;
	const struct tui_theme *t;
	const char *labels[] = { "alpha", "beta", "gamma" };

	TEST("list_draw renders rows via callback");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	memset(&l, 0, sizeof(l));
	l.count = 3;
	l.sel = 1;
	l.scroll = 0;

	tui_list_draw(&p, &l, t, test_list_draw_cb, (void *)labels,
	    "Items", 24, 80);

	ASSERT(l.visible == 3, "visible should be 3");
	/* row 1 = first item "alpha", row 2 = "beta" (selected) */
	ASSERT(p.cells[1][1].codepoint == 'a', "first item char");
	ASSERT(p.cells[2][1].codepoint == 'b', "second item char");
	/* selected row should be bold */
	ASSERT(p.cells[2][1].attrs & VT_ATTR_BOLD, "selected bold");
	PASS();
}

static void
test_list_scroll(void)
{
	struct tui_pad p;
	struct tui_list l;
	const struct tui_theme *t;
	const char *labels[] = {
		"a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
		"k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
		"u", "v", "w", "x", "y", "z", "A", "B", "C", "D",
		"E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
	};

	TEST("list_draw adjusts scroll for selection");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	memset(&l, 0, sizeof(l));
	l.count = 40;
	l.sel = 30; /* way past visible */
	l.scroll = 0;

	tui_list_draw(&p, &l, t, test_list_draw_cb, (void *)labels,
	    "Big", 24, 80);

	/* scroll should have adjusted so sel is visible */
	ASSERT(l.scroll > 0, "should have scrolled");
	ASSERT(l.sel >= l.scroll, "sel before scroll");
	ASSERT(l.sel < l.scroll + l.visible, "sel after visible");
	PASS();
}

/* ---- separator test ---- */

static void
test_sep_draw(void)
{
	struct tui_pad p;
	const struct tui_theme *t;

	TEST("sep_draw renders separator glyphs");
	t = tui_theme_by_name("ascii");
	ASSERT(t != NULL, "need ascii theme");

	tui_pad_clear(&p, 20, 5);
	tui_sep_draw(&p, t, 2, 0, 10);

	/* ascii theme: sep_l = "+", sep_fill = "-", sep_r = "+" */
	ASSERT(p.cells[2][0].codepoint == '+', "sep_l not +");
	ASSERT(p.cells[2][5].codepoint == '-', "sep_fill not -");
	ASSERT(p.cells[2][9].codepoint == '+', "sep_r not +");
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libtui tests:\n");

	/* pad primitives */
	test_pad_clear();
	test_pad_clear_clamp();
	test_pad_put();
	test_pad_put_oob();
	test_pad_puts_ascii();
	test_pad_puts_utf8();
	test_pad_fill();

	/* themes */
	test_theme_by_name();
	test_theme_default();

	/* parse helpers */
	test_parse_color();
	test_parse_color_invalid();
	test_parse_shadow();

	/* box */
	test_box_draw_corners();
	test_box_draw_title();

	/* stack */
	test_stack_push_pop();

	/* compositing */
	test_render_opaque();
	test_render_transparent_falls_through();
	test_render_color_bg_blend();
	test_render_two_layers();

	/* erase */
	test_erase_restores_base();

	/* menu widget */
	test_menu_measure();
	test_menu_measure_submenu();
	test_menu_draw_content();
	test_menu_draw_submenu_indicator();

	/* list widget */
	test_list_draw();
	test_list_scroll();

	/* separator */
	test_sep_draw();

	printf("test_tui: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
