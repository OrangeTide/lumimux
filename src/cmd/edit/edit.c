/* edit.c : lumi edit -- modeless text editor */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "text.h"
#include "tkbd.h"
#include "tui_out.h"
#include "tui_theme.h"
#include "utf8.h"
#include "rune_width.h"
#include "vt_cell.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TAB_WIDTH 8

static const char *progname = "lumi-edit";

/* Set by the SIGWINCH handler; polled from the input loop's idle tick. */
static volatile sig_atomic_t resized;

struct editor {
	struct text	*t;
	char		path[PATH_MAX];
	int		has_name;
	size_t		cy;		/* cursor line */
	size_t		cx;		/* cursor byte offset within the line */
	size_t		top;		/* first visible line */
	size_t		left;		/* horizontal scroll, display columns */
	int		rows;
	int		cols;
	int		in_session;
	int		sel_active;	/* a selection is being extended */
	size_t		ay;		/* selection anchor line */
	size_t		ax;		/* selection anchor byte column */
	char		*clip;		/* internal clipboard bytes */
	size_t		clip_len;	/* length of clip in bytes */
	char		last_find[256];	/* last search string, for repeat */
	char		status[160];
};

/****************************************************************
 * Keymap -- the table a modal (vi) personality would swap out
 ****************************************************************/

enum cmd {
	CMD_NONE,
	CMD_INSERT,		/* self-insert the typed character */
	CMD_TAB,
	CMD_NEWLINE,
	CMD_BACKSPACE,
	CMD_DELETE,
	CMD_LEFT,
	CMD_RIGHT,
	CMD_UP,
	CMD_DOWN,
	CMD_HOME,
	CMD_END,
	CMD_PGUP,
	CMD_PGDN,
	CMD_UNDO,
	CMD_REDO,
	CMD_COPY,
	CMD_CUT,
	CMD_PASTE,
	CMD_SEND,		/* send selection/line to another pane */
	CMD_FIND,		/* prompt for a string and jump to it */
	CMD_GOTO,		/* prompt for a line number and jump to it */
	CMD_HELP,		/* show the key bindings */
	CMD_SAVE,
	CMD_QUIT,
};

/* What the main loop must do after a command; save and quit may prompt, so
 * they are carried out there where the input stream is available. */
enum req {
	REQ_CONTINUE,
	REQ_FIND,
	REQ_GOTO,
	REQ_HELP,
	REQ_SAVE,
	REQ_QUIT,
};

struct keybind {
	uint16_t	key;		/* TKBD_KEY_* to match */
	uint8_t		ctrl;		/* require the Ctrl modifier */
	enum cmd	cmd;
};

static const struct keybind keymap[] = {
	{ TKBD_KEY_Q,		1, CMD_QUIT },
	{ TKBD_KEY_S,		1, CMD_SAVE },
	{ TKBD_KEY_Z,		1, CMD_UNDO },
	{ TKBD_KEY_Y,		1, CMD_REDO },
	{ TKBD_KEY_C,		1, CMD_COPY },
	{ TKBD_KEY_X,		1, CMD_CUT },
	{ TKBD_KEY_V,		1, CMD_PASTE },
	{ TKBD_KEY_G,		1, CMD_SEND },
	{ TKBD_KEY_F,		1, CMD_FIND },
	{ TKBD_KEY_L,		1, CMD_GOTO },
	{ TKBD_KEY_F1,		0, CMD_HELP },
	{ TKBD_KEY_LEFT,	0, CMD_LEFT },
	{ TKBD_KEY_RIGHT,	0, CMD_RIGHT },
	{ TKBD_KEY_UP,		0, CMD_UP },
	{ TKBD_KEY_DOWN,	0, CMD_DOWN },
	{ TKBD_KEY_HOME,	0, CMD_HOME },
	{ TKBD_KEY_END,		0, CMD_END },
	{ TKBD_KEY_PGUP,	0, CMD_PGUP },
	{ TKBD_KEY_PGDN,	0, CMD_PGDN },
	{ TKBD_KEY_ENTER,	0, CMD_NEWLINE },
	{ TKBD_KEY_BACKSPACE,	0, CMD_BACKSPACE },
	{ TKBD_KEY_BACKSPACE2,	0, CMD_BACKSPACE },
	{ TKBD_KEY_DEL,		0, CMD_DELETE },
	{ TKBD_KEY_TAB,		0, CMD_TAB },
};

#define KEYMAP_COUNT ((int)(sizeof(keymap) / sizeof(keymap[0])))

static enum cmd
key_to_cmd(const struct tkbd_seq *seq)
{
	int i;

	if (seq->type != TKBD_KEY)
		return CMD_NONE;

	for (i = 0; i < KEYMAP_COUNT; i++) {
		int want_ctrl = keymap[i].ctrl;
		int has_ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;

		if (keymap[i].key == seq->key && want_ctrl == has_ctrl)
			return keymap[i].cmd;
	}

	/* an ordinary printable character self-inserts */
	if (!(seq->mod & TKBD_MOD_CTRL) && seq->ch != TKBD_CH_NONE &&
	    seq->ch >= 0x20 && seq->ch != 0x7f)
		return CMD_INSERT;

	return CMD_NONE;
}

/****************************************************************
 * UTF-8 and display-column helpers
 ****************************************************************/

