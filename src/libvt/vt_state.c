/* vt_state.c : terminal emulation state */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_state.h"
#include "utf8.h"
#include "xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SCROLLBACK 2000
#define DEFAULT_TAB_WIDTH  8

static void
tabstops_init(struct vt_state *st)
{
	int i;
	int cols = vt_buf_cols(st->buf);

	st->tabstops = xcalloc(1, (size_t)cols);
	for (i = 0; i < cols; i += DEFAULT_TAB_WIDTH)
		st->tabstops[i] = 1;
}

struct vt_state *
vt_state_new(int rows, int cols, int scrollback)
{
	struct vt_state *st;

	if (scrollback < 0)
		scrollback = DEFAULT_SCROLLBACK;

	st = xcalloc(1, sizeof(*st));
	st->targets[VT_TARGET_PRIMARY] = vt_buf_new(rows, cols, scrollback);
	st->active_target = VT_TARGET_PRIMARY;
	st->buf = st->targets[VT_TARGET_PRIMARY];

	st->scroll_top = 0;
	st->scroll_bot = rows;

	st->modes = VT_MODE_AUTOWRAP | VT_MODE_CURSOR_VIS;
	st->reply_fd = -1;

	tabstops_init(st);

	return st;
}

void
vt_state_free(struct vt_state *st)
{
	if (!st)
		return;

	{
		int i;

		for (i = 0; i < VT_TARGET_COUNT; i++)
			vt_buf_free(st->targets[i]);
	}
	free(st->tabstops);
	free(st->title);
	{
		int i;

		for (i = 0; i < st->title_stack_len; i++)
			free(st->title_stack[i]);
		free(st->title_stack);
	}
	free(st);
}

void
vt_state_set_reply_fd(struct vt_state *st, int fd)
{
	st->reply_fd = fd;
}

/* push a new entry onto the kitty keyboard flag stack; when the stack is
 * full the oldest entry is dropped, matching the kitty protocol */
void
vt_state_kitty_push(struct vt_state *st, int flags)
{
	if (st->kitty_kbd_depth == VT_KITTY_KBD_STACK_MAX) {
		memmove(&st->kitty_kbd_stack[0], &st->kitty_kbd_stack[1],
		    (VT_KITTY_KBD_STACK_MAX - 1) * sizeof(int));
		st->kitty_kbd_depth--;
	}
	st->kitty_kbd_stack[st->kitty_kbd_depth++] = flags;
	st->kitty_kbd_flags = flags;
}

/* pop count entries (default 1); effective flags become the new top, or 0 */
void
vt_state_kitty_pop(struct vt_state *st, int count)
{
	if (count < 1)
		count = 1;
	if (count > st->kitty_kbd_depth)
		count = st->kitty_kbd_depth;
	st->kitty_kbd_depth -= count;
	st->kitty_kbd_flags = st->kitty_kbd_depth
	    ? st->kitty_kbd_stack[st->kitty_kbd_depth - 1] : 0;
}

/* set the top entry's flags in place (CSI = flags ; mode u).
 * mode 1 = replace (default), 2 = set bits, 3 = clear bits. */
void
vt_state_kitty_set(struct vt_state *st, int flags, int mode)
{
	int cur = st->kitty_kbd_flags;

	switch (mode) {
	case 3:	cur &= ~flags; break;
	case 2:	cur |= flags; break;
	default: cur = flags; break;
	}
	st->kitty_kbd_flags = cur;
	if (st->kitty_kbd_depth)
		st->kitty_kbd_stack[st->kitty_kbd_depth - 1] = cur;
}

const char *
vt_state_title(const struct vt_state *st)
{
	return st->title;
}

#define TITLE_STACK_MAX 16

void
vt_state_title_push(struct vt_state *st)
{
	char *copy;

	if (st->title_stack_len >= TITLE_STACK_MAX)
		return;
	if (st->title_stack_len >= st->title_stack_cap) {
		int newcap = st->title_stack_cap ? st->title_stack_cap * 2 : 4;
		char **p;

		if (newcap > TITLE_STACK_MAX)
			newcap = TITLE_STACK_MAX;
		p = realloc(st->title_stack,
		    (size_t)newcap * sizeof(*p));
		if (!p)
			return;
		st->title_stack = p;
		st->title_stack_cap = newcap;
	}
	copy = st->title ? strdup(st->title) : NULL;
	st->title_stack[st->title_stack_len++] = copy;
}

void
vt_state_title_pop(struct vt_state *st)
{
	char *saved;

	if (st->title_stack_len <= 0)
		return;
	saved = st->title_stack[--st->title_stack_len];
	free(st->title);
	st->title = saved;
}

