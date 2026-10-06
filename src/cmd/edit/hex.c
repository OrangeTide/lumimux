/* hex.c : hex-dump view helpers for lumi edit.
 *
 * The editor can render the current buffer as a hex dump instead of as text.
 * Both views share one struct text, so these helpers reconstruct the byte
 * stream that view walks: each line's bytes in order, joined by an implied
 * newline, with a trailing newline only when the content ends with one. The
 * functions are pure over struct text; the rendering and key handling that use
 * them live in edit.c. */

#include "editor.h"

#include "text.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whether an implied newline follows line i (a separator before the next line,
 * or the trailing newline after the last line when the file carried one). */
static int
line_has_newline(const struct text *t, size_t i, size_t nlines)
{
	return (i + 1 < nlines) || text_final_newline(t);
}

/* Total bytes the hex view shows: every line's bytes plus one for each implied
 * newline. */
size_t
hex_total(const struct text *t)
{
	size_t n = text_lines(t), i, tot = 0;

	for (i = 0; i < n; i++) {
		tot += text_line_len(t, i);
		if (line_has_newline(t, i, n))
			tot++;
	}
	return tot;
}

/* Byte offset of the text position (cy, cx). cx is a byte column within its
 * line and may equal the line length (the cursor sitting on the newline). */
size_t
hex_offset_of(const struct text *t, size_t cy, size_t cx)
{
	size_t n = text_lines(t), i, off = 0;

	if (cy >= n)
		cy = n ? n - 1 : 0;
	for (i = 0; i < cy; i++) {
		off += text_line_len(t, i);
		if (line_has_newline(t, i, n))
			off++;
	}
	return off + cx;
}

/* Inverse of hex_offset_of: the text position holding byte "off". An offset on
 * an implied newline maps to (line, line_len); at or past the end it clamps to
 * the last position. */
void
hex_pos_at(const struct text *t, size_t off, size_t *cy, size_t *cx)
{
	size_t n = text_lines(t), i, base = 0;

	for (i = 0; i < n; i++) {
		size_t len = text_line_len(t, i);

		if (off <= base + len) {	/* within the line or on its \n */
			*cy = i;
			*cx = off - base;
			return;
		}
		base += len + (line_has_newline(t, i, n) ? 1 : 0);
	}
	*cy = n ? n - 1 : 0;
	*cx = text_line_len(t, *cy);
}

/* Copy up to "count" bytes of the reconstructed stream starting at byte offset
 * "start" into buf. Returns how many were copied (fewer than count only near
 * the end of the buffer). */
size_t
hex_gather(const struct text *t, size_t start, unsigned char *buf,
    size_t count)
{
	size_t n = text_lines(t), cy, cx, got = 0;

	hex_pos_at(t, start, &cy, &cx);
	while (got < count && cy < n) {
		size_t len = 0;
		const char *b = text_line(t, cy, &len);

		while (cx < len && got < count)
			buf[got++] = (unsigned char)b[cx++];
		if (got >= count)
			break;
		if (cx >= len) {		/* reached the implied newline */
			if (line_has_newline(t, cy, n)) {
				if (got >= count)
					break;
				buf[got++] = '\n';
			}
			cy++;
			cx = 0;
		}
	}
	return got;
}

/* Column of byte j's two-character hex pair within a dump row. A wider gap
 * falls after each group of eight bytes, so this is independent of the row's
 * total column count. */
int
hex_hexcol(int j)
{
	return 10 + j * 3 + j / 8;
}

/* Column where the ascii gutter begins for a row of "cols" bytes: past the
 * offset field, the hex pairs, their group gaps, and two spaces. */
int
hex_ascii_start(int cols)
{
	return 10 + cols * 3 + (cols - 1) / 8 + 2;
}

/* Column of byte j's character in the ascii gutter of a "cols"-wide row. */
int
hex_asciicol(int j, int cols)
{
	return hex_ascii_start(cols) + j;
}

/* Total display width of a "cols"-wide dump row (through the closing bar). */
int
hex_row_width(int cols)
{
	return hex_ascii_start(cols) + cols + 1;
}

/* Format one dump row into out: "OFFSET  hex...  |ascii|" for cols bytes per
 * row. buf holds n valid bytes (1..cols); columns past n are left blank. out_sz
 * must be at least hex_row_width(cols) + 1. */
void
hex_format_row(char *out, size_t out_sz, size_t offset,
    const unsigned char *buf, size_t n, int cols)
{
	static const char hexd[] = "0123456789abcdef";
	size_t width = (size_t)hex_row_width(cols);
	char off[24];
	int ol;
	size_t j;

	if (out_sz < width + 1) {
		if (out_sz)
			out[0] = '\0';
		return;
	}
	memset(out, ' ', width);
	out[width] = '\0';

	ol = snprintf(off, sizeof(off), "%08zx", offset);
	if (ol > 8)			/* an offset past 2^32: keep the low digits */
		memcpy(out, off + (ol - 8), 8);
	else
		memcpy(out, off, (size_t)ol);

	out[hex_asciicol(0, cols) - 1] = '|';
	out[hex_asciicol(cols - 1, cols) + 1] = '|';
	for (j = 0; j < (size_t)cols && j < n; j++) {
		unsigned char b = buf[j];
		int hc = hex_hexcol((int)j);

		out[hc] = hexd[b >> 4];
		out[hc + 1] = hexd[b & 0x0f];
		out[hex_asciicol((int)j, cols)] =
		    (b >= 0x20 && b < 0x7f) ? (char)b : '.';
	}
}

