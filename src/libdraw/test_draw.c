/* test_draw.c : tests for libdraw */

#include "draw.h"
#include "draw_driver.h"
#include "tkbd.h"

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

/* ---- mock driver ---- */

#define MOCK_MAX 4096
#define MOCK_ROWS 256

struct mock {
	int		rows, cols;
	int		begin_calls, end_calls, present_calls, poll_calls;
	int		poll_timeout;
	int		last_rows, last_cols;
	int		cur_row, cur_col, cur_vis, cur_shape;
	int		dirty_null;
	uint8_t		dirty[MOCK_ROWS];
	struct vt_cell	cells[MOCK_MAX];
	int		clip_calls;
	size_t		clip_len;
	char		clip[256];
	struct tkbd_seq	feed[16];	/* events to dispatch on next poll */
	int		feed_n;
	int		feed_eof;	/* poll reports EOF */
	int		pending_sig;	/* bits returned by the next signals() */
	int		suspend_calls;
};

static int
mock_size(void *ctx, int *rows, int *cols)
{
	struct mock *m = ctx;

	*rows = m->rows;
	*cols = m->cols;
	return 0;
}

static void
mock_begin(void *ctx)
{
	((struct mock *)ctx)->begin_calls++;
}

static void
mock_end(void *ctx)
{
	((struct mock *)ctx)->end_calls++;
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
	m->cur_row = f->cur_row;
	m->cur_col = f->cur_col;
	m->cur_vis = f->cur_vis;
	m->cur_shape = f->cur_shape;
	m->dirty_null = f->row_dirty == NULL;
	if (f->row_dirty && f->rows <= MOCK_ROWS)
		memcpy(m->dirty, f->row_dirty, (size_t)f->rows);
}

static int
mock_poll(void *ctx, int timeout_ms,
    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u)
{
	struct mock *m = ctx;
	int i, n;

	m->poll_calls++;
	m->poll_timeout = timeout_ms;
	if (m->feed_eof)
		return -1;
	n = m->feed_n;
	for (i = 0; i < n; i++)
		if (on_event)
			on_event(u, &m->feed[i]);
	m->feed_n = 0;			/* consumed */
	return n;
}

/* Queue a single-key event to be delivered by the next poll. */
static void
mock_feed_key(struct mock *m, uint32_t ch)
{
	struct tkbd_seq *s;

	if (m->feed_n >= (int)(sizeof(m->feed) / sizeof(m->feed[0])))
		return;
	s = &m->feed[m->feed_n++];
	memset(s, 0, sizeof(*s));
	s->type = TKBD_KEY;
	s->ch = ch;
}

static void
mock_clip(void *ctx, const char *utf8, size_t n)
{
	struct mock *m = ctx;

	m->clip_calls++;
	if (n >= sizeof(m->clip))
		n = sizeof(m->clip) - 1;
	memcpy(m->clip, utf8, n);
	m->clip[n] = '\0';
	m->clip_len = n;
}

static void
mock_free(void *ctx)
{
	(void)ctx;			/* mock is caller-owned */
}

static int
mock_signals(void *ctx)
{
	struct mock *m = ctx;
	int bits = m->pending_sig;

	m->pending_sig = 0;
	return bits;
}

static void
mock_suspend(void *ctx)
{
	((struct mock *)ctx)->suspend_calls++;
}

static const struct draw_driver mock_driver = {
	.begin = mock_begin,
	.end = mock_end,
	.size = mock_size,
	.present = mock_present,
	.poll = mock_poll,
	.free = mock_free,
	.set_clipboard = mock_clip,
	.signals = mock_signals,
	.suspend = mock_suspend,
};

static void
mock_init(struct mock *m, int rows, int cols)
{
	memset(m, 0, sizeof(*m));
	m->rows = rows;
	m->cols = cols;
}

static const struct vt_cell *
mcell(struct mock *m, int r, int c)
{
	return &m->cells[r * m->last_cols + c];
}

/* ---- tests ---- */

static const struct vt_color def = { .type = VT_COLOR_DEFAULT };

