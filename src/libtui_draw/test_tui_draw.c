/* test_tui_draw.c : tests for the draw-surface pad backend */

#include "tui_draw.h"
#include "tui_pad.h"
#include "draw.h"
#include "draw_driver.h"
#include "vt_buf.h"
#include "vt_cell.h"

#include <stdio.h>
#include <string.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() printf("ok\n")

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

/* ---- capturing mock driver ---- */

#define MOCK_ROWS 24
#define MOCK_COLS 80
#define MOCK_MAX (MOCK_ROWS * MOCK_COLS)

struct mock {
	int		present_calls;
	int		last_rows, last_cols;
	struct vt_cell	cells[MOCK_MAX];
};

static int
mock_size(void *ctx, int *rows, int *cols)
{
	(void)ctx;
	*rows = MOCK_ROWS;
	*cols = MOCK_COLS;
	return 0;
}

static void
mock_present(void *ctx, const struct draw_frame *f)
{
	struct mock *m = ctx;
	size_t n = (size_t)f->rows * f->cols;

	m->present_calls++;
	m->last_rows = f->rows;
	m->last_cols = f->cols;
	if (n > MOCK_MAX)
		n = MOCK_MAX;
	memcpy(m->cells, f->cells, n * sizeof(struct vt_cell));
}

static const struct draw_driver mock_driver = {
	.size = mock_size,
	.present = mock_present,
};

static const struct vt_cell *
mock_cell(struct mock *m, int r, int c)
{
	return &m->cells[r * MOCK_COLS + c];
}

/* ---- tests ---- */

static void
test_render_puts_cells_on_surface(void)
{
	struct mock m;
	struct draw *d;
	struct tui_backend *be;
	struct tui_stack stack;
	struct tui_pad *pad;
	struct vt_buf *base;
	struct vt_color fg, bg;
	const struct vt_cell *c;

	TEST("render writes pad cells into the surface and presents");
	memset(&m, 0, sizeof(m));
	d = draw_new(&mock_driver, &m);
	ASSERT(d != NULL, "draw_new");
	be = tui_draw_new(d);
	ASSERT(be != NULL, "tui_draw_new");
	base = vt_buf_new(MOCK_ROWS, MOCK_COLS, 0);
	ASSERT(base != NULL, "vt_buf_new");

	fg.type = VT_COLOR_INDEXED;
	fg.index = 3;
	bg.type = VT_COLOR_DEFAULT;

	tui_stack_init(&stack);
	pad = tui_stack_push(&stack);
	ASSERT(pad != NULL, "push");
	tui_pad_clear(pad, 4, 2);	/* w=4, h=2 at (0,0) */
	pad->screen_row = 1;
	pad->screen_col = 2;
	tui_pad_puts(pad, 0, 0, "hi", fg, bg, 0, TUI_OPAQUE);

	tui_stack_render(&stack, base, be, tui_draw_ctx(be));

	ASSERT(m.present_calls == 1, "one present");
	ASSERT(m.last_rows == MOCK_ROWS && m.last_cols == MOCK_COLS,
	    "full-screen frame");
	c = mock_cell(&m, 1, 2);
	ASSERT(c->codepoint == 'h', "h at (1,2)");
	ASSERT(c->fg.type == VT_COLOR_INDEXED && c->fg.index == 3,
	    "fg carried through");
	c = mock_cell(&m, 1, 3);
	ASSERT(c->codepoint == 'i', "i at (1,3)");

	vt_buf_free(base);
	tui_draw_free(be);
	draw_free(d);
	PASS();
}

static void
test_erase_restores_base(void)
{
	struct mock m;
	struct draw *d;
	struct tui_backend *be;
	struct tui_stack stack;
	struct vt_buf *base;
	struct vt_cell *bc;
	const struct vt_cell *c;

	TEST("erase paints the underlying base cells");
	memset(&m, 0, sizeof(m));
	d = draw_new(&mock_driver, &m);
	be = tui_draw_new(d);
	base = vt_buf_new(MOCK_ROWS, MOCK_COLS, 0);
	ASSERT(d && be && base, "setup");

	bc = vt_buf_cell(base, 0, 0);
	ASSERT(bc != NULL, "base cell");
	bc->codepoint = 'Z';

	tui_stack_init(&stack);		/* empty stack: erase shows base */
	tui_stack_erase(&stack, base, be, tui_draw_ctx(be), 0, 0, 1, 1);

	ASSERT(m.present_calls == 1, "one present");
	c = mock_cell(&m, 0, 0);
	ASSERT(c->codepoint == 'Z', "base char restored at (0,0)");

	vt_buf_free(base);
	tui_draw_free(be);
	draw_free(d);
	PASS();
}

int
main(void)
{
	printf("test_tui_draw:\n");
	test_render_puts_cells_on_surface();
	test_erase_restores_base();
	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
