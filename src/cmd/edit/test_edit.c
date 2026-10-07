/* test_edit.c : whitebox tests for lumi edit.
 *
 * The editor core (edit.c) and the vi personality (vi.c) are static-heavy
 * units that share only editor.h, so the test includes both directly to reach
 * their internals and drives them against a mock draw surface instead of a
 * real terminal. Rendering is checked by reading the cell grid the mock
 * captures at present time. */

#include "edit.c"
#include "vi.c"
#include "hex.c"

#include "draw_driver.h"

#include <stdio.h>
#include <string.h>

/* edit.c calls lu_send_input, which lives in the send-input command source
 * rather than a library, so the test provides its own. The send path is not
 * exercised here; this only resolves the symbol. */
enum lu_send_result
lu_send_input(const char *session, pid_t target, const char *data, size_t len)
{
	(void)session;
	(void)target;
	(void)data;
	(void)len;
	return LU_SEND_NO_SESSION;
}

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

/* ---- mock draw driver ---- */

#define MOCK_MAX 8192

struct mock {
	int		rows, cols;
	int		last_cols;
	struct vt_cell	cells[MOCK_MAX];
	int		clip_calls;
	size_t		clip_len;
	char		clip[512];
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
mock_present(void *ctx, const struct draw_frame *f)
{
	struct mock *m = ctx;
	size_t n = (size_t)f->rows * f->cols;

	if (n > MOCK_MAX)
		n = MOCK_MAX;
	memcpy(m->cells, f->cells, n * sizeof(struct vt_cell));
	m->last_cols = f->cols;
}

static int
mock_poll(void *ctx, int timeout_ms,
    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u)
{
	(void)ctx;
	(void)timeout_ms;
	(void)on_event;
	(void)u;
	return 0;			/* no queued input in these tests */
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
mock_noop(void *ctx)
{
	(void)ctx;
}

static const struct draw_driver mock_driver = {
	.begin = mock_noop,
	.end = mock_noop,
	.size = mock_size,
	.present = mock_present,
	.poll = mock_poll,
	.free = mock_noop,
	.set_clipboard = mock_clip,
};

/* ---- editor harness ---- */

/* Build an editor bound to a fresh mock surface of rows by cols. Highlighting
 * is off so rendered codepoints are the buffer text, not styled tokens. */
static void
ed_open(struct editor *e, struct mock *m, int rows, int cols)
{
	memset(e, 0, sizeof(*e));
	memset(m, 0, sizeof(*m));
	m->rows = rows;
	m->cols = cols;
	e->d = draw_new(&mock_driver, m);
	e->t = text_new();
	e->dos_chrome = 1;
	e->hl_on = 0;
	e->mode = MODE_MODELESS;
	e->err_cur = -1;
	draw_size(e->d, &e->rows, &e->cols);
	buf_slot(e);			/* register the initial buffer as 0 */
	buf_save(e, &e->bufs[0]);
}

static void
ed_close(struct editor *e)
{
	int i;

	draw_free(e->d);
	if (e->nbuf > 0) {
		buf_save(e, &e->bufs[e->cur]);
		for (i = 0; i < e->nbuf; i++)
			buf_free_fields(e->bufs[i].t, e->bufs[i].line_state);
	} else {
		buf_free_fields(e->t, e->line_state);
	}
	free(e->bufs);
	free(e->errs);
	free(e->clip);
	for (i = 0; i < 26; i++)
		free(e->vi_regs[i].bytes);
	free(e->vi_dot.ev);
	free(e->vi_rec.ev);
	free(e->hl_buf);
}

/* Fill the buffer from a string; '\n' starts a new line. */
static void
ed_settext(struct editor *e, const char *s)
{
	size_t line = 0, col = 0;

	for (; *s; s++) {
		if (*s == '\n') {
			text_split(e->t, line, col);
			line++;
			col = 0;
		} else {
			text_insert(e->t, line, col, s, 1);
			col++;
		}
	}
}

/* One key event carrying a printable character. */
static struct tkbd_seq
ed_ch(uint32_t ch)
{
	struct tkbd_seq s;

	memset(&s, 0, sizeof(s));
	s.type = TKBD_KEY;
	s.ch = ch;
	return s;
}

/* One key event for a named key (no character), e.g. Esc. */
static struct tkbd_seq
ed_key(uint16_t key)
{
	struct tkbd_seq s;

	memset(&s, 0, sizeof(s));
	s.type = TKBD_KEY;
	s.key = key;
	return s;
}

/* Feed a run of printable characters through vi_dispatch. */
static void
ed_vi_type(struct editor *e, const char *keys)
{
	for (; *keys; keys++) {
		struct tkbd_seq s = ed_ch((uint32_t)(unsigned char)*keys);

		vi_dispatch(e, &s);
	}
}

/* Decode a rendered row into ASCII; non-ASCII cells (box glyphs) read as
 * spaces. out must hold at least cols + 1 bytes. */
static void
ed_row(struct mock *m, int r, char *out, int cap)
{
	int c;

	for (c = 0; c < m->last_cols && c < cap - 1; c++) {
		uint32_t cp = m->cells[r * m->last_cols + c].codepoint;

		out[c] = (cp >= 0x20 && cp < 0x7f) ? (char)cp : ' ';
	}
	out[c] = '\0';
}

/* ---- tests ---- */

static void
test_render_text_and_menubar(void)
{
	struct editor e;
	struct mock m;
	char row[128];

	TEST("render paints the menu bar and buffer text");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello world");
	render(&e, e.d);

	ed_row(&m, 0, row, sizeof(row));
	ASSERT(strstr(row, "File") && strstr(row, "Edit") &&
	    strstr(row, "Help"), "menu bar titles missing from row 0");
	ed_row(&m, CHROME_TOP, row, sizeof(row));
	ASSERT(strstr(row, "hello world"), "buffer text not on first text row");
	ed_close(&e);
	PASS();
}

static void
test_insert_char(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;
	const char *line;
	size_t len = 0;
	char row[128];

	TEST("inserting a char updates the buffer and the screen");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc");
	s = ed_ch('X');
	dispatch(&e, key_to_cmd(&s), &s);

	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "Xabc", 4) == 0, "buffer not 'Xabc'");
	ASSERT(e.cx == 1, "cursor did not advance past the insert");
	render(&e, e.d);
	ed_row(&m, CHROME_TOP, row, sizeof(row));
	ASSERT(strstr(row, "Xabc"), "inserted text not rendered");
	ed_close(&e);
	PASS();
}

static void
test_cursor_move_right(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;

	TEST("CMD_RIGHT advances the cursor one byte");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc");
	s = ed_ch(0);			/* no shift, so no selection */
	dispatch(&e, CMD_RIGHT, &s);
	ASSERT(e.cx == 1, "cursor did not move right");
	ed_close(&e);
	PASS();
}

static void
test_copy_line_no_selection(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;

	TEST("Copy with no selection copies the whole line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello");
	s = ed_ch(0);
	dispatch(&e, CMD_COPY, &s);
	ASSERT(e.clip != NULL && e.clip_len == 6 &&
	    memcmp(e.clip, "hello\n", 6) == 0, "clipboard not 'hello\\n'");
	ASSERT(m.clip_calls >= 1 && strcmp(m.clip, "hello\n") == 0,
	    "OSC 52 mirror did not carry the line");
	ed_close(&e);
	PASS();
}

static void
test_selection_copy(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;

	TEST("Copy of a selection copies just the span");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello world");
	e.sel_active = 1;		/* select "hello" */
	e.ay = 0;
	e.ax = 0;
	e.cy = 0;
	e.cx = 5;
	s = ed_ch(0);
	dispatch(&e, CMD_COPY, &s);
	ASSERT(e.clip != NULL && e.clip_len == 5 &&
	    memcmp(e.clip, "hello", 5) == 0, "selection copy not 'hello'");
	ed_close(&e);
	PASS();
}

static void
test_menu_mnemonic_lookup(void)
{
	TEST("mnemonic lookup resolves titles and items");
	ASSERT(menu_title_by_mnemonic('e') == 1, "'e' should open Edit");
	ASSERT(menu_title_by_mnemonic('h') == MENU_HELP, "'h' should open Help");
	ASSERT(menu_title_by_mnemonic('z') == -1, "'z' matches no menu");
	/* File menu: New, Open, Save, Save As, ---, Next Buffer, Prev
	 * Buffer, Buffer List, ---, Exit. */
	ASSERT(menu_item_by_mnemonic(0, 'a') == 3, "'a' should hit Save As");
	ASSERT(menu_item_by_mnemonic(0, 'b') == 5, "'b' should hit Next Buffer");
	ASSERT(menu_item_by_mnemonic(0, 'p') == 6, "'p' should hit Prev Buffer");
	ASSERT(menu_item_by_mnemonic(0, 'l') == 7, "'l' should hit Buffer List");
	ASSERT(menu_item_by_mnemonic(0, 'x') == 9, "'x' should hit Exit");
	ASSERT(menu_item_by_mnemonic(0, 'q') == -1, "'q' matches no File item");
	PASS();
}

static void
test_dropdown_underline(void)
{
	struct editor e;
	struct mock m;
	int r, c, found = 0;

	TEST("a drop-down underlines its mnemonic letter");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "x");
	render(&e, e.d);		/* establish last_cols */
	draw_dropdown(&e, 0, menu_first(0));	/* File menu open */
	draw_present(e.d);
	for (r = 0; r < 24 && !found; r++)
		for (c = 0; c < m.last_cols; c++) {
			const struct vt_cell *cell =
			    &m.cells[r * m.last_cols + c];

			if (cell->codepoint == 'N' &&
			    (cell->attrs & VT_ATTR_UNDERLINE)) {
				found = 1;
				break;
			}
		}
	ASSERT(found, "New's underlined 'N' mnemonic was not rendered");
	ed_close(&e);
	PASS();
}

static void
test_vi_motion_l(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;

	TEST("vi 'l' moves the cursor right in NORMAL mode");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	s = ed_ch('l');
	vi_dispatch(&e, &s);
	ASSERT(e.cx == 1, "'l' did not advance the cursor");
	ed_close(&e);
	PASS();
}

static void
test_visual_enter(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;

	TEST("v enters charwise visual and anchors the selection");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	s = ed_ch('v');
	vi_dispatch(&e, &s);
	ASSERT(e.vi_visual == 'v' && e.sel_active == 1, "not in charwise visual");
	ASSERT(e.ay == 0 && e.ax == 0, "anchor not at the cursor");
	ed_close(&e);
	PASS();
}

