/* test_vt.c : tests for libvt */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_parse.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "vt_state.h"
#include "vt_ops.h"
#include "rune_width.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---- buffer tests ---- */

static void
test_buf_new_free(void)
{
	struct vt_buf *buf;

	TEST("buf new/free");
	buf = vt_buf_new(24, 80, 100);
	ASSERT(buf != NULL, "vt_buf_new returned NULL");
	ASSERT(vt_buf_rows(buf) == 24, "wrong rows");
	ASSERT(vt_buf_cols(buf) == 80, "wrong cols");
	vt_buf_free(buf);
	PASS();
}

static void
test_buf_cell_access(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	TEST("buf cell access");
	buf = vt_buf_new(24, 80, 0);
	ASSERT(buf != NULL, "buf new failed");

	c = vt_buf_cell(buf, 0, 0);
	ASSERT(c != NULL, "cell(0,0) is NULL");
	ASSERT(c->codepoint == ' ', "initial cell not space");

	/* write and read back */
	c->codepoint = 'A';
	c = vt_buf_cell(buf, 0, 0);
	ASSERT(c->codepoint == 'A', "cell read-back failed");

	/* out of bounds */
	ASSERT(vt_buf_cell(buf, -1, 0) == NULL, "negative row not NULL");
	ASSERT(vt_buf_cell(buf, 24, 0) == NULL, "row past end not NULL");
	ASSERT(vt_buf_cell(buf, 0, 80) == NULL, "col past end not NULL");

	vt_buf_free(buf);
	PASS();
}

static void
test_buf_scroll_up(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	TEST("buf scroll up");
	buf = vt_buf_new(3, 10, 100);
	ASSERT(buf != NULL, "buf new failed");

	/* put markers in each row */
	vt_buf_cell(buf, 0, 0)->codepoint = '0';
	vt_buf_cell(buf, 1, 0)->codepoint = '1';
	vt_buf_cell(buf, 2, 0)->codepoint = '2';

	/* scroll up 1 line -- row 0 goes to scrollback */
	vt_buf_scroll(buf, 0, 3, 1);

	c = vt_buf_cell(buf, 0, 0);
	ASSERT(c->codepoint == '1', "row 0 should be old row 1");
	c = vt_buf_cell(buf, 1, 0);
	ASSERT(c->codepoint == '2', "row 1 should be old row 2");
	c = vt_buf_cell(buf, 2, 0);
	ASSERT(c->codepoint == ' ', "row 2 should be blank");

	/* check scrollback */
	ASSERT(vt_buf_scrollback_lines(buf) == 1, "should have 1 scrollback");
	{
		struct vt_row *sr = vt_buf_scrollback_row(buf, -1);

		ASSERT(sr != NULL, "scrollback row is NULL");
		ASSERT(sr->cells[0].codepoint == '0',
		    "scrollback content wrong");
	}

	vt_buf_free(buf);
	PASS();
}

static void
test_buf_scroll_down(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	TEST("buf scroll down");
	buf = vt_buf_new(3, 10, 0);
	ASSERT(buf != NULL, "buf new failed");

	vt_buf_cell(buf, 0, 0)->codepoint = 'A';
	vt_buf_cell(buf, 1, 0)->codepoint = 'B';
	vt_buf_cell(buf, 2, 0)->codepoint = 'C';

	/* scroll down 1 */
	vt_buf_scroll(buf, 0, 3, -1);

	c = vt_buf_cell(buf, 0, 0);
	ASSERT(c->codepoint == ' ', "row 0 should be blank");
	c = vt_buf_cell(buf, 1, 0);
	ASSERT(c->codepoint == 'A', "row 1 should be old row 0");
	c = vt_buf_cell(buf, 2, 0);
	ASSERT(c->codepoint == 'B', "row 2 should be old row 1");

	vt_buf_free(buf);
	PASS();
}

static void
test_buf_resize(void)
{
	struct vt_buf *buf;
	struct vt_cell *c;

	TEST("buf resize preserves content");
	buf = vt_buf_new(24, 80, 0);
	ASSERT(buf != NULL, "buf new failed");

	vt_buf_cell(buf, 0, 0)->codepoint = 'X';

	ASSERT(vt_buf_resize(buf, 30, 120) == 0, "resize failed");
	ASSERT(vt_buf_rows(buf) == 30, "wrong rows after resize");
	ASSERT(vt_buf_cols(buf) == 120, "wrong cols after resize");

	c = vt_buf_cell(buf, 0, 0);
	ASSERT(c->codepoint == 'X', "content lost after resize");

	vt_buf_free(buf);
	PASS();
}