/* Display columns spanned by the first nbytes of s, expanding tabs to the
 * next TAB_WIDTH stop and honoring rune widths. */
static int
disp_cols(const char *s, size_t nbytes)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;

	while (i < nbytes) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, nbytes - i);
		int w;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}
		col += w;
		i += (size_t)n;
	}
	return col;
}

/* Byte length of the UTF-8 rune ending just before byte offset cx. */
static size_t
prev_rune_len(const char *s, size_t cx)
{
	size_t n = 1;

	while (n < cx && ((unsigned char)s[cx - n] & 0xc0) == 0x80)
		n++;
	return n;
}

/* Byte length of the UTF-8 rune starting at byte offset cx. */
static size_t
rune_len_at(const char *s, size_t len, size_t cx)
{
	uint32_t r;
	int n;

	if (cx >= len)
		return 0;
	n = utf8_decode(&r, (const unsigned char *)s + cx, len - cx);
	return n > 0 ? (size_t)n : 1;
}

/****************************************************************
 * Rendering
 ****************************************************************/

/* Draw one text line clipped to the display window [left, left+width),
 * expanding tabs. Trailing space pads the field to width. Display columns in
 * [hl_start, hl_end) are shown in reverse video for the selection; pass
 * hl_start >= hl_end for no highlight. */
static void
draw_line(struct tui_out *o, const char *s, size_t len, int left, int width,
    int hl_start, int hl_end)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;		/* display column at the start of this rune */
	int drawn = 0;		/* columns emitted into the window */
	int hl_on = 0;		/* reverse video currently emitted */

	while (i < len && drawn < width) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, len - i);
		int w;
		int want;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}

		if (col + w <= left) {
			/* wholly left of the window */
			col += w;
			i += (size_t)n;
			continue;
		}

		/* toggle reverse video at the selection boundaries (which fall
		 * on rune edges, so the whole rune is in or out) */
		want = (hl_start < hl_end && col >= hl_start && col < hl_end);
		if (want != hl_on) {
			tui_out_puts(o, want ? "\033[7m" : "\033[27m");
			hl_on = want;
		}

		if (r == '\t' || r < 0x20 || r == 0x7f) {
			/* render as spaces, clipped at both edges */
			int c;

			for (c = 0; c < w; c++) {
				if (col + c < left)
					continue;
				if (drawn >= width)
					break;
				tui_out_put(o, " ", 1);
				drawn++;
			}
		} else if (col < left) {
			/* a wide rune straddling the left edge: pad */
			int c;

			for (c = 0; c < w && drawn < width; c++) {
				tui_out_put(o, " ", 1);
				drawn++;
			}
		} else if (drawn + w > width) {
			break;			/* would overflow right edge */
		} else {
			tui_out_put(o, (const char *)p + i, (size_t)n);
			drawn += w;
		}
		col += w;
		i += (size_t)n;
	}
	if (hl_on)
		tui_out_puts(o, "\033[27m");	/* stop before the padding */
	while (drawn < width) {
		tui_out_put(o, " ", 1);
		drawn++;
	}
}

/* Adjust top/left so the cursor stays on screen. */
static void
scroll_to_cursor(struct editor *e, int text_h)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);
	int cur_col = line ? disp_cols(line, e->cx) : 0;

	if (e->cy < e->top)
		e->top = e->cy;
	if (text_h > 0 && e->cy >= e->top + (size_t)text_h)
		e->top = e->cy - (size_t)text_h + 1;

	if ((size_t)cur_col < e->left)
		e->left = (size_t)cur_col;
	if (e->cols > 0 && (size_t)cur_col >= e->left + (size_t)e->cols)
		e->left = (size_t)cur_col - (size_t)e->cols + 1;
}

/* Order the anchor and cursor so (*y1,*x1) is the start of the selection and
 * (*y2,*x2) the end. */
static void
sel_bounds(const struct editor *e, size_t *y1, size_t *x1,
    size_t *y2, size_t *x2)
{
	if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
		*y1 = e->ay;
		*x1 = e->ax;
		*y2 = e->cy;
		*x2 = e->cx;
	} else {
		*y1 = e->cy;
		*x1 = e->cx;
		*y2 = e->ay;
		*x2 = e->ax;
	}
}

