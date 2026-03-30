/* selection.c : mouse text selection, copy, and paste */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "selection.h"
#include "tio_write.h"
#include "utf8.h"

#include <string.h>
#include <unistd.h>

/* ---- selection state ---- */

static int sel_dragging;		/* drag in progress */
static int sel_visible;			/* selection shown (after release) */
static uint32_t sel_win_id;
static int sel_sr, sel_sc;		/* start (anchor) */
static int sel_er, sel_ec;		/* end (current drag point) */

/* ---- clipboard buffer ---- */

#define COPY_MAX 65536

static char copy_buf[COPY_MAX];
static size_t copy_len;

/* ---- base64 encoder ---- */

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t
base64_encode(char *dst, size_t dstsz, const char *src, size_t srclen)
{
	size_t i, o;
	unsigned a, b, c;

	o = 0;
	for (i = 0; i < srclen; i += 3) {
		a = (unsigned char)src[i];
		b = (i + 1 < srclen) ? (unsigned char)src[i + 1] : 0;
		c = (i + 2 < srclen) ? (unsigned char)src[i + 2] : 0;

		if (o + 4 > dstsz)
			break;
		dst[o++] = b64[a >> 2];
		dst[o++] = b64[((a & 3) << 4) | (b >> 4)];
		dst[o++] = (i + 1 < srclen) ?
		    b64[((b & 0x0f) << 2) | (c >> 6)] : '=';
		dst[o++] = (i + 2 < srclen) ? b64[c & 0x3f] : '=';
	}
	return o;
}

/* ---- normalize selection range ---- */

static void
sel_normalize(int *r0, int *c0, int *r1, int *c1)
{
	if (sel_sr < sel_er || (sel_sr == sel_er && sel_sc <= sel_ec)) {
		*r0 = sel_sr;
		*c0 = sel_sc;
		*r1 = sel_er;
		*c1 = sel_ec;
	} else {
		*r0 = sel_er;
		*c0 = sel_ec;
		*r1 = sel_sr;
		*c1 = sel_sc;
	}
}

/* ---- public API ---- */

void
sel_begin(uint32_t win_id, int row, int col)
{
	sel_dragging = 1;
	sel_visible = 0;
	sel_win_id = win_id;
	sel_sr = row;
	sel_sc = col;
	sel_er = row;
	sel_ec = col;
}

void
sel_update(int row, int col)
{
	sel_er = row;
	sel_ec = col;
}

void
sel_finish(const struct vt_cell *screen, int rows, int cols)
{
	int r0, c0, r1, c1;
	int r, c, start, end;
	size_t pos;

	sel_dragging = 0;
	sel_visible = 1;

	/* extract text into copy_buf */
	sel_normalize(&r0, &c0, &r1, &c1);

	pos = 0;
	for (r = r0; r <= r1 && r < rows; r++) {
		start = (r == r0) ? c0 : 0;
		end = (r == r1) ? c1 : cols - 1;
		if (start < 0)
			start = 0;
		if (end >= cols)
			end = cols - 1;

		/* find last non-space to strip trailing whitespace */
		int last_nonspace = start - 1;

		for (c = start; c <= end; c++) {
			const struct vt_cell *cell =
			    &screen[r * cols + c];

			/* skip continuation cells of wide characters */
			if (cell->codepoint == 0 && cell->width == 0)
				continue;
			if (cell->codepoint != ' ' &&
			    cell->codepoint != 0)
				last_nonspace = c;
		}

		for (c = start; c <= last_nonspace; c++) {
			const struct vt_cell *cell =
			    &screen[r * cols + c];
			unsigned char u8[4];
			int n;

			/* skip continuation cells of wide characters */
			if (cell->codepoint == 0 && cell->width == 0)
				continue;

			if (cell->codepoint == 0) {
				/* empty cell -> space */
				if (pos < COPY_MAX - 1)
					copy_buf[pos++] = ' ';
				continue;
			}

			n = utf8_encode(u8, cell->codepoint);
			if (n > 0 && pos + (size_t)n < COPY_MAX)  {
				memcpy(copy_buf + pos, u8, (size_t)n);
				pos += (size_t)n;
			}
		}

		/* newline between rows (not after last) */
		if (r < r1 && pos < COPY_MAX - 1)
			copy_buf[pos++] = '\n';
	}
	copy_len = pos;

	/* send to clipboard via OSC 52 */
	if (copy_len > 0) {
		/* \033]52;c;<base64>\033\\ */
		char hdr[] = "\033]52;c;";
		char trl[] = "\033\\";
		/* base64 output is ceil(len/3)*4 */
		size_t b64sz = ((copy_len + 2) / 3) * 4;
		char b64buf[((COPY_MAX + 2) / 3) * 4];
		size_t b64len;

		b64len = base64_encode(b64buf, b64sz, copy_buf, copy_len);
		tio_write(STDOUT_FILENO, hdr, sizeof(hdr) - 1);
		tio_write(STDOUT_FILENO, b64buf, b64len);
		tio_write(STDOUT_FILENO, trl, sizeof(trl) - 1);
		tio_flush(STDOUT_FILENO);
	}
}

void
sel_clear(void)
{
	sel_dragging = 0;
	sel_visible = 0;
}

int
sel_active(void)
{
	return sel_dragging || sel_visible;
}

void
sel_highlight(struct vt_cell *screen, int rows, int cols)
{
	int r0, c0, r1, c1;
	int r, c, start, end;

	if (!sel_active())
		return;

	sel_normalize(&r0, &c0, &r1, &c1);

	for (r = r0; r <= r1 && r < rows; r++) {
		if (r < 0)
			continue;
		start = (r == r0) ? c0 : 0;
		end = (r == r1) ? c1 : cols - 1;
		if (start < 0)
			start = 0;
		if (end >= cols)
			end = cols - 1;
		for (c = start; c <= end; c++)
			screen[r * cols + c].attrs ^= VT_ATTR_REVERSE;
	}
}

const char *
sel_copy_buf(void)
{
	return copy_buf;
}

size_t
sel_copy_len(void)
{
	return copy_len;
}