static void
test_visual_charwise_yank(void)
{
	struct editor e;
	struct mock m;

	TEST("charwise visual yank copies the inclusive span");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello world");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "vllll");	/* select 'hello' (cols 0..4 inclusive) */
	ed_vi_type(&e, "y");
	ASSERT(e.clip != NULL && e.clip_len == 5 &&
	    memcmp(e.clip, "hello", 5) == 0, "yank not 'hello'");
	ASSERT(e.vi_visual == 0 && e.sel_active == 0, "still in visual mode");
	ed_close(&e);
	PASS();
}

static void
test_visual_charwise_delete(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("charwise visual delete removes the inclusive span");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "vll");	/* select a,b,c */
	ed_vi_type(&e, "d");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "def", 3) == 0, "line not 'def'");
	ASSERT(e.vi_visual == 0, "still in visual mode");
	ed_close(&e);
	PASS();
}

static void
test_visual_linewise_delete(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("linewise visual delete removes whole lines");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo\nthree");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "Vj");	/* lines 0..1 */
	ed_vi_type(&e, "d");
	ASSERT(text_lines(e.t) == 1, "should have one line left");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 5 && memcmp(line, "three", 5) == 0, "survivor not 'three'");
	ed_close(&e);
	PASS();
}

static void
test_visual_escape(void)
{
	struct editor e;
	struct mock m;
	struct tkbd_seq s;
	const char *line;
	size_t len = 0;

	TEST("Esc leaves visual mode without editing");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "vl");
	s = ed_key(TKBD_KEY_ESC);
	vi_dispatch(&e, &s);
	ASSERT(e.vi_visual == 0 && e.sel_active == 0, "still selecting");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "abc", 3) == 0, "buffer changed");
	ed_close(&e);
	PASS();
}

static void
test_visual_put_over(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("p over a selection replaces it with the register");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	/* yank "ef" into the register: select the last two chars */
	e.cy = 0;
	e.cx = 4;
	ed_vi_type(&e, "vl");	/* select e,f */
	ed_vi_type(&e, "y");
	/* now select "abc" and put the register over it */
	e.cx = 0;
	ed_vi_type(&e, "vll");
	ed_vi_type(&e, "p");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 5 && memcmp(line, "efdef", 5) == 0, "line not 'efdef'");
	ASSERT(e.vi_visual == 0 && e.sel_active == 0, "still selecting");
	/* the delete and the put undo together as one step */
	ed_vi_type(&e, "u");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "abcdef", 6) == 0,
	    "one undo did not restore the line");
	ed_close(&e);
	PASS();
}

static void
test_visual_swap_ends(void)
{
	struct editor e;
	struct mock m;

	TEST("o swaps the cursor and the anchor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "vll");	/* anchor 0, cursor 2 */
	ed_vi_type(&e, "o");
	ASSERT(e.cx == 0 && e.ax == 2, "ends not swapped");
	ed_close(&e);
	PASS();
}

static void
test_textobj_diw(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("diw deletes the inner word under the cursor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo bar baz");
	e.mode = MODE_NORMAL;
	e.cx = 5;			/* inside 'bar' */
	ed_vi_type(&e, "diw");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 8 && memcmp(line, "foo  baz", 8) == 0,
	    "line not 'foo  baz'");
	ed_close(&e);
	PASS();
}

static void
test_textobj_daw(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("daw deletes the word and its trailing whitespace");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo bar baz");
	e.mode = MODE_NORMAL;
	e.cx = 4;			/* start of 'bar' */
	ed_vi_type(&e, "daw");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 7 && memcmp(line, "foo baz", 7) == 0,
	    "line not 'foo baz'");
	ed_close(&e);
	PASS();
}

static void
test_textobj_di_paren(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("di( deletes inside the enclosing parentheses");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "call(a, b, c)");
	e.mode = MODE_NORMAL;
	e.cx = 7;			/* inside the parens */
	ed_vi_type(&e, "di(");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "call()", 6) == 0,
	    "line not 'call()'");
	ed_close(&e);
	PASS();
}

static void
test_textobj_da_paren(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("da( deletes the parentheses and their contents");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "call(a, b, c)");
	e.mode = MODE_NORMAL;
	e.cx = 7;
	ed_vi_type(&e, "da(");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "call", 4) == 0, "line not 'call'");
	ed_close(&e);
	PASS();
}

static void
test_textobj_ci_paren(void)
{
	struct editor e;
	struct mock m;

	TEST("ci( deletes inside the parens and enters insert mode");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "call(a, b, c)");
	e.mode = MODE_NORMAL;
	e.cx = 7;
	ed_vi_type(&e, "ci(");
	ASSERT(e.mode == MODE_INSERT, "not in insert mode");
	ASSERT(e.cx == 5, "cursor not at first inner column");
	ed_close(&e);
	PASS();
}

static void
test_textobj_di_quote(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("di\" deletes inside the double quotes");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "say \"hello\" now");
	e.mode = MODE_NORMAL;
	e.cx = 6;			/* inside the quotes */
	ed_vi_type(&e, "di\"");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 10 && memcmp(line, "say \"\" now", 10) == 0,
	    "line not 'say \"\" now'");
	ed_close(&e);
	PASS();
}

static void
test_textobj_visual_iw(void)
{
	struct editor e;
	struct mock m;

	TEST("viw selects the inner word");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo bar baz");
	e.mode = MODE_NORMAL;
	e.cx = 5;			/* inside 'bar' */
	ed_vi_type(&e, "viw");
	ASSERT(e.vi_visual == 'v' && e.sel_active, "not in charwise visual");
	ASSERT(e.ay == 0 && e.ax == 4, "anchor not at start of 'bar'");
	ASSERT(e.cy == 0 && e.cx == 6, "cursor not on last rune of 'bar'");
	ed_close(&e);
	PASS();
}

static void
test_mark_set_and_jump(void)
{
	struct editor e;
	struct mock m;

	TEST("`a jumps back to the exact spot marked with ma");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one two\nthree four\nfive six");
	e.mode = MODE_NORMAL;
	e.cy = 1;
	e.cx = 6;			/* on 'four' */
	ed_vi_type(&e, "ma");		/* set mark a here */
	e.cy = 0;
	e.cx = 0;			/* wander off */
	ed_vi_type(&e, "`a");		/* jump back */
	ASSERT(e.cy == 1 && e.cx == 6, "did not return to the mark");
	ed_close(&e);
	PASS();
}

static void
test_mark_line_jump(void)
{
	struct editor e;
	struct mock m;

	TEST("'a jumps to the first non-blank of the mark's line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\n    indented\nthree");
	e.mode = MODE_NORMAL;
	e.cy = 1;
	e.cx = 10;			/* somewhere inside the indented line */
	ed_vi_type(&e, "mb");
	e.cy = 2;
	e.cx = 0;
	ed_vi_type(&e, "'b");
	ASSERT(e.cy == 1 && e.cx == 4, "did not land on first non-blank");
	ed_close(&e);
	PASS();
}

static void
test_mark_operator(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("d`a deletes the charwise span up to the mark");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdefgh");
	e.mode = MODE_NORMAL;
	e.cx = 2;			/* on 'c' */
	ed_vi_type(&e, "ma");
	e.cx = 6;			/* on 'g' */
	ed_vi_type(&e, "d`a");		/* delete c..f (exclusive of g) */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "abgh", 4) == 0, "line not 'abgh'");
	ed_close(&e);
	PASS();
}

static void
test_register_named_yank_put(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("\"ax then \"ap puts the named register's text");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "\"ax");		/* delete 'a' into register a */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 5 && memcmp(line, "bcdef", 5) == 0, "delete failed");
	e.cx = 4;			/* on 'f' */
	ed_vi_type(&e, "\"ap");		/* put register a after 'f' */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "bcdefa", 6) == 0,
	    "named put not 'bcdefa'");
	ed_close(&e);
	PASS();
}

static void
test_register_two_independent(void)
{
	struct editor e;
	struct mock m;

	TEST("\"a and \"b hold independent linewise yanks");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo\nthree");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	ed_vi_type(&e, "\"ayy");		/* register a = "one\n" */
	e.cy = 1;
	ed_vi_type(&e, "\"byy");		/* register b = "two\n" */
	ASSERT(e.vi_regs[0].len == 4 &&
	    memcmp(e.vi_regs[0].bytes, "one\n", 4) == 0, "reg a wrong");
	ASSERT(e.vi_regs[1].len == 4 &&
	    memcmp(e.vi_regs[1].bytes, "two\n", 4) == 0, "reg b wrong");
	ASSERT(e.vi_reg == 0, "register selection not released");
	ed_close(&e);
	PASS();
}

static void
test_register_unnamed_mirrors(void)
{
	struct editor e;
	struct mock m;

	TEST("a named delete also fills the unnamed register");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "\"add");		/* delete 'one' into a */
	ASSERT(e.vi_regs[0].len == 4 &&
	    memcmp(e.vi_regs[0].bytes, "one\n", 4) == 0, "reg a wrong");
	ASSERT(e.clip_len == 4 && memcmp(e.clip, "one\n", 4) == 0,
	    "unnamed did not mirror");
	ed_close(&e);
	PASS();
}

static void
test_register_append_uppercase(void)
{
	struct editor e;
	struct mock m;

	TEST("\"A appends to the lowercase register");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo\nthree");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	ed_vi_type(&e, "\"ayy");		/* a = "one\n" */
	e.cy = 1;
	ed_vi_type(&e, "\"Ayy");		/* a = "one\ntwo\n" */
	ASSERT(e.vi_regs[0].len == 8 &&
	    memcmp(e.vi_regs[0].bytes, "one\ntwo\n", 8) == 0,
	    "append failed");
	ed_close(&e);
	PASS();
}

static void
test_register_released_by_motion(void)
{
	struct editor e;
	struct mock m;
	size_t nlines;

	TEST("an unused register selection is released before the next put");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "yy");		/* unnamed = "aa\n" */
	ed_vi_type(&e, "\"z");		/* arm empty register z */
	ed_vi_type(&e, "l");		/* a non-consumer: abandons z */
	ed_vi_type(&e, "p");		/* must fall back to unnamed */
	nlines = text_lines(e.t);
	ASSERT(nlines == 3, "unnamed put did not happen");
	ASSERT(e.vi_reg == 0, "register not released");
	ed_close(&e);
	PASS();
}

