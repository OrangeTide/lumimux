/* vt_ops.c : connect VT parser to terminal state */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_ops.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "vt_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* ---- helpers ---- */

static int
param_or(const int *params, int nparam, int idx, int def)
{
	if (idx < nparam && params[idx] >= 0)
		return params[idx];
	return def;
}

/* write a reply string back to the PTY master (for DSR/DA responses) */
static void
vt_reply(struct vt_state *st, const char *data, size_t len)
{
	if (st->reply_fd >= 0)
		write(st->reply_fd, data, len);
}

/* line drawing character map (DEC special graphics, 0x5F-0x7E) */
static const uint32_t dec_line_drawing[] = {
	/* 0x5F */ 0x00A0,	/* non-breaking space */
	/* 0x60 */ 0x25C6,	/* diamond */
	/* 0x61 */ 0x2592,	/* checkerboard */
	/* 0x62 */ 0x2409,	/* HT symbol */
	/* 0x63 */ 0x240C,	/* FF symbol */
	/* 0x64 */ 0x240D,	/* CR symbol */
	/* 0x65 */ 0x240A,	/* LF symbol */
	/* 0x66 */ 0x00B0,	/* degree */
	/* 0x67 */ 0x00B1,	/* plus/minus */
	/* 0x68 */ 0x2424,	/* NL symbol */
	/* 0x69 */ 0x240B,	/* VT symbol */
	/* 0x6A */ 0x2518,	/* lower right corner */
	/* 0x6B */ 0x2510,	/* upper right corner */
	/* 0x6C */ 0x250C,	/* upper left corner */
	/* 0x6D */ 0x2514,	/* lower left corner */
	/* 0x6E */ 0x253C,	/* crossing */
	/* 0x6F */ 0x23BA,	/* scan line 1 */
	/* 0x70 */ 0x23BB,	/* scan line 3 */
	/* 0x71 */ 0x2500,	/* horizontal line */
	/* 0x72 */ 0x23BC,	/* scan line 7 */
	/* 0x73 */ 0x23BD,	/* scan line 9 */
	/* 0x74 */ 0x251C,	/* T right */
	/* 0x75 */ 0x2524,	/* T left */
	/* 0x76 */ 0x2534,	/* T up */
	/* 0x77 */ 0x252C,	/* T down */
	/* 0x78 */ 0x2502,	/* vertical line */
	/* 0x79 */ 0x2264,	/* less-equal */
	/* 0x7A */ 0x2265,	/* greater-equal */
	/* 0x7B */ 0x03C0,	/* pi */
	/* 0x7C */ 0x2260,	/* not-equal */
	/* 0x7D */ 0x00A3,	/* pound sterling */
	/* 0x7E */ 0x00B7,	/* middle dot */
};

/* ---- print ---- */

static void
op_print(void *ctx, uint32_t cp, int width)
{
	struct vt_state *st = ctx;
	int active_set;

	/* apply character set translation */
	active_set = st->charset == 0 ? st->g0_set : st->g1_set;
	if (active_set == 1 && cp >= 0x5F && cp <= 0x7E)
		cp = dec_line_drawing[cp - 0x5F];

	vt_state_putchar(st, cp, width);
}

/* ---- C0/C1 execute ---- */

static void
op_execute(void *ctx, uint8_t c)
{
	struct vt_state *st = ctx;
	int rows = vt_buf_rows(st->buf);

	switch (c) {
	case 0x07:	/* BEL -- ignore for now */
		break;

	case 0x08:	/* BS -- backspace */
		if (st->cursor_col > 0)
			st->cursor_col--;
		break;

	case 0x09:	/* HT -- horizontal tab */
		st->cursor_col = vt_state_tab_next(st, st->cursor_col);
		break;

	case 0x0A:	/* LF */
	case 0x0B:	/* VT */
	case 0x0C:	/* FF */
		st->cursor_row++;
		if (st->cursor_row >= st->scroll_bot) {
			st->cursor_row = st->scroll_bot - 1;
			vt_buf_scroll(st->buf, st->scroll_top,
			    st->scroll_bot, 1);
		}
		break;

	case 0x0D:	/* CR -- carriage return */
		st->cursor_col = 0;
		break;

	case 0x0E:	/* SO -- shift out (G1) */
		st->charset = 1;
		break;

	case 0x0F:	/* SI -- shift in (G0) */
		st->charset = 0;
		break;
	}
	(void)rows;
}