int
vt_state_resize(struct vt_state *st, int rows, int cols)
{
	int old_cols = vt_buf_cols(st->buf);
	int rc, i;

	for (i = 0; i < VT_TARGET_COUNT; i++) {
		if (!st->targets[i])
			continue;
		rc = vt_buf_resize(st->targets[i], rows, cols);
		if (rc < 0)
			return rc;
	}

	/* rebuild tab stops if column count changed */
	if (cols != old_cols) {
		free(st->tabstops);
		st->tabstops = NULL;
		tabstops_init(st);
	}

	st->scroll_top = 0;
	st->scroll_bot = rows;

	vt_state_cursor_clamp(st);
	return 0;
}

void
vt_state_set_target(struct vt_state *st, enum vt_target tgt)
{
	if (tgt < 0 || tgt >= VT_TARGET_COUNT)
		return;
	if (!st->targets[tgt])
		return;
	st->active_target = tgt;
	st->buf = st->targets[tgt];
}

void
vt_state_altscreen_enter(struct vt_state *st)
{
	int rows, cols;

	if (st->modes & VT_MODE_ALTSCREEN)
		return;

	vt_state_cursor_save(st);
	st->modes |= VT_MODE_ALTSCREEN;

	rows = vt_buf_rows(st->targets[VT_TARGET_PRIMARY]);
	cols = vt_buf_cols(st->targets[VT_TARGET_PRIMARY]);

	/* create alt screen on demand -- no scrollback */
	if (!st->targets[VT_TARGET_ALT])
		st->targets[VT_TARGET_ALT] = vt_buf_new(rows, cols, 0);

	vt_state_set_target(st, VT_TARGET_ALT);
	vt_buf_clear_rows(st->buf, 0, rows);
	st->cursor_row = 0;
	st->cursor_col = 0;
	st->scroll_top = 0;
	st->scroll_bot = rows;
}

void
vt_state_altscreen_leave(struct vt_state *st)
{
	if (!(st->modes & VT_MODE_ALTSCREEN))
		return;

	st->modes &= ~VT_MODE_ALTSCREEN;

	/* destroy alt screen */
	vt_buf_free(st->targets[VT_TARGET_ALT]);
	st->targets[VT_TARGET_ALT] = NULL;

	vt_state_set_target(st, VT_TARGET_PRIMARY);
	st->scroll_top = 0;
	st->scroll_bot = vt_buf_rows(st->buf);

	vt_state_cursor_restore(st);
}

void
vt_state_cursor_save(struct vt_state *st)
{
	st->saved.row = st->cursor_row;
	st->saved.col = st->cursor_col;
	st->saved.attrs = st->attrs;
	st->saved.fg = st->fg;
	st->saved.bg = st->bg;
}

void
vt_state_cursor_restore(struct vt_state *st)
{
	st->cursor_row = st->saved.row;
	st->cursor_col = st->saved.col;
	st->attrs = st->saved.attrs;
	st->fg = st->saved.fg;
	st->bg = st->saved.bg;
	vt_state_cursor_clamp(st);
}

void
vt_state_cursor_clamp(struct vt_state *st)
{
	int rows = vt_buf_rows(st->buf);
	int cols = vt_buf_cols(st->buf);

	if (st->cursor_row < 0)
		st->cursor_row = 0;
	if (st->cursor_row >= rows)
		st->cursor_row = rows - 1;
	if (st->cursor_col < 0)
		st->cursor_col = 0;
	if (st->cursor_col >= cols)
		st->cursor_col = cols - 1;
}