static void
ed_vi_esc(struct editor *e)
{
	struct tkbd_seq s = ed_key(TKBD_KEY_ESC);

	vi_dispatch(e, &s);
}

/* Run an ex command line directly, without the interactive ':' prompt. */
static enum req
ed_ex(struct editor *e, const char *cmd)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "%s", cmd);
	return vi_ex_exec(e, buf);
}

static void
test_dot_repeat_x(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(". repeats the last x");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "x");		/* delete 'a' */
	ed_vi_type(&e, ".");		/* delete 'b' */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "cdef", 4) == 0, "not 'cdef'");
	ed_close(&e);
	PASS();
}

static void
test_dot_repeat_dw(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(". repeats the last dw");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo bar baz");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "dw");		/* delete 'foo ' */
	ed_vi_type(&e, ".");		/* delete 'bar ' */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "baz", 3) == 0, "not 'baz'");
	ed_close(&e);
	PASS();
}

static void
test_dot_repeat_insert(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(". repeats an insert command");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ab");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "A");		/* append at end of line */
	ed_vi_type(&e, "X");
	ed_vi_esc(&e);			/* "abX" */
	ed_vi_type(&e, ".");		/* append X again */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "abXX", 4) == 0, "not 'abXX'");
	ed_close(&e);
	PASS();
}

static void
test_dot_not_undo(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(". repeats the change, not an intervening undo");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "x");		/* delete 'a' -> "bc" */
	ed_vi_type(&e, "u");		/* undo -> "abc" (not recorded) */
	e.cx = 0;			/* isolate from undo's cursor rest */
	ed_vi_type(&e, ".");		/* must repeat x, not u */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 2 && memcmp(line, "bc", 2) == 0, "not 'bc'");
	ed_close(&e);
	PASS();
}

static void
test_dot_repeat_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(". repeats a counted command verbatim");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "2x");		/* delete 'ab' -> "cdef" */
	ed_vi_type(&e, ".");		/* delete 'cd' -> "ef" */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 2 && memcmp(line, "ef", 2) == 0, "not 'ef'");
	ed_close(&e);
	PASS();
}

static void
test_join_lines(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("J joins the next line with a single space");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\n   two");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "J");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 7 && memcmp(line, "one two", 7) == 0, "not 'one two'");
	ASSERT(text_lines(e.t) == 1, "line count not 1");
	ASSERT(e.cx == 3, "cursor not on the join");
	ed_close(&e);
	PASS();
}

static void
test_toggle_case_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("3~ toggles the case of three characters");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc.def");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "3~");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 7 && memcmp(line, "ABC.def", 7) == 0, "not 'ABC.def'");
	ASSERT(e.cx == 3, "cursor did not advance past three runes");
	ed_close(&e);
	PASS();
}

static void
test_delete_to_eol(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("D deletes from the cursor to the end of the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello world");
	e.mode = MODE_NORMAL;
	e.cx = 5;			/* on the space */
	ed_vi_type(&e, "D");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 5 && memcmp(line, "hello", 5) == 0, "not 'hello'");
	ed_close(&e);
	PASS();
}

static void
test_change_to_eol(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("C changes from the cursor to the end of the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "hello world");
	e.mode = MODE_NORMAL;
	e.cx = 6;			/* on 'w' */
	ed_vi_type(&e, "C");
	ASSERT(e.mode == MODE_INSERT, "not in insert mode");
	ed_vi_type(&e, "there");
	ed_vi_esc(&e);
	line = text_line(e.t, 0, &len);
	ASSERT(len == 11 && memcmp(line, "hello there", 11) == 0,
	    "not 'hello there'");
	ed_close(&e);
	PASS();
}

static void
test_substitute_char(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("s substitutes the char under the cursor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "s");		/* delete 'a', enter insert */
	ASSERT(e.mode == MODE_INSERT, "not in insert mode");
	ed_vi_type(&e, "XY");
	ed_vi_esc(&e);
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "XYbc", 4) == 0, "not 'XYbc'");
	ed_close(&e);
	PASS();
}

static void
test_substitute_line(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("S substitutes the whole line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "S");
	ASSERT(e.mode == MODE_INSERT, "not in insert mode");
	ed_vi_type(&e, "X");
	ed_vi_esc(&e);
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && memcmp(line, "X", 1) == 0, "first line not 'X'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "two", 3) == 0, "second line changed");
	ed_close(&e);
	PASS();
}

static void
test_replace_char_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("3rX replaces three characters with X");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "3rX");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "XXXdef", 6) == 0, "not 'XXXdef'");
	ASSERT(e.cx == 2, "cursor not on the last replaced rune");
	ed_close(&e);
	PASS();
}

static void
test_replace_char_over_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("r with a count past the line end does nothing");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ab");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "5rX");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 2 && memcmp(line, "ab", 2) == 0, "line changed");
	ed_close(&e);
	PASS();
}

static void
test_replace_char_newline(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("r<Enter> breaks the line at the character");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcd");
	e.mode = MODE_NORMAL;
	e.cx = 1;			/* on 'b' */
	{
		struct tkbd_seq s = ed_ch('r');

		vi_dispatch(&e, &s);
		s = ed_key(TKBD_KEY_ENTER);
		vi_dispatch(&e, &s);
	}
	ASSERT(text_lines(e.t) == 2, "line not split");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && memcmp(line, "a", 1) == 0, "first line not 'a'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 2 && memcmp(line, "cd", 2) == 0, "second line not 'cd'");
	ed_close(&e);
	PASS();
}

static void
test_replace_mode_overtype(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("R overwrites and then extends past the line end");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ab");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "R");
	ASSERT(e.mode == MODE_INSERT && e.vi_overtype, "not in Replace mode");
	ed_vi_type(&e, "XYZ");		/* overwrite a,b then extend */
	ed_vi_esc(&e);
	ASSERT(e.vi_overtype == 0, "Replace mode not cleared on Esc");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "XYZ", 3) == 0, "not 'XYZ'");
	ed_close(&e);
	PASS();
}

static void
test_shift_right_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("2>> indents two lines with a tab");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb\ncc");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "2>>");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "line 0 not '\\taa'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "\tbb", 3) == 0, "line 1 not '\\tbb'");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 2 && memcmp(line, "cc", 2) == 0, "line 2 changed");
	ASSERT(e.cx == 1, "cursor not on the first non-blank");
	ed_close(&e);
	PASS();
}

static void
test_shift_left_tab(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("<< removes one leading tab");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "\tfoo");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "<<");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "foo", 3) == 0, "not 'foo'");
	ed_close(&e);
	PASS();
}

static void
test_shift_left_spaces(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("<< removes up to a tab-width of leading spaces");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "    x");		/* four spaces */
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "<<");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && memcmp(line, "x", 1) == 0, "not 'x'");
	ed_close(&e);
	PASS();
}

static void
test_shift_operator_motion(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(">j indents the current and next line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb\ncc");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, ">j");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "line 0 not indented");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "\tbb", 3) == 0, "line 1 not indented");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 2 && memcmp(line, "cc", 2) == 0, "line 2 changed");
	ed_close(&e);
	PASS();
}

static void
test_shift_over_mark(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(">'a shifts the marked range instead of deleting it");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb\ncc");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	ed_vi_type(&e, "ma");		/* mark line 0 */
	e.cy = 2;
	ed_vi_type(&e, ">'a");		/* shift lines 0..2 right */
	ASSERT(text_lines(e.t) == 3, "lines were removed");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "line 0 not shifted");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 3 && memcmp(line, "\tcc", 3) == 0, "line 2 not shifted");
	ed_close(&e);
	PASS();
}

static void
test_shift_over_textobject(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(">i( shifts the object's lines instead of deleting them");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "(\naa\n)");
	e.mode = MODE_NORMAL;
	e.cy = 1;
	e.cx = 0;
	ed_vi_type(&e, ">i(");
	ASSERT(text_lines(e.t) == 3, "lines were removed");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "inner not shifted");
	ed_close(&e);
	PASS();
}

static void
test_visual_shift(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("V j > shifts the selected lines and leaves visual mode");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "Vj>");
	ASSERT(e.vi_visual == 0 && e.sel_active == 0, "still in visual mode");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "line 0 not shifted");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "\tbb", 3) == 0, "line 1 not shifted");
	ed_close(&e);
	PASS();
}

static void
test_mark_operator_stale(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("d`a into a shortened line stays in bounds");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcdef");
	e.mode = MODE_NORMAL;
	e.cx = 5;			/* on 'f' */
	ed_vi_type(&e, "ma");		/* mark column 5 */
	e.cx = 0;
	ed_vi_type(&e, "3x");		/* shorten to "def" */
	e.cx = 0;
	ed_vi_type(&e, "d`a");		/* delete to the stale mark, clamped */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 0, "line not emptied by clamped delete");
	ed_close(&e);
	PASS();
}

static void
test_search_backward(void)
{
	struct editor e;
	struct mock m;

	TEST("backward search finds the nearest match above the cursor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo\nbar\nfoo\nbaz");
	e.mode = MODE_NORMAL;
	e.cy = 3;			/* on 'baz' */
	e.cx = 0;
	do_find_dir(&e, "foo", -1);
	ASSERT(e.cy == 2 && e.cx == 0, "did not land on the line 2 'foo'");
	ed_close(&e);
	PASS();
}

static void
test_search_backward_wrap(void)
{
	struct editor e;
	struct mock m;

	TEST("backward search wraps to the last match in the buffer");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo\nbar\nbaz");
	e.mode = MODE_NORMAL;
	e.cy = 0;			/* only match is on this line */
	e.cx = 0;
	do_find_dir(&e, "baz", -1);	/* nothing before: wrap down to it */
	ASSERT(e.cy == 2 && e.cx == 0, "did not wrap to 'baz'");
	ed_close(&e);
	PASS();
}