/* ---- state tests ---- */

static void
test_state_new_free(void)
{
	struct vt_state *st;

	TEST("state new/free");
	st = vt_state_new(24, 80, 2000);
	ASSERT(st != NULL, "vt_state_new returned NULL");
	ASSERT(st->cursor_row == 0, "cursor_row not 0");
	ASSERT(st->cursor_col == 0, "cursor_col not 0");
	ASSERT(st->modes & VT_MODE_AUTOWRAP, "autowrap not set");
	vt_state_free(st);
	PASS();
}

static void
test_state_putchar(void)
{
	struct vt_state *st;
	struct vt_cell *c;

	TEST("state putchar");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	vt_state_putchar(st, 'H', 1);
	vt_state_putchar(st, 'i', 1);

	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == 'H', "cell 0 wrong");
	c = vt_buf_cell(st->buf, 0, 1);
	ASSERT(c->codepoint == 'i', "cell 1 wrong");
	ASSERT(st->cursor_col == 2, "cursor_col wrong");

	vt_state_free(st);
	PASS();
}

static void
test_state_autowrap(void)
{
	struct vt_state *st;
	struct vt_cell *c;
	int i;

	TEST("state autowrap");
	st = vt_state_new(3, 5, 0);
	ASSERT(st != NULL, "state new failed");

	/* fill first row */
	for (i = 0; i < 5; i++)
		vt_state_putchar(st, 'A' + i, 1);

	/* next char should wrap */
	vt_state_putchar(st, 'F', 1);

	ASSERT(st->cursor_row == 1, "should be on row 1");
	ASSERT(st->cursor_col == 1, "should be at col 1");
	c = vt_buf_cell(st->buf, 1, 0);
	ASSERT(c->codepoint == 'F', "wrapped char wrong");

	vt_state_free(st);
	PASS();
}

static void
test_state_altscreen(void)
{
	struct vt_state *st;
	struct vt_cell *c;

	TEST("state altscreen enter/leave");
	st = vt_state_new(24, 80, 100);
	ASSERT(st != NULL, "state new failed");

	/* write on primary */
	vt_state_putchar(st, 'P', 1);

	vt_state_altscreen_enter(st);
	ASSERT(st->modes & VT_MODE_ALTSCREEN, "altscreen mode not set");

	/* alt screen should be blank */
	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == ' ', "alt screen not blank");

	/* write on alt */
	vt_state_putchar(st, 'A', 1);

	vt_state_altscreen_leave(st);
	ASSERT(!(st->modes & VT_MODE_ALTSCREEN), "altscreen mode still set");

	/* primary should still have 'P' */
	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == 'P', "primary content lost");

	vt_state_free(st);
	PASS();
}

static void
test_state_cursor_save_restore(void)
{
	struct vt_state *st;

	TEST("state cursor save/restore");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	st->cursor_row = 5;
	st->cursor_col = 10;
	st->attrs = VT_ATTR_BOLD;

	vt_state_cursor_save(st);

	st->cursor_row = 0;
	st->cursor_col = 0;
	st->attrs = 0;

	vt_state_cursor_restore(st);

	ASSERT(st->cursor_row == 5, "row not restored");
	ASSERT(st->cursor_col == 10, "col not restored");
	ASSERT(st->attrs & VT_ATTR_BOLD, "attrs not restored");

	vt_state_free(st);
	PASS();
}

static void
test_state_tabs(void)
{
	struct vt_state *st;

	TEST("state tab stops");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	/* default tabs every 8 columns */
	ASSERT(vt_state_tab_next(st, 0) == 8, "tab 0 -> 8");
	ASSERT(vt_state_tab_next(st, 7) == 8, "tab 7 -> 8");
	ASSERT(vt_state_tab_next(st, 8) == 16, "tab 8 -> 16");

	/* set custom tab */
	vt_state_tab_set(st, 3);
	ASSERT(vt_state_tab_next(st, 0) == 3, "custom tab at 3");

	/* clear it */
	vt_state_tab_clear(st, 3);
	ASSERT(vt_state_tab_next(st, 0) == 8, "tab back to 8");

	vt_state_free(st);
	PASS();
}

/* ---- parser tests ---- */