static void
test_new_size(void)
{
	struct mock m;
	struct draw *d;
	int rows = 0, cols = 0;

	TEST("draw_new queries size from driver");
	mock_init(&m, 10, 20);
	d = draw_new(&mock_driver, &m);
	ASSERT(d != NULL, "draw_new returned NULL");
	draw_size(d, &rows, &cols);
	ASSERT(rows == 10 && cols == 20, "size mismatch");
	draw_free(d);
	PASS();
}

static void
test_clear_default(void)
{
	struct mock m;
	struct draw *d;

	TEST("draw_clear resets to default cells");
	mock_init(&m, 4, 6);
	d = draw_new(&mock_driver, &m);
	draw_clear(d);
	draw_present(d);
	ASSERT(m.present_calls == 1, "present not called");
	ASSERT(mcell(&m, 0, 0)->codepoint == ' ', "not a space");
	ASSERT(mcell(&m, 0, 0)->width == 1, "width not 1");
	ASSERT(mcell(&m, 3, 5)->fg.type == VT_COLOR_DEFAULT, "fg not default");
	draw_free(d);
	PASS();
}

static void
test_cell(void)
{
	struct mock m;
	struct draw *d;
	struct vt_color fg = { .type = VT_COLOR_INDEXED, { .index = 5 } };

	TEST("draw_cell writes one cell");
	mock_init(&m, 5, 8);
	d = draw_new(&mock_driver, &m);
	draw_cell(d, 2, 3, 'A', fg, def, VT_ATTR_BOLD);
	draw_present(d);
	ASSERT(mcell(&m, 2, 3)->codepoint == 'A', "codepoint mismatch");
	ASSERT(mcell(&m, 2, 3)->fg.index == 5, "fg mismatch");
	ASSERT(mcell(&m, 2, 3)->attrs == VT_ATTR_BOLD, "attrs mismatch");
	draw_free(d);
	PASS();
}

static void
test_cell_oob(void)
{
	struct mock m;
	struct draw *d;

	TEST("draw_cell out of bounds is a no-op");
	mock_init(&m, 3, 4);
	d = draw_new(&mock_driver, &m);
	draw_cell(d, -1, 0, 'X', def, def, 0);
	draw_cell(d, 3, 0, 'X', def, def, 0);
	draw_cell(d, 0, 4, 'X', def, def, 0);
	draw_present(d);
	ASSERT(mcell(&m, 0, 0)->codepoint == ' ', "cell modified");
	ASSERT(mcell(&m, 2, 3)->codepoint == ' ', "cell modified");
	draw_free(d);
	PASS();
}

static void
test_text_ascii(void)
{
	struct mock m;
	struct draw *d;
	int end;

	TEST("draw_text writes an ASCII run");
	mock_init(&m, 2, 10);
	d = draw_new(&mock_driver, &m);
	end = draw_text(d, 0, 2, "hi", def, def, 0);
	draw_present(d);
	ASSERT(end == 4, "end column mismatch");
	ASSERT(mcell(&m, 0, 2)->codepoint == 'h', "char 0 mismatch");
	ASSERT(mcell(&m, 0, 3)->codepoint == 'i', "char 1 mismatch");
	draw_free(d);
	PASS();
}

static void
test_text_clip(void)
{
	struct mock m;
	struct draw *d;
	int end;

	TEST("draw_text clips at the right edge");
	mock_init(&m, 1, 4);
	d = draw_new(&mock_driver, &m);
	end = draw_text(d, 0, 0, "abcdef", def, def, 0);
	draw_present(d);
	ASSERT(end == 4, "should stop at cols");
	ASSERT(mcell(&m, 0, 3)->codepoint == 'd', "last visible char");
	draw_free(d);
	PASS();
}

static void
test_text_control(void)
{
	struct mock m;
	struct draw *d;

	TEST("draw_text renders control bytes as space");
	mock_init(&m, 1, 6);
	d = draw_new(&mock_driver, &m);
	draw_text(d, 0, 0, "a\tb", def, def, 0);
	draw_present(d);
	ASSERT(mcell(&m, 0, 0)->codepoint == 'a', "a mismatch");
	ASSERT(mcell(&m, 0, 1)->codepoint == ' ', "tab not a space");
	ASSERT(mcell(&m, 0, 2)->codepoint == 'b', "b mismatch");
	draw_free(d);
	PASS();
}