static void
test_search_word_star(void)
{
	struct editor e;
	struct mock m;

	TEST("* searches forward for the word under the cursor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "cat dog cat");
	e.mode = MODE_NORMAL;
	e.cx = 0;			/* on the first 'cat' */
	ed_vi_type(&e, "*");
	ASSERT(e.cx == 8 && e.cy == 0, "did not jump to the next 'cat'");
	ASSERT(strcmp(e.last_find, "cat") == 0, "last_find not 'cat'");
	ASSERT(e.vi_search_dir == 1, "direction not forward");
	ed_close(&e);
	PASS();
}

static void
test_search_word_hash(void)
{
	struct editor e;
	struct mock m;

	TEST("# searches backward for the word under the cursor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "cat dog cat");
	e.mode = MODE_NORMAL;
	e.cx = 8;			/* on the second 'cat' */
	ed_vi_type(&e, "#");
	ASSERT(e.cx == 0 && e.cy == 0, "did not jump back to the first 'cat'");
	ASSERT(e.vi_search_dir == -1, "direction not backward");
	ed_close(&e);
	PASS();
}

static void
test_search_repeat_reverse(void)
{
	struct editor e;
	struct mock m;

	TEST("N repeats the last search in the opposite direction");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "x foo y foo z foo");	/* foo at 2, 8, 14 */
	e.mode = MODE_NORMAL;
	e.last_find[0] = '\0';
	snprintf(e.last_find, sizeof(e.last_find), "foo");
	e.vi_search_dir = 1;
	e.cx = 8;			/* on the middle 'foo' */
	ed_vi_type(&e, "n");		/* forward -> 14 */
	ASSERT(e.cx == 14, "n did not advance forward");
	ed_vi_type(&e, "N");		/* reverse -> back to 8 */
	ASSERT(e.cx == 8, "N did not go backward");
	ed_close(&e);
	PASS();
}

static void
test_search_offset_end(void)
{
	struct editor e;
	struct mock m;

	TEST("/pat/e lands on the match end and n moves on");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "x foo y foo z");	/* foo at 2 and 8 */
	e.mode = MODE_NORMAL;
	e.cx = 0;
	ASSERT(vi_search_cmd(&e, "foo/e", 1) == 0, "search rejected");
	ASSERT(e.cy == 0 && e.cx == 4, "did not land on the last 'o'");
	ed_vi_type(&e, "n");
	ASSERT(e.cx == 10, "n did not reach the second match end");
	ed_vi_type(&e, "n");			/* wraps to the first */
	ASSERT(e.cx == 4, "n did not wrap to the first match end");
	ASSERT(vi_search_cmd(&e, "foo/e-1", 1) == 0, "e-1 rejected");
	ASSERT(e.cx == 9, "e-1 did not step back one character");
	ed_close(&e);
	PASS();
}

static void
test_search_offset_start_negative(void)
{
	struct editor e;
	struct mock m;

	TEST("/pat/s-2 leaves the cursor before the match without sticking");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ab foo cd foo");	/* foo at 3 and 10 */
	e.mode = MODE_NORMAL;
	e.cx = 0;
	ASSERT(vi_search_cmd(&e, "foo/s-2", 1) == 0, "search rejected");
	ASSERT(e.cx == 1, "s-2 did not land two before the match");
	ed_vi_type(&e, "n");
	ASSERT(e.cx == 8, "n re-found the same match instead of the next");
	ed_vi_type(&e, "N");
	ASSERT(e.cx == 1, "N did not go back to the first match's offset");
	ASSERT(vi_search_cmd(&e, "foo/b+", 1) == 0, "b+ rejected");
	ASSERT(e.cx == 11, "b+ did not land one into the match");
	ed_close(&e);
	PASS();
}

static void
test_search_offset_lines(void)
{
	struct editor e;
	struct mock m;

	TEST("/pat/+N and ?pat?-N move whole lines and rest in column 0");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "  foo\nbar\nbaz\n  foo\nend");
	e.mode = MODE_NORMAL;
	e.cy = 1;
	e.cx = 0;
	ASSERT(vi_search_cmd(&e, "foo/+1", 1) == 0, "search rejected");
	ASSERT(e.cy == 4 && e.cx == 0, "+1 did not land on the line below");
	ed_vi_type(&e, "n");			/* wraps to the first foo */
	ASSERT(e.cy == 1 && e.cx == 0, "n did not move to the next match");
	ASSERT(vi_search_cmd(&e, "foo?-1", -1) == 0, "backward rejected");
	ASSERT(e.cy == 2 && e.cx == 0, "?-1 did not land above the match");
	ASSERT(vi_search_cmd(&e, "foo/+9", 1) == 0, "+9 rejected");
	ASSERT(e.cy == 4, "a line offset past the end did not clamp");
	ed_close(&e);
	PASS();
}

static void
test_search_offset_reuse_and_errors(void)
{
	struct editor e;
	struct mock m;

	TEST("//e reuses the last pattern; a bad offset is refused");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a foo b a/b");
	e.mode = MODE_NORMAL;
	e.cx = 0;
	ASSERT(vi_search_cmd(&e, "foo", 1) == 0, "plain search rejected");
	ASSERT(e.cx == 2 && e.vi_off_kind == 0, "plain search has an offset");
	ASSERT(vi_search_cmd(&e, "/e", 1) == 0, "empty pattern rejected");
	ASSERT(e.cx == 4, "//e did not reuse the pattern with the offset");
	ASSERT(vi_search_cmd(&e, "foo/x", 1) == -1, "bad offset accepted");
	ASSERT(e.cx == 4, "a bad offset moved the cursor");
	ASSERT(strstr(e.status, "bad search offset") != NULL, "no error");
	ASSERT(vi_search_cmd(&e, "a\\/b", 1) == 0, "escaped delimiter rejected");
	ASSERT(e.cx == 8, "escaped '/' was not part of the pattern");
	ed_vi_type(&e, "*");			/* word search drops the offset */
	ASSERT(e.vi_off_kind == 0, "* kept the previous offset");
	ed_close(&e);
	PASS();
}

static void
test_ex_delete_range(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":2,3d deletes the given line range");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "2,3d");
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && line[0] == 'a', "line 0 not 'a'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 1 && line[0] == 'd', "line 1 not 'd'");
	ed_close(&e);
	PASS();
}

static void
test_ex_delete_all(void)
{
	struct editor e;
	struct mock m;
	size_t len = 0;

	TEST(":%d empties the buffer");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "%d");
	ASSERT(text_lines(e.t) == 1, "buffer not reduced to one line");
	text_line(e.t, 0, &len);
	ASSERT(len == 0, "remaining line not empty");
	ed_close(&e);
	PASS();
}

static void
test_ex_goto_dollar(void)
{
	struct editor e;
	struct mock m;

	TEST(":$ jumps to the last line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "$");
	ASSERT(e.cy == 3, "did not go to the last line");
	ed_close(&e);
	PASS();
}

static void
test_ex_shift_range(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":1,2> shifts the line range");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aa\nbb\ncc");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "1,2>");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "\taa", 3) == 0, "line 0 not shifted");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 2 && memcmp(line, "cc", 2) == 0, "line 2 changed");
	ed_close(&e);
	PASS();
}

static void
test_ex_mark_range(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":'a,'bd deletes between two marks");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd\ne");
	e.mode = MODE_NORMAL;
	e.cy = 1;
	ed_vi_type(&e, "ma");		/* mark a on line 1 */
	e.cy = 3;
	ed_vi_type(&e, "mb");		/* mark b on line 3 */
	ed_ex(&e, "'a,'bd");		/* delete lines 1..3 */
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && line[0] == 'a', "line 0 not 'a'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 1 && line[0] == 'e', "line 1 not 'e'");
	ed_close(&e);
	PASS();
}

static void
test_delete_region_multiline(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("a charwise delete across 3 lines drops the middle line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc\ndef\nghi");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	e.cx = 1;			/* on 'b' */
	ed_vi_type(&e, "ma");
	e.cy = 2;
	e.cx = 1;			/* on 'h' */
	ed_vi_type(&e, "d`a");		/* delete (0,1)..(2,1): "bc\ndef\ng" */
	ASSERT(text_lines(e.t) == 1, "lines not collapsed to 1");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "ahi", 3) == 0, "not 'ahi'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_first(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":s/a/X/ replaces the first match on the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "banana");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "s/a/X/");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "bXnana", 6) == 0, "not 'bXnana'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_global(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":s/a/X/g replaces every match on the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "banana");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "s/a/X/g");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "bXnXnX", 6) == 0, "not 'bXnXnX'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_all_lines(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":%s/o/0/g substitutes across the whole file");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "foo\nboo");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "%s/o/0/g");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "f00", 3) == 0, "line 0 not 'f00'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 3 && memcmp(line, "b00", 3) == 0, "line 1 not 'b00'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_delete(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":s/X//g with an empty replacement removes the matches");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "aXbXc");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "s/X//g");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "abc", 3) == 0, "not 'abc'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_grows_line(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":s with a longer replacement grows the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a.b.c");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "s/./--/g");		/* '.' is literal, not a wildcard */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 7 && memcmp(line, "a--b--c", 7) == 0, "not 'a--b--c'");
	ed_close(&e);
	PASS();
}

static void
test_ex_global_delete(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":g/x/d deletes every matching line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "keep\ndrop x\nkeep2\ndrop x2");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "g/x/d");
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "keep", 4) == 0, "line 0 not 'keep'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 5 && memcmp(line, "keep2", 5) == 0, "line 1 not 'keep2'");
	ed_close(&e);
	PASS();
}

static void
test_ex_global_inverse(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":v/foo/d deletes every non-matching line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a foo\nbar\nc foo");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "v/foo/d");
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 5 && memcmp(line, "a foo", 5) == 0, "line 0 wrong");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 5 && memcmp(line, "c foo", 5) == 0, "line 1 wrong");
	ed_close(&e);
	PASS();
}

static void
test_ex_global_subst(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":g/x:/s/:/=/ substitutes only on matching lines");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "x: one\ny: two\nx: three");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "g/x:/s/:/=/");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 6 && memcmp(line, "x= one", 6) == 0, "line 0 not '='");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 6 && memcmp(line, "y: two", 6) == 0, "line 1 changed");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 8 && memcmp(line, "x= three", 8) == 0, "line 2 not '='");
	ed_close(&e);
	PASS();
}

static void
test_ex_global_delete_all(void)
{
	struct editor e;
	struct mock m;
	size_t len = 0;

	TEST(":g//d matching every line leaves one empty line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ax\nbx\ncx");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "g/x/d");		/* every line matches */
	ASSERT(text_lines(e.t) == 1, "not reduced to one line");
	text_line(e.t, 0, &len);
	ASSERT(len == 0, "remaining line not empty");
	ASSERT(e.cy == 0, "cursor out of range");
	ed_close(&e);
	PASS();
}

static void
test_ex_range_reversed(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":3,1d orders a reversed range");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "3,1d");		/* should delete lines 1..3 */
	ASSERT(text_lines(e.t) == 1, "line count not 1");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && line[0] == 'd', "remaining line not 'd'");
	ed_close(&e);
	PASS();
}

