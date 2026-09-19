/* tui_out.c : terminal output buffer for full-screen TUI programs */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tui_out.h"

#include "utf8.h"
#include "rune_width.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void
tui_out_reset(struct tui_out *o)
{
	o->len = 0;
}

void
tui_out_free(struct tui_out *o)
{
	if (!o)
		return;
	free(o->buf);
	o->buf = NULL;
	o->len = o->cap = 0;
}

static int
out_reserve(struct tui_out *o, size_t extra)
{
	if (o->len + extra > o->cap) {
		size_t cap = o->cap ? o->cap : 4096;
		char *p;

		while (cap < o->len + extra)
			cap *= 2;
		p = realloc(o->buf, cap);
		if (!p)
			return -1;
		o->buf = p;
		o->cap = cap;
	}
	return 0;
}

void
tui_out_put(struct tui_out *o, const char *s, size_t n)
{
	if (out_reserve(o, n) < 0)
		return;
	memcpy(o->buf + o->len, s, n);
	o->len += n;
}

void
tui_out_puts(struct tui_out *o, const char *s)
{
	tui_out_put(o, s, strlen(s));
}

void
tui_out_printf(struct tui_out *o, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0)
		tui_out_put(o, tmp, (size_t)n < sizeof(tmp) ? (size_t)n
		    : sizeof(tmp) - 1);
}

void
tui_out_move(struct tui_out *o, int row, int col)
{
	tui_out_printf(o, "\033[%d;%dH", row, col);
}

/*
 * The SGR encoding (39/49 default, 38;5;n indexed, 38;2;r;g;b rgb) is
 * standardized, so it is built directly here the way the renderer does;
 * txl intentionally omits a color-to-SGR helper.
 */
static void
out_color(struct tui_out *o, const struct vt_color *c, int is_fg)
{
	int base = is_fg ? 38 : 48;
	int def = is_fg ? 39 : 49;

	switch (c->type) {
	case VT_COLOR_INDEXED:
		tui_out_printf(o, "%d;5;%u", base, c->index);
		break;
	case VT_COLOR_RGB:
		tui_out_printf(o, "%d;2;%u;%u;%u", base,
		    c->rgb.r, c->rgb.g, c->rgb.b);
		break;
	case VT_COLOR_DEFAULT:
	default:
		tui_out_printf(o, "%d", def);
		break;
	}
}

void
tui_out_sgr(struct tui_out *o, const struct vt_color *fg,
    const struct vt_color *bg, int bold)
{
	tui_out_puts(o, "\033[0");
	if (bold)
		tui_out_puts(o, ";1");
	tui_out_puts(o, ";");
	out_color(o, fg, 1);
	tui_out_puts(o, ";");
	out_color(o, bg, 0);
	tui_out_puts(o, "m");
}

void
tui_out_sgr_reset(struct tui_out *o)
{
	tui_out_puts(o, "\033[0m");
}

void
tui_out_field(struct tui_out *o, const char *s, int cols)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t rem = strlen(s);
	int used = 0;

	while (rem > 0 && used < cols) {
		uint32_t r;
		int n = utf8_decode(&r, p, rem);
		int w;

		if (n <= 0)
			n = 1;
		if (r < 0x20 || r == 0x7f) {
			if (used + 1 > cols)
				break;
			tui_out_put(o, " ", 1);
			used += 1;
			p += n;
			rem -= (size_t)n;
			continue;
		}
		w = rune_width(r);
		if (w < 0)
			w = 1;
		if (w > 0 && used + w > cols)
			break;			/* wide rune won't fit */
		tui_out_put(o, (const char *)p, (size_t)n);
		used += w;
		p += n;
		rem -= (size_t)n;
	}
	while (used < cols) {
		tui_out_put(o, " ", 1);
		used++;
	}
}

int
tui_out_flush(struct tui_out *o, int fd)
{
	const char *buf = o->buf;
	size_t len = o->len;

	while (len > 0) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}
