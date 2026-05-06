/* test_render.c : tests for librender */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "render.h"
#include "tio_write.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "rune_width.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* capture output into a buffer via pipe */
struct capture {
	int	rfd;
	int	wfd;
};

static void
capture_open(struct capture *cap)
{
	int pfd[2];

	if (pipe(pfd) < 0) {
		cap->rfd = -1;
		cap->wfd = -1;
		return;
	}
	cap->rfd = pfd[0];
	cap->wfd = pfd[1];
}

static size_t
capture_read(struct capture *cap, char *buf, size_t bufsz)
{
	ssize_t n;

	close(cap->wfd);
	cap->wfd = -1;
	n = read(cap->rfd, buf, bufsz - 1);
	close(cap->rfd);
	cap->rfd = -1;
	if (n < 0)
		n = 0;
	buf[n] = '\0';
	return (size_t)n;
}

static void
capture_close(struct capture *cap)
{
	if (cap->wfd >= 0)
		close(cap->wfd);
	if (cap->rfd >= 0)
		close(cap->rfd);
}

/* ---- tests ---- */

static void
test_render_new_free(void)
{
	struct render *r;

	TEST("render new/free");
	r = render_new(24, 80, NULL);
	ASSERT(r != NULL, "render_new returned NULL");
	render_free(r);
	PASS();
}