void
vt_state_putchar(struct vt_state *st, uint32_t cp, int width)
{
	struct vt_cell *c;
	int cols = vt_buf_cols(st->buf);
	int rows = vt_buf_rows(st->buf);

	/* auto-wrap: if we're past the edge, wrap to next line */
	if (st->cursor_col + width > cols) {
		if (st->modes & VT_MODE_AUTOWRAP) {
			struct vt_row *r = vt_buf_row(st->buf, st->cursor_row);

			if (r)
				r->flags |= VT_ROW_WRAPPED;
			st->cursor_col = 0;
			st->cursor_row++;
			if (st->cursor_row >= st->scroll_bot) {
				st->cursor_row = st->scroll_bot - 1;
				vt_buf_scroll(st->buf, st->scroll_top,
				    st->scroll_bot, 1);
			}
		} else {
			st->cursor_col = cols - width;
		}
	}

	/* insert mode: shift cells right */
	if (st->modes & VT_MODE_INSERT) {
		struct vt_row *r = vt_buf_row(st->buf, st->cursor_row);

		if (r) {
			int i;

			for (i = cols - 1; i >= st->cursor_col + width; i--)
				r->cells[i] = r->cells[i - width];
		}
	}

	c = vt_buf_cell(st->buf, st->cursor_row, st->cursor_col);
	if (!c)
		return;

	c->codepoint = cp;
	c->attrs = st->attrs;
	c->fg = st->fg;
	c->bg = st->bg;
	c->width = (uint8_t)width;

	/* for wide chars, blank the trailing cell */
	if (width == 2 && st->cursor_col + 1 < cols) {
		struct vt_cell *c2;

		c2 = vt_buf_cell(st->buf, st->cursor_row,
		    st->cursor_col + 1);
		if (c2) {
			vt_cell_clear(c2);
			c2->width = 0; /* continuation cell */
		}
	}

	/* mark row dirty */
	{
		struct vt_row *r = vt_buf_row(st->buf, st->cursor_row);

		if (r)
			r->flags |= VT_ROW_DIRTY;
	}

	st->cursor_col += width;

	/* clamp -- cursor can sit at cols (pending wrap) only with autowrap */
	if (!(st->modes & VT_MODE_AUTOWRAP) && st->cursor_col >= cols)
		st->cursor_col = cols - 1;

	(void)rows;
}

void
vt_state_tab_reset(struct vt_state *st)
{
	int cols = vt_buf_cols(st->buf);
	int i;

	memset(st->tabstops, 0, (size_t)cols);
	for (i = 0; i < cols; i += DEFAULT_TAB_WIDTH)
		st->tabstops[i] = 1;
}

void
vt_state_tab_set(struct vt_state *st, int col)
{
	if (col >= 0 && col < vt_buf_cols(st->buf))
		st->tabstops[col] = 1;
}

void
vt_state_tab_clear(struct vt_state *st, int col)
{
	if (col >= 0 && col < vt_buf_cols(st->buf))
		st->tabstops[col] = 0;
}

int
vt_state_tab_next(struct vt_state *st, int col)
{
	int cols = vt_buf_cols(st->buf);
	int i;

	for (i = col + 1; i < cols; i++) {
		if (st->tabstops[i])
			return i;
	}
	return cols - 1;
}

int
vt_state_tab_prev(struct vt_state *st, int col)
{
	int i;

	for (i = col - 1; i >= 0; i--) {
		if (st->tabstops[i])
			return i;
	}
	return 0;
}

/* ---- screen dump ---- */