static void
render(struct editor *e, struct tui_out *o)
{
	const struct tui_theme *t = tui_theme_default();
	int text_h = e->rows - 1;	/* last row is the status bar */
	int i;
	size_t len = 0;
	const char *cur = text_line(e->t, e->cy, &len);
	int cur_col;
	char bar[PATH_MAX + 64];

	if (text_h < 1)
		text_h = 1;
	scroll_to_cursor(e, text_h);
	cur_col = cur ? disp_cols(cur, e->cx) : 0;

	tui_out_reset(o);
	tui_out_puts(o, "\033[H");	/* home; cursor stays visible */

	for (i = 0; i < text_h; i++) {
		size_t idx = e->top + (size_t)i;
		size_t llen = 0;
		const char *s = text_line(e->t, idx, &llen);
		int hs = -1, he = -1;

		if (e->sel_active && s) {
			size_t y1, x1, y2, x2;

			sel_bounds(e, &y1, &x1, &y2, &x2);
			if (idx >= y1 && idx <= y2) {
				size_t a = (idx == y1) ? x1 : 0;
				size_t b = (idx == y2) ? x2 : llen;

				hs = disp_cols(s, a);
				he = disp_cols(s, b);
			}
		}

		tui_out_move(o, i + 1, 1);
		tui_out_sgr(o, &t->content_fg, &t->content_bg, 0);
		if (s)
			draw_line(o, s, llen, (int)e->left, e->cols, hs, he);
		else
			draw_line(o, "", 0, 0, e->cols, -1, -1);  /* past EOF */
		tui_out_sgr_reset(o);
	}

	/* status bar: a transient message when set, else name and position */
	if (e->status[0])
		snprintf(bar, sizeof(bar), " %s", e->status);
	else
		snprintf(bar, sizeof(bar), " %s%s  %zu:%zu%s",
		    e->has_name ? e->path : "[no name]",
		    text_dirty(e->t) ? " *" : "",
		    e->cy + 1, (size_t)cur_col + 1,
		    e->in_session ? "  [session]" : "");
	tui_out_move(o, e->rows > 0 ? e->rows : 24, 1);
	tui_out_sgr(o, &t->title_fg, &t->border_bg, 1);
	tui_out_field(o, bar, e->cols);
	tui_out_sgr_reset(o);

	/* place the hardware cursor */
	tui_out_move(o, (int)(e->cy - e->top) + 1,
	    cur_col - (int)e->left + 1);

	(void)tui_out_flush(o, STDOUT_FILENO);
}

/****************************************************************
 * Editing operations
 ****************************************************************/

static void
move_left(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx > 0) {
		e->cx -= prev_rune_len(line, e->cx);
	} else if (e->cy > 0) {
		e->cy--;
		e->cx = text_line_len(e->t, e->cy);
	}
}

static void
move_right(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx < len) {
		e->cx += rune_len_at(line, len, e->cx);
	} else if (e->cy + 1 < text_lines(e->t)) {
		e->cy++;
		e->cx = 0;
	}
}

/* Clamp the cursor to a valid line and column. */
static void
clamp_col(struct editor *e)
{
	size_t len;

	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	len = text_line_len(e->t, e->cy);
	if (e->cx > len)
		e->cx = len;
}

static void
do_insert(struct editor *e, const char *bytes, size_t n)
{
	if (text_insert(e->t, e->cy, e->cx, bytes, n) == 0)
		e->cx += n;
}

static void
do_backspace(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx > 0) {
		size_t rl = prev_rune_len(line, e->cx);

		text_delete(e->t, e->cy, e->cx - rl, rl);
		e->cx -= rl;
	} else if (e->cy > 0) {
		size_t plen = text_line_len(e->t, e->cy - 1);

		text_join(e->t, e->cy - 1);
		e->cy--;
		e->cx = plen;
	}
}

static void
do_delete(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx < len)
		text_delete(e->t, e->cy, e->cx, rune_len_at(line, len, e->cx));
	else if (e->cy + 1 < text_lines(e->t))
		text_join(e->t, e->cy);
}

static void
do_newline(struct editor *e)
{
	if (text_split(e->t, e->cy, e->cx) == 0) {
		e->cy++;
		e->cx = 0;
	}
}

/* defined below, in the selection and clipboard section */
static void delete_region(struct editor *e, size_t y1, size_t x1,
    size_t y2, size_t x2);

/* Consume a bracketed-paste payload (PASTE_BEGIN was just read) and insert
 * it literally, so control bytes in the paste never fire editor commands.
 * CR, LF, and CRLF all become one newline. Undo boundaries fence the paste
 * off from the surrounding edits. */
static void
paste_input(struct editor *e, struct tkbd_stream *s)
{
	int saw_cr = 0;

	if (e->sel_active) {		/* a paste replaces the selection */
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
	}
	text_undo_boundary(e->t);	/* separate the paste from prior typing */
	for (;;) {
		struct tkbd_seq seq;
		unsigned char buf[8];
		int n;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;			/* read error ends the paste */
		}
		if (n == 0)
			continue;		/* idle tick */
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_PASTE_END)
			break;

		if (seq.key == TKBD_KEY_ENTER) {
			/* collapse a CR immediately followed by LF */
			if (seq.ch == 0x0a && saw_cr) {
				saw_cr = 0;
				continue;
			}
			do_newline(e);
			saw_cr = (seq.ch == 0x0d);
			continue;
		}
		saw_cr = 0;
		if (seq.key == TKBD_KEY_TAB) {
			do_insert(e, "\t", 1);
			continue;
		}
		if (seq.ch != TKBD_CH_NONE && seq.ch >= 0x20 &&
		    seq.ch != 0x7f) {
			n = utf8_encode(buf, seq.ch);
			if (n > 0)
				do_insert(e, (char *)buf, (size_t)n);
		}
		/* other control and function keys are dropped inside a paste */
	}
	text_undo_boundary(e->t);
}

static int
is_movement(enum cmd cmd)
{
	switch (cmd) {
	case CMD_LEFT:
	case CMD_RIGHT:
	case CMD_UP:
	case CMD_DOWN:
	case CMD_HOME:
	case CMD_END:
	case CMD_PGUP:
	case CMD_PGDN:
		return 1;
	default:
		return 0;
	}
}