/* ---- CSI dispatch ---- */

static void
csi_sgr(struct vt_state *st, const int *params, int nparam)
{
	int i, p;

	if (nparam == 0) {
		/* CSI m -- reset all */
		st->attrs = 0;
		st->fg.type = VT_COLOR_DEFAULT;
		st->bg.type = VT_COLOR_DEFAULT;
		return;
	}

	for (i = 0; i < nparam; i++) {
		p = params[i] < 0 ? 0 : params[i];

		switch (p) {
		case 0:
			st->attrs = 0;
			st->fg.type = VT_COLOR_DEFAULT;
			st->bg.type = VT_COLOR_DEFAULT;
			break;
		case 1:
			st->attrs |= VT_ATTR_BOLD;
			break;
		case 2:
			st->attrs |= VT_ATTR_DIM;
			break;
		case 3:
			st->attrs |= VT_ATTR_ITALIC;
			break;
		case 4:
			st->attrs |= VT_ATTR_UNDERLINE;
			break;
		case 5:
			st->attrs |= VT_ATTR_BLINK;
			break;
		case 7:
			st->attrs |= VT_ATTR_REVERSE;
			break;
		case 8:
			st->attrs |= VT_ATTR_HIDDEN;
			break;
		case 9:
			st->attrs |= VT_ATTR_STRIKE;
			break;
		case 21:
			st->attrs &= (uint16_t)~VT_ATTR_BOLD;
			break;
		case 22:
			st->attrs &= (uint16_t)~(VT_ATTR_BOLD | VT_ATTR_DIM);
			break;
		case 23:
			st->attrs &= (uint16_t)~VT_ATTR_ITALIC;
			break;
		case 24:
			st->attrs &= (uint16_t)~VT_ATTR_UNDERLINE;
			break;
		case 25:
			st->attrs &= (uint16_t)~VT_ATTR_BLINK;
			break;
		case 27:
			st->attrs &= (uint16_t)~VT_ATTR_REVERSE;
			break;
		case 28:
			st->attrs &= (uint16_t)~VT_ATTR_HIDDEN;
			break;
		case 29:
			st->attrs &= (uint16_t)~VT_ATTR_STRIKE;
			break;

		/* foreground: standard 8 */
		case 30: case 31: case 32: case 33:
		case 34: case 35: case 36: case 37:
			st->fg.type = VT_COLOR_INDEXED;
			st->fg.index = (uint8_t)(p - 30);
			break;

		/* foreground: extended */
		case 38:
			if (i + 1 < nparam && params[i + 1] == 5) {
				/* 256-color: CSI 38;5;N m */
				if (i + 2 < nparam) {
					st->fg.type = VT_COLOR_INDEXED;
					st->fg.index = (uint8_t)params[i + 2];
					i += 2;
				}
			} else if (i + 1 < nparam && params[i + 1] == 2) {
				/* RGB: CSI 38;2;R;G;B m */
				if (i + 4 < nparam) {
					st->fg.type = VT_COLOR_RGB;
					st->fg.rgb.r = (uint8_t)params[i + 2];
					st->fg.rgb.g = (uint8_t)params[i + 3];
					st->fg.rgb.b = (uint8_t)params[i + 4];
					i += 4;
				}
			}
			break;

		case 39:	/* default fg */
			st->fg.type = VT_COLOR_DEFAULT;
			break;

		/* background: standard 8 */
		case 40: case 41: case 42: case 43:
		case 44: case 45: case 46: case 47:
			st->bg.type = VT_COLOR_INDEXED;
			st->bg.index = (uint8_t)(p - 40);
			break;

		/* background: extended */
		case 48:
			if (i + 1 < nparam && params[i + 1] == 5) {
				if (i + 2 < nparam) {
					st->bg.type = VT_COLOR_INDEXED;
					st->bg.index = (uint8_t)params[i + 2];
					i += 2;
				}
			} else if (i + 1 < nparam && params[i + 1] == 2) {
				if (i + 4 < nparam) {
					st->bg.type = VT_COLOR_RGB;
					st->bg.rgb.r = (uint8_t)params[i + 2];
					st->bg.rgb.g = (uint8_t)params[i + 3];
					st->bg.rgb.b = (uint8_t)params[i + 4];
					i += 4;
				}
			}
			break;

		case 49:	/* default bg */
			st->bg.type = VT_COLOR_DEFAULT;
			break;

		/* foreground: bright 8 */
		case 90: case 91: case 92: case 93:
		case 94: case 95: case 96: case 97:
			st->fg.type = VT_COLOR_INDEXED;
			st->fg.index = (uint8_t)(p - 90 + 8);
			break;

		/* background: bright 8 */
		case 100: case 101: case 102: case 103:
		case 104: case 105: case 106: case 107:
			st->bg.type = VT_COLOR_INDEXED;
			st->bg.index = (uint8_t)(p - 100 + 8);
			break;
		}
	}
}