static uint32_t printed_chars[64];
static int printed_count;

static void
test_print_cb(void *ctx, uint32_t cp, int width)
{
	(void)ctx;
	(void)width;
	if (printed_count < 64)
		printed_chars[printed_count++] = cp;
}

static uint8_t executed_chars[64];
static int executed_count;

static void
test_execute_cb(void *ctx, uint8_t c)
{
	(void)ctx;
	if (executed_count < 64)
		executed_chars[executed_count++] = c;
}

static int csi_final_char;
static int csi_params[16];
static int csi_nparam;
static int csi_intermed;

static void
test_csi_cb(void *ctx, const int *params, int nparam, int intermed,
    int final)
{
	int i;

	(void)ctx;
	csi_final_char = final;
	csi_intermed = intermed;
	csi_nparam = nparam;
	for (i = 0; i < nparam && i < 16; i++)
		csi_params[i] = params[i];
}

static int esc_intermed;
static int esc_final_char;

static void
test_esc_cb(void *ctx, int intermed, int final)
{
	(void)ctx;
	esc_intermed = intermed;
	esc_final_char = final;
}

static const struct vt_ops test_ops = {
	.print = test_print_cb,
	.execute = test_execute_cb,
	.csi = test_csi_cb,
	.esc = test_esc_cb,
	.osc = NULL,
};

/* DCS callback test state */
static char dcs_data[4096];
static size_t dcs_data_len;
static int dcs_intro;
static int dcs_call_count;

static void
test_dcs_cb(void *ctx, int introducer, const char *data, size_t len)
{
	(void)ctx;
	dcs_intro = introducer;
	if (len > sizeof(dcs_data))
		len = sizeof(dcs_data);
	memcpy(dcs_data, data, len);
	dcs_data_len = len;
	dcs_call_count++;
}