/* Copy the selection [y1,x1)-(y2,x2) into a fresh buffer, joining lines with
 * '\n'. Returns NULL on allocation failure. Sets *outlen to the byte count. */
static char *
region_text(struct editor *e, size_t y1, size_t x1, size_t y2, size_t x2,
    size_t *outlen)
{
	size_t cap = 0, len = 0, y;
	char *buf = NULL;

	for (y = y1; y <= y2; y++) {
		size_t llen = 0;
		const char *s = text_line(e->t, y, &llen);
		size_t a = (y == y1) ? x1 : 0;
		size_t b = (y == y2) ? x2 : llen;
		size_t seg = (b > a) ? b - a : 0;
		size_t need = len + seg + 1;	/* room for a joining newline */

		if (need > cap) {
			char *nb = realloc(buf, need + 64);

			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap = need + 64;
		}
		if (seg && s) {
			memcpy(buf + len, s + a, seg);
			len += seg;
		}
		if (y < y2)
			buf[len++] = '\n';
	}
	if (!buf)
		buf = malloc(1);	/* empty selection: a valid 0-byte clip */
	*outlen = len;
	return buf;
}

/* Delete the selection [y1,x1)-(y2,x2) and leave the cursor at its start. */
static void
delete_region(struct editor *e, size_t y1, size_t x1, size_t y2, size_t x2)
{
	if (y1 == y2) {
		text_delete(e->t, y1, x1, x2 - x1);
	} else {
		size_t first_len = 0;
		size_t k;

		text_line(e->t, y1, &first_len);
		text_delete(e->t, y1, x1, first_len - x1);
		text_delete(e->t, y2, 0, x2);
		for (k = y1; k < y2; k++)	/* pull each later line up */
			text_join(e->t, y1);
	}
	e->cy = y1;
	e->cx = x1;
}

/* Insert the clipboard at the cursor, breaking lines on embedded newlines. */
static void
insert_clip(struct editor *e)
{
	size_t i = 0;

	while (i < e->clip_len) {
		size_t j = i;

		while (j < e->clip_len && e->clip[j] != '\n')
			j++;
		if (j > i)
			do_insert(e, e->clip + i, j - i);
		if (j < e->clip_len)
			do_newline(e);		/* the newline itself */
		i = (j < e->clip_len) ? j + 1 : j;
	}
}

/* Cap on the raw byte count mirrored to the system clipboard. OSC 52 rides
 * the terminal's input path, and many terminals cap or drop very long
 * sequences, so keep the payload modest; the internal clipboard is unbounded.
 */
#define OSC52_MAX 100000

/* Write all of a buffer, retrying short writes and EINTR. Best effort: on a
 * hard error it gives up rather than disturbing the editing session. */
static void
full_write(int fd, const char *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		p += w;
		n -= (size_t)w;
	}
}

/* Mirror a copied span to the terminal's system clipboard via OSC 52, so it
 * can be pasted into other applications. Terminals without OSC 52 ignore it,
 * and the reverse direction (system to editor) is handled by bracketed paste.
 */
static void
osc52_copy(const char *bytes, size_t len)
{
	static const char b64[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *enc;
	size_t i, o;

	if (len == 0 || len > OSC52_MAX)
		return;
	enc = malloc(((len + 2) / 3) * 4);
	if (!enc)
		return;
	for (i = 0, o = 0; i < len; i += 3) {
		unsigned a = (unsigned char)bytes[i];
		unsigned b = (i + 1 < len) ? (unsigned char)bytes[i + 1] : 0;
		unsigned c = (i + 2 < len) ? (unsigned char)bytes[i + 2] : 0;

		enc[o++] = b64[a >> 2];
		enc[o++] = b64[((a & 3) << 4) | (b >> 4)];
		enc[o++] = (i + 1 < len) ? b64[((b & 0x0f) << 2) | (c >> 6)] : '=';
		enc[o++] = (i + 2 < len) ? b64[c & 0x3f] : '=';
	}
	full_write(STDOUT_FILENO, "\033]52;c;", 7);
	full_write(STDOUT_FILENO, enc, o);
	full_write(STDOUT_FILENO, "\033\\", 2);	/* ST terminator */
	free(enc);
}

static void
clip_set(struct editor *e, char *bytes, size_t len)
{
	free(e->clip);
	e->clip = bytes;
	e->clip_len = len;
	osc52_copy(bytes, len);		/* mirror to the system clipboard */
}

/* Send the selection, or the current line when nothing is selected, to
 * another window of the session as typed input. A carriage return is
 * appended so the line runs in the target shell or REPL. */
static void
do_send(struct editor *e)
{
	const char *session = getenv("LUMI_SESSION");
	char *text = NULL, *payload;
	size_t tlen = 0;

	if (!session) {
		snprintf(e->status, sizeof(e->status), "not in a session");
		return;
	}
	if (e->sel_active) {
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		text = region_text(e, y1, x1, y2, x2, &tlen);
	} else {
		size_t ll = 0;
		const char *s = text_line(e->t, e->cy, &ll);

		text = malloc(ll + 1);
		if (text && ll)
			memcpy(text, s, ll);
		tlen = ll;
	}
	if (!text) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}

	payload = malloc(tlen + 1);
	if (!payload) {
		free(text);
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}
	memcpy(payload, text, tlen);
	payload[tlen] = '\r';		/* submit the line in the target */
	free(text);

	switch (lu_send_input(session, -1, payload, tlen + 1)) {
	case LU_SEND_OK:
		snprintf(e->status, sizeof(e->status),
		    "sent %zu bytes to another pane", tlen);
		break;
	case LU_SEND_NO_TARGET:
		snprintf(e->status, sizeof(e->status),
		    "no other window to send to");
		break;
	case LU_SEND_READONLY:
		snprintf(e->status, sizeof(e->status),
		    "target pane is read-only");
		break;
	case LU_SEND_NO_SESSION:
		snprintf(e->status, sizeof(e->status), "session not found");
		break;
	case LU_SEND_ERROR:
		snprintf(e->status, sizeof(e->status), "send failed");
		break;
	}
	free(payload);
	e->sel_active = 0;		/* consume the selection */
}