/* The value 0-15 of a hex digit, or -1 if c is not one. */
static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parse a hex-pair pattern such as "de ad be ef" (spaces and tabs ignored)
 * into out, storing at most max bytes. Returns 0 with *outlen set on success,
 * or -1 for a non-hex character, an odd trailing nibble, or an overflow. */
int
hex_parse_bytes(const char *s, unsigned char *out, size_t max, size_t *outlen)
{
	size_t n = 0;
	int hi = -1;

	for (; *s; s++) {
		int d;

		if (*s == ' ' || *s == '\t')
			continue;
		d = hexval(*s);
		if (d < 0)
			return -1;
		if (hi < 0) {
			hi = d;
		} else {
			if (n >= max)
				return -1;
			out[n++] = (unsigned char)((hi << 4) | d);
			hi = -1;
		}
	}
	if (hi >= 0)
		return -1;		/* a lone trailing nibble */
	*outlen = n;
	return 0;
}

/* Format a data-inspector line describing the up-to-four bytes b[0..n) that
 * start at the cursor: the first byte as unsigned and signed 8-bit and as a
 * character, then the 16- and 32-bit values in little-endian and big-endian.
 * Fields without enough bytes show "-". */
void
hex_inspect_line(char *out, size_t out_sz, const unsigned char *b, size_t n)
{
	char su8[12], si8[12], sch[8], su16[24], su32[40];

	if (n >= 1) {
		snprintf(su8, sizeof(su8), "%u", (unsigned)b[0]);
		snprintf(si8, sizeof(si8), "%d", (int)(signed char)b[0]);
		if (b[0] >= 0x20 && b[0] < 0x7f)
			snprintf(sch, sizeof(sch), "'%c'", (char)b[0]);
		else
			snprintf(sch, sizeof(sch), "'.'");
	} else {
		snprintf(su8, sizeof(su8), "-");
		snprintf(si8, sizeof(si8), "-");
		snprintf(sch, sizeof(sch), "-");
	}
	if (n >= 2) {
		unsigned le = (unsigned)b[0] | ((unsigned)b[1] << 8);
		unsigned be = ((unsigned)b[0] << 8) | (unsigned)b[1];

		snprintf(su16, sizeof(su16), "%u/%u", le, be);
	} else {
		snprintf(su16, sizeof(su16), "-");
	}
	if (n >= 4) {
		unsigned long le = (unsigned long)b[0] |
		    ((unsigned long)b[1] << 8) | ((unsigned long)b[2] << 16) |
		    ((unsigned long)b[3] << 24);
		unsigned long be = ((unsigned long)b[0] << 24) |
		    ((unsigned long)b[1] << 16) | ((unsigned long)b[2] << 8) |
		    (unsigned long)b[3];

		snprintf(su32, sizeof(su32), "%lu/%lu", le, be);
	} else {
		snprintf(su32, sizeof(su32), "-");
	}
	snprintf(out, out_sz, " u8 %s  i8 %s  %s  u16 %s  u32 %s  (le/be)",
	    su8, si8, sch, su16, su32);
}

/* Search the reconstructed byte stream for the plen-byte pattern pat. The scan
 * starts one byte off the cursor position "from" in direction dir (>=0 forward,
 * <0 backward) and wraps around the buffer once, so the match under the cursor
 * is skipped but every other position is tried. Returns 1 and the match offset
 * in *found, or 0 when the pattern does not occur (or on allocation failure). */
int
hex_find(const struct text *t, const unsigned char *pat, size_t plen,
    size_t from, int dir, size_t *found)
{
	size_t total = hex_total(t), maxstart, i;
	unsigned char *buf;
	int hit = 0;

	if (plen == 0 || plen > total)
		return 0;
	buf = malloc(total);
	if (buf == NULL)
		return 0;
	hex_gather(t, 0, buf, total);
	maxstart = total - plen;		/* last position a match can start */

	if (dir >= 0) {
		for (i = from + 1; i <= maxstart; i++)
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
		for (i = 0; i <= from && i <= maxstart; i++)	/* wrap */
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
	} else {
		i = from;			/* positions below the cursor */
		while (i-- > 0)
			if (i <= maxstart && memcmp(buf + i, pat, plen) == 0)
				goto found;
		i = maxstart + 1;		/* wrap: from the end down to from+1 */
		while (i-- > from + 1)
			if (memcmp(buf + i, pat, plen) == 0)
				goto found;
	}
	free(buf);
	return 0;
found:
	*found = i;
	hit = 1;
	free(buf);
	return hit;
}