static void
test_parse_printable(void)
{
	struct vt_parse *p;

	TEST("parse printable ASCII");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	printed_count = 0;
	vt_parse_feed(p, "Hello", 5);

	ASSERT(printed_count == 5, "wrong print count");
	ASSERT(printed_chars[0] == 'H', "wrong char 0");
	ASSERT(printed_chars[4] == 'o', "wrong char 4");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_c0_controls(void)
{
	struct vt_parse *p;

	TEST("parse C0 controls");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	executed_count = 0;
	vt_parse_feed(p, "\r\n\t", 3);

	ASSERT(executed_count == 3, "wrong execute count");
	ASSERT(executed_chars[0] == '\r', "wrong control 0");
	ASSERT(executed_chars[1] == '\n', "wrong control 1");
	ASSERT(executed_chars[2] == '\t', "wrong control 2");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_csi_cursor_up(void)
{
	struct vt_parse *p;

	TEST("parse CSI cursor up");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	csi_final_char = 0;
	csi_nparam = 0;
	/* ESC [ 5 A */
	vt_parse_feed(p, "\033[5A", 4);

	ASSERT(csi_final_char == 'A', "wrong final");
	ASSERT(csi_nparam == 1, "wrong nparam");
	ASSERT(csi_params[0] == 5, "wrong param");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_csi_sgr(void)
{
	struct vt_parse *p;

	TEST("parse CSI SGR multiple params");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	csi_final_char = 0;
	csi_nparam = 0;
	/* ESC [ 1 ; 3 1 m -- bold + red fg */
	vt_parse_feed(p, "\033[1;31m", 7);

	ASSERT(csi_final_char == 'm', "wrong final");
	ASSERT(csi_nparam == 2, "wrong nparam");
	ASSERT(csi_params[0] == 1, "param 0 wrong");
	ASSERT(csi_params[1] == 31, "param 1 wrong");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_csi_private(void)
{
	struct vt_parse *p;

	TEST("parse CSI private mode (DECSET)");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	csi_final_char = 0;
	csi_intermed = 0;
	/* ESC [ ? 1 0 4 9 h */
	vt_parse_feed(p, "\033[?1049h", 8);

	ASSERT(csi_final_char == 'h', "wrong final");
	ASSERT(csi_intermed == '?', "wrong intermediate");
	ASSERT(csi_nparam == 1, "wrong nparam");
	ASSERT(csi_params[0] == 1049, "wrong param");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_csi_no_params(void)
{
	struct vt_parse *p;

	TEST("parse CSI with no params");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	csi_final_char = 0;
	csi_nparam = 0;
	/* ESC [ H -- cursor home, no params */
	vt_parse_feed(p, "\033[H", 3);

	ASSERT(csi_final_char == 'H', "wrong final");
	/* default param: -1 (unset) */
	ASSERT(csi_nparam == 1, "should have 1 default param");
	ASSERT(csi_params[0] == -1, "default param should be -1");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_esc_sequence(void)
{
	struct vt_parse *p;

	TEST("parse ESC sequence");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	esc_final_char = 0;
	esc_intermed = 0;
	/* ESC 7 -- save cursor */
	vt_parse_feed(p, "\0337", 2);

	ASSERT(esc_final_char == '7', "wrong final");
	ASSERT(esc_intermed == 0, "should have no intermediate");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_esc_charset(void)
{
	struct vt_parse *p;

	TEST("parse ESC charset designation");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	esc_final_char = 0;
	esc_intermed = 0;
	/* ESC ( 0 -- G0 = line drawing */
	vt_parse_feed(p, "\033(0", 3);

	ASSERT(esc_final_char == '0', "wrong final");
	ASSERT(esc_intermed == '(', "wrong intermediate");

	vt_parse_free(p);
	PASS();
}

static void
test_parse_utf8(void)
{
	struct vt_parse *p;

	TEST("parse UTF-8 multibyte");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");

	printed_count = 0;
	/* U+00E9 = e-acute = C3 A9 */
	vt_parse_feed(p, "\xC3\xA9", 2);

	ASSERT(printed_count == 1, "should produce 1 codepoint");
	ASSERT(printed_chars[0] == 0x00E9, "wrong codepoint");

	vt_parse_free(p);
	PASS();
}

/* ---- DCS passthrough tests ---- */

static void
test_dcs_basic_esc_st(void)
{
	struct vt_parse *p;

	TEST("DCS terminated by ESC backslash");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	vt_parse_set_dcs_cb(p, test_dcs_cb, NULL);

	dcs_call_count = 0;
	dcs_data_len = 0;
	/* ESC P q #0;2;0;0;0 ESC \ */
	vt_parse_feed(p, "\033Pq#0;2;0;0;0\033\\", 16);

	ASSERT(dcs_call_count == 1, "callback should fire once");
	ASSERT(dcs_intro == 'P', "introducer should be 'P'");
	ASSERT(dcs_data_len == 11, "wrong data length");
	ASSERT(memcmp(dcs_data, "q#0;2;0;0;0", 11) == 0,
	    "wrong data content");

	vt_parse_free(p);
	PASS();
}

static void
test_dcs_basic_9c(void)
{
	struct vt_parse *p;

	TEST("DCS terminated by 0x9C");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	vt_parse_set_dcs_cb(p, test_dcs_cb, NULL);

	dcs_call_count = 0;
	dcs_data_len = 0;
	/* 0x9C is NOT treated as ST in UTF-8 mode -- it collides
	 * with valid UTF-8 continuation bytes.  the byte is
	 * accumulated as data; ESC \ is needed to terminate. */
	vt_parse_feed(p, "\033Phello\x9C", 8);

	ASSERT(dcs_call_count == 0, "0x9C should not terminate DCS");

	/* now terminate properly with ESC \ */
	vt_parse_feed(p, "\033\\", 2);
	ASSERT(dcs_call_count == 1, "ESC \\ should terminate DCS");
	ASSERT(dcs_intro == 'P', "introducer should be 'P'");
	ASSERT(dcs_data_len == 6, "data should include 0x9C byte");
	ASSERT(memcmp(dcs_data, "hello\x9C", 6) == 0, "wrong content");

	vt_parse_free(p);
	PASS();
}

static void
test_dcs_no_callback(void)
{
	struct vt_parse *p;

	TEST("DCS without callback does not crash");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	/* no dcs_cb set -- should silently consume */

	vt_parse_feed(p, "\033Pdata\033\\", 8);

	/* just verify parser returns to ground */
	printed_count = 0;
	vt_parse_feed(p, "A", 1);
	ASSERT(printed_count == 1, "parser stuck after DCS");
	ASSERT(printed_chars[0] == 'A', "wrong char after DCS");

	vt_parse_free(p);
	PASS();
}

static void
test_dcs_apc(void)
{
	struct vt_parse *p;

	TEST("APC sequence passthrough");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	vt_parse_set_dcs_cb(p, test_dcs_cb, NULL);

	dcs_call_count = 0;
	/* ESC _ (APC) test ESC \ */
	vt_parse_feed(p, "\033_test\033\\", 9);

	ASSERT(dcs_call_count == 1, "callback should fire");
	ASSERT(dcs_intro == '_', "introducer should be '_'");
	ASSERT(dcs_data_len == 4, "wrong length");

	vt_parse_free(p);
	PASS();
}

static void
test_dcs_empty(void)
{
	struct vt_parse *p;

	TEST("DCS empty payload not emitted");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	vt_parse_set_dcs_cb(p, test_dcs_cb, NULL);

	dcs_call_count = 0;
	/* ESC P ESC \ -- empty DCS */
	vt_parse_feed(p, "\033P\033\\", 4);

	ASSERT(dcs_call_count == 0, "empty DCS should not fire callback");

	vt_parse_free(p);
	PASS();
}

static void
test_dcs_normal_after(void)
{
	struct vt_parse *p;

	TEST("normal input resumes after DCS");
	p = vt_parse_new(&test_ops, NULL);
	ASSERT(p != NULL, "parse new failed");
	vt_parse_set_dcs_cb(p, test_dcs_cb, NULL);

	printed_count = 0;
	dcs_call_count = 0;
	vt_parse_feed(p, "\033Pdata\033\\XY", 11);

	ASSERT(dcs_call_count == 1, "DCS should fire");
	ASSERT(printed_count == 2, "should print X and Y");
	ASSERT(printed_chars[0] == 'X', "first char wrong");
	ASSERT(printed_chars[1] == 'Y', "second char wrong");

	vt_parse_free(p);
	PASS();
}

/* ---- integrated parser + state tests ---- */

static void
test_integrated_cursor_movement(void)
{
	struct vt_state *st;
	struct vt_parse *p;

	TEST("integrated: cursor movement");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* ESC [ 5 ; 1 0 H -- cursor to row 5, col 10 */
	vt_parse_feed(p, "\033[5;10H", 7);
	ASSERT(st->cursor_row == 4, "row should be 4 (0-based)");
	ASSERT(st->cursor_col == 9, "col should be 9 (0-based)");

	/* ESC [ 3 A -- up 3 */
	vt_parse_feed(p, "\033[3A", 4);
	ASSERT(st->cursor_row == 1, "row should be 1 after up 3");

	/* ESC [ 2 B -- down 2 */
	vt_parse_feed(p, "\033[2B", 4);
	ASSERT(st->cursor_row == 3, "row should be 3 after down 2");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

static void
test_integrated_erase(void)
{
	struct vt_state *st;
	struct vt_parse *p;
	struct vt_cell *c;

	TEST("integrated: erase line");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* print some text */
	vt_parse_feed(p, "Hello World", 11);

	/* move to col 5 */
	vt_parse_feed(p, "\033[1;6H", 6);

	/* erase to end of line */
	vt_parse_feed(p, "\033[K", 3);

	/* cols 0-4 should still have text */
	c = vt_buf_cell(st->buf, 0, 4);
	ASSERT(c->codepoint == 'o', "col 4 should be 'o'");

	/* col 5 should be erased */
	c = vt_buf_cell(st->buf, 0, 5);
	ASSERT(c->codepoint == ' ', "col 5 should be blank");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

static void
test_integrated_sgr(void)
{
	struct vt_state *st;
	struct vt_parse *p;
	struct vt_cell *c;

	TEST("integrated: SGR attributes");
	st = vt_state_new(24, 80, 0);
	ASSERT(st != NULL, "state new failed");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* bold + red fg */
	vt_parse_feed(p, "\033[1;31mX", 8);

	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == 'X', "wrong char");
	ASSERT(c->attrs & VT_ATTR_BOLD, "not bold");
	ASSERT(c->fg.type == VT_COLOR_INDEXED, "not indexed color");
	ASSERT(c->fg.index == 1, "not red (index 1)");

	/* reset */
	vt_parse_feed(p, "\033[0mY", 5);
	c = vt_buf_cell(st->buf, 0, 1);
	ASSERT(c->codepoint == 'Y', "wrong char after reset");
	ASSERT(c->attrs == 0, "attrs not reset");
	ASSERT(c->fg.type == VT_COLOR_DEFAULT, "fg not default");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

static void
test_integrated_scroll_region(void)
{
	struct vt_state *st;
	struct vt_parse *p;
	struct vt_cell *c;

	TEST("integrated: scroll region");
	st = vt_state_new(5, 10, 0);
	ASSERT(st != NULL, "state new failed");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* put markers on rows */
	vt_parse_feed(p, "\033[1;1H0\033[2;1H1\033[3;1H2\033[4;1H3\033[5;1H4",
	    5 * 7);

	/* set scroll region to rows 2-4 (1-based) */
	vt_parse_feed(p, "\033[2;4r", 6);

	/* cursor to row 4 (bottom of scroll region) */
	vt_parse_feed(p, "\033[4;1H", 6);

	/* LF should scroll within region */
	vt_parse_feed(p, "\n", 1);

	/* row 0 (outside region) should be untouched */
	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == '0', "row 0 should be untouched");

	/* row 4 (outside region) should be untouched */
	c = vt_buf_cell(st->buf, 4, 0);
	ASSERT(c->codepoint == '4', "row 4 should be untouched");

	/* row 1 should now have what was on row 2 */
	c = vt_buf_cell(st->buf, 1, 0);
	ASSERT(c->codepoint == '2', "row 1 should have old row 2");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

static void
test_integrated_altscreen(void)
{
	struct vt_state *st;
	struct vt_parse *p;
	struct vt_cell *c;

	TEST("integrated: alt screen via DECSET/DECRST");
	st = vt_state_new(24, 80, 100);
	ASSERT(st != NULL, "state new failed");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* write on primary */
	vt_parse_feed(p, "Primary", 7);

	/* enter alt screen */
	vt_parse_feed(p, "\033[?1049h", 8);
	ASSERT(st->modes & VT_MODE_ALTSCREEN, "not in alt screen");

	/* alt should be blank */
	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == ' ', "alt not blank");

	/* leave alt screen */
	vt_parse_feed(p, "\033[?1049l", 8);
	ASSERT(!(st->modes & VT_MODE_ALTSCREEN), "still in alt screen");

	/* primary content preserved */
	c = vt_buf_cell(st->buf, 0, 0);
	ASSERT(c->codepoint == 'P', "primary content lost");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

static void
test_integrated_cursor_shape(void)
{
	struct vt_state *st;
	struct vt_parse *p;

	TEST("integrated: DECSCUSR cursor shape");
	st = vt_state_new(24, 80, 100);
	ASSERT(st != NULL, "state new failed");
	ASSERT(st->cursor_shape == 0, "default shape not 0");

	p = vt_parse_new(vt_ops_default(), st);
	ASSERT(p != NULL, "parse new failed");

	/* CSI 5 SP q -> blinking bar */
	vt_parse_feed(p, "\033[5 q", 5);
	ASSERT(st->cursor_shape == 5, "shape not 5 (blinking bar)");

	/* CSI 2 SP q -> steady block */
	vt_parse_feed(p, "\033[2 q", 5);
	ASSERT(st->cursor_shape == 2, "shape not 2 (steady block)");

	/* CSI 0 SP q -> default */
	vt_parse_feed(p, "\033[0 q", 5);
	ASSERT(st->cursor_shape == 0, "shape not 0 (default)");

	vt_parse_free(p);
	vt_state_free(st);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	rune_width_init();

	printf("libvt tests:\n");

	/* buffer */
	test_buf_new_free();
	test_buf_cell_access();
	test_buf_scroll_up();
	test_buf_scroll_down();
	test_buf_resize();

	/* state */
	test_state_new_free();
	test_state_putchar();
	test_state_autowrap();
	test_state_altscreen();
	test_state_cursor_save_restore();
	test_state_tabs();

	/* parser (isolated) */
	test_parse_printable();
	test_parse_c0_controls();
	test_parse_csi_cursor_up();
	test_parse_csi_sgr();
	test_parse_csi_private();
	test_parse_csi_no_params();
	test_parse_esc_sequence();
	test_parse_esc_charset();
	test_parse_utf8();

	/* DCS passthrough */
	test_dcs_basic_esc_st();
	test_dcs_basic_9c();
	test_dcs_no_callback();
	test_dcs_apc();
	test_dcs_empty();
	test_dcs_normal_after();

	/* integrated */
	test_integrated_cursor_movement();
	test_integrated_erase();
	test_integrated_sgr();
	test_integrated_scroll_region();
	test_integrated_altscreen();
	test_integrated_cursor_shape();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