/* Carry out one command, returning what the main loop must do next. */
static enum req
dispatch(struct editor *e, enum cmd cmd, const struct tkbd_seq *seq)
{
	int page = e->rows - 2;
	unsigned char buf[8];
	int n;

	if (page < 1)
		page = 1;

	/* any command other than typing ends the current undo run */
	if (cmd != CMD_INSERT && cmd != CMD_TAB)
		text_undo_boundary(e->t);

	/* Shift + a movement key extends a selection from an anchor; an
	 * unshifted movement clears it. */
	if (is_movement(cmd)) {
		if (seq && (seq->mod & TKBD_MOD_SHIFT)) {
			if (!e->sel_active) {
				e->sel_active = 1;
				e->ay = e->cy;
				e->ax = e->cx;
			}
		} else {
			e->sel_active = 0;
		}
	}

	/* Editing over a selection replaces it: drop the selected text first,
	 * then let insertion proceed. Backspace and Delete are satisfied by
	 * that removal alone. */
	if (e->sel_active && (cmd == CMD_INSERT || cmd == CMD_TAB ||
	    cmd == CMD_NEWLINE || cmd == CMD_BACKSPACE || cmd == CMD_DELETE)) {
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		if (cmd == CMD_BACKSPACE || cmd == CMD_DELETE)
			return REQ_CONTINUE;
	}

	switch (cmd) {
	case CMD_QUIT:
		return REQ_QUIT;
	case CMD_SAVE:
		return REQ_SAVE;
	case CMD_UNDO:
		e->sel_active = 0;	/* the buffer shifts under the anchor */
		if (text_undo(e->t, &e->cy, &e->cx) != 0)
			snprintf(e->status, sizeof(e->status),
			    "nothing to undo");
		else
			clamp_col(e);
		break;
	case CMD_REDO:
		e->sel_active = 0;
		if (text_redo(e->t, &e->cy, &e->cx) != 0)
			snprintf(e->status, sizeof(e->status),
			    "nothing to redo");
		else
			clamp_col(e);
		break;
	case CMD_COPY: {
		size_t y1, x1, y2, x2, rl = 0;
		char *r;

		if (e->sel_active) {
			sel_bounds(e, &y1, &x1, &y2, &x2);
			r = region_text(e, y1, x1, y2, x2, &rl);
			if (r) {
				clip_set(e, r, rl);
				snprintf(e->status, sizeof(e->status),
				    "copied %zu bytes", rl);
			}
		} else {
			size_t ll = 0;
			const char *s = text_line(e->t, e->cy, &ll);

			r = malloc(ll + 1);	/* the line plus its newline */
			if (r) {
				if (ll && s)
					memcpy(r, s, ll);
				r[ll] = '\n';
				clip_set(e, r, ll + 1);
				snprintf(e->status, sizeof(e->status),
				    "copied line");
			}
		}
		break;
	}
	case CMD_CUT: {
		size_t y1, x1, y2, x2, rl = 0;
		char *r;

		if (!e->sel_active) {
			snprintf(e->status, sizeof(e->status),
			    "select text first (Shift+arrows)");
			break;
		}
		sel_bounds(e, &y1, &x1, &y2, &x2);
		r = region_text(e, y1, x1, y2, x2, &rl);
		if (r)
			clip_set(e, r, rl);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		snprintf(e->status, sizeof(e->status), "cut %zu bytes", rl);
		break;
	}
	case CMD_PASTE:
		if (!e->clip || e->clip_len == 0) {
			snprintf(e->status, sizeof(e->status),
			    "clipboard is empty");
			break;
		}
		if (e->sel_active) {		/* paste replaces the selection */
			size_t y1, x1, y2, x2;

			sel_bounds(e, &y1, &x1, &y2, &x2);
			delete_region(e, y1, x1, y2, x2);
			e->sel_active = 0;
		}
		insert_clip(e);
		break;
	case CMD_SEND:
		do_send(e);
		break;
	case CMD_FIND:
		return REQ_FIND;
	case CMD_GOTO:
		return REQ_GOTO;
	case CMD_HELP:
		return REQ_HELP;
	case CMD_INSERT:
		n = utf8_encode(buf, seq->ch);
		if (n > 0)
			do_insert(e, (char *)buf, (size_t)n);
		break;
	case CMD_TAB:
		do_insert(e, "\t", 1);
		break;
	case CMD_NEWLINE:
		do_newline(e);
		break;
	case CMD_BACKSPACE:
		do_backspace(e);
		break;
	case CMD_DELETE:
		do_delete(e);
		break;
	case CMD_LEFT:
		move_left(e);
		break;
	case CMD_RIGHT:
		move_right(e);
		break;
	case CMD_UP:
		if (e->cy > 0) {
			e->cy--;
			clamp_col(e);
		}
		break;
	case CMD_DOWN:
		if (e->cy + 1 < text_lines(e->t)) {
			e->cy++;
			clamp_col(e);
		}
		break;
	case CMD_HOME:
		e->cx = 0;
		break;
	case CMD_END:
		e->cx = text_line_len(e->t, e->cy);
		break;
	case CMD_PGUP:
		e->cy = e->cy > (size_t)page ? e->cy - (size_t)page : 0;
		clamp_col(e);
		break;
	case CMD_PGDN:
		e->cy += (size_t)page;
		if (e->cy >= text_lines(e->t))
			e->cy = text_lines(e->t) - 1;
		clamp_col(e);
		break;
	case CMD_NONE:
		break;
	}
	return REQ_CONTINUE;
}