static void
test_ex_subst_long_rep(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST(":s with a many-char replacement sizes the line correctly");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "xyx");
	e.mode = MODE_NORMAL;
	ed_ex(&e, "s/x/ABCDE/g");	/* two matches, 5 bytes each */
	line = text_line(e.t, 0, &len);
	ASSERT(len == 11 && memcmp(line, "ABCDEyABCDE", 11) == 0,
	    "not 'ABCDEyABCDE'");
	ed_close(&e);
	PASS();
}

static void
test_delete_paragraph_linewise(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("d} from column 0 deletes whole lines up to the blank line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "one\ntwo\n\nfour");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	e.cx = 0;
	ed_vi_type(&e, "d}");
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 0, "line 0 not the blank line");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 4 && memcmp(line, "four", 4) == 0, "line 1 not 'four'");
	ed_close(&e);
	PASS();
}

static void
test_delete_paragraph_charwise(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("d} from mid-line stays charwise to the line end");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abc\n\ndef");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	e.cx = 1;			/* on 'b', not the first non-blank */
	ed_vi_type(&e, "d}");
	ASSERT(text_lines(e.t) == 3, "line count changed");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && line[0] == 'a', "line 0 not 'a'");
	ed_close(&e);
	PASS();
}

static void
test_percent_goto(void)
{
	struct editor e;
	struct mock m;

	TEST("N% jumps to N percent through the file");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9");	/* 10 lines */
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "50%");
	ASSERT(e.cy == 4, "50% not on the 5th line");
	ed_vi_type(&e, "100%");
	ASSERT(e.cy == 9, "100% not on the last line");
	ed_close(&e);
	PASS();
}

static void
test_percent_match_still_works(void)
{
	struct editor e;
	struct mock m;

	TEST("bare % still jumps to the matching bracket");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "(abc)");
	e.mode = MODE_NORMAL;
	e.cx = 0;			/* on '(' */
	ed_vi_type(&e, "%");
	ASSERT(e.cx == 4 && e.cy == 0, "did not jump to ')'");
	ed_close(&e);
	PASS();
}

static void
test_column_memory_jk(void)
{
	struct editor e;
	struct mock m;

	TEST("j through a short line preserves the display column");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "longlineXX\nsh\nlonglineYY");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	e.cx = 8;
	ed_vi_type(&e, "j");		/* onto "sh": clamps to col 1 */
	ASSERT(e.cy == 1 && e.cx == 1, "not clamped on the short line");
	ed_vi_type(&e, "j");		/* back onto a long line: column 8 */
	ASSERT(e.cy == 2 && e.cx == 8, "did not restore column 8");
	ed_close(&e);
	PASS();
}

static void
test_column_memory_reset(void)
{
	struct editor e;
	struct mock m;

	TEST("a horizontal move ends the column run");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "longlineXX\nsh\nlonglineYY");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	e.cx = 8;
	ed_vi_type(&e, "j");		/* col 1 on "sh", want_col = 8 */
	ed_vi_type(&e, "h");		/* horizontal: ends the run */
	ed_vi_type(&e, "j");		/* recompute from current column 0 */
	ASSERT(e.cy == 2 && e.cx == 0, "column run not reset by h");
	ed_close(&e);
	PASS();
}

static void
test_column_memory_dollar(void)
{
	struct editor e;
	struct mock m;

	TEST("$ makes j/k stick to the end of the line");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "abcd\nx\nefgh");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	ed_vi_type(&e, "$");		/* col 3 on 'd', sticky EOL */
	ed_vi_type(&e, "j");		/* "x": col 0 */
	ASSERT(e.cy == 1 && e.cx == 0, "not at end of short line");
	ed_vi_type(&e, "j");		/* "efgh": end again, col 3 */
	ASSERT(e.cy == 2 && e.cx == 3, "did not stick to end of line");
	ed_close(&e);
	PASS();
}

static void
test_goto_line_counts(void)
{
	struct editor e;
	struct mock m;

	TEST("NG, gg, Ngg, and G land on the right lines");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd\ne");
	e.mode = MODE_NORMAL;
	ed_vi_type(&e, "3G");
	ASSERT(e.cy == 2, "3G not on line 3");
	ed_vi_type(&e, "gg");
	ASSERT(e.cy == 0, "gg not on line 1");
	ed_vi_type(&e, "4gg");
	ASSERT(e.cy == 3, "4gg not on line 4");
	ed_vi_type(&e, "G");
	ASSERT(e.cy == 4, "G not on the last line");
	ed_close(&e);
	PASS();
}

static void
test_delete_to_line_count(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("d3G deletes linewise from the cursor through line 3");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a\nb\nc\nd\ne");
	e.mode = MODE_NORMAL;
	e.cy = 0;
	ed_vi_type(&e, "d3G");		/* delete lines 1..3 */
	ASSERT(text_lines(e.t) == 2, "line count not 2");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 1 && line[0] == 'd', "line 0 not 'd'");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 1 && line[0] == 'e', "line 1 not 'e'");
	ed_close(&e);
	PASS();
}

static void
test_buffers_open_switch(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("opening a buffer switches to it; switching restores content");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "buffer zero");
	ASSERT(e.nbuf == 1 && e.cur == 0, "did not start with one buffer");
	buf_open(&e, NULL);		/* a new empty buffer */
	ASSERT(e.nbuf == 2 && e.cur == 1, "new buffer not active");
	ed_settext(&e, "buffer one");
	buf_switch(&e, 0);
	ASSERT(e.cur == 0, "did not switch to buffer 0");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 11 && memcmp(line, "buffer zero", 11) == 0,
	    "buffer 0 content lost");
	buf_switch(&e, 1);
	line = text_line(e.t, 0, &len);
	ASSERT(len == 10 && memcmp(line, "buffer one", 10) == 0,
	    "buffer 1 content lost");
	ed_close(&e);
	PASS();
}

static void
test_buffers_cycle(void)
{
	struct editor e;
	struct mock m;

	TEST("buf_cycle moves next and previous, wrapping");
	ed_open(&e, &m, 24, 80);
	buf_open(&e, NULL);
	buf_open(&e, NULL);		/* now 3 buffers, cur = 2 */
	ASSERT(e.nbuf == 3 && e.cur == 2, "not three buffers");
	buf_cycle(&e, 1);		/* wrap to 0 */
	ASSERT(e.cur == 0, "next did not wrap to 0");
	buf_cycle(&e, -1);		/* wrap back to 2 */
	ASSERT(e.cur == 2, "prev did not wrap to 2");
	buf_cycle(&e, -1);
	ASSERT(e.cur == 1, "prev did not reach 1");
	ed_close(&e);
	PASS();
}

static void
test_buffers_open_existing(void)
{
	struct editor e;
	struct mock m;

	TEST("re-opening a named path switches instead of duplicating");
	ed_open(&e, &m, 24, 80);
	buf_open(&e, "/nonexistent/aa.c");	/* ENOENT: empty named buffer */
	buf_open(&e, "/nonexistent/bb.c");
	ASSERT(e.nbuf == 3 && e.cur == 2, "two files not opened");
	buf_open(&e, "/nonexistent/aa.c");	/* already open: switch */
	ASSERT(e.nbuf == 3 && e.cur == 1, "did not switch to the open file");
	ed_close(&e);
	PASS();
}

static void
test_buffers_close(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("closing the active buffer loads a neighbor");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "zero");
	buf_open(&e, NULL);
	ed_settext(&e, "one");
	ASSERT(e.nbuf == 2 && e.cur == 1, "setup wrong");
	ASSERT(buf_close(&e, 1) == 0, "close refused");
	ASSERT(e.nbuf == 1 && e.cur == 0, "did not fall back to buffer 0");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "zero", 4) == 0, "neighbor content lost");
	ASSERT(buf_close(&e, 0) == -1, "closed the last buffer");
	ed_close(&e);
	PASS();
}

static void
test_ex_buffer_commands(void)
{
	struct editor e;
	struct mock m;

	TEST(":enew, :bn/:bp, :b N, :ls, and :bd manage buffers");
	ed_open(&e, &m, 24, 80);
	ed_ex(&e, "enew");
	ed_ex(&e, "enew");
	ASSERT(e.nbuf == 3 && e.cur == 2, ":enew did not add buffers");
	ed_ex(&e, "b 1");
	ASSERT(e.cur == 0, ":b 1 did not switch to buffer 1");
	ed_ex(&e, "bn");
	ASSERT(e.cur == 1, ":bn did not advance");
	ed_ex(&e, "bp");
	ASSERT(e.cur == 0, ":bp did not go back");
	ed_ex(&e, "ls");
	ASSERT(strstr(e.status, "1:*") != NULL, ":ls did not mark active");
	ed_ex(&e, "bd!");
	ASSERT(e.nbuf == 2, ":bd! did not close a buffer");
	ed_close(&e);
	PASS();
}

static void
test_ex_edit_opens_file(void)
{
	struct editor e;
	struct mock m;

	TEST(":e opens a named buffer and :e again switches to it");
	ed_open(&e, &m, 24, 80);
	ed_ex(&e, "e /nonexistent/one.c");
	ASSERT(e.nbuf == 2 && e.has_name, ":e did not open a named buffer");
	ed_ex(&e, "e /nonexistent/two.c");
	ASSERT(e.nbuf == 3 && e.cur == 2, "second :e did not add a buffer");
	ed_ex(&e, "e /nonexistent/one.c");
	ASSERT(e.nbuf == 3 && e.cur == 1, ":e did not switch to the open file");
	ed_close(&e);
	PASS();
}

static void
test_ex_bd_refuses_dirty(void)
{
	struct editor e;
	struct mock m;

	TEST(":bd refuses a modified buffer without !");
	ed_open(&e, &m, 24, 80);
	ed_ex(&e, "enew");
	ed_settext(&e, "changed");	/* mark it dirty */
	ed_ex(&e, "bd");
	ASSERT(e.nbuf == 2, ":bd closed a dirty buffer");
	ASSERT(strstr(e.status, "no write") != NULL, "no warning shown");
	ed_close(&e);
	PASS();
}