static void
csi_erase_display(struct vt_state *st, int mode)
{
	int rows = vt_buf_rows(st->buf);
	int cols = vt_buf_cols(st->buf);

	switch (mode) {
	case 0:		/* below */
		/* clear rest of current row */
		{
			struct vt_row *r;
			int i;

			r = vt_buf_row(st->buf, st->cursor_row);
			if (r) {
				for (i = st->cursor_col; i < cols; i++)
					vt_cell_clear(&r->cells[i]);
				r->flags |= VT_ROW_DIRTY;
			}
		}
		vt_buf_clear_rows(st->buf, st->cursor_row + 1, rows);
		break;
	case 1:		/* above */
		vt_buf_clear_rows(st->buf, 0, st->cursor_row);
		/* clear beginning of current row */
		{
			struct vt_row *r;
			int i;

			r = vt_buf_row(st->buf, st->cursor_row);
			if (r) {
				for (i = 0; i <= st->cursor_col && i < cols;
				    i++)
					vt_cell_clear(&r->cells[i]);
				r->flags |= VT_ROW_DIRTY;
			}
		}
		break;
	case 2:		/* entire display */
	case 3:		/* entire display + scrollback (xterm) */
		vt_buf_clear_rows(st->buf, 0, rows);
		break;
	}
}

static void
csi_erase_line(struct vt_state *st, int mode)
{
	struct vt_row *r;
	int cols = vt_buf_cols(st->buf);
	int i, start, end;

	r = vt_buf_row(st->buf, st->cursor_row);
	if (!r)
		return;

	switch (mode) {
	case 0:
		start = st->cursor_col;
		end = cols;
		break;
	case 1:
		start = 0;
		end = st->cursor_col + 1;
		if (end > cols)
			end = cols;
		break;
	case 2:
		start = 0;
		end = cols;
		break;
	default:
		return;
	}

	for (i = start; i < end; i++)
		vt_cell_clear(&r->cells[i]);
	r->flags |= VT_ROW_DIRTY;
}

static void
csi_insert_lines(struct vt_state *st, int count)
{
	if (st->cursor_row < st->scroll_top ||
	    st->cursor_row >= st->scroll_bot)
		return;
	vt_buf_scroll(st->buf, st->cursor_row, st->scroll_bot, -count);
}

static void
csi_delete_lines(struct vt_state *st, int count)
{
	if (st->cursor_row < st->scroll_top ||
	    st->cursor_row >= st->scroll_bot)
		return;
	vt_buf_scroll(st->buf, st->cursor_row, st->scroll_bot, count);
}

