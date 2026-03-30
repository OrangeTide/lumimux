/* tui_term.c : terminal backend for libtui */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_term.h"
#include "txl.h"
#include "tio_write.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFSZ 4096

struct tui_term {
	struct tui_backend	be;
	struct txl		*txl;
	int			fd;
	char			buf[BUFSZ];
	int			pos;
	/* SGR state tracking */
	uint16_t		cur_attrs;
	struct vt_color		cur_fg;
	struct vt_color		cur_bg;
	int			cur_row;
	int			cur_col;
};

static int
sgr_int(char *buf, int v)
{
	return snprintf(buf, 12, "%d", v < 0 ? 0 : v);
}

/* ---- buffered output helpers ---- */

static void
tb_flush_buf(struct tui_term *t)
{
	if (t->pos > 0) {
		tio_write(t->fd, t->buf, (size_t)t->pos);
		t->pos = 0;
	}
}

static void
tb_ensure(struct tui_term *t, int need)
{
	if (t->pos + need > BUFSZ)
		tb_flush_buf(t);
}

static void
tb_putc(struct tui_term *t, char c)
{
	tb_ensure(t, 1);
	t->buf[t->pos++] = c;
}

static void
tb_putmem(struct tui_term *t, const char *s, int len)
{
	tb_ensure(t, len);
	memcpy(t->buf + t->pos, s, (size_t)len);
	t->pos += len;
}

/* ---- cursor positioning ---- */

static void
tb_cup(struct tui_term *t, int row, int col)
{
	char seq[24];
	int n;

	if (row == t->cur_row && col == t->cur_col)
		return;

	n = txl_cup(t->txl, seq, sizeof(seq), row, col);
	if (n > 0)
		tb_putmem(t, seq, n);
	t->cur_row = row;
	t->cur_col = col;
}

/* ---- SGR emission ---- */

static int
color_eq(const struct vt_color *a, const struct vt_color *b)
{
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case VT_COLOR_DEFAULT:
		return 1;
	case VT_COLOR_INDEXED:
		return a->index == b->index;
	case VT_COLOR_RGB:
		return a->rgb.r == b->rgb.r &&
		    a->rgb.g == b->rgb.g &&
		    a->rgb.b == b->rgb.b;
	}
	return 0;
}