static void
test_buffers_highlight_lifecycle(void)
{
	struct editor e;
	struct mock m;
	char big[4096];
	size_t o = 0;
	int i;

	TEST("per-buffer line_state survives switches and frees cleanly");
	for (i = 0; i < 100 && o < sizeof(big) - 8; i++)	/* > 64 lines */
		o += (size_t)snprintf(big + o, sizeof(big) - o, "int x%d;\n", i);
	ed_open(&e, &m, 24, 80);
	e.hl_on = 1;
	e.syn = syn_for_ext("c");
	ASSERT(e.syn != NULL, "no C syntax available");
	ed_settext(&e, big);
	render(&e, e.d);		/* allocates buffer 0's line_state */

	buf_open(&e, NULL);		/* buffer 1 */
	e.hl_on = 1;
	e.syn = syn_for_ext("c");
	ed_settext(&e, "int y;\n");
	render(&e, e.d);		/* buffer 1's own line_state */

	buf_switch(&e, 0);		/* load buffer 0's line_state */
	ed_settext(&e, big);		/* re-fill; may realloc line_state */
	render(&e, e.d);
	buf_switch(&e, 1);		/* parks buffer 0 with its live pointer */
	ASSERT(buf_close(&e, 1) == 0, "close failed");	/* frees buffer 1 */
	render(&e, e.d);		/* buffer 0 is active again */
	ed_close(&e);			/* frees the rest; valgrind checks */
	PASS();
}

/*
 * do_new/do_open replace the active buffer's text in place. Exercise the
 * path where that replacement is followed by a close (both of the active
 * buffer and of a neighbor) to prove the slot stays consistent with the
 * live pointer and nothing is freed twice.
 */
static void
test_buffers_do_new_in_place(void)
{
	struct editor e;
	struct mock m;
	const char *line;
	size_t len = 0;

	TEST("do_new replaces the active buffer without desyncing its slot");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "zero");
	buf_open(&e, NULL);		/* buffer 1, clean (do_new won't prompt) */
	do_new(&e);			/* replaces buffer 1's text in place */
	ASSERT(e.nbuf == 2 && e.cur == 1, "do_new changed buffer set");
	ASSERT(e.bufs[e.cur].t == e.t, "slot not resynced with live text");

	/* close the active buffer we just replaced: must not double-free */
	ASSERT(buf_close(&e, 1) == 0, "close of replaced buffer failed");
	ASSERT(e.nbuf == 1 && e.cur == 0, "did not fall back to buffer 0");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 4 && memcmp(line, "zero", 4) == 0, "neighbor lost");
	ed_close(&e);			/* valgrind checks the free paths */
	PASS();
}

static void
test_ex_read_file(void)
{
	struct editor e;
	struct mock m;
	char tmpl[] = "/tmp/lumi_edit_rXXXXXX";
	char cmd[300];
	const char *line;
	size_t len = 0;
	int fd;

	TEST(":r reads a file in below the current line");
	fd = mkstemp(tmpl);
	ASSERT(fd >= 0, "mkstemp failed");
	ASSERT(write(fd, "ins1\nins2\n", 10) == 10, "short write");
	close(fd);

	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "top\nbot");	/* two lines */
	e.cy = 0;
	e.cx = 0;
	snprintf(cmd, sizeof(cmd), "r %s", tmpl);
	ed_ex(&e, cmd);

	ASSERT(text_lines(e.t) == 4, "wrong line count after :r");
	line = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(line, "top", 3) == 0, "line 0 disturbed");
	line = text_line(e.t, 1, &len);
	ASSERT(len == 4 && memcmp(line, "ins1", 4) == 0, "ins1 missing");
	line = text_line(e.t, 2, &len);
	ASSERT(len == 4 && memcmp(line, "ins2", 4) == 0, "ins2 missing");
	line = text_line(e.t, 3, &len);
	ASSERT(len == 3 && memcmp(line, "bot", 3) == 0, "bot lost");
	ASSERT(e.cy == 1, "cursor not on the first read line");
	unlink(tmpl);
	ed_close(&e);
	PASS();
}

static void
test_ex_read_missing_file(void)
{
	struct editor e;
	struct mock m;

	TEST(":r on a missing file reports an error and edits nothing");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "only");
	ed_ex(&e, "r /no/such/file");
	ASSERT(text_lines(e.t) == 1, "buffer changed on failed :r");
	ASSERT(strstr(e.status, "cannot open") != NULL, "no error shown");
	ed_close(&e);
	PASS();
}

static void
test_menu_buffer_nav(void)
{
	struct editor e;
	struct mock m;

	TEST("File-menu buffer actions cycle and list");
	ed_open(&e, &m, 24, 80);
	buf_open(&e, NULL);		/* buffer 1 */
	buf_open(&e, NULL);		/* buffer 2, cur == 2 */
	(void)run_menu_act(&e, MA_BUF_NEXT);
	ASSERT(e.cur == 0, "Next Buffer did not wrap to 0");
	(void)run_menu_act(&e, MA_BUF_PREV);
	ASSERT(e.cur == 2, "Prev Buffer did not wrap back");
	(void)run_menu_act(&e, MA_BUF_LIST);
	ASSERT(strstr(e.status, "3:*") != NULL, "Buffer List missed active");
	ed_close(&e);
	PASS();
}

static void
test_build_errs_global(void)
{
	struct editor e;
	struct mock m;

	TEST("the diagnostics list is shared across buffers");
	ed_open(&e, &m, 24, 80);
	build_add_err(&e, "a.c", 4, 0, "boom");
	ASSERT(e.n_errs == 1, "setup: error not recorded");
	buf_open(&e, NULL);		/* a fresh buffer must not clear it */
	ASSERT(e.n_errs == 1, "opening a buffer dropped the diagnostics");
	buf_switch(&e, 0);
	ASSERT(e.n_errs == 1, "switching buffers dropped the diagnostics");
	ed_close(&e);
	PASS();
}

static void
test_parse_diag_skips_context(void)
{
	struct editor e;
	struct mock m;

	TEST("gcc \"In file included from\" lines are not diagnostics");
	ed_open(&e, &m, 24, 80);
	build_clear_errors(&e);
	parse_one_diag(&e, "In file included from main.c:1:");
	ASSERT(e.n_errs == 0, "context line parsed as a diagnostic");
	parse_one_diag(&e, "bad.h:3:16: error: oops undeclared");
	ASSERT(e.n_errs == 1, "real diagnostic not parsed");
	ASSERT(e.errs[0].line == 3 && e.errs[0].col == 16, "line/col wrong");
	ASSERT(strcmp(e.errs[0].path, "bad.h") == 0, "path wrong");
	ed_close(&e);
	PASS();
}

static void
test_build_goto_cross_file(void)
{
	struct editor e;
	struct mock m;
	char dir[] = "/tmp/lumi_b2_XXXXXX";
	char pa[512];
	int fd;

	TEST("build_goto opens another file for a cross-file diagnostic");
	ASSERT(mkdtemp(dir) != NULL, "mkdtemp failed");
	snprintf(pa, sizeof(pa), "%s/aaa.c", dir);
	fd = open(pa, O_CREAT | O_WRONLY, 0600);
	ASSERT(fd >= 0, "create aaa.c");
	ASSERT(write(fd, "a1\na2\na3\n", 9) == 9, "write aaa.c");
	close(fd);
	snprintf(pa, sizeof(pa), "%s/bbb.c", dir);
	fd = open(pa, O_CREAT | O_WRONLY, 0600);
	ASSERT(fd >= 0, "create bbb.c");
	ASSERT(write(fd, "b1\nb2\nb3\nb4\n", 12) == 12, "write bbb.c");
	close(fd);

	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "a1\na2\na3");	/* current buffer stands in for aaa.c */
	snprintf(e.path, sizeof(e.path), "%s/aaa.c", dir);
	e.has_name = 1;
	snprintf(e.build_dir, sizeof(e.build_dir), "%s", dir);
	build_add_err(&e, "aaa.c", 2, 0, "in a");
	build_add_err(&e, "bbb.c", 3, 0, "in b");

	/* first jump stays in aaa.c (the current buffer) */
	ASSERT(build_goto(&e, 1) == 1, "goto aaa failed");
	ASSERT(e.nbuf == 1, "opened a buffer for the current file");
	ASSERT(e.cy == 1, "not on aaa.c line 2");

	/* next jump crosses to bbb.c, opening it */
	ASSERT(build_goto(&e, 1) == 1, "goto bbb failed");
	ASSERT(e.nbuf == 2, "did not open bbb.c");
	ASSERT(strstr(e.path, "bbb.c") != NULL, "current file is not bbb.c");
	ASSERT(e.cy == 2, "not on bbb.c line 3");

	ed_close(&e);
	snprintf(pa, sizeof(pa), "%s/aaa.c", dir);
	unlink(pa);
	snprintf(pa, sizeof(pa), "%s/bbb.c", dir);
	unlink(pa);
	rmdir(dir);
	PASS();
}

static void
test_build_lines_index(void)
{
	struct build_lines b;

	TEST("streamed build output is indexed line by line");
	build_lines_init(&b);
	build_lines_append(&b, "line1\nline2\n", 12);
	ASSERT(b.nlines == 2, "two whole lines");
	ASSERT(strcmp(build_out_line(NULL, &b, 0), "line1") == 0, "line 0");
	ASSERT(strcmp(build_out_line(NULL, &b, 1), "line2") == 0, "line 1");
	ASSERT(build_out_line(NULL, &b, 2) == NULL, "no third line yet");

	/* a line split across two appends is stitched back together */
	build_lines_append(&b, "par", 3);
	ASSERT(b.nlines == 3, "partial line opens line 2");
	ASSERT(strcmp(build_out_line(NULL, &b, 2), "par") == 0, "partial");
	build_lines_append(&b, "tial\n", 5);
	ASSERT(b.nlines == 3, "completing a line adds no new line");
	ASSERT(strcmp(build_out_line(NULL, &b, 2), "partial") == 0, "joined");

	/* CRLF is trimmed; a trailing newline adds no empty line */
	build_lines_append(&b, "crlf\r\n", 6);
	ASSERT(b.nlines == 4, "crlf line counted");
	ASSERT(strcmp(build_out_line(NULL, &b, 3), "crlf") == 0, "crlf trim");

	/* the raw buffer keeps its newlines so parse_diagnostics can read it */
	ASSERT(strncmp(b.buf, "line1\nline2\n", 12) == 0, "raw newlines kept");
	build_lines_free(&b);
	PASS();
}