static void
csi_insert_chars(struct vt_state *st, int count)
{
	struct vt_row *r;
	int cols = vt_buf_cols(st->buf);
	int i;

	r = vt_buf_row(st->buf, st->cursor_row);
	if (!r)
		return;

	if (count > cols - st->cursor_col)
		count = cols - st->cursor_col;

	/* shift cells right */
	for (i = cols - 1; i >= st->cursor_col + count; i--)
		r->cells[i] = r->cells[i - count];

	/* clear inserted area */
	for (i = st->cursor_col; i < st->cursor_col + count; i++)
		vt_cell_clear(&r->cells[i]);

	r->flags |= VT_ROW_DIRTY;
}

static void
csi_delete_chars(struct vt_state *st, int count)
{
	struct vt_row *r;
	int cols = vt_buf_cols(st->buf);
	int i;

	r = vt_buf_row(st->buf, st->cursor_row);
	if (!r)
		return;

	if (count > cols - st->cursor_col)
		count = cols - st->cursor_col;

	/* shift cells left */
	for (i = st->cursor_col; i < cols - count; i++)
		r->cells[i] = r->cells[i + count];

	/* clear vacated area */
	for (i = cols - count; i < cols; i++)
		vt_cell_clear(&r->cells[i]);

	r->flags |= VT_ROW_DIRTY;
}

static void
csi_erase_chars(struct vt_state *st, int count)
{
	struct vt_row *r;
	int cols = vt_buf_cols(st->buf);
	int i;

	r = vt_buf_row(st->buf, st->cursor_row);
	if (!r)
		return;

	if (count > cols - st->cursor_col)
		count = cols - st->cursor_col;

	for (i = st->cursor_col; i < st->cursor_col + count; i++)
		vt_cell_clear(&r->cells[i]);

	r->flags |= VT_ROW_DIRTY;
}