static void
tb_emit_sgr(struct tui_term *t, uint16_t attrs,
    const struct vt_color *fg, const struct vt_color *bg)
{
	char sgr[96];
	int len = 0;
	int need_sep = 0;

	if (attrs == t->cur_attrs &&
	    color_eq(fg, &t->cur_fg) &&
	    color_eq(bg, &t->cur_bg))
		return;

	sgr[len++] = '\033';
	sgr[len++] = '[';

	/* reset if attrs were removed or colors reverted to default */
	if ((t->cur_attrs & ~attrs) != 0 ||
	    (t->cur_fg.type != VT_COLOR_DEFAULT &&
	    fg->type == VT_COLOR_DEFAULT) ||
	    (t->cur_bg.type != VT_COLOR_DEFAULT &&
	    bg->type == VT_COLOR_DEFAULT)) {
		sgr[len++] = '0';
		need_sep = 1;
		t->cur_attrs = 0;
		t->cur_fg.type = VT_COLOR_DEFAULT;
		t->cur_bg.type = VT_COLOR_DEFAULT;
	}

#define SEP() do { if (need_sep) sgr[len++] = ';'; need_sep = 1; } while (0)
#define ATTR(flag, code) \
	do { \
		if ((attrs & (flag)) && !(t->cur_attrs & (flag))) { \
			SEP(); \
			len += sgr_int(sgr + len, (code)); \
		} \
	} while (0)

	ATTR(VT_ATTR_BOLD, 1);
	ATTR(VT_ATTR_DIM, 2);
	ATTR(VT_ATTR_ITALIC, 3);
	ATTR(VT_ATTR_UNDERLINE, 4);
	ATTR(VT_ATTR_BLINK, 5);
	ATTR(VT_ATTR_REVERSE, 7);
	ATTR(VT_ATTR_HIDDEN, 8);
	ATTR(VT_ATTR_STRIKE, 9);

#undef ATTR

	/* foreground */
	if (!color_eq(fg, &t->cur_fg)) {
		switch (fg->type) {
		case VT_COLOR_DEFAULT:
			SEP();
			sgr[len++] = '3';
			sgr[len++] = '9';
			break;
		case VT_COLOR_INDEXED:
			if (fg->index < 8) {
				SEP();
				len += sgr_int(sgr + len, 30 + fg->index);
			} else if (fg->index < 16) {
				SEP();
				len += sgr_int(sgr + len,
				    90 + fg->index - 8);
			} else {
				SEP();
				sgr[len++] = '3';
				sgr[len++] = '8';
				sgr[len++] = ';';
				sgr[len++] = '5';
				sgr[len++] = ';';
				len += sgr_int(sgr + len, fg->index);
			}
			break;
		case VT_COLOR_RGB:
			SEP();
			sgr[len++] = '3';
			sgr[len++] = '8';
			sgr[len++] = ';';
			sgr[len++] = '2';
			sgr[len++] = ';';
			len += sgr_int(sgr + len, fg->rgb.r);
			sgr[len++] = ';';
			len += sgr_int(sgr + len, fg->rgb.g);
			sgr[len++] = ';';
			len += sgr_int(sgr + len, fg->rgb.b);
			break;
		}
	}

	/* background */
	if (!color_eq(bg, &t->cur_bg)) {
		switch (bg->type) {
		case VT_COLOR_DEFAULT:
			SEP();
			sgr[len++] = '4';
			sgr[len++] = '9';
			break;
		case VT_COLOR_INDEXED:
			if (bg->index < 8) {
				SEP();
				len += sgr_int(sgr + len, 40 + bg->index);
			} else if (bg->index < 16) {
				SEP();
				len += sgr_int(sgr + len,
				    100 + bg->index - 8);
			} else {
				SEP();
				sgr[len++] = '4';
				sgr[len++] = '8';
				sgr[len++] = ';';
				sgr[len++] = '5';
				sgr[len++] = ';';
				len += sgr_int(sgr + len, bg->index);
			}
			break;
		case VT_COLOR_RGB:
			SEP();
			sgr[len++] = '4';
			sgr[len++] = '8';
			sgr[len++] = ';';
			sgr[len++] = '2';
			sgr[len++] = ';';
			len += sgr_int(sgr + len, bg->rgb.r);
			sgr[len++] = ';';
			len += sgr_int(sgr + len, bg->rgb.g);
			sgr[len++] = ';';
			len += sgr_int(sgr + len, bg->rgb.b);
			break;
		}
	}

#undef SEP

	sgr[len++] = 'm';

	t->cur_attrs = attrs;
	t->cur_fg = *fg;
	t->cur_bg = *bg;

	tb_putmem(t, sgr, len);
}

/* ---- backend callbacks ---- */

static void
term_cell(void *ctx, int row, int col, uint32_t cp,
    const struct vt_color *fg, const struct vt_color *bg,
    uint16_t attrs)
{
	struct tui_term *t = ctx;
	unsigned char ubuf[4];
	int ulen;

	tb_cup(t, row, col);
	tb_emit_sgr(t, attrs, fg, bg);

	if (cp == 0 || cp == ' ') {
		tb_putc(t, ' ');
	} else {
		ulen = utf8_encode(ubuf, cp);
		if (ulen > 0)
			tb_putmem(t, (const char *)ubuf, ulen);
		else
			tb_putc(t, '?');
	}

	t->cur_col++;
}

static void
term_flush(void *ctx)
{
	struct tui_term *t = ctx;
	const char *sgr0;

	/* reset SGR before flushing */
	sgr0 = txl_str(t->txl, TXL_SGR0);
	if (sgr0) {
		int slen = (int)strlen(sgr0);

		tb_putmem(t, sgr0, slen);
	}
	t->cur_attrs = 0;
	t->cur_fg.type = VT_COLOR_DEFAULT;
	t->cur_bg.type = VT_COLOR_DEFAULT;

	tb_flush_buf(t);
	tio_flush(t->fd);
}

/* ---- public API ---- */

struct tui_backend *
tui_term_new(struct txl *txl, int fd)
{
	struct tui_term *t;

	t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;

	t->be.cell = term_cell;
	t->be.flush = term_flush;
	t->txl = txl;
	t->fd = fd;
	t->cur_fg.type = VT_COLOR_DEFAULT;
	t->cur_bg.type = VT_COLOR_DEFAULT;
	t->cur_row = -1;
	t->cur_col = -1;

	return &t->be;
}

void
tui_term_free(struct tui_backend *be)
{
	if (be)
		free(be); /* tui_term starts with tui_backend */
}

void *
tui_term_ctx(struct tui_backend *be)
{
	return be; /* the struct starts with tui_backend, but ctx IS the struct */
}