static void
test_build_lines_bytewise(void)
{
	struct build_lines b;
	const char *src = "alpha\nbeta\ngamma\n";
	size_t i;

	TEST("byte-at-a-time streaming indexes the same lines");
	build_lines_init(&b);
	for (i = 0; src[i] != '\0'; i++)
		build_lines_append(&b, src + i, 1);
	ASSERT(b.nlines == 3, "three lines");
	ASSERT(strcmp(build_out_line(NULL, &b, 0), "alpha") == 0, "l0");
	ASSERT(strcmp(build_out_line(NULL, &b, 1), "beta") == 0, "l1");
	ASSERT(strcmp(build_out_line(NULL, &b, 2), "gamma") == 0, "l2");
	ASSERT(build_out_line(NULL, &b, 3) == NULL, "no trailing empty line");
	build_lines_free(&b);
	PASS();
}

static int
vc_eq(struct vt_color a, struct vt_color b)
{
	if (a.type != b.type)
		return 0;
	if (a.type == VT_COLOR_INDEXED)
		return a.index == b.index;
	return 1;
}

static void
test_chrome_from_theme(void)
{
	const struct tui_theme *t = tui_theme_by_name("turbo");
	struct chrome_pal p;

	TEST("chrome palette is derived from a tui_theme");
	ASSERT(t != NULL, "turbo theme missing");
	memset(&p, 0, sizeof(p));
	chrome_from_theme(t, &p);
	ASSERT(vc_eq(p.content_fg, t->content_fg), "content fg from theme");
	ASSERT(vc_eq(p.content_bg, t->content_bg), "content bg from theme");
	ASSERT(vc_eq(p.frame_fg, t->border_fg), "frame fg from border");
	ASSERT(vc_eq(p.frame_bg, t->border_bg), "frame bg from border");
	ASSERT(vc_eq(p.title_fg, t->title_fg), "title fg from theme");
	ASSERT(vc_eq(p.bar_fg, t->content_bg), "bar fg inverts content");
	ASSERT(vc_eq(p.bar_bg, t->content_fg), "bar bg inverts content");
	ASSERT(p.reverse_bars == 0, "themed bars use explicit colors");
	PASS();
}

static void
test_syntax_color_config(void)
{
	struct vt_color pal[SYN_STYLE_COUNT];

	TEST("[edit.syntax] maps names to palette slots and parses colors");
	memset(pal, 0, sizeof(pal));
	ASSERT(syn_style_by_name("keyword") == SYN_KEYWORD, "keyword maps");
	ASSERT(syn_style_by_name("preproc") == SYN_PREPROC, "preproc maps");
	ASSERT(syn_style_by_name("nope") == -1, "unknown name rejected");

	ASSERT(syn_apply(pal, "keyword", "9") == 0, "indexed applied");
	ASSERT(pal[SYN_KEYWORD].type == VT_COLOR_INDEXED &&
	    pal[SYN_KEYWORD].index == 9, "keyword set to index 9");
	ASSERT(syn_apply(pal, "string", "default") == 0, "default applied");
	ASSERT(pal[SYN_STRING].type == VT_COLOR_DEFAULT, "string set default");
	ASSERT(syn_apply(pal, "comment", "#ff8800") == 0, "rgb applied");
	ASSERT(pal[SYN_COMMENT].type == VT_COLOR_RGB, "comment set rgb");

	ASSERT(syn_apply(pal, "bogus", "1") == -1, "unknown name not applied");
	ASSERT(syn_apply(pal, "keyword", "notacolor") == -1, "bad color rejected");
	ASSERT(pal[SYN_KEYWORD].index == 9, "bad value left slot unchanged");
	PASS();
}

static void
test_hex_helpers(void)
{
	struct editor e;
	struct mock m;
	unsigned char buf[16];
	char row[96];
	size_t cy, cx, got;
	unsigned char b2[2] = { 0x41, 0x0a };	/* 'A', newline */

	TEST("hex offset mapping, gather, and dump formatting");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ab\ncd");	/* bytes: a b \n c d, no trailing NL */
	ASSERT(hex_total(e.t) == 5, "total counts the implied newline");
	ASSERT(hex_offset_of(e.t, 0, 2) == 2, "offset of the newline slot");
	ASSERT(hex_offset_of(e.t, 1, 0) == 3, "offset of the second line");
	hex_pos_at(e.t, 3, &cy, &cx);
	ASSERT(cy == 1 && cx == 0, "pos at offset 3");
	hex_pos_at(e.t, 2, &cy, &cx);
	ASSERT(cy == 0 && cx == 2, "newline offset maps to end of line 0");

	got = hex_gather(e.t, 0, buf, sizeof(buf));
	ASSERT(got == 5, "gathered the whole stream");
	ASSERT(buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\n' &&
	    buf[3] == 'c' && buf[4] == 'd', "stream includes the newline");
	got = hex_gather(e.t, 3, buf, sizeof(buf));
	ASSERT(got == 2 && buf[0] == 'c' && buf[1] == 'd', "gather from mid");

	hex_format_row(row, sizeof(row), 0x10, b2, 2, 16);
	ASSERT(strncmp(row, "00000010", 8) == 0, "offset label");
	ASSERT(row[hex_hexcol(0)] == '4' && row[hex_hexcol(0) + 1] == '1',
	    "byte 0 hex pair");
	ASSERT(row[hex_hexcol(1)] == '0' && row[hex_hexcol(1) + 1] == 'a',
	    "byte 1 hex pair");
	ASSERT(row[hex_asciicol(0, 16)] == 'A', "printable ascii cell");
	ASSERT(row[hex_asciicol(1, 16)] == '.', "nonprintable ascii is a dot");
	ASSERT(hex_hexcol(0) == 10 && hex_hexcol(8) == 35, "the mid gap");

	/* the layout generalizes to other row widths */
	ASSERT(hex_asciicol(0, 16) == 61 && hex_row_width(16) == 78,
	    "16-byte row matches the original layout");
	ASSERT(hex_ascii_start(8) == 36 && hex_row_width(8) == 45,
	    "8-byte row is narrower");
	ASSERT(hex_hexcol(16) == 60 && hex_hexcol(24) == 85,
	    "hex columns keep an extra gap after each group of eight");
	hex_format_row(row, sizeof(row), 0, b2, 2, 8);
	ASSERT(row[hex_asciicol(0, 8) - 1] == '|' &&
	    row[hex_asciicol(7, 8) + 1] == '|', "8-byte gutter bars in place");
	ed_close(&e);
	PASS();
}

static void
test_hex_overwrite(void)
{
	struct editor e;
	struct mock m;
	const char *l;
	size_t len = 0, uy, ux;

	TEST("hex overwrite edits a data byte and refuses newline bytes");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "AB\nCD");	/* bytes: A B \n C D, total 5 */
	e.hex_pending = -1;

	hex_pos_at(e.t, 3, &e.cy, &e.cx);	/* the 'C' on line 1 */
	ASSERT(hex_overwrite(&e, 'x') == 1, "overwrote a data byte");
	l = text_line(e.t, 1, &len);
	ASSERT(len == 2 && memcmp(l, "xD", 2) == 0, "byte replaced in place");
	ASSERT(text_dirty(e.t), "the edit marks the buffer dirty");
	ASSERT(e.cy == 1 && e.cx == 0, "cursor stays on the edited byte");

	hex_pos_at(e.t, 2, &e.cy, &e.cx);	/* the implied newline */
	ASSERT(hex_overwrite(&e, 'q') == 0, "a newline byte is not editable");
	hex_pos_at(e.t, 0, &e.cy, &e.cx);
	ASSERT(hex_overwrite(&e, '\n') == 0, "writing a newline is refused");
	l = text_line(e.t, 0, &len);
	ASSERT(memcmp(l, "AB", 2) == 0, "line 0 unchanged by refusals");

	ASSERT(text_undo(e.t, &uy, &ux) == 0, "undo succeeds");
	l = text_line(e.t, 1, &len);
	ASSERT(memcmp(l, "CD", 2) == 0, "undo restores the original byte");
	ed_close(&e);
	PASS();
}

static void
test_hex_structural(void)
{
	struct editor e;
	struct mock m;
	const char *l;
	size_t len = 0, uy, ux;

	TEST("hex insert and delete change buffer structure");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "AC");		/* one line, bytes A C, total 2 */
	e.hex_pending = -1;

	/* insert a data byte between A and C */
	hex_pos_at(e.t, 1, &e.cy, &e.cx);
	hex_insert_byte(&e, 'B');
	l = text_line(e.t, 0, &len);
	ASSERT(len == 3 && memcmp(l, "ABC", 3) == 0, "byte inserted in line");
	ASSERT(hex_total(e.t) == 3, "total grew by one byte");
	ASSERT(hex_offset_of(e.t, e.cy, e.cx) == 2, "cursor lands after insert");

	/* insert a newline at the end: the line splits into two */
	hex_pos_at(e.t, hex_total(e.t), &e.cy, &e.cx);
	hex_insert_byte(&e, '\n');
	ASSERT(text_lines(e.t) == 2, "newline split adds a line");
	ASSERT(hex_total(e.t) == 4, "total counts the implied newline");

	/* delete that interior newline: the lines join back */
	hex_pos_at(e.t, 3, &e.cy, &e.cx);
	ASSERT(hex_delete_at(&e) == 1, "interior newline is deletable");
	ASSERT(text_lines(e.t) == 1, "join removes the split line");
	ASSERT(hex_total(e.t) == 3, "total shrank by the newline");

	/* delete the leading data byte */
	hex_pos_at(e.t, 0, &e.cy, &e.cx);
	ASSERT(hex_delete_at(&e) == 1, "data byte is deletable");
	l = text_line(e.t, 0, &len);
	ASSERT(len == 2 && memcmp(l, "BC", 2) == 0, "leading byte removed");

	/* backspace removes the byte before the cursor */
	hex_pos_at(e.t, 1, &e.cy, &e.cx);	/* on 'C' */
	hex_delete_prev(&e);
	l = text_line(e.t, 0, &len);
	ASSERT(len == 1 && memcmp(l, "C", 1) == 0, "backspace deletes prior byte");

	/* the sole trailing newline (the final_newline flag) is not a byte the
	 * keys can delete; construct it with a real load so the flag is set */
	{
		char tmpl[] = "/tmp/lumi_hexXXXXXX";
		int fd = mkstemp(tmpl);

		ASSERT(fd >= 0, "mkstemp failed");
		ASSERT(write(fd, "Z\n", 2) == 2, "short write");
		close(fd);
		ASSERT(text_load(e.t, tmpl) == 0, "load one line + newline");
		unlink(tmpl);
	}
	ASSERT(text_lines(e.t) == 1 && text_final_newline(e.t),
	    "loaded a single line with a trailing newline");
	hex_pos_at(e.t, 1, &e.cy, &e.cx);	/* the trailing newline */
	ASSERT(hex_delete_at(&e) == 0, "trailing final newline is protected");

	ed_close(&e);

	/* each edit is one undo step (a fresh buffer, since ed_settext appends) */
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "AC");
	e.hex_pending = -1;
	hex_pos_at(e.t, 1, &e.cy, &e.cx);
	hex_insert_byte(&e, 'B');
	ASSERT(text_undo(e.t, &uy, &ux) == 0, "undo succeeds");
	l = text_line(e.t, 0, &len);
	ASSERT(len == 2 && memcmp(l, "AC", 2) == 0, "undo removes the insert");
	ed_close(&e);
	PASS();
}