static void
op_csi(void *ctx, const int *params, int nparam, int intermed, int final)
{
	struct vt_state *st = ctx;
	int rows = vt_buf_rows(st->buf);
	int cols = vt_buf_cols(st->buf);
	int n, m;

	/* private mode sequences */
	if (intermed == '?') {
		n = param_or(params, nparam, 0, 0);
		switch (final) {
		case 'h':	/* DECSET */
			switch (n) {
			case 1:		/* DECCKM (application cursor keys) */
				st->modes |= VT_MODE_DECCKM;
				break;
			case 6:		/* DECOM (origin mode) */
				st->modes |= VT_MODE_ORIGIN;
				st->cursor_row = st->scroll_top;
				st->cursor_col = 0;
				break;
			case 7:
				st->modes |= VT_MODE_AUTOWRAP;
				break;
			case 25:
				st->modes |= VT_MODE_CURSOR_VIS;
				break;
			case 47:	/* alt screen (old xterm) */
			case 1047:	/* alt screen (xterm) */
			case 1049:	/* alt screen + save cursor */
				vt_state_altscreen_enter(st);
				break;
			case 2004:
				st->modes |= VT_MODE_BRACKETPASTE;
				break;
			}
			break;
		case 'l':	/* DECRST */
			switch (n) {
			case 1:		/* DECCKM off */
				st->modes &= ~VT_MODE_DECCKM;
				break;
			case 6:		/* DECOM off */
				st->modes &= ~VT_MODE_ORIGIN;
				st->cursor_row = 0;
				st->cursor_col = 0;
				break;
			case 7:
				st->modes &= ~VT_MODE_AUTOWRAP;
				break;
			case 25:
				st->modes &= ~VT_MODE_CURSOR_VIS;
				break;
			case 47:	/* alt screen (old xterm) */
			case 1047:	/* alt screen (xterm) */
			case 1049:	/* alt screen + restore cursor */
				vt_state_altscreen_leave(st);
				break;
			case 2004:
				st->modes &= ~VT_MODE_BRACKETPASTE;
				break;
			}
			break;
		}
		return;
	}

	/* CSI Ps SP q -- DECSCUSR (set cursor shape) */
	if (intermed == ' ' && final == 'q') {
		n = param_or(params, nparam, 0, 0);
		if (n >= 0 && n <= 6)
			st->cursor_shape = n;
		return;
	}

	/* keyboard enhancement protocol sequences */
	if (intermed == '>' && final == 'u') {
		/* CSI > flags u -- kitty keyboard protocol push */
		st->kitty_kbd_flags = param_or(params, nparam, 0, 0);
		return;
	}
	if (intermed == '<' && final == 'u') {
		/* CSI < u -- kitty keyboard protocol pop */
		st->kitty_kbd_flags = 0;
		return;
	}
	if (intermed == '>' && final == 'm') {
		n = param_or(params, nparam, 0, 0);
		if (n == 4) {
			/* CSI > 4 ; Pm m -- xterm modifyOtherKeys */
			st->modify_other_keys = param_or(params, nparam, 1, 0);
		}
		return;
	}

	/* ignore sequences with any private modifier or intermediate byte
	 * that we don't handle (CSI = ..., CSI ! p, etc.) */
	if (intermed != 0)
		return;

	switch (final) {
	case 'A':	/* CUU -- cursor up */
		n = param_or(params, nparam, 0, 1);
		st->cursor_row -= n;
		if (st->cursor_row < 0)
			st->cursor_row = 0;
		break;

	case 'B':	/* CUD -- cursor down */
		n = param_or(params, nparam, 0, 1);
		st->cursor_row += n;
		if (st->cursor_row >= rows)
			st->cursor_row = rows - 1;
		break;

	case 'C':	/* CUF -- cursor forward */
		n = param_or(params, nparam, 0, 1);
		st->cursor_col += n;
		if (st->cursor_col >= cols)
			st->cursor_col = cols - 1;
		break;

	case 'D':	/* CUB -- cursor back */
		n = param_or(params, nparam, 0, 1);
		st->cursor_col -= n;
		if (st->cursor_col < 0)
			st->cursor_col = 0;
		break;

	case 'E':	/* CNL -- cursor next line */
		n = param_or(params, nparam, 0, 1);
		st->cursor_row += n;
		if (st->cursor_row >= rows)
			st->cursor_row = rows - 1;
		st->cursor_col = 0;
		break;

	case 'F':	/* CPL -- cursor previous line */
		n = param_or(params, nparam, 0, 1);
		st->cursor_row -= n;
		if (st->cursor_row < 0)
			st->cursor_row = 0;
		st->cursor_col = 0;
		break;

	case 'G':	/* CHA -- cursor horizontal absolute */
		n = param_or(params, nparam, 0, 1);
		st->cursor_col = n - 1;
		vt_state_cursor_clamp(st);
		break;

	case 'H':	/* CUP -- cursor position */
	case 'f':	/* HVP -- horizontal/vertical position */
		n = param_or(params, nparam, 0, 1);
		m = param_or(params, nparam, 1, 1);
		if (st->modes & VT_MODE_ORIGIN) {
			st->cursor_row = st->scroll_top + n - 1;
			if (st->cursor_row >= st->scroll_bot)
				st->cursor_row = st->scroll_bot - 1;
		} else {
			st->cursor_row = n - 1;
		}
		st->cursor_col = m - 1;
		vt_state_cursor_clamp(st);
		break;

	case 'J':	/* ED -- erase display */
		n = param_or(params, nparam, 0, 0);
		csi_erase_display(st, n);
		break;

	case 'K':	/* EL -- erase line */
		n = param_or(params, nparam, 0, 0);
		csi_erase_line(st, n);
		break;

	case 'L':	/* IL -- insert lines */
		n = param_or(params, nparam, 0, 1);
		csi_insert_lines(st, n);
		break;

	case 'M':	/* DL -- delete lines */
		n = param_or(params, nparam, 0, 1);
		csi_delete_lines(st, n);
		break;

	case 'P':	/* DCH -- delete characters */
		n = param_or(params, nparam, 0, 1);
		csi_delete_chars(st, n);
		break;

	case 'S':	/* SU -- scroll up */
		n = param_or(params, nparam, 0, 1);
		vt_buf_scroll(st->buf, st->scroll_top, st->scroll_bot, n);
		break;

	case 'T':	/* SD -- scroll down */
		n = param_or(params, nparam, 0, 1);
		vt_buf_scroll(st->buf, st->scroll_top, st->scroll_bot, -n);
		break;

	case 'X':	/* ECH -- erase characters */
		n = param_or(params, nparam, 0, 1);
		csi_erase_chars(st, n);
		break;

	case '@':	/* ICH -- insert characters */
		n = param_or(params, nparam, 0, 1);
		csi_insert_chars(st, n);
		break;

	case 'c':	/* DA -- device attributes */
		n = param_or(params, nparam, 0, 0);
		if (n == 0) {
			/* DA1: report VT100 with AVO */
			char da[] = "\033[?1;2c";
			vt_reply(st, da, sizeof(da) - 1);
		}
		break;

	case 'n':	/* DSR -- device status report */
		n = param_or(params, nparam, 0, 0);
		if (n == 6) {
			/* CPR: report cursor position (1-based) */
			char cpr[32];
			int len;

			len = snprintf(cpr, sizeof(cpr), "\033[%d;%dR",
			    st->cursor_row + 1, st->cursor_col + 1);
			vt_reply(st, cpr, (size_t)len);
		} else if (n == 5) {
			/* status report: terminal OK */
			vt_reply(st, "\033[0n", 4);
		}
		break;

	case 'd':	/* VPA -- line position absolute */
		n = param_or(params, nparam, 0, 1);
		if (st->modes & VT_MODE_ORIGIN) {
			st->cursor_row = st->scroll_top + n - 1;
			if (st->cursor_row >= st->scroll_bot)
				st->cursor_row = st->scroll_bot - 1;
		} else {
			st->cursor_row = n - 1;
		}
		vt_state_cursor_clamp(st);
		break;

	case 'h':	/* SM -- set mode */
		n = param_or(params, nparam, 0, 0);
		if (n == 4)
			st->modes |= VT_MODE_INSERT;
		break;

	case 'l':	/* RM -- reset mode */
		n = param_or(params, nparam, 0, 0);
		if (n == 4)
			st->modes &= ~VT_MODE_INSERT;
		break;

	case 'm':	/* SGR */
		csi_sgr(st, params, nparam);
		break;

	case 'r':	/* DECSTBM -- set scrolling region */
		n = param_or(params, nparam, 0, 1);
		m = param_or(params, nparam, 1, rows);
		if (n < 1)
			n = 1;
		if (m > rows)
			m = rows;
		if (n < m) {
			st->scroll_top = n - 1;
			st->scroll_bot = m;
		}
		st->cursor_row = 0;
		st->cursor_col = 0;
		break;

	case 's':	/* SCOSC -- save cursor */
		vt_state_cursor_save(st);
		break;

	case 'u':	/* SCORC -- restore cursor */
		vt_state_cursor_restore(st);
		break;

	case 'g':	/* TBC -- tab clear */
		n = param_or(params, nparam, 0, 0);
		if (n == 0)
			vt_state_tab_clear(st, st->cursor_col);
		else if (n == 3)
			vt_state_tab_reset(st);
		break;

	case 'Z':	/* CBT -- cursor back tab */
		n = param_or(params, nparam, 0, 1);
		for (; n > 0; n--)
			st->cursor_col = vt_state_tab_prev(st,
			    st->cursor_col);
		break;
	}
}