/****************************************************************
 * Status-line prompts
 ****************************************************************/

/* Draw the editor frame, then overlay a prompt on the status row and leave
 * the cursor at the end of the typed text. */
static void
draw_prompt(struct editor *e, struct tui_out *o, const char *q,
    const char *buf)
{
	const struct tui_theme *t = tui_theme_default();
	char line[512];
	int col;

	render(e, o);
	tui_out_reset(o);
	col = snprintf(line, sizeof(line), "%s%s", q, buf ? buf : "");
	tui_out_move(o, e->rows > 0 ? e->rows : 24, 1);
	tui_out_sgr(o, &t->sel_fg, &t->sel_bg, 1);
	tui_out_field(o, line, e->cols);
	tui_out_sgr_reset(o);
	if (col >= e->cols)
		col = e->cols - 1;
	tui_out_move(o, e->rows > 0 ? e->rows : 24, col + 1);
	(void)tui_out_flush(o, STDOUT_FILENO);
}

/* Read a y/n answer. Returns 1 for yes, 0 for no, -1 if cancelled. */
static int
confirm_yn(struct editor *e, struct tui_out *o, struct tkbd_stream *s,
    const char *q)
{
	draw_prompt(e, o, q, "");
	for (;;) {
		struct tkbd_seq seq;
		int n;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			continue;
		if (seq.ch == 'y' || seq.ch == 'Y')
			return 1;
		if (seq.ch == 'n' || seq.ch == 'N')
			return 0;
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return -1;
	}
}

/* Read a line of text. buf is edited in place, so a caller may pre-fill it
 * with a default (for example the last search). Returns 1 with buf filled, or
 * 0 if cancelled or left empty. */
static int
prompt_line(struct editor *e, struct tui_out *o, struct tkbd_stream *s,
    const char *q, char *buf, size_t bufsz)
{
	size_t len = strlen(buf);

	for (;;) {
		struct tkbd_seq seq;
		int n;

		draw_prompt(e, o, q, buf);
		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (n == 0)
			continue;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_ENTER)
			return len > 0;
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return 0;
		if (seq.key == TKBD_KEY_BACKSPACE ||
		    seq.key == TKBD_KEY_BACKSPACE2) {
			while (len > 0 &&
			    ((unsigned char)buf[len - 1] & 0xc0) == 0x80)
				len--;		/* drop UTF-8 continuation */
			if (len > 0)
				len--;
			buf[len] = '\0';
			continue;
		}
		if (!(seq.mod & TKBD_MOD_CTRL) && seq.ch != TKBD_CH_NONE &&
		    seq.ch >= 0x20 && seq.ch != 0x7f) {
			unsigned char enc[8];
			int el = utf8_encode(enc, seq.ch);

			if (el > 0 && len + (size_t)el < bufsz) {
				memcpy(buf + len, enc, (size_t)el);
				len += (size_t)el;
				buf[len] = '\0';
			}
		}
	}
}

/* Save the buffer, prompting for a name if it has none. Returns 0 on a
 * successful save, -1 on failure or when the save was cancelled. */
static int
save_editor(struct editor *e, struct tui_out *o, struct tkbd_stream *s)
{
	if (!e->has_name) {
		char name[PATH_MAX];

		name[0] = '\0';
		if (!prompt_line(e, o, s, "Save as: ", name, sizeof(name))) {
			snprintf(e->status, sizeof(e->status), "save cancelled");
			return -1;
		}
		snprintf(e->path, sizeof(e->path), "%s", name);
		e->has_name = 1;
	}

	if (text_save(e->t, e->path) < 0) {
		snprintf(e->status, sizeof(e->status), "save failed: %s",
		    strerror(errno));
		return -1;
	}
	snprintf(e->status, sizeof(e->status), "wrote %.120s", e->path);
	return 0;
}

/* Move the cursor to the next occurrence of q at or after the cursor, wrapping
 * to the top of the buffer. The search is over raw bytes and case-sensitive.
 * Sets a status message reporting the result. */