static void
test_hex_search(void)
{
	struct editor e;
	struct mock m;
	unsigned char pat[16];
	size_t plen = 0, found = 0;

	TEST("hex byte-pair parsing and wrapping search");
	ed_open(&e, &m, 24, 80);
	/* bytes: a b \n c a b (two lines), so "ab" occurs at 0 and 4 */
	ed_settext(&e, "ab\ncab");

	/* parser: spaces ignored, pairs required */
	ASSERT(hex_parse_bytes("61 62", pat, sizeof(pat), &plen) == 0 &&
	    plen == 2 && pat[0] == 0x61 && pat[1] == 0x62, "parsed two bytes");
	ASSERT(hex_parse_bytes("6162", pat, sizeof(pat), &plen) == 0 &&
	    plen == 2, "parsed without spaces");
	ASSERT(hex_parse_bytes("6", pat, sizeof(pat), &plen) == -1,
	    "a lone nibble is rejected");
	ASSERT(hex_parse_bytes("zz", pat, sizeof(pat), &plen) == -1,
	    "a non-hex character is rejected");

	pat[0] = 'a';
	pat[1] = 'b';
	plen = 2;
	/* forward from offset 0 finds the second "ab" at offset 4 */
	ASSERT(hex_find(e.t, pat, plen, 0, 1, &found) && found == 4,
	    "forward search skips the cursor and finds the next match");
	/* forward from 4 wraps around to the first match at 0 */
	ASSERT(hex_find(e.t, pat, plen, 4, 1, &found) && found == 0,
	    "forward search wraps to the start");
	/* backward from 4 finds the earlier match at 0 */
	ASSERT(hex_find(e.t, pat, plen, 4, -1, &found) && found == 0,
	    "backward search finds the previous match");
	/* backward from 0 wraps around to the last match at 4 */
	ASSERT(hex_find(e.t, pat, plen, 0, -1, &found) && found == 4,
	    "backward search wraps to the end");

	pat[0] = 'z';
	plen = 1;
	ASSERT(!hex_find(e.t, pat, plen, 0, 1, &found),
	    "a missing byte reports no match");

	/* a pattern spanning the implied newline: "b \n c" = 62 0a 63 */
	pat[0] = 0x62;
	pat[1] = 0x0a;
	pat[2] = 0x63;
	plen = 3;
	ASSERT(hex_find(e.t, pat, plen, 5, 1, &found) && found == 1,
	    "search matches across an implied newline");
	ed_close(&e);
	PASS();
}

static void
test_hex_yank_paste(void)
{
	struct editor e;
	struct mock m;
	const char *l;
	size_t len = 0;

	TEST("hex yank and paste move a byte range");
	ed_open(&e, &m, 24, 80);
	ed_settext(&e, "ABCDEF");	/* one line, 6 bytes */
	e.hex_pending = -1;

	/* select bytes 1..3 (BCD) with the anchor at 1 and cursor at 3 */
	e.hex_sel = 1;
	e.hex_anchor = 1;
	hex_pos_at(e.t, 3, &e.cy, &e.cx);
	hex_yank(&e);
	ASSERT(e.clip_len == 3 && memcmp(e.clip, "BCD", 3) == 0, "yanked BCD");
	ASSERT(e.hex_sel == 0, "yank clears the selection");

	/* paste the three bytes at offset 0 */
	hex_pos_at(e.t, 0, &e.cy, &e.cx);
	hex_paste(&e);
	l = text_line(e.t, 0, &len);
	ASSERT(len == 9 && memcmp(l, "BCDABCDEF", 9) == 0, "pasted at the start");

	/* with no selection, yank copies just the byte under the cursor */
	hex_pos_at(e.t, 0, &e.cy, &e.cx);
	hex_yank(&e);
	ASSERT(e.clip_len == 1 && e.clip[0] == 'B', "cursor-byte yank");

	/* a pasted newline splits the line */
	clip_set(&e, NULL, 0);
	{
		char *nl = malloc(1);

		nl[0] = '\n';
		clip_set(&e, nl, 1);
	}
	hex_pos_at(e.t, 3, &e.cy, &e.cx);	/* after "BCD" */
	hex_paste(&e);
	ASSERT(text_lines(e.t) == 2, "a pasted newline splits the buffer");
	ed_close(&e);
	PASS();
}

static void
test_hex_inspect(void)
{
	char line[160];
	unsigned char b[4];

	TEST("hex data inspector decodes bytes under the cursor");
	/* 0x41 0x00 0x01 0x00: u8 65, u16 le 65 / be 16640, u32 le 65601 */
	b[0] = 0x41;
	b[1] = 0x00;
	b[2] = 0x01;
	b[3] = 0x00;
	hex_inspect_line(line, sizeof(line), b, 4);
	ASSERT(strstr(line, "u8 65") != NULL, "u8 value");
	ASSERT(strstr(line, "i8 65") != NULL, "i8 value");
	ASSERT(strstr(line, "'A'") != NULL, "printable char");
	ASSERT(strstr(line, "u16 65/16640") != NULL, "u16 le/be");
	ASSERT(strstr(line, "u32 65601/1090519296") != NULL, "u32 le/be");

	/* a high byte: unsigned 255, signed -1, nonprintable char */
	b[0] = 0xff;
	hex_inspect_line(line, sizeof(line), b, 1);
	ASSERT(strstr(line, "u8 255") != NULL, "u8 of 0xff");
	ASSERT(strstr(line, "i8 -1") != NULL, "i8 of 0xff");
	ASSERT(strstr(line, "'.'") != NULL, "nonprintable shown as dot");
	/* with only one byte the wider fields are blank */
	ASSERT(strstr(line, "u16 -") != NULL && strstr(line, "u32 -") != NULL,
	    "wider fields need more bytes");
	PASS();
}

static void
test_center_box(void)
{
	struct editor e;
	struct mock m;
	int x = 0, y = 0;

	TEST("center_box centers a box in the screen");
	ed_open(&e, &m, 24, 80);
	center_box(&e, 20, 6, &x, &y);
	ASSERT(x == 30 && y == 9, "box not centered at (30, 9)");
	ed_close(&e);
	PASS();
}

int
main(void)
{
	printf("lumi edit tests:\n");

	test_render_text_and_menubar();
	test_insert_char();
	test_cursor_move_right();
	test_copy_line_no_selection();
	test_selection_copy();
	test_menu_mnemonic_lookup();
	test_dropdown_underline();
	test_vi_motion_l();
	test_visual_enter();
	test_visual_charwise_yank();
	test_visual_charwise_delete();
	test_visual_linewise_delete();
	test_visual_escape();
	test_visual_put_over();
	test_visual_swap_ends();
	test_textobj_diw();
	test_textobj_daw();
	test_textobj_di_paren();
	test_textobj_da_paren();
	test_textobj_ci_paren();
	test_textobj_di_quote();
	test_textobj_visual_iw();
	test_mark_set_and_jump();
	test_mark_line_jump();
	test_mark_operator();
	test_register_named_yank_put();
	test_register_two_independent();
	test_register_unnamed_mirrors();
	test_register_append_uppercase();
	test_register_released_by_motion();
	test_dot_repeat_x();
	test_dot_repeat_dw();
	test_dot_repeat_insert();
	test_dot_not_undo();
	test_dot_repeat_count();
	test_join_lines();
	test_toggle_case_count();
	test_delete_to_eol();
	test_change_to_eol();
	test_substitute_char();
	test_substitute_line();
	test_replace_char_count();
	test_replace_char_over_count();
	test_replace_char_newline();
	test_replace_mode_overtype();
	test_shift_right_count();
	test_shift_left_tab();
	test_shift_left_spaces();
	test_shift_operator_motion();
	test_shift_over_mark();
	test_shift_over_textobject();
	test_visual_shift();
	test_mark_operator_stale();
	test_search_backward();
	test_search_backward_wrap();
	test_search_word_star();
	test_search_word_hash();
	test_search_repeat_reverse();
	test_search_offset_end();
	test_search_offset_start_negative();
	test_search_offset_lines();
	test_search_offset_reuse_and_errors();
	test_ex_delete_range();
	test_ex_delete_all();
	test_ex_goto_dollar();
	test_ex_shift_range();
	test_ex_mark_range();
	test_ex_subst_first();
	test_ex_subst_global();
	test_ex_subst_all_lines();
	test_ex_subst_delete();
	test_ex_subst_grows_line();
	test_ex_global_delete();
	test_ex_global_inverse();
	test_ex_global_subst();
	test_ex_global_delete_all();
	test_ex_range_reversed();
	test_ex_subst_long_rep();
	test_delete_paragraph_linewise();
	test_delete_paragraph_charwise();
	test_percent_goto();
	test_percent_match_still_works();
	test_column_memory_jk();
	test_column_memory_reset();
	test_column_memory_dollar();
	test_goto_line_counts();
	test_delete_to_line_count();
	test_buffers_open_switch();
	test_buffers_cycle();
	test_buffers_open_existing();
	test_buffers_close();
	test_ex_buffer_commands();
	test_ex_edit_opens_file();
	test_ex_bd_refuses_dirty();
	test_buffers_highlight_lifecycle();
	test_buffers_do_new_in_place();
	test_ex_read_file();
	test_ex_read_missing_file();
	test_menu_buffer_nav();
	test_build_errs_global();
	test_parse_diag_skips_context();
	test_build_goto_cross_file();
	test_build_lines_index();
	test_build_lines_bytewise();
	test_chrome_from_theme();
	test_syntax_color_config();
	test_hex_helpers();
	test_hex_overwrite();
	test_hex_structural();
	test_hex_search();
	test_hex_yank_paste();
	test_hex_inspect();
	test_delete_region_multiline();
	test_center_box();

	printf("test_edit: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