static void
test_wide(void)
{
	struct mock m;
	struct draw *d;

	TEST("wide glyph sets a continuation cell");
	mock_init(&m, 1, 6);
	d = draw_new(&mock_driver, &m);
	/* U+4E00 CJK is width 2 */
	draw_cell(d, 0, 1, 0x4E00, def, def, 0);
	draw_present(d);
	ASSERT(mcell(&m, 0, 1)->codepoint == 0x4E00, "lead mismatch");
	ASSERT(mcell(&m, 0, 1)->width == 2, "lead width not 2");
	ASSERT(mcell(&m, 0, 2)->width == 0, "continuation width not 0");
	ASSERT(mcell(&m, 0, 2)->codepoint == DRAW_CELL_CONT, "cont marker");
	draw_free(d);
	PASS();
}

static void
test_wide_edge(void)
{
	struct mock m;
	struct draw *d;

	TEST("wide glyph at right edge becomes a space");
	mock_init(&m, 1, 4);
	d = draw_new(&mock_driver, &m);
	draw_cell(d, 0, 3, 0x4E00, def, def, 0);
	draw_present(d);
	ASSERT(mcell(&m, 0, 3)->codepoint == ' ', "edge not blanked");
	ASSERT(mcell(&m, 0, 3)->width == 1, "edge width not 1");
	draw_free(d);
	PASS();
}

static void
test_wide_split(void)
{
	struct mock m;
	struct draw *d;

	TEST("overwriting a wide half blanks its orphan");
	mock_init(&m, 1, 6);
	d = draw_new(&mock_driver, &m);
	draw_cell(d, 0, 0, 0x4E00, def, def, 0);	/* lead 0, cont 1 */
	draw_cell(d, 0, 1, 'X', def, def, 0);		/* overwrite cont */
	draw_present(d);
	ASSERT(mcell(&m, 0, 1)->codepoint == 'X', "X not written");
	ASSERT(mcell(&m, 0, 0)->codepoint == ' ', "orphan lead not blanked");
	ASSERT(mcell(&m, 0, 0)->width == 1, "orphan lead width");
	draw_free(d);
	PASS();
}

static void
test_fill(void)
{
	struct mock m;
	struct draw *d;
	int i;

	TEST("draw_fill fills N cells");
	mock_init(&m, 2, 10);
	d = draw_new(&mock_driver, &m);
	draw_fill(d, 1, 3, 4, '#', def, def, 0);
	draw_present(d);
	for (i = 0; i < 4; i++)
		ASSERT(mcell(&m, 1, 3 + i)->codepoint == '#', "fill mismatch");
	ASSERT(mcell(&m, 1, 2)->codepoint == ' ', "before fill modified");
	ASSERT(mcell(&m, 1, 7)->codepoint == ' ', "after fill modified");
	draw_free(d);
	PASS();
}

static void
test_dirty(void)
{
	struct mock m;
	struct draw *d;

	TEST("dirty marks track written rows");
	mock_init(&m, 4, 8);
	d = draw_new(&mock_driver, &m);
	draw_present(d);			/* first frame clears all dirty */
	draw_cell(d, 2, 0, 'A', def, def, 0);
	draw_present(d);
	ASSERT(m.dirty[2] == 1, "written row not dirty");
	ASSERT(m.dirty[0] == 0, "untouched row marked dirty");
	ASSERT(m.dirty[3] == 0, "untouched row marked dirty");
	draw_free(d);
	PASS();
}

static void
test_cursor(void)
{
	struct mock m;
	struct draw *d;

	TEST("cursor state reaches the driver");
	mock_init(&m, 6, 6);
	d = draw_new(&mock_driver, &m);
	draw_cursor(d, 3, 4);
	draw_cursor_vis(d, 0);
	draw_cursor_shape(d, DRAW_CURSOR_BAR);
	draw_present(d);
	ASSERT(m.cur_row == 3 && m.cur_col == 4, "cursor position mismatch");
	ASSERT(m.cur_vis == 0, "cursor visibility mismatch");
	ASSERT(m.cur_shape == DRAW_CURSOR_BAR, "cursor shape mismatch");
	draw_free(d);
	PASS();
}