static void
do_find(struct editor *e, const char *q)
{
	size_t nlines = text_lines(e->t);
	size_t i;

	if (!q[0])
		return;

	/* Scan the current line after the cursor, then each following line,
	 * then wrap around and finish the start of the current line. */
	for (i = 0; i <= nlines; i++) {
		size_t ln = (e->cy + i) % nlines;
		size_t llen = 0;
		const char *s = text_line(e->t, ln, &llen);
		size_t from = (i == 0) ? e->cx + 1 : 0;
		const char *hit;

		if (!s || from > llen)
			continue;
		hit = strstr(s + from, q);
		if (hit) {
			e->cy = ln;
			e->cx = (size_t)(hit - s);
			e->sel_active = 0;
			snprintf(e->status, sizeof(e->status),
			    "found '%.80s' (line %zu)", q, ln + 1);
			return;
		}
	}
	snprintf(e->status, sizeof(e->status), "not found: %.80s", q);
}

/* Prompt for a search string (defaulting to the last one, so Enter repeats)
 * and jump to the next match. */
static void
find_prompt(struct editor *e, struct tui_out *o, struct tkbd_stream *s)
{
	char q[256];

	snprintf(q, sizeof(q), "%s", e->last_find);
	if (!prompt_line(e, o, s, "Search: ", q, sizeof(q))) {
		snprintf(e->status, sizeof(e->status), "search cancelled");
		return;
	}
	snprintf(e->last_find, sizeof(e->last_find), "%s", q);
	do_find(e, q);
}

/* Prompt for a 1-based line number and move the cursor to that line. A number
 * past the end clamps to the last line. */
static void
goto_prompt(struct editor *e, struct tui_out *o, struct tkbd_stream *s)
{
	char buf[32], *end;
	long ln;

	buf[0] = '\0';
	if (!prompt_line(e, o, s, "Go to line: ", buf, sizeof(buf))) {
		snprintf(e->status, sizeof(e->status), "goto cancelled");
		return;
	}
	ln = strtol(buf, &end, 10);
	if (end == buf || ln < 1) {
		snprintf(e->status, sizeof(e->status), "bad line number");
		return;
	}
	if ((size_t)ln > text_lines(e->t))
		ln = (long)text_lines(e->t);
	e->cy = (size_t)ln - 1;
	e->cx = 0;
	e->sel_active = 0;
	clamp_col(e);
	snprintf(e->status, sizeof(e->status), "line %ld", ln);
}

/* The key bindings, as shown by the help screen. Kept next to the keymap so
 * the two stay in step. */
static const struct {
	const char	*keys;
	const char	*desc;
} help_entries[] = {
	{ "arrows",		"Move the cursor" },
	{ "Home / End",		"Start / end of line" },
	{ "PgUp / PgDn",	"Scroll by a screen" },
	{ "Shift+arrows",	"Extend a selection" },
	{ "Enter",		"Split the line" },
	{ "Backspace / Del",	"Delete before / after the cursor" },
	{ "Ctrl-F",		"Find (Enter repeats the last search)" },
	{ "Ctrl-L",		"Go to a line number" },
	{ "Ctrl-C / Ctrl-X",	"Copy (line if none selected) / cut" },
	{ "Ctrl-V",		"Paste the clipboard" },
	{ "Ctrl-G",		"Send selection/line to another pane" },
	{ "Ctrl-Z / Ctrl-Y",	"Undo / redo" },
	{ "Ctrl-S",		"Save (asks for a name if none)" },
	{ "Ctrl-Q",		"Quit (asks if there are unsaved changes)" },
	{ "F1",			"Show this help" },
};

#define HELP_COUNT ((int)(sizeof(help_entries) / sizeof(help_entries[0])))

/* Draw a full-screen list of the key bindings, joe/nano style. */
static void
render_help(struct editor *e, struct tui_out *o)
{
	const struct tui_theme *t = tui_theme_default();
	int rows = e->rows > 0 ? e->rows : 24;
	int row;
	char line[128];

	tui_out_reset(o);
	tui_out_puts(o, "\033[H");

	tui_out_move(o, 1, 1);
	tui_out_sgr(o, &t->title_fg, &t->border_bg, 1);
	tui_out_field(o, " lumi edit -- key bindings", e->cols);
	tui_out_sgr_reset(o);

	for (row = 2; row < rows; row++) {
		int idx = row - 2;

		tui_out_move(o, row, 1);
		tui_out_sgr(o, &t->content_fg, &t->content_bg, 0);
		if (idx < HELP_COUNT)
			snprintf(line, sizeof(line), "  %-17s %s",
			    help_entries[idx].keys, help_entries[idx].desc);
		else
			line[0] = '\0';
		tui_out_field(o, line, e->cols);
		tui_out_sgr_reset(o);
	}

	tui_out_move(o, rows, 1);
	tui_out_sgr(o, &t->title_fg, &t->border_bg, 1);
	tui_out_field(o, " Press any key to return", e->cols);
	tui_out_sgr_reset(o);

	(void)tui_out_flush(o, STDOUT_FILENO);
}

/* Show the help screen and wait for one key press to dismiss it. */
static void
show_help(struct editor *e, struct tui_out *o, struct tkbd_stream *s)
{
	render_help(e, o);
	for (;;) {
		struct tkbd_seq seq;
		int n;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (n > 0 && seq.type == TKBD_KEY)
			return;			/* any key returns to editing */
	}
}