/* format a decimal integer, return length */
static int
dump_fmt_int(char *buf, int n)
{
	int len = 0;
	char tmp[12];
	int i;

	if (n <= 0) {
		buf[0] = '0';
		return 1;
	}
	while (n > 0) {
		tmp[len++] = '0' + (n % 10);
		n /= 10;
	}
	for (i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	return len;
}

static void
dump_sgr(vt_dump_fn emit, void *ctx, const struct vt_cell *c)
{
	char buf[64];
	int len = 0;
	int need_sep = 0;

	buf[len++] = '\033';
	buf[len++] = '[';
	buf[len++] = '0'; /* reset first */
	need_sep = 1;

#define SEP() do { if (need_sep) buf[len++] = ';'; need_sep = 1; } while (0)
#define ATTR(flag, code) \
	do { if (c->attrs & (flag)) { SEP(); len += dump_fmt_int(buf + len, (code)); } } while (0)

	ATTR(VT_ATTR_BOLD, 1);
	ATTR(VT_ATTR_DIM, 2);
	ATTR(VT_ATTR_ITALIC, 3);
	ATTR(VT_ATTR_UNDERLINE, 4);
	ATTR(VT_ATTR_BLINK, 5);
	ATTR(VT_ATTR_REVERSE, 7);
	ATTR(VT_ATTR_HIDDEN, 8);
	ATTR(VT_ATTR_STRIKE, 9);

#undef ATTR

	if (c->fg.type == VT_COLOR_INDEXED) {
		if (c->fg.index < 8) {
			SEP(); len += dump_fmt_int(buf + len, 30 + c->fg.index);
		} else if (c->fg.index < 16) {
			SEP(); len += dump_fmt_int(buf + len, 90 + c->fg.index - 8);
		} else {
			SEP(); buf[len++]='3'; buf[len++]='8'; buf[len++]=';';
			buf[len++]='5'; buf[len++]=';';
			len += dump_fmt_int(buf + len, c->fg.index);
		}
	} else if (c->fg.type == VT_COLOR_RGB) {
		SEP(); buf[len++]='3'; buf[len++]='8'; buf[len++]=';';
		buf[len++]='2'; buf[len++]=';';
		len += dump_fmt_int(buf + len, c->fg.rgb.r); buf[len++]=';';
		len += dump_fmt_int(buf + len, c->fg.rgb.g); buf[len++]=';';
		len += dump_fmt_int(buf + len, c->fg.rgb.b);
	}

	if (c->bg.type == VT_COLOR_INDEXED) {
		if (c->bg.index < 8) {
			SEP(); len += dump_fmt_int(buf + len, 40 + c->bg.index);
		} else if (c->bg.index < 16) {
			SEP(); len += dump_fmt_int(buf + len, 100 + c->bg.index - 8);
		} else {
			SEP(); buf[len++]='4'; buf[len++]='8'; buf[len++]=';';
			buf[len++]='5'; buf[len++]=';';
			len += dump_fmt_int(buf + len, c->bg.index);
		}
	} else if (c->bg.type == VT_COLOR_RGB) {
		SEP(); buf[len++]='4'; buf[len++]='8'; buf[len++]=';';
		buf[len++]='2'; buf[len++]=';';
		len += dump_fmt_int(buf + len, c->bg.rgb.r); buf[len++]=';';
		len += dump_fmt_int(buf + len, c->bg.rgb.g); buf[len++]=';';
		len += dump_fmt_int(buf + len, c->bg.rgb.b);
	}

#undef SEP

	buf[len++] = 'm';
	emit(ctx, buf, (size_t)len);
}

void
vt_state_dump(struct vt_state *st, vt_dump_fn emit, void *ctx)
{
	int rows = vt_buf_rows(st->buf);
	int cols = vt_buf_cols(st->buf);
	int row, col;
	char esc[32];
	int esc_len;

	/* reset terminal state */
	emit(ctx, "\033[0m\033[2J\033[H", 12);

	/* alt screen mode if active */
	if (st->modes & VT_MODE_ALTSCREEN)
		emit(ctx, "\033[?1049h", 8);

	/* restore window title */
	if (st->title && st->title[0]) {
		char tbuf[256];
		int tlen;

		tlen = snprintf(tbuf, sizeof(tbuf),
		    "\033]2;%s\033\\", st->title);
		if (tlen > 0 && tlen < (int)sizeof(tbuf))
			emit(ctx, tbuf, (size_t)tlen);
	}

	for (row = 0; row < rows; row++) {
		struct vt_row *vr = vt_buf_row(st->buf, row);

		if (!vr)
			continue;

		/* CUP to start of row */
		esc_len = 0;
		esc[esc_len++] = '\033';
		esc[esc_len++] = '[';
		esc_len += dump_fmt_int(esc + esc_len, row + 1);
		esc[esc_len++] = ';';
		esc[esc_len++] = '1';
		esc[esc_len++] = 'H';
		emit(ctx, esc, (size_t)esc_len);

		for (col = 0; col < cols; col++) {
			struct vt_cell *c = &vr->cells[col];
			unsigned char ubuf[4];
			int ulen;

			/* skip continuation cells */
			if (c->width == 0)
				continue;

			/* emit SGR if cell has any attributes or colors */
			if (c->attrs != 0 ||
			    c->fg.type != VT_COLOR_DEFAULT ||
			    c->bg.type != VT_COLOR_DEFAULT)
				dump_sgr(emit, ctx, c);
			else if (col == 0 || row == 0)
				emit(ctx, "\033[0m", 4);

			/* emit character */
			ulen = utf8_encode(ubuf, c->codepoint);
			if (ulen > 0)
				emit(ctx, (const char *)ubuf, (size_t)ulen);
		}
	}

	/* reset SGR */
	emit(ctx, "\033[0m", 4);

	/* position cursor */
	esc_len = 0;
	esc[esc_len++] = '\033';
	esc[esc_len++] = '[';
	esc_len += dump_fmt_int(esc + esc_len, st->cursor_row + 1);
	esc[esc_len++] = ';';
	esc_len += dump_fmt_int(esc + esc_len, st->cursor_col + 1);
	esc[esc_len++] = 'H';
	emit(ctx, esc, (size_t)esc_len);

	/* cursor visibility */
	if (st->modes & VT_MODE_CURSOR_VIS)
		emit(ctx, "\033[?25h", 6);
	else
		emit(ctx, "\033[?25l", 6);

	/* cursor shape (DECSCUSR) */
	if (st->cursor_shape > 0) {
		esc_len = snprintf(esc, sizeof(esc),
		    "\033[%d q", st->cursor_shape);
		if (esc_len > 0)
			emit(ctx, esc, (size_t)esc_len);
	}
}