static void
test_cursor_clamp(void)
{
	struct mock m;
	struct draw *d;

	TEST("cursor clamps to the grid");
	mock_init(&m, 5, 5);
	d = draw_new(&mock_driver, &m);
	draw_cursor(d, 99, 99);
	draw_present(d);
	ASSERT(m.cur_row == 4 && m.cur_col == 4, "cursor not clamped");
	draw_free(d);
	PASS();
}

static void
test_resize(void)
{
	struct mock m;
	struct draw *d;
	int rows = 0, cols = 0;

	TEST("draw_resize reshapes the grid");
	mock_init(&m, 10, 20);
	d = draw_new(&mock_driver, &m);
	draw_resize(d, 5, 8);
	draw_size(d, &rows, &cols);
	ASSERT(rows == 5 && cols == 8, "cached size mismatch");
	draw_present(d);
	ASSERT(m.last_rows == 5 && m.last_cols == 8, "presented size mismatch");
	draw_free(d);
	PASS();
}

static void
test_begin_end(void)
{
	struct mock m;
	struct draw *d;

	TEST("begin/end forward and repeat");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	draw_begin(d);
	draw_end(d);
	draw_begin(d);			/* resume */
	ASSERT(m.begin_calls == 2, "begin count mismatch");
	ASSERT(m.end_calls == 1, "end count mismatch");
	draw_free(d);
	PASS();
}

static void
test_poll_forward(void)
{
	struct mock m;
	struct draw *d;
	int n;

	TEST("draw_poll forwards to the driver");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	mock_feed_key(&m, 'a');
	mock_feed_key(&m, 'b');
	mock_feed_key(&m, 'c');
	n = draw_poll(d, 250, NULL, NULL);
	ASSERT(n == 3, "poll count not forwarded");
	ASSERT(m.poll_calls == 1, "poll not called");
	ASSERT(m.poll_timeout == 250, "timeout not forwarded");
	draw_free(d);
	PASS();
}

static void
test_next_event(void)
{
	struct mock m;
	struct draw *d;
	struct tkbd_seq seq;

	TEST("draw_next_event drains the queue one at a time");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	mock_feed_key(&m, 'A');
	mock_feed_key(&m, 'B');
	/* first call polls once and returns the head */
	ASSERT(draw_next_event(d, 10, &seq) == 1, "no first event");
	ASSERT(seq.ch == 'A', "first event mismatch");
	ASSERT(m.poll_calls == 1, "should have polled once");
	/* second call returns the queued event without polling */
	ASSERT(draw_next_event(d, 10, &seq) == 1, "no second event");
	ASSERT(seq.ch == 'B', "second event mismatch");
	ASSERT(m.poll_calls == 1, "should not poll while queued");
	/* queue empty: polls again, no feed, reports timeout */
	ASSERT(draw_next_event(d, 10, &seq) == 0, "expected timeout");
	ASSERT(m.poll_calls == 2, "should poll when empty");
	draw_free(d);
	PASS();
}

static void
test_peek_event(void)
{
	struct mock m;
	struct draw *d;
	struct tkbd_seq seq;

	TEST("draw_peek_event does not remove the event");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	mock_feed_key(&m, 'Z');
	ASSERT(draw_peek_event(d, &seq) == 1, "peek found nothing");
	ASSERT(seq.ch == 'Z', "peek value mismatch");
	/* peeking again still shows it */
	ASSERT(draw_peek_event(d, &seq) == 1, "second peek empty");
	ASSERT(seq.ch == 'Z', "second peek mismatch");
	/* next_event removes it */
	ASSERT(draw_next_event(d, 0, &seq) == 1, "next after peek empty");
	ASSERT(seq.ch == 'Z', "next value mismatch");
	ASSERT(draw_peek_event(d, &seq) == 0, "queue should be empty");
	draw_free(d);
	PASS();
}