/****************************************************************
 * Terminal / lifecycle
 ****************************************************************/

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [file]\n"
	    "\n"
	    "Edit a text file in a full-screen terminal UI.\n"
	    "\n"
	    "Keys:\n"
	    "  arrows        move the cursor\n"
	    "  Home / End    start / end of line\n"
	    "  PgUp / PgDn   scroll by a screen\n"
	    "  Enter         split the line\n"
	    "  Backspace     delete left; Delete removes right\n"
	    "  Shift-arrows  extend a selection\n"
	    "  Ctrl-C / Ctrl-X  copy / cut (Ctrl-C with no selection copies the line)\n"
	    "  Ctrl-V        paste the internal clipboard\n"
	    "  Ctrl-G        send selection/line to another pane (in a session)\n"
	    "  Ctrl-F        find (Enter repeats the last search)\n"
	    "  Ctrl-L        go to a line number\n"
	    "  Ctrl-Z / Ctrl-Y  undo / redo\n"
	    "  Ctrl-S        save (prompts for a name if the buffer has none)\n"
	    "  Ctrl-Q        quit (prompts if the buffer was modified)\n"
	    "  F1            show the key bindings\n"
	    "\n"
	    "Pasted text from the terminal is inserted literally (bracketed"
	    " paste).\n",
	    progname);
}

static void
on_sigwinch(int sig)
{
	(void)sig;
	resized = 1;
}

static int
get_term_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0)
		return -1;
	*rows = ws.ws_row > 0 ? ws.ws_row : 24;
	*cols = ws.ws_col > 0 ? ws.ws_col : 80;
	return 0;
}

int
cmd_edit_main(int argc, char **argv)
{
	struct editor e;
	struct tui_out o = { 0 };
	struct tkbd_stream stream;
	struct sigaction sa;
	int rc = 0;

	if (argv[0])
		progname = argv[0];

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0)) {
		usage();
		return 0;
	}

	rune_width_init();

	memset(&e, 0, sizeof(e));
	e.in_session = getenv("LUMI_SESSION") != NULL;
	e.t = text_new();
	if (!e.t) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}

	if (argc > 1) {
		snprintf(e.path, sizeof(e.path), "%s", argv[1]);
		e.has_name = 1;
		if (text_load(e.t, e.path) < 0 && errno != ENOENT) {
			fprintf(stderr, "%s: %s: %s\n", progname, e.path,
			    strerror(errno));
			text_free(e.t);
			return 1;
		}
		/* a missing file opens as an empty new buffer */
	}

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "%s: not a terminal\n", progname);
		text_free(e.t);
		return 1;
	}

	if (get_term_size(&e.rows, &e.cols) < 0) {
		e.rows = 24;
		e.cols = 80;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigwinch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, NULL);

	if (tkbd_attach(&stream, STDIN_FILENO) < 0) {
		fprintf(stderr, "%s: cannot enter raw mode: %s\n",
		    progname, strerror(errno));
		text_free(e.t);
		return 1;
	}

	/* alt screen, show cursor, enable bracketed paste (DECSET 2004) so
	 * pasted text arrives wrapped in PASTE_BEGIN/END and never triggers
	 * editor commands. */
	tui_out_puts(&o, "\033[?1049h\033[?25h\033[?2004h");
	(void)tui_out_flush(&o, STDOUT_FILENO);
	snprintf(e.status, sizeof(e.status), "Press F1 for help");
	render(&e, &o);

	for (;;) {
		struct tkbd_seq seq;
		int n;

		if (resized) {
			resized = 0;
			if (get_term_size(&e.rows, &e.cols) == 0)
				render(&e, &o);
		}

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(&stream, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = 1;
			break;
		}
		if (n == 0)
			continue;

		e.status[0] = '\0';	/* clear any transient message */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_BEGIN) {
			paste_input(&e, &stream);
			render(&e, &o);
			continue;
		}
		switch (dispatch(&e, key_to_cmd(&seq), &seq)) {
		case REQ_FIND:
			find_prompt(&e, &o, &stream);
			break;
		case REQ_GOTO:
			goto_prompt(&e, &o, &stream);
			break;
		case REQ_HELP:
			show_help(&e, &o, &stream);
			break;
		case REQ_SAVE:
			(void)save_editor(&e, &o, &stream);
			break;
		case REQ_QUIT:
			if (!text_dirty(e.t))
				goto done;
			{
				int yn = confirm_yn(&e, &o, &stream,
				    "Save changes? (y/n, Esc cancels) ");

				if (yn < 0)
					break;		/* cancelled: stay */
				if (yn == 0)
					goto done;	/* discard */
				if (save_editor(&e, &o, &stream) == 0)
					goto done;	/* saved: quit */
			}
			break;
		case REQ_CONTINUE:
			break;
		}
		render(&e, &o);
	}
done:

	tui_out_reset(&o);
	tui_out_puts(&o, "\033[?2004l\033[?1049l");
	(void)tui_out_flush(&o, STDOUT_FILENO);

	tkbd_detach(&stream);
	tui_out_free(&o);
	text_free(e.t);
	free(e.clip);
	return rc;
}