/* ---- ESC dispatch ---- */

static void
op_esc(void *ctx, int intermed, int final)
{
	struct vt_state *st = ctx;

	if (intermed == 0) {
		switch (final) {
		case '7':	/* DECSC -- save cursor */
			vt_state_cursor_save(st);
			break;
		case '8':	/* DECRC -- restore cursor */
			vt_state_cursor_restore(st);
			break;
		case 'D':	/* IND -- index (scroll up) */
			st->cursor_row++;
			if (st->cursor_row >= st->scroll_bot) {
				st->cursor_row = st->scroll_bot - 1;
				vt_buf_scroll(st->buf, st->scroll_top,
				    st->scroll_bot, 1);
			}
			break;
		case 'E':	/* NEL -- next line */
			st->cursor_col = 0;
			st->cursor_row++;
			if (st->cursor_row >= st->scroll_bot) {
				st->cursor_row = st->scroll_bot - 1;
				vt_buf_scroll(st->buf, st->scroll_top,
				    st->scroll_bot, 1);
			}
			break;
		case 'H':	/* HTS -- horizontal tab set */
			vt_state_tab_set(st, st->cursor_col);
			break;
		case 'M':	/* RI -- reverse index */
			st->cursor_row--;
			if (st->cursor_row < st->scroll_top) {
				st->cursor_row = st->scroll_top;
				vt_buf_scroll(st->buf, st->scroll_top,
				    st->scroll_bot, -1);
			}
			break;
		case '=':	/* DECKPAM -- application keypad mode */
			st->modes |= VT_MODE_DECKPAM;
			break;
		case '>':	/* DECKPNM -- normal keypad mode */
			st->modes &= ~VT_MODE_DECKPAM;
			break;
		case 'c':	/* RIS -- full reset */
			if (st->modes & VT_MODE_ALTSCREEN)
				vt_state_altscreen_leave(st);
			st->modes = VT_MODE_AUTOWRAP | VT_MODE_CURSOR_VIS;
			st->attrs = 0;
			st->fg.type = VT_COLOR_DEFAULT;
			st->bg.type = VT_COLOR_DEFAULT;
			st->charset = 0;
			st->g0_set = 0;
			st->g1_set = 0;
			st->scroll_top = 0;
			st->scroll_bot = vt_buf_rows(st->buf);
			st->cursor_row = 0;
			st->cursor_col = 0;
			st->kitty_kbd_flags = 0;
			st->modify_other_keys = 0;
			vt_state_tab_reset(st);
			csi_erase_display(st, 2);
			break;
		}
	} else if (intermed == '(') {
		/* G0 charset designation */
		switch (final) {
		case 'B':	/* ASCII */
			st->g0_set = 0;
			break;
		case '0':	/* DEC line drawing */
			st->g0_set = 1;
			break;
		}
	} else if (intermed == ')') {
		/* G1 charset designation */
		switch (final) {
		case 'B':
			st->g1_set = 0;
			break;
		case '0':
			st->g1_set = 1;
			break;
		}
	}
}

/* ---- OSC dispatch ---- */

static void
op_osc(void *ctx, const char *data, size_t len)
{
	struct vt_state *st = ctx;
	int num;
	const char *semi;

	if (len == 0)
		return;

	/* parse "N;text" format */
	semi = memchr(data, ';', len);
	if (!semi)
		return;

	num = 0;
	for (const char *p = data; p < semi; p++) {
		if (*p < '0' || *p > '9')
			return;
		num = num * 10 + (*p - '0');
	}

	semi++;	/* skip ';' */
	len -= (size_t)(semi - data);

	switch (num) {
	case 0:		/* set icon name + title */
	case 2:		/* set title */
		free(st->title);
		st->title = malloc(len + 1);
		if (st->title) {
			memcpy(st->title, semi, len);
			st->title[len] = '\0';
		}
		break;
	}
}

/* ---- vtable ---- */

static const struct vt_ops default_ops = {
	.print = op_print,
	.execute = op_execute,
	.csi = op_csi,
	.esc = op_esc,
	.osc = op_osc,
};

const struct vt_ops *
vt_ops_default(void)
{
	return &default_ops;
}