static void
test_event_eof(void)
{
	struct mock m;
	struct draw *d;
	struct tkbd_seq seq;

	TEST("draw_next_event reports EOF");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	m.feed_eof = 1;
	ASSERT(draw_next_event(d, 10, &seq) == -1, "EOF not reported");
	draw_free(d);
	PASS();
}

static void
test_wait_key(void)
{
	struct mock m;
	struct draw *d;
	struct draw_event ev;

	TEST("draw_wait delivers a key event");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	mock_feed_key(&m, 'k');
	ASSERT(draw_wait(d, &ev) == DRAW_EVENT_KEY, "not a key event");
	ASSERT(ev.key.ch == 'k', "key value mismatch");
	draw_free(d);
	PASS();
}

static void
test_wait_resize(void)
{
	struct mock m;
	struct draw *d;
	struct draw_event ev;
	int rows = 0, cols = 0;

	TEST("draw_wait delivers a resize and resizes the surface");
	mock_init(&m, 10, 20);
	d = draw_new(&mock_driver, &m);
	/* the terminal grew; the driver reports a resize */
	m.rows = 12;
	m.cols = 40;
	m.pending_sig = DRAW_SIG_RESIZE;
	ASSERT(draw_wait(d, &ev) == DRAW_EVENT_RESIZE, "not a resize event");
	draw_size(d, &rows, &cols);
	ASSERT(rows == 12 && cols == 40, "surface not resized");
	draw_free(d);
	PASS();
}

static void
test_wait_suspend(void)
{
	struct mock m;
	struct draw *d;
	struct draw_event ev;

	TEST("draw_wait suspends and resumes");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	m.pending_sig = DRAW_SIG_SUSPEND;
	ASSERT(draw_wait(d, &ev) == DRAW_EVENT_RESUME, "not a resume event");
	ASSERT(m.suspend_calls == 1, "driver suspend not called");
	ASSERT(m.end_calls == 1, "terminal not left before stop");
	ASSERT(m.begin_calls == 1, "terminal not re-entered on resume");
	draw_free(d);
	PASS();
}

static void
test_wait_eof(void)
{
	struct mock m;
	struct draw *d;
	struct draw_event ev;

	TEST("draw_wait reports EOF");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	m.feed_eof = 1;
	ASSERT(draw_wait(d, &ev) == DRAW_EVENT_EOF, "EOF not reported");
	draw_free(d);
	PASS();
}

static void
test_clipboard(void)
{
	struct mock m;
	struct draw *d;

	TEST("draw_set_clipboard forwards to the driver");
	mock_init(&m, 4, 4);
	d = draw_new(&mock_driver, &m);
	draw_set_clipboard(d, "copy", 4);
	ASSERT(m.clip_calls == 1, "clipboard not set");
	ASSERT(m.clip_len == 4 && strcmp(m.clip, "copy") == 0, "clip mismatch");
	draw_free(d);
	PASS();
}

static void
test_optional_null(void)
{
	struct mock m;
	struct draw *d;
	static const struct draw_driver bare = {
		.begin = mock_begin,
		.end = mock_end,
		.size = mock_size,
		.present = mock_present,
		.poll = mock_poll,
		.free = mock_free,
	};

	TEST("NULL optional slots are safe no-ops");
	mock_init(&m, 4, 4);
	d = draw_new(&bare, &m);
	draw_set_clipboard(d, "x", 1);	/* no set_clipboard: no crash */
	draw_set_title(d, "t");
	draw_bell(d);
	ASSERT(m.clip_calls == 0, "clipboard should not be called");
	draw_free(d);
	PASS();
}

int
main(void)
{
	printf("libdraw tests:\n");

	test_new_size();
	test_clear_default();
	test_cell();
	test_cell_oob();
	test_text_ascii();
	test_text_clip();
	test_text_control();
	test_wide();
	test_wide_edge();
	test_wide_split();
	test_fill();
	test_dirty();
	test_cursor();
	test_cursor_clamp();
	test_resize();
	test_begin_end();
	test_poll_forward();
	test_next_event();
	test_peek_event();
	test_event_eof();
	test_wait_key();
	test_wait_resize();
	test_wait_suspend();
	test_wait_eof();
	test_clipboard();
	test_optional_null();

	printf("test_draw: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