static void
test_render_full_basic(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render_full outputs content");
	st = vt_state_new(3, 5, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(3, 5, NULL);
	ASSERT(r != NULL, "render new failed");

	/* put "Hi" at 0,0 */
	vt_state_putchar(st, 'H', 1);
	vt_state_putchar(st, 'i', 1);

	capture_open(&cap);
	ASSERT(cap.wfd >= 0, "pipe failed");

	render_full(r, cap.wfd, st);

	len = capture_read(&cap, buf, sizeof(buf));
	ASSERT(len > 0, "no output");

	/* should contain "Hi" somewhere */
	ASSERT(strstr(buf, "Hi") != NULL, "output missing 'Hi'");

	/* should contain CUP sequence ESC [ */
	ASSERT(strstr(buf, "\033[") != NULL, "no escape sequences");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_diff_skip_unchanged(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render_diff skips unchanged cells");
	st = vt_state_new(3, 5, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(3, 5, NULL);
	ASSERT(r != NULL, "render new failed");

	vt_state_putchar(st, 'A', 1);

	/* first render -- full */
	capture_open(&cap);
	render_full(r, cap.wfd, st);
	capture_read(&cap, buf, sizeof(buf));

	/* no changes -- diff should produce minimal output */
	/* clear dirty flags first (full already did) */
	capture_open(&cap);
	render_diff(r, cap.wfd, st);
	len = capture_read(&cap, buf, sizeof(buf));

	/* should NOT contain 'A' -- nothing changed */
	ASSERT(strstr(buf, "A") == NULL,
	    "diff should not re-emit unchanged 'A'");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_diff_emits_changes(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render_diff emits changed cells");
	st = vt_state_new(3, 10, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(3, 10, NULL);
	ASSERT(r != NULL, "render new failed");

	/* initial full render (blank screen) */
	capture_open(&cap);
	render_full(r, cap.wfd, st);
	capture_read(&cap, buf, sizeof(buf));

	/* now write a character -- creates a change */
	vt_state_putchar(st, 'X', 1);

	capture_open(&cap);
	render_diff(r, cap.wfd, st);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no diff output");
	ASSERT(strstr(buf, "X") != NULL, "diff missing changed cell 'X'");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_sgr_output(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render emits SGR for attributes");
	st = vt_state_new(3, 10, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(3, 10, NULL);
	ASSERT(r != NULL, "render new failed");

	/* set bold + write char */
	st->attrs = VT_ATTR_BOLD;
	vt_state_putchar(st, 'B', 1);

	capture_open(&cap);
	render_full(r, cap.wfd, st);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no output");
	/* should contain SGR bold (ESC[1m or ESC[...1...m) */
	ASSERT(strstr(buf, "\033[1m") != NULL ||
	    strstr(buf, ";1m") != NULL ||
	    strstr(buf, ";1;") != NULL,
	    "missing bold SGR");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_color_output(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render emits color SGR");
	st = vt_state_new(3, 10, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(3, 10, NULL);
	ASSERT(r != NULL, "render new failed");

	/* set red foreground (index 1 = SGR 31) */
	st->fg.type = VT_COLOR_INDEXED;
	st->fg.index = 1;
	vt_state_putchar(st, 'R', 1);

	capture_open(&cap);
	render_full(r, cap.wfd, st);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no output");
	ASSERT(strstr(buf, "31") != NULL, "missing red fg SGR 31");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_cursor_position(void)
{
	struct vt_state *st;
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render positions cursor");
	st = vt_state_new(10, 20, 0);
	ASSERT(st != NULL, "state new failed");
	r = render_new(10, 20, NULL);
	ASSERT(r != NULL, "render new failed");

	st->cursor_row = 3;
	st->cursor_col = 7;

	capture_open(&cap);
	render_full(r, cap.wfd, st);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no output");
	/* should contain CUP to row 4, col 8 (1-based): ESC[4;8H */
	ASSERT(strstr(buf, "\033[4;8H") != NULL,
	    "missing cursor position ESC[4;8H");

	render_free(r);
	vt_state_free(st);
	PASS();
}

static void
test_render_resize(void)
{
	struct render *r;

	TEST("render resize");
	r = render_new(24, 80, NULL);
	ASSERT(r != NULL, "render new failed");

	ASSERT(render_resize(r, 30, 120) == 0, "resize failed");

	render_free(r);
	PASS();
}

static void
test_render_cells_full(void)
{
	struct render *r;
	struct capture cap;
	struct vt_cell cells[6]; /* 2 rows x 3 cols */
	char buf[8192];
	size_t len;
	int i;

	TEST("render_cells_full outputs cell content");
	r = render_new(2, 3, NULL);
	ASSERT(r != NULL, "render new failed");

	for (i = 0; i < 6; i++)
		vt_cell_clear(&cells[i]);
	cells[0].codepoint = 'A';
	cells[0].width = 1;
	cells[1].codepoint = 'B';
	cells[1].width = 1;

	capture_open(&cap);
	ASSERT(cap.wfd >= 0, "pipe failed");
	render_cells_full(r, cap.wfd, cells, 2, 3, 0, 2, 1);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no output");
	ASSERT(strstr(buf, "AB") != NULL, "output missing 'AB'");

	render_free(r);
	PASS();
}

static void
test_render_cells_diff(void)
{
	struct render *r;
	struct capture cap;
	struct vt_cell cells[6]; /* 2 rows x 3 cols */
	char buf[8192];
	size_t len;
	int i;

	TEST("render_cells_diff emits only changes");
	r = render_new(2, 3, NULL);
	ASSERT(r != NULL, "render new failed");

	for (i = 0; i < 6; i++)
		vt_cell_clear(&cells[i]);
	cells[0].codepoint = 'X';
	cells[0].width = 1;

	/* initial full render */
	capture_open(&cap);
	render_cells_full(r, cap.wfd, cells, 2, 3, 0, 0, 0);
	capture_read(&cap, buf, sizeof(buf));

	/* change cell[1] */
	cells[1].codepoint = 'Y';
	cells[1].width = 1;

	capture_open(&cap);
	render_cells_diff(r, cap.wfd, cells, 2, 3, 0, 0, 0, NULL);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no diff output");
	ASSERT(strstr(buf, "Y") != NULL, "diff missing changed 'Y'");
	/* X should not be re-emitted */
	ASSERT(strstr(buf, "X") == NULL,
	    "diff should not re-emit unchanged 'X'");

	render_free(r);
	PASS();
}

static void
test_render_move_cursor(void)
{
	struct render *r;
	struct capture cap;
	char buf[8192];
	size_t len;

	TEST("render_move_cursor emits CUP");
	r = render_new(24, 80, NULL);
	ASSERT(r != NULL, "render new failed");

	capture_open(&cap);
	render_move_cursor(r, cap.wfd, 3, 7);
	tio_flush(cap.wfd);
	len = capture_read(&cap, buf, sizeof(buf));

	ASSERT(len > 0, "no output");
	ASSERT(strstr(buf, "\033[4;8H") != NULL,
	    "missing CUP ESC[4;8H");

	/* calling again with same position should emit nothing */
	capture_open(&cap);
	render_move_cursor(r, cap.wfd, 3, 7);
	tio_flush(cap.wfd);
	len = capture_read(&cap, buf, sizeof(buf));
	ASSERT(len == 0, "duplicate CUP emitted");

	render_free(r);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	rune_width_init();

	printf("librender tests:\n");

	test_render_new_free();
	test_render_full_basic();
	test_render_diff_skip_unchanged();
	test_render_diff_emits_changes();
	test_render_sgr_output();
	test_render_color_output();
	test_render_cursor_position();
	test_render_resize();
	test_render_cells_full();
	test_render_cells_diff();
	test_render_move_cursor();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
