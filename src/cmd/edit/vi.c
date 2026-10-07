/* vi.c : the vi personality for lumi edit.
 *
 * A self-contained modal layer over the modeless editor's buffer operations
 * in edit.c. It keeps its state on struct editor (mode, a pending count, a
 * pending operator) so one key press advances a small state machine: digits
 * build a count, d/c/y arm an operator, and a motion either moves the cursor
 * or, when an operator is armed, defines the span it acts on. It reaches the
 * shared buffer, cursor, and prompt helpers through editor.h. */

#include "editor.h"

#include "rune_width.h"
#include "syntax.h"
#include "text.h"
#include "tkbd.h"
#include "utf8.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * vi personality -- modal editing (toggle with F2)
 *
 * This is a self-contained block layered on top of the modeless editor's
 * buffer operations. It keeps its state on struct editor (mode, a pending
 * count, and a pending operator) so a single key press advances a small
 * state machine: digits build a count, d/c/y arm an operator, and a motion
 * either moves the cursor or, when an operator is armed, defines the span it
 * acts on. When this grows (visual mode, named registers, marks) it should
 * move to its own vi.c with a shared editor-core header.
 ****************************************************************/

/* Byte offset of the first non-blank on a line, or 0 if the line is blank. */
static size_t
first_nonblank(struct editor *e, size_t y)
{
	size_t len = 0, x = 0;
	const char *s = text_line(e->t, y, &len);

	if (!s)
		return 0;
	while (x < len && (s[x] == ' ' || s[x] == '\t'))
		x++;
	if (x >= len)
		x = 0;
	return x;
}

/* Keep the cursor on a valid line and rune boundary. In normal mode the
 * cursor rests on a character, so it may not sit past the last rune. */
void
vi_clamp(struct editor *e)
{
	size_t len = 0;
	const char *s;

	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	s = text_line(e->t, e->cy, &len);
	if (e->mode == MODE_NORMAL && len > 0) {
		if (e->cx >= len)
			e->cx = len - prev_rune_len(s, len);
	} else if (e->cx > len) {
		e->cx = len;
	}
	while (e->cx > 0 && e->cx < len &&
	    ((unsigned char)s[e->cx] & 0xc0) == 0x80)
		e->cx--;			/* snap off a continuation byte */
}

/* Clear any pending count, operator, and 'g' prefix. */
void
vi_reset_pending(struct editor *e)
{
	e->vi_count = 0;
	e->vi_op = 0;
	e->vi_op_count = 0;
	e->vi_gpending = 0;
	e->vi_charsearch = 0;
	e->vi_textobj = 0;
	e->vi_markcmd = 0;
	e->vi_rpending = 0;
	e->vi_regpending = 0;
	e->vi_zpending = 0;
}

/* Character class for word motions: 0 blank, 1 word, 2 punctuation. With big
 * set (W/B/E motions) every non-blank rune is a word rune. */
static int
vi_class(uint32_t r, int big)
{
	if (r == ' ' || r == '\t')
		return 0;
	if (big)
		return 1;
	if (r == '_' || (r >= '0' && r <= '9') || (r >= 'A' && r <= 'Z') ||
	    (r >= 'a' && r <= 'z') || r >= 0x80)
		return 1;
	return 2;
}

/* Decode the rune at (y,x); returns its byte length and fills *r, or 0 when
 * the position is at or past the end of the line. */
static int
vi_rune(struct editor *e, size_t y, size_t x, uint32_t *r)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);
	int n;

	if (!s || x >= len)
		return 0;
	n = utf8_decode(r, (const unsigned char *)s + x, len - x);
	return n > 0 ? n : 1;
}

/* Step one rune left, crossing to the end of the previous line. Returns 0
 * when already at the start of the buffer. */
static int
vi_step_back(struct editor *e, size_t *y, size_t *x)
{
	if (*x > 0) {
		size_t len = 0;
		const char *s = text_line(e->t, *y, &len);

		*x -= prev_rune_len(s, *x);
		return 1;
	}
	if (*y == 0)
		return 0;
	(*y)--;
	*x = text_line_len(e->t, *y);
	return 1;
}

/* Advance (*py,*px) to the start of the next word. */
static void
vi_pos_word_fwd(struct editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px, len;
	uint32_t r;
	int n, cls;

	n = vi_rune(e, y, x, &r);
	if (n > 0) {
		cls = vi_class(r, big);
		if (cls != 0)
			while ((n = vi_rune(e, y, x, &r)) > 0 &&
			    vi_class(r, big) == cls)
				x += (size_t)n;
	}
	for (;;) {
		len = text_line_len(e->t, y);
		if (x >= len) {
			if (y + 1 >= text_lines(e->t)) {
				x = len;
				break;
			}
			y++;
			x = 0;
			if (text_line_len(e->t, y) == 0)
				break;		/* an empty line is a word */
			continue;
		}
		n = vi_rune(e, y, x, &r);
		if (n <= 0)
			break;
		if (vi_class(r, big) != 0)
			break;
		x += (size_t)n;
	}
	*py = y;
	*px = x;
}

/* Move (*py,*px) back to the start of the previous word. */
static void
vi_pos_word_back(struct editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px;
	uint32_t r;
	int cls;

	if (!vi_step_back(e, &y, &x)) {
		*py = 0;
		*px = 0;
		return;
	}
	for (;;) {
		size_t len = text_line_len(e->t, y);

		if (len == 0)
			break;			/* an empty line is a word */
		if (x >= len) {
			if (!vi_step_back(e, &y, &x))
				goto done;
			continue;
		}
		if (vi_rune(e, y, x, &r) > 0 && vi_class(r, big) != 0)
			break;
		if (!vi_step_back(e, &y, &x))
			goto done;
	}
	if (vi_rune(e, y, x, &r) > 0) {
		cls = vi_class(r, big);
		for (;;) {
			size_t ny = y, nx = x;

			if (!vi_step_back(e, &ny, &nx))
				break;
			if (vi_rune(e, ny, nx, &r) <= 0 ||
			    vi_class(r, big) != cls)
				break;
			y = ny;
			x = nx;
		}
	}
done:
	*py = y;
	*px = x;
}

/* Advance (*py,*px) to the last rune of the next word (inclusive target). */
static void
vi_pos_word_end(struct editor *e, size_t *py, size_t *px, int big)
{
	size_t y = *py, x = *px;
	uint32_t r;
	int n, cls;

	n = vi_rune(e, y, x, &r);
	if (n > 0)
		x += (size_t)n;			/* leave the current rune */
	for (;;) {
		size_t len = text_line_len(e->t, y);

		if (x >= len) {
			if (y + 1 >= text_lines(e->t)) {
				x = len;
				goto done;
			}
			y++;
			x = 0;
			continue;
		}
		n = vi_rune(e, y, x, &r);
		if (n <= 0)
			goto done;
		if (vi_class(r, big) != 0)
			break;
		x += (size_t)n;
	}
	cls = vi_class(r, big);
	for (;;) {
		size_t nx = x + (size_t)n;
		uint32_t r2;
		int n2 = vi_rune(e, y, nx, &r2);

		if (n2 <= 0 || vi_class(r2, big) != cls)
			break;
		x = nx;
		n = n2;
	}
done:
	*py = y;
	*px = x;
}

/* Move one rune forward, crossing to the start of the next line. Returns 0
 * at the end of the buffer. */
static int
vi_step_fwd(struct editor *e, size_t *y, size_t *x)
{
	size_t len = 0;
	const char *s = text_line(e->t, *y, &len);

	if (s && *x < len) {
		*x += rune_len_at(s, len, *x);
		return 1;
	}
	if (*y + 1 >= text_lines(e->t))
		return 0;
	(*y)++;
	*x = 0;
	return 1;
}

/* Paragraphs are separated by empty lines. '}' moves to the next empty line
 * below (or the end of the buffer); '{' to the previous one (or the top). */
static void
vi_para_fwd(struct editor *e, size_t *py, size_t *px, int count)
{
	size_t y = *py, nlines = text_lines(e->t);
	int i;

	for (i = 0; i < count; i++) {
		size_t z = y + 1;

		while (z < nlines && text_line_len(e->t, z) != 0)
			z++;
		if (z >= nlines) {
			*py = nlines - 1;
			*px = text_line_len(e->t, nlines - 1);
			return;
		}
		y = z;
	}
	*py = y;
	*px = 0;
}

static void
vi_para_back(struct editor *e, size_t *py, size_t *px, int count)
{
	size_t y = *py;
	int i;

	for (i = 0; i < count && y > 0; i++) {
		size_t z = y - 1;

		while (z > 0 && text_line_len(e->t, z) != 0)
			z--;
		y = z;
	}
	*py = y;
	*px = 0;
}

static int
vi_is_closer(uint32_t r)
{
	return r == ')' || r == ']' || r == '"' || r == '\'';
}

/* True when a<b in reading order. */
static int
vi_pos_lt(size_t ay, size_t ax, size_t by, size_t bx)
{
	return ay < by || (ay == by && ax < bx);
}

/* Advance (*py,*px) to the start of the next sentence. A sentence ends at
 * '.', '!' or '?' followed by optional closers and then whitespace or the end
 * of a line; an empty line is also a boundary. */
static void
vi_sentence_step_fwd(struct editor *e, size_t *py, size_t *px)
{
	size_t y = *py, x = *px, nlines = text_lines(e->t);
	uint32_t r;

	if (!vi_step_fwd(e, &y, &x))
		goto done;
	for (;;) {
		int n;

		if (text_line_len(e->t, y) == 0) {	/* paragraph boundary */
			x = 0;
			goto done;
		}
		n = vi_rune(e, y, x, &r);
		if (n == 0) {				/* end of line */
			if (y + 1 >= nlines) {
				x = text_line_len(e->t, y);
				goto done;
			}
			y++;
			x = 0;
			continue;
		}
		if (r == '.' || r == '!' || r == '?') {
			size_t yy = y, xx = x + (size_t)n;
			uint32_t r2;
			int n2;

			while ((n2 = vi_rune(e, yy, xx, &r2)) > 0 &&
			    vi_is_closer(r2))
				xx += (size_t)n2;
			n2 = vi_rune(e, yy, xx, &r2);
			if (n2 == 0 || r2 == ' ' || r2 == '\t') {
				y = yy;
				x = xx;
				for (;;) {	/* skip to the next non-blank */
					int n3;
					uint32_t r3;

					if (text_line_len(e->t, y) == 0) {
						x = 0;
						goto done;
					}
					n3 = vi_rune(e, y, x, &r3);
					if (n3 == 0) {
						if (y + 1 >= nlines) {
							x = text_line_len(
							    e->t, y);
							goto done;
						}
						y++;
						x = 0;
						continue;
					}
					if (r3 == ' ' || r3 == '\t') {
						x += (size_t)n3;
						continue;
					}
					goto done;
				}
			}
		}
		if (!vi_step_fwd(e, &y, &x))
			goto done;
	}
done:
	*py = y;
	*px = x;
}

/* First content line of the paragraph containing y, at its first non-blank.
 * A blank line is its own paragraph start. */
static void
vi_para_content_start(struct editor *e, size_t y, size_t *sy, size_t *sx)
{
	if (text_line_len(e->t, y) == 0) {
		*sy = y;
		*sx = 0;
		return;
	}
	while (y > 0 && text_line_len(e->t, y - 1) != 0)
		y--;
	*sy = y;
	*sx = first_nonblank(e, y);
}

/* Move (*py,*px) back to the start of the current or previous sentence. It
 * walks forward from a point at most a paragraph earlier and keeps the last
 * sentence start before the cursor, which handles crossing a blank line. */
static void
vi_sentence_step_back(struct editor *e, size_t *py, size_t *px)
{
	size_t cy = *py, cx = *px, scan_y, scan_x, best_y, best_x, cur_y, cur_x;

	if (cy == 0 && cx == 0)
		return;

	vi_para_content_start(e, cy, &scan_y, &scan_x);
	if (!vi_pos_lt(scan_y, scan_x, cy, cx)) {
		/* cursor is at the paragraph's first sentence: back up into
		 * the previous paragraph so its sentences are in range */
		size_t z = scan_y;

		if (z == 0) {
			*py = 0;
			*px = 0;
			return;
		}
		z--;
		while (z > 0 && text_line_len(e->t, z) == 0)
			z--;
		vi_para_content_start(e, z, &scan_y, &scan_x);
	}

	best_y = scan_y;
	best_x = scan_x;
	cur_y = scan_y;
	cur_x = scan_x;
	for (;;) {
		size_t ny = cur_y, nx = cur_x;

		vi_sentence_step_fwd(e, &ny, &nx);
		if (!vi_pos_lt(cur_y, cur_x, ny, nx))
			break;			/* no forward progress */
		if (!vi_pos_lt(ny, nx, cy, cx))
			break;			/* reached the cursor */
		best_y = ny;
		best_x = nx;
		cur_y = ny;
		cur_x = nx;
	}
	*py = best_y;
	*px = best_x;
}

/* A resolved motion: a target position plus how an operator treats it. */
struct vi_mot {
	size_t	y, x;
	int	line;		/* operate on whole lines */
	int	incl;		/* charwise: include the target rune */
	int	valid;
};

/* Classify a bracket rune: fill *match with its partner and *forward with the
 * direction to search for it. Returns 0 for a non-bracket. */
static int
vi_bracket_info(uint32_t r, uint32_t *match, int *forward)
{
	switch (r) {
	case '(': *match = ')'; *forward = 1; return 1;
	case '[': *match = ']'; *forward = 1; return 1;
	case '{': *match = '}'; *forward = 1; return 1;
	case ')': *match = '('; *forward = 0; return 1;
	case ']': *match = '['; *forward = 0; return 1;
	case '}': *match = '{'; *forward = 0; return 1;
	default:  return 0;
	}
}

/* The vi '%' motion: from the bracket at or forward of the cursor on the
 * current line, jump to its match, counting nesting across lines. The result
 * is inclusive so d% covers through the match. Invalid when the line holds no
 * bracket at or after the cursor, or the match is unbalanced. */
static struct vi_mot
vi_match_pair(struct editor *e)
{
	struct vi_mot r = { e->cy, e->cx, 0, 0, 0 };
	size_t len = 0, bx = e->cx;
	const char *s = text_line(e->t, e->cy, &len);
	uint32_t open = 0, want = 0;
	int forward = 0, depth = 0;
	size_t y, x;

	if (!s)
		return r;
	while (bx < len) {			/* first bracket at/after cursor */
		uint32_t rr;
		int n = utf8_decode(&rr, (const unsigned char *)s + bx,
		    len - bx);

		if (n <= 0)
			n = 1;
		if (vi_bracket_info(rr, &want, &forward)) {
			open = rr;
			break;
		}
		bx += (size_t)n;
	}
	if (!open)
		return r;

	y = e->cy;
	x = bx;
	for (;;) {
		uint32_t rr;
		int n = vi_rune(e, y, x, &rr);

		if (n > 0) {
			if (rr == open)
				depth++;
			else if (rr == want && --depth == 0) {
				r.y = y;
				r.x = x;
				r.incl = 1;
				r.valid = 1;
				return r;
			}
		}
		if (forward) {
			if (!vi_step_fwd(e, &y, &x))
				break;
		} else if (!vi_step_back(e, &y, &x)) {
			break;
		}
	}
	return r;				/* no match found */
}

/* Byte offset of the rune that occupies display column target_col (0-based)
 * on line y, expanding tabs and honoring rune widths. Returns the line length
 * when the column is past the end. This is the inverse of disp_cols(). */
size_t
vi_col_to_byte(struct editor *e, size_t y, int target_col)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;

	if (!s || target_col < 0)
		return 0;
	while (i < len) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, len - i);
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
		if (target_col < col + w)
			break;			/* the column falls in this rune */
		col += w;
		i += (size_t)n;
	}
	return i;
}

/* Resolve a motion character to a target. have_count says whether the caller
 * actually typed a count (matters for G/gg, whose count is a line number). */
static struct vi_mot
vi_motion(struct editor *e, uint32_t m, int count, int have_count)
{
	struct vi_mot r = { e->cy, e->cx, 0, 0, 1 };
	size_t len = text_line_len(e->t, e->cy);
	const char *line = text_line(e->t, e->cy, NULL);
	int i;

	if (count < 1)
		count = 1;

	switch (m) {
	case 'h':
		for (i = 0; i < count && r.x > 0; i++)
			r.x -= prev_rune_len(line, r.x);
		break;
	case 'l':
	case ' ':
		for (i = 0; i < count && r.x < len; i++)
			r.x += rune_len_at(line, len, r.x);
		break;
	case '0':
		r.x = 0;
		break;
	case '^':
		r.x = first_nonblank(e, e->cy);
		break;
	case '$':
		r.x = len;
		break;
	case 'w':
	case 'W': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_fwd(e, &y, &x, m == 'W');
		r.y = y;
		r.x = x;
		break;
	}
	case 'b':
	case 'B': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_back(e, &y, &x, m == 'B');
		r.y = y;
		r.x = x;
		break;
	}
	case 'e':
	case 'E': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_pos_word_end(e, &y, &x, m == 'E');
		r.y = y;
		r.x = x;
		r.incl = 1;
		break;
	}
	case 'j':
		r.line = 1;
		r.y = e->cy + (size_t)count;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case 'k':
		r.line = 1;
		r.y = e->cy > (size_t)count ? e->cy - (size_t)count : 0;
		break;
	case 'G':
		r.line = 1;
		r.y = have_count ? (size_t)(count - 1) : text_lines(e->t) - 1;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case 'g':				/* the second g of gg */
		r.line = 1;
		r.y = have_count ? (size_t)(count - 1) : 0;
		if (r.y >= text_lines(e->t))
			r.y = text_lines(e->t) - 1;
		break;
	case '}': {
		size_t y = e->cy, x = e->cx;

		vi_para_fwd(e, &y, &x, count);
		r.y = y;
		r.x = x;
		break;
	}
	case '{': {
		size_t y = e->cy, x = e->cx;

		vi_para_back(e, &y, &x, count);
		r.y = y;
		r.x = x;
		break;
	}
	case ')': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_sentence_step_fwd(e, &y, &x);
		r.y = y;
		r.x = x;
		break;
	}
	case '(': {
		size_t y = e->cy, x = e->cx;

		for (i = 0; i < count; i++)
			vi_sentence_step_back(e, &y, &x);
		r.y = y;
		r.x = x;
		break;
	}
	case '%':
		if (have_count) {		/* N% -- go to N percent of file */
			size_t nlines = text_lines(e->t);
			size_t ln = (size_t)((long)count * (long)nlines + 99)
			    / 100;

			if (ln < 1)
				ln = 1;
			if (ln > nlines)
				ln = nlines;
			r.line = 1;
			r.y = ln - 1;
			break;
		}
		return vi_match_pair(e);	/* bare % -- jump to match */
	case '|':				/* go to display column count */
		r.x = vi_col_to_byte(e, e->cy, count - 1);
		break;
	case 'H':				/* top line of the window */
	case 'M':				/* middle line */
	case 'L': {				/* bottom line */
		int text_h = text_height(e);
		size_t top = e->top, last;

		last = top + (size_t)text_h - 1;
		if (last >= text_lines(e->t))
			last = text_lines(e->t) - 1;
		r.line = 1;
		if (m == 'H') {
			r.y = top + (size_t)(count - 1);
			if (r.y > last)
				r.y = last;
		} else if (m == 'L') {
			r.y = last >= (size_t)(count - 1) ?
			    last - (size_t)(count - 1) : 0;
			if (r.y < top)
				r.y = top;
		} else {
			r.y = top + (last - top) / 2;
		}
		break;
	}
	default:
		r.valid = 0;
	}
	return r;
}

/* Byte offset of the count-th occurrence of target at or after `from` on the
 * line s[0,len). Sets *found. */
static size_t
find_char_fwd(const char *s, size_t len, size_t from, uint32_t target,
    int count, int *found)
{
	size_t x = from;
	int hits = 0;

	while (x < len) {
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, len - x);

		if (n <= 0)
			n = 1;
		if (r == target && ++hits == count) {
			*found = 1;
			return x;
		}
		x += (size_t)n;
	}
	*found = 0;
	return 0;
}

/* Byte offset of the count-th occurrence of target strictly before `from`,
 * counting leftward from just before `from`. Sets *found. */
static size_t
find_char_back(const char *s, size_t from, uint32_t target, int count,
    int *found)
{
	size_t x = 0;
	int tot = 0, want, idx = 0;

	while (x < from) {			/* count matches before `from` */
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, from - x);

		if (n <= 0)
			n = 1;
		if (r == target)
			tot++;
		x += (size_t)n;
	}
	if (count > tot) {
		*found = 0;
		return 0;
	}
	want = tot - count;			/* nearest-left is the last match */
	x = 0;
	while (x < from) {
		uint32_t r;
		int n = utf8_decode(&r, (const unsigned char *)s + x, from - x);

		if (n <= 0)
			n = 1;
		if (r == target && idx++ == want) {
			*found = 1;
			return x;
		}
		x += (size_t)n;
	}
	*found = 0;
	return 0;
}

/* Resolve an f/F/t/T search for target on the current line. repeat is set for
 * ; and , so a t/T advances past an adjacent match instead of sticking. */
static struct vi_mot
vi_charsearch_motion(struct editor *e, char cmd, uint32_t target, int count,
    int repeat)
{
	struct vi_mot r = { e->cy, e->cx, 0, 0, 0 };
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	int forward = (cmd == 'f' || cmd == 't');
	int till = (cmd == 't' || cmd == 'T');
	int found = 0;
	size_t pos;

	if (!s || count < 1)
		return r;

	if (forward) {
		size_t from = e->cx + rune_len_at(s, len, e->cx);

		if (till && repeat && from < len)
			from += rune_len_at(s, len, from);
		pos = find_char_fwd(s, len, from, target, count, &found);
		if (!found)
			return r;
		r.x = till ? pos - prev_rune_len(s, pos) : pos;
		r.incl = 1;		/* f/t include the target for operators */
	} else {
		size_t from = e->cx;

		if (till && repeat && from > 0)
			from -= prev_rune_len(s, from);
		pos = find_char_back(s, from, target, count, &found);
		if (!found)
			return r;
		r.x = till ? pos + rune_len_at(s, len, pos) : pos;
		r.incl = 0;		/* backward: region excludes the cursor */
	}
	r.valid = 1;
	return r;
}

/* Remove whole lines [y1,y2], keeping the buffer's one-line invariant. */
static void
vi_delete_lines(struct editor *e, size_t y1, size_t y2)
{
	size_t nlines = text_lines(e->t);
	size_t count, i;

	if (y2 >= nlines)
		y2 = nlines - 1;
	if (y1 > y2) {
		size_t tmp = y1;

		y1 = y2;
		y2 = tmp;
	}
	count = y2 - y1 + 1;
	hl_touch(e, y1);

	if (count >= nlines) {			/* the whole buffer */
		size_t last = 0;

		text_line(e->t, nlines - 1, &last);
		delete_region(e, 0, 0, nlines - 1, last);
		e->cy = 0;
		e->cx = 0;
		return;
	}
	for (i = 0; i < count; i++) {
		size_t ll = text_line_len(e->t, y1);

		text_delete(e->t, y1, 0, ll);
		if (y1 + 1 < text_lines(e->t))
			text_join(e->t, y1);		/* pull the next line up */
		else
			text_join(e->t, y1 - 1);	/* drop the last line */
	}
	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
}

/* Store bytes (ownership transferred) into the current register: the unnamed
 * register when none is armed, else the register named by e->vi_reg, with an
 * uppercase name appending rather than replacing. The unnamed register always
 * mirrors the stored text, and the one-shot register selection is released. */
static void
vi_reg_store(struct editor *e, char *bytes, size_t len, int linewise)
{
	char reg = e->vi_reg;

	if (reg >= 'a' && reg <= 'z') {
		struct vi_reg *r = &e->vi_regs[reg - 'a'];
		char *dup = malloc(len ? len : 1);

		if (dup) {
			memcpy(dup, bytes, len);
			free(r->bytes);
			r->bytes = dup;
			r->len = len;
			r->linewise = linewise;
		}
	} else if (reg >= 'A' && reg <= 'Z') {
		struct vi_reg *r = &e->vi_regs[reg - 'A'];
		size_t nl = r->len + len;
		char *cat = malloc(nl ? nl : 1);

		if (cat) {
			if (r->len)
				memcpy(cat, r->bytes, r->len);
			memcpy(cat + r->len, bytes, len);
			free(r->bytes);
			r->bytes = cat;
			r->len = nl;
			r->linewise = r->linewise || linewise;
			free(bytes);			/* mirror the whole reg */
			bytes = malloc(nl ? nl : 1);
			len = bytes ? nl : 0;
			if (bytes)
				memcpy(bytes, cat, nl);
			linewise = r->linewise;
		}
	}
	clip_set(e, bytes, len);		/* unnamed register */
	e->clip_linewise = linewise;
	e->vi_reg = 0;				/* consume the selection */
}

/* Resolve the register to read for a put: the named register selected by
 * e->vi_reg, or the unnamed clip. The returned bytes are owned by the editor
 * and must not be freed by the caller. */
static void
vi_reg_get(struct editor *e, const char **bytes, size_t *len, int *linewise)
{
	char reg = e->vi_reg;

	if (reg >= 'A' && reg <= 'Z')
		reg += 'a' - 'A';
	if (reg >= 'a' && reg <= 'z') {
		struct vi_reg *r = &e->vi_regs[reg - 'a'];

		*bytes = r->bytes;
		*len = r->len;
		*linewise = r->linewise;
	} else {
		*bytes = e->clip;
		*len = e->clip_len;
		*linewise = e->clip_linewise;
	}
}

/* Yank a charwise span into the register. */
static void
vi_yank_region(struct editor *e, size_t sy, size_t sx, size_t ey, size_t ex)
{
	size_t rl = 0;
	char *r = region_text(e, sy, sx, ey, ex, &rl);

	if (r)
		vi_reg_store(e, r, rl, 0);
}

/* Yank whole lines [y1,y2] into the register, with a trailing newline so a
 * later put reproduces them as lines. */
static void
vi_yank_lines(struct editor *e, size_t y1, size_t y2)
{
	size_t rl = 0, lastlen = text_line_len(e->t, y2);
	char *r = region_text(e, y1, 0, y2, lastlen, &rl);
	char *r2;

	if (!r)
		return;
	r2 = realloc(r, rl + 1);
	if (r2) {
		r2[rl] = '\n';
		vi_reg_store(e, r2, rl + 1, 1);
	} else {
		vi_reg_store(e, r, rl, 1);
	}
}

static void
enter_insert(struct editor *e)
{
	e->mode = MODE_INSERT;
}

static void vi_shift_lines(struct editor *e, size_t y1, size_t y2, int dir);

/* Apply operator op linewise over lines [lo,hi]. The caller has already opened
 * the undo group; this closes it (except for a change, which stays open until
 * the insert Esc). */
static enum req
vi_op_lines(struct editor *e, char op, size_t lo, size_t hi)
{
	vi_yank_lines(e, lo, hi);
	if (op == 'y') {
		e->cy = lo;
		e->cx = first_nonblank(e, lo);
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_delete_lines(e, lo, hi);
	if (op == 'c') {
		if (lo >= text_lines(e->t)) {
			size_t last = text_lines(e->t) - 1;

			e->cy = last;
			e->cx = text_line_len(e->t, last);
			do_newline(e);
		} else {
			text_split(e->t, lo, 0);
			e->cy = lo;
			e->cx = 0;
		}
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	e->cy = lo;
	e->cx = first_nonblank(e, lo);
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Apply an armed operator (d/c/y, or the > / < shifts) over a resolved
 * motion. */
static enum req
vi_apply_operator(struct editor *e, char op, struct vi_mot m)
{
	size_t sy, sx, ey, ex;

	/* Shift operators act on whole lines and neither yank nor delete. */
	if (op == '>' || op == '<') {
		size_t lo = e->cy < m.y ? e->cy : m.y;
		size_t hi = e->cy < m.y ? m.y : e->cy;

		vi_shift_lines(e, lo, hi, op == '>' ? 1 : -1);
		return REQ_CONTINUE;
	}

	/* One undo step per operator. A change (c) keeps the group open so
	 * the text typed afterward undoes together with the deletion; the
	 * insert-mode Esc closes it. */
	text_undo_group_begin(e->t);

	if (m.line) {
		size_t lo = e->cy < m.y ? e->cy : m.y;
		size_t hi = e->cy < m.y ? m.y : e->cy;

		return vi_op_lines(e, op, lo, hi);
	}

	if (e->cy < m.y || (e->cy == m.y && e->cx <= m.x)) {
		sy = e->cy;
		sx = e->cx;
		ey = m.y;
		ex = m.x;
	} else {
		sy = m.y;
		sx = m.x;
		ey = e->cy;
		ex = e->cx;
	}
	if (m.incl) {
		size_t elen = 0;
		const char *es = text_line(e->t, ey, &elen);

		if (ex < elen)
			ex += rune_len_at(es, elen, ex);
	} else if (ey > sy && ex == 0) {
		/* Exclusive-motion special case (d}, d{): an end in column 0 of
		 * a lower line pulls back to the close of the previous line, and
		 * becomes linewise when the start is at or before its first
		 * non-blank. */
		ey--;
		ex = text_line_len(e->t, ey);
		if (sx <= first_nonblank(e, sy))
			return vi_op_lines(e, op, sy, ey);
	}
	if (sy == ey && sx == ex) {		/* empty span */
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_yank_region(e, sy, sx, ey, ex);
	if (op == 'y') {
		e->cy = sy;
		e->cx = sx;
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	delete_region(e, sy, sx, ey, ex);
	if (op == 'c') {
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Put the register after (or before) the cursor: linewise as new lines,
 * charwise inline. */
static void
vi_put(struct editor *e, int after)
{
	const char *clip;
	size_t clip_len;
	int linewise;

	vi_reg_get(e, &clip, &clip_len, &linewise);
	if (!clip || clip_len == 0) {
		snprintf(e->status, sizeof(e->status), "clipboard is empty");
		e->vi_reg = 0;
		return;
	}
	text_undo_group_begin(e->t);		/* the whole put is one undo */

	if (linewise) {
		size_t l = clip_len, i = 0, first;
		int top_before = (!after && e->cy == 0);

		if (l && clip[l - 1] == '\n')
			l--;
		if (after) {
			e->cx = text_line_len(e->t, e->cy);
			first = e->cy + 1;
		} else if (top_before) {
			e->cy = 0;
			e->cx = 0;
			first = 0;
		} else {
			e->cy -= 1;
			e->cx = text_line_len(e->t, e->cy);
			first = e->cy + 1;
		}
		for (;;) {
			size_t j = i;

			while (j < l && clip[j] != '\n')
				j++;
			if (top_before) {
				if (j > i)
					do_insert(e, clip + i, j - i);
				do_newline(e);
			} else {
				do_newline(e);
				if (j > i)
					do_insert(e, clip + i, j - i);
			}
			if (j >= l)
				break;
			i = j + 1;
		}
		e->cy = first;
		e->cx = first_nonblank(e, first);
		vi_clamp(e);
	} else {
		if (after) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			if (e->cx < len)
				e->cx += rune_len_at(s, len, e->cx);
		}
		insert_bytes(e, clip, clip_len);
		if (e->cx > 0) {		/* rest on the last pasted rune */
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx -= prev_rune_len(s, e->cx);
		}
		vi_clamp(e);
	}
	text_undo_group_end(e->t);
	e->vi_reg = 0;				/* consume the selection */
}

/* Delete count runes at the cursor (the vi 'x' command). */
static void
vi_delete_char(struct editor *e, int count)
{
	size_t len = 0, start = e->cx, x = e->cx, rl = 0;
	const char *s = text_line(e->t, e->cy, &len);
	char *r;
	int i;

	if (count < 1)
		count = 1;
	if (start >= len)
		return;
	for (i = 0; i < count && x < len; i++)
		x += rune_len_at(s, len, x);
	r = region_text(e, e->cy, start, e->cy, x, &rl);
	if (r)
		vi_reg_store(e, r, rl, 0);
	text_undo_boundary(e->t);
	hl_touch(e, e->cy);
	text_delete(e->t, e->cy, start, x - start);
	vi_clamp(e);
	text_undo_boundary(e->t);
}

/* Enter insert mode at the point implied by an insert-entry command. */
static void
vi_enter_insert_cmd(struct editor *e, uint32_t c)
{
	size_t len = 0;
	const char *s;

	/* One undo step for the whole insert session; Esc closes the group. */
	text_undo_group_begin(e->t);
	switch (c) {
	case 'i':
		break;
	case 'a':
		s = text_line(e->t, e->cy, &len);
		if (e->cx < len)
			e->cx += rune_len_at(s, len, e->cx);
		break;
	case 'A':
		e->cx = text_line_len(e->t, e->cy);
		break;
	case 'I':
		e->cx = first_nonblank(e, e->cy);
		break;
	case 'o':
		e->cx = text_line_len(e->t, e->cy);
		do_newline(e);
		break;
	case 'O':
		e->cx = 0;
		hl_touch(e, e->cy);
		text_split(e->t, e->cy, 0);	/* empty line; content moves down */
		break;
	}
	enter_insert(e);
}

/* Move the cursor by a signed number of lines, for the scroll keys. */
static void
vi_move_lines(struct editor *e, int delta)
{
	if (delta < 0) {
		size_t d = (size_t)(-delta);

		e->cy = e->cy > d ? e->cy - d : 0;
	} else {
		e->cy += (size_t)delta;
		if (e->cy >= text_lines(e->t))
			e->cy = text_lines(e->t) - 1;
	}
	vi_clamp(e);
}

/* Shift lines [y1,y2] one indent level: dir > 0 prepends a tab, dir < 0 drops
 * a leading tab or up to TAB_WIDTH leading spaces. Blank lines are left alone.
 * The cursor rests on the first non-blank of the first shifted line. */
static void
vi_shift_lines(struct editor *e, size_t y1, size_t y2, int dir)
{
	size_t y;

	if (y2 < y1) {
		size_t tmp = y1;

		y1 = y2;
		y2 = tmp;
	}
	if (y2 >= text_lines(e->t))
		y2 = text_lines(e->t) - 1;

	text_undo_group_begin(e->t);
	for (y = y1; y <= y2; y++) {
		size_t len = 0;
		const char *s = text_line(e->t, y, &len);

		if (len == 0)			/* leave blank lines unindented */
			continue;
		if (dir > 0) {
			text_insert(e->t, y, 0, "\t", 1);
		} else if (s[0] == '\t') {
			text_delete(e->t, y, 0, 1);
		} else {
			size_t sp = 0;

			while (sp < len && sp < TAB_WIDTH && s[sp] == ' ')
				sp++;
			if (sp)
				text_delete(e->t, y, 0, sp);
		}
	}
	e->cy = y1;
	e->cx = first_nonblank(e, y1);
	hl_touch(e, y1);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Carry out a motion character: move the cursor, or, when an operator is
 * armed, apply it over the motion's span. */
static enum req
vi_do_motion(struct editor *e, uint32_t motchar)
{
	int mot_have = e->vi_count > 0;
	int mot_count = mot_have ? e->vi_count : 1;
	int have, count;
	struct vi_mot m;

	if (e->vi_op) {
		int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;

		count = oc * mot_count;
		have = e->vi_op_count > 0 || mot_have;
	} else {
		count = mot_count;
		have = mot_have;
	}

	m = vi_motion(e, motchar, count, have);
	if (!m.valid) {
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	if (e->vi_op) {
		char op = e->vi_op;

		/* cw/cW change to the end of the word, like ce/cE */
		if (op == 'c' && (motchar == 'w' || motchar == 'W'))
			m = vi_motion(e, motchar == 'w' ? 'e' : 'E', count,
			    have);
		/* dw/yw stop at the end of the line rather than joining */
		else if ((motchar == 'w' || motchar == 'W') && m.y != e->cy) {
			m.y = e->cy;
			m.x = text_line_len(e->t, e->cy);
		}
		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}

	if (motchar == 'j' || motchar == 'k') {
		/* j/k aim for the display column of the run's first line, so
		 * passing through short lines does not lose the column. */
		if (!e->vi_vert_prev) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->vi_want_col = s ? disp_cols(s, e->cx) : 0;
		}
		e->cy = m.y;
		e->cx = vi_col_to_byte(e, e->cy, e->vi_want_col);
		vi_clamp(e);
		e->vi_vert_run = 1;
	} else if (m.line) {
		e->cy = m.y;
		if (motchar == 'G' || motchar == 'g' || motchar == 'H' ||
		    motchar == 'M' || motchar == 'L' || motchar == '%')
			e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
	} else {
		e->cy = m.y;
		e->cx = m.x;
		vi_clamp(e);
		if (motchar == '$') {		/* stick to end of line under j/k */
			e->vi_want_col = INT_MAX;
			e->vi_vert_run = 1;
		}
	}
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Carry out an f/F/t/T (or a ; / , repeat) search, moving the cursor or, when
 * an operator is armed, applying it over the span. repeat is set for ; and ,
 * so the remembered search is not overwritten. */
static enum req
vi_do_charsearch(struct editor *e, char cmd, uint32_t target, int repeat)
{
	int mot_count = e->vi_count > 0 ? e->vi_count : 1;
	int count;
	struct vi_mot m;

	if (!repeat) {
		e->vi_last_fT = cmd;
		e->vi_last_fT_ch = target;
	}
	if (e->vi_op) {
		int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;

		count = oc * mot_count;
	} else {
		count = mot_count;
	}

	m = vi_charsearch_motion(e, cmd, target, count, repeat);
	if (!m.valid) {
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
	if (e->vi_op) {
		char op = e->vi_op;

		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}
	e->cy = m.y;
	e->cx = m.x;
	vi_clamp(e);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Text objects: iw/aw, i(/a( and friends, i"/a" and friends. Each resolves to
 * a charwise span [sy,sx)..(ey,ex) with an exclusive end, which an operator
 * (diw, ci() or visual mode (viw) then acts on. */

/* Word object on the current line. The run of same-class runes under the
 * cursor for 'i'; 'a' extends by trailing whitespace, or leading whitespace
 * when there is none, or (on whitespace) the following word. big picks WORD
 * class (whitespace-delimited) over word class. */
static int
vi_word_object(struct editor *e, char kind, int big, size_t *sx, size_t *ex)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t cx, start, end;
	uint32_t r;
	int cls;

	if (!s || len == 0)
		return 0;
	cx = e->cx;
	if (cx >= len)
		cx = len - prev_rune_len(s, len);	/* last rune */
	vi_rune(e, e->cy, cx, &r);
	cls = vi_class(r, big);

	start = cx;
	while (start > 0) {				/* back over same class */
		size_t pl = prev_rune_len(s, start);
		uint32_t pr;

		vi_rune(e, e->cy, start - pl, &pr);
		if (vi_class(pr, big) != cls)
			break;
		start -= pl;
	}
	end = cx;
	while (end < len) {				/* forward over same class */
		uint32_t nr;
		int nl = vi_rune(e, e->cy, end, &nr);

		if (vi_class(nr, big) != cls)
			break;
		end += (size_t)nl;
	}

	if (kind == 'a' && cls != 0) {			/* word + trailing ws */
		size_t e2 = end;

		while (e2 < len) {
			uint32_t nr;
			int nl = vi_rune(e, e->cy, e2, &nr);

			if (vi_class(nr, big) != 0)
				break;
			e2 += (size_t)nl;
		}
		if (e2 > end) {
			end = e2;
		} else {				/* none: leading ws */
			while (start > 0) {
				size_t pl = prev_rune_len(s, start);
				uint32_t pr;

				vi_rune(e, e->cy, start - pl, &pr);
				if (vi_class(pr, big) != 0)
					break;
				start -= pl;
			}
		}
	} else if (kind == 'a') {			/* ws + following word */
		while (end < len) {
			uint32_t nr;
			int nl = vi_rune(e, e->cy, end, &nr);

			if (vi_class(nr, big) == 0)
				break;
			end += (size_t)nl;
		}
	}

	*sx = start;
	*ex = end;
	return 1;
}

/* Bracket object: find the pair of open/close brackets enclosing the cursor,
 * counting nesting across lines. 'i' spans inside them, 'a' includes them. */
static int
vi_bracket_object(struct editor *e, char kind, uint32_t open, uint32_t close,
    size_t *sy, size_t *sx, size_t *ey, size_t *ex)
{
	size_t oy = e->cy, ox = e->cx, ny, nx;
	uint32_t r;
	int depth, ol, cl;

	depth = 0;					/* enclosing open, back */
	for (;;) {
		if (vi_rune(e, oy, ox, &r) > 0) {
			if (r == close && !(oy == e->cy && ox == e->cx))
				depth++;
			else if (r == open) {
				if (depth == 0)
					break;
				depth--;
			}
		}
		if (!vi_step_back(e, &oy, &ox))
			return 0;
	}

	ny = oy;					/* matching close, fwd */
	nx = ox;
	depth = 0;
	for (;;) {
		if (vi_rune(e, ny, nx, &r) > 0) {
			if (r == open)
				depth++;
			else if (r == close && --depth == 0)
				break;
		}
		if (!vi_step_fwd(e, &ny, &nx))
			return 0;
	}

	{ uint32_t rr; ol = vi_rune(e, oy, ox, &rr); cl = vi_rune(e, ny, nx, &rr); }
	if (ol <= 0)
		ol = 1;
	if (cl <= 0)
		cl = 1;
	if (kind == 'a') {
		*sy = oy;
		*sx = ox;
		*ey = ny;
		*ex = nx + (size_t)cl;
	} else {
		*sy = oy;
		*sx = ox + (size_t)ol;
		*ey = ny;
		*ex = nx;
	}
	return 1;
}

/* Quote object on the current line. Quotes pair left to right; the chosen pair
 * is the first whose closing quote is at or after the cursor. 'i' spans
 * between the quotes, 'a' includes them. */
static int
vi_quote_object(struct editor *e, char kind, uint32_t q, size_t *sx,
    size_t *ex)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = 0, open_pos = 0;
	int have_open = 0;

	if (!s)
		return 0;
	while (x < len) {
		uint32_t r;
		int n = vi_rune(e, e->cy, x, &r);

		if (n <= 0)
			n = 1;
		if (r == q) {
			if (!have_open) {
				open_pos = x;
				have_open = 1;
			} else {
				if (e->cx <= x) {
					if (kind == 'a') {
						*sx = open_pos;
						*ex = x + (size_t)n;
					} else {
						*sx = open_pos + 1;
						*ex = x;
					}
					return 1;
				}
				have_open = 0;
			}
		}
		x += (size_t)n;
	}
	return 0;
}

/* Resolve a text object named by obj under the cursor into a charwise span.
 * Returns 1 on success. Word and quote objects are line-local; bracket objects
 * may span lines. */
static int
vi_text_object(struct editor *e, char kind, uint32_t obj, size_t *sy,
    size_t *sx, size_t *ey, size_t *ex)
{
	*sy = *ey = e->cy;
	switch (obj) {
	case 'w':
		return vi_word_object(e, kind, 0, sx, ex);
	case 'W':
		return vi_word_object(e, kind, 1, sx, ex);
	case '(':
	case ')':
	case 'b':
		return vi_bracket_object(e, kind, '(', ')', sy, sx, ey, ex);
	case '{':
	case '}':
	case 'B':
		return vi_bracket_object(e, kind, '{', '}', sy, sx, ey, ex);
	case '[':
	case ']':
		return vi_bracket_object(e, kind, '[', ']', sy, sx, ey, ex);
	case '<':
	case '>':
		return vi_bracket_object(e, kind, '<', '>', sy, sx, ey, ex);
	case '"':
		return vi_quote_object(e, kind, '"', sx, ex);
	case '\'':
		return vi_quote_object(e, kind, '\'', sx, ex);
	case '`':
		return vi_quote_object(e, kind, '`', sx, ex);
	default:
		return 0;
	}
}

/* Apply an armed operator (d/c/y) over a resolved text-object span. Mirrors
 * the charwise branch of vi_apply_operator, but the span is explicit rather
 * than cursor-to-motion. */
static enum req
vi_apply_textobject_op(struct editor *e, char op, size_t sy, size_t sx,
    size_t ey, size_t ex)
{
	if (op == '>' || op == '<') {		/* shift the object's lines */
		vi_shift_lines(e, sy, ey, op == '>' ? 1 : -1);
		return REQ_CONTINUE;
	}
	text_undo_group_begin(e->t);
	if (sy == ey && sx == ex) {		/* empty object: nothing to do */
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	vi_yank_region(e, sy, sx, ey, ex);
	if (op == 'y') {
		e->cy = sy;
		e->cx = sx;
		vi_clamp(e);
		text_undo_group_end(e->t);
		return REQ_CONTINUE;
	}
	delete_region(e, sy, sx, ey, ex);
	if (op == 'c') {
		enter_insert(e);
		return REQ_CONTINUE;		/* group stays open until Esc */
	}
	vi_clamp(e);
	text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/* Set or jump to a mark. cmd is 'm' (set), '`' (jump to the exact spot), or
 * '\'' (jump to the first non-blank of the mark's line); idx is 0..25 for
 * 'a'..'z'. A jump with an operator armed applies it over the span: charwise
 * and exclusive for '`', linewise for '\''. Marks hold absolute positions and
 * do not shift as the buffer is edited. */
static enum req
vi_do_mark(struct editor *e, char cmd, int idx)
{
	size_t y, x;
	char op;

	if (cmd == 'm') {
		e->vi_mark_y[idx] = e->cy;
		e->vi_mark_x[idx] = e->cx;
		e->vi_marks_set |= (uint32_t)1 << idx;
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
	if (!(e->vi_marks_set & ((uint32_t)1 << idx))) {
		snprintf(e->status, sizeof(e->status), "E20: mark not set");
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
	y = e->vi_mark_y[idx];
	if (y >= text_lines(e->t))
		y = text_lines(e->t) - 1;
	x = (cmd == '`') ? e->vi_mark_x[idx] : first_nonblank(e, y);

	op = e->vi_op;
	if (op) {
		struct vi_mot m = { y, x, 0, 0, 1 };

		if (cmd == '\'')
			m.line = 1;		/* '<mark> is linewise */
		vi_reset_pending(e);
		return vi_apply_operator(e, op, m);
	}
	e->cy = y;
	e->cx = x;
	vi_clamp(e);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* Toggle the case of count runes from the cursor, advancing past each. Only
 * ASCII letters flip; other runes are stepped over unchanged (the vi '~'). */
static void
vi_toggle_case(struct editor *e, int count)
{
	int i;

	if (count < 1)
		count = 1;
	text_undo_group_begin(e->t);
	for (i = 0; i < count; i++) {
		size_t len = 0;
		const char *s = text_line(e->t, e->cy, &len);
		uint32_t r;
		int n;

		if (!s || e->cx >= len)
			break;
		n = vi_rune(e, e->cy, e->cx, &r);
		if (n == 1 && ((r >= 'a' && r <= 'z') ||
		    (r >= 'A' && r <= 'Z'))) {
			char t = (char)(r ^ 0x20);

			text_delete(e->t, e->cy, e->cx, 1);
			text_insert(e->t, e->cy, e->cx, &t, 1);
		}
		e->cx += (size_t)(n > 0 ? n : 1);
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Join the current line with the ones below (the vi 'J'). count is the number
 * of lines involved, so it performs count-1 joins (a bare J joins one pair).
 * The newline goes away and a single space replaces the next line's leading
 * blanks, unless the current line is empty or already ends in whitespace. The
 * cursor rests at the join. */
static void
vi_join_lines(struct editor *e, int count)
{
	int joins = count > 1 ? count - 1 : 1;
	int i;

	text_undo_group_begin(e->t);
	for (i = 0; i < joins; i++) {
		size_t curlen, nlen = 0, lead = 0, joinpos;
		const char *ns;

		if (e->cy + 1 >= text_lines(e->t))
			break;
		ns = text_line(e->t, e->cy + 1, &nlen);
		while (lead < nlen && (ns[lead] == ' ' || ns[lead] == '\t'))
			lead++;
		if (lead)
			text_delete(e->t, e->cy + 1, 0, lead);
		curlen = text_line_len(e->t, e->cy);
		joinpos = curlen;
		if (curlen > 0) {
			size_t cl = 0;
			const char *cs = text_line(e->t, e->cy, &cl);

			if (cs[cl - 1] != ' ' && cs[cl - 1] != '\t')
				text_insert(e->t, e->cy, curlen, " ", 1);
		}
		text_join(e->t, e->cy);		/* pull the next line up */
		e->cx = joinpos;
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
}

/* Change ('c') or delete ('d') the charwise span from the cursor to (ey,ex),
 * end exclusive. A change enters insert even when the span is empty, so C and
 * s at the end of a line still open for typing. */
static enum req
vi_edit_span(struct editor *e, char op, size_t ey, size_t ex)
{
	struct vi_mot m = { ey, ex, 0, 0, 1 };

	if (op == 'c' && e->cy == ey && e->cx == ex) {
		text_undo_group_begin(e->t);	/* closed by the insert Esc */
		enter_insert(e);
		return REQ_CONTINUE;
	}
	return vi_apply_operator(e, op, m);
}

/* Replace count runes at the cursor with the character ch (the vi 'r'). A
 * newline replaces them with a line break. Nothing happens when the line does
 * not hold count runes from the cursor, matching vi. The cursor rests on the
 * last replaced rune. */
static enum req
vi_do_replace(struct editor *e, uint32_t ch)
{
	int cnt = e->vi_count > 0 ? e->vi_count : 1;
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = e->cx, probe = e->cx;
	int avail = 0, i;

	while (probe < len) {			/* runes from cursor to EOL */
		probe += rune_len_at(s, len, probe);
		avail++;
	}
	if (cnt > avail) {			/* not enough on the line */
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	text_undo_group_begin(e->t);
	if (ch == '\n') {			/* r<CR>: drop the runes, break */
		size_t end = x;

		for (i = 0; i < cnt; i++)
			end += rune_len_at(s, len, end);
		text_delete(e->t, e->cy, x, end - x);
		text_split(e->t, e->cy, x);
		e->cy++;
		e->cx = 0;
	} else {
		unsigned char buf[8];
		int bn = utf8_encode(buf, ch);

		if (bn <= 0) {
			buf[0] = (unsigned char)ch;
			bn = 1;
		}
		for (i = 0; i < cnt; i++) {
			size_t rl;

			s = text_line(e->t, e->cy, &len);
			rl = rune_len_at(s, len, x);
			text_delete(e->t, e->cy, x, rl);
			text_insert(e->t, e->cy, x, (char *)buf, (size_t)bn);
			x += (size_t)bn;
		}
		e->cx = x - (size_t)bn;		/* rest on the last one */
	}
	hl_touch(e, e->cy);
	vi_clamp(e);
	text_undo_group_end(e->t);
	vi_reset_pending(e);
	return REQ_CONTINUE;
}

/* ---- search offsets ----
 *
 * A vi search may carry an offset after a second delimiter: "/pat/e" lands
 * on the last character of the match, "/pat/e+2" and "/pat/s-1" shift from
 * the match's end or start by that many characters (within the line), and
 * "/pat/+3" or "/pat/-1" move that many whole lines and rest in column 0.
 * The offset is kept with the search so n and N apply it again.
 *
 * An offset can leave the cursor before the match it came from, where the
 * next n would find the same match forever, so the match start and the
 * offset's landing place are both remembered: a repeat that starts from
 * the landing place searches from the match start instead. */

/* Parse a search offset. Returns 0 and fills kind/n, or -1 when malformed. */
static int
vi_offset_parse(const char *s, int *kind, long *n)
{
	char *end;

	*kind = 0;
	*n = 0;
	if (!s[0])
		return 0;
	if (s[0] == 'e' || s[0] == 's' || s[0] == 'b') {
		*kind = (s[0] == 'e') ? 'e' : 's';
		s++;
		if (!s[0])
			return 0;
		if (s[0] != '+' && s[0] != '-')
			return -1;
		if (!s[1]) {			/* a bare sign means one */
			*n = (s[0] == '-') ? -1 : 1;
			return 0;
		}
	} else {
		*kind = 'l';
		if (s[0] != '+' && s[0] != '-' && !(s[0] >= '0' && s[0] <= '9'))
			return -1;
		if ((s[0] == '+' || s[0] == '-') && !s[1]) {
			*n = (s[0] == '-') ? -1 : 1;
			return 0;
		}
	}
	*n = strtol(s, &end, 10);
	return *end ? -1 : 0;
}

/* Move cx by n characters along line y, stopping at either end. */
static size_t
vi_shift_chars(struct editor *e, size_t y, size_t cx, long n)
{
	size_t len = 0;
	const char *s = text_line(e->t, y, &len);

	if (!s)
		return 0;
	while (n > 0 && cx < len) {
		cx += rune_len_at(s, len, cx);
		n--;
	}
	while (n < 0 && cx > 0) {
		cx -= prev_rune_len(s, cx);
		n++;
	}
	return cx;
}

/* Apply the remembered offset to a match of q starting at the cursor. */
static void
vi_offset_apply(struct editor *e, const char *q)
{
	size_t nlines = text_lines(e->t);

	switch (e->vi_off_kind) {
	case 'l':
		if (e->vi_off_n < 0 && (size_t)-e->vi_off_n > e->cy)
			e->cy = 0;
		else if (e->vi_off_n > 0 &&
		    (size_t)e->vi_off_n >= nlines - e->cy)
			e->cy = nlines - 1;
		else
			e->cy = (size_t)((long)e->cy + e->vi_off_n);
		e->cx = 0;
		break;
	case 'e': {
		size_t len = 0;
		const char *s = text_line(e->t, e->cy, &len);
		size_t end = e->cx + strlen(q);

		if (s && end > e->cx)
			e->cx = end - prev_rune_len(s, end);
		e->cx = vi_shift_chars(e, e->cy, e->cx, e->vi_off_n);
		break;
	}
	case 's':
		e->cx = vi_shift_chars(e, e->cy, e->cx, e->vi_off_n);
		break;
	}
}

/* Search for q in direction dir and apply the current offset. */
void
vi_search_run(struct editor *e, const char *q, int dir)
{
	/* repeating from where the offset left us: resume from the match */
	if (e->vi_match_valid && e->cy == e->vi_placed_cy &&
	    e->cx == e->vi_placed_cx) {
		e->cy = e->vi_match_cy;
		e->cx = e->vi_match_cx;
	}
	if (!do_find_dir(e, q, dir)) {
		e->vi_match_valid = 0;
		vi_clamp(e);
		return;
	}
	e->vi_match_cy = e->cy;
	e->vi_match_cx = e->cx;
	vi_offset_apply(e, q);
	vi_clamp(e);
	e->vi_placed_cy = e->cy;
	e->vi_placed_cx = e->cx;
	e->vi_match_valid = 1;
}

/* Run a typed search line, "pat", "pat/off", or "/off" (reusing the last
 * pattern), where the delimiter is '/' forward and '?' backward and may be
 * escaped inside the pattern. Returns 0, or -1 for a bad offset. */
int
vi_search_cmd(struct editor *e, const char *typed, int dir)
{
	char delim = dir < 0 ? '?' : '/';
	char pat[256];
	const char *off = "";
	size_t i, o = 0;
	int kind;
	long n;

	for (i = 0; typed[i] && o < sizeof(pat) - 1; i++) {
		if (typed[i] == '\\' && typed[i + 1] == delim) {
			pat[o++] = delim;
			i++;
		} else if (typed[i] == delim) {
			off = typed + i + 1;
			break;
		} else {
			pat[o++] = typed[i];
		}
	}
	pat[o] = '\0';
	if (vi_offset_parse(off, &kind, &n) < 0) {
		snprintf(e->status, sizeof(e->status),
		    "bad search offset: %.40s", off);
		return -1;
	}
	if (pat[0])
		snprintf(e->last_find, sizeof(e->last_find), "%s", pat);
	else if (!e->last_find[0]) {
		snprintf(e->status, sizeof(e->status), "no previous search");
		return -1;
	}
	e->vi_off_kind = kind;
	e->vi_off_n = n;
	e->vi_search_dir = dir;
	vi_search_run(e, e->last_find, dir);
	return 0;
}

/* Search for the word under (or next on the line after) the cursor, in
 * direction dir (the vi '*' and '#'). The word is matched as a plain
 * substring; there are no word boundaries, so it also matches inside longer
 * words. */
static void
vi_search_word(struct editor *e, int dir)
{
	size_t len = 0;
	const char *s = text_line(e->t, e->cy, &len);
	size_t x = e->cx, start, end, wl;
	char word[256];

	if (!s || len == 0) {
		snprintf(e->status, sizeof(e->status), "no word under cursor");
		return;
	}
	while (x < len) {			/* find a word char on the line */
		uint32_t r;
		int n = vi_rune(e, e->cy, x, &r);

		if (vi_class(r, 0) == 1)
			break;
		x += (size_t)(n > 0 ? n : 1);
	}
	if (x >= len) {
		snprintf(e->status, sizeof(e->status), "no word under cursor");
		return;
	}
	start = x;
	while (start > 0) {			/* back to the word start */
		size_t pl = prev_rune_len(s, start);
		uint32_t pr;

		vi_rune(e, e->cy, start - pl, &pr);
		if (vi_class(pr, 0) != 1)
			break;
		start -= pl;
	}
	end = x;
	while (end < len) {			/* out to the word end */
		uint32_t r;
		int n = vi_rune(e, e->cy, end, &r);

		if (vi_class(r, 0) != 1)
			break;
		end += (size_t)(n > 0 ? n : 1);
	}
	wl = end - start;
	if (wl == 0 || wl >= sizeof(word))
		return;
	memcpy(word, s + start, wl);
	word[wl] = '\0';
	snprintf(e->last_find, sizeof(e->last_find), "%s", word);
	e->vi_search_dir = dir;
	e->vi_off_kind = 0;			/* a word search has no offset */
	e->vi_off_n = 0;
	e->vi_match_valid = 0;
	e->cx = start;				/* search from the word start */
	vi_search_run(e, word, dir);
}

static void vi_dot_replay(struct editor *e);

/* Handle one key in vi normal mode. */
static enum req
vi_normal_key(struct editor *e, const struct tkbd_seq *seq)
{
	uint32_t c = seq->ch;
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;
	int page = text_height(e) - 1;
	int reg_fresh = e->vi_reg_fresh;

	if (page < 1)
		page = 1;

	/* Track whether the previous command was a vertical j/k/$ move, so a
	 * run of them keeps aiming at the same display column. Any other
	 * command leaves vi_vert_run clear and ends the run. */
	e->vi_vert_prev = e->vi_vert_run;
	e->vi_vert_run = 0;

	/* Release a register armed with " once the command that might use it
	 * has come and gone. It survives the arming key and any pending state
	 * that keeps a command in flight (a count, an operator, and so on). */
	e->vi_reg_fresh = 0;
	if (!reg_fresh && e->vi_reg && !e->vi_op && e->vi_count == 0 &&
	    !e->vi_gpending && !e->vi_charsearch && !e->vi_textobj &&
	    !e->vi_markcmd && !e->vi_regpending && !e->vi_zpending)
		e->vi_reg = 0;

	/* A pending f/F/t/T takes the next key as its literal target char. */
	if (e->vi_charsearch) {
		char cmd = e->vi_charsearch;

		e->vi_charsearch = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		return vi_do_charsearch(e, cmd, seq->ch, 0);
	}

	/* A pending i/a (after an operator) takes the next key as the object
	 * name: diw, ci(, ya" and so on. */
	if (e->vi_textobj) {
		char kind = e->vi_textobj;
		char op = e->vi_op;
		size_t sy, sx, ey, ex;

		e->vi_textobj = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC) ||
		    !op || !vi_text_object(e, kind, c, &sy, &sx, &ey, &ex)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		vi_reset_pending(e);
		return vi_apply_textobject_op(e, op, sy, sx, ey, ex);
	}

	/* A pending m/`/' takes the next key as the mark letter (a-z). */
	if (e->vi_markcmd) {
		char cmd = e->vi_markcmd;

		e->vi_markcmd = 0;
		if (ctrl || c < 'a' || c > 'z') {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		return vi_do_mark(e, cmd, (int)(c - 'a'));
	}

	/* A pending " takes the next key as the register name (a-z/A-Z). */
	if (e->vi_regpending) {
		e->vi_regpending = 0;
		if (ctrl || !((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z'))) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		e->vi_reg = (char)c;		/* armed for the next command */
		e->vi_reg_fresh = 1;
		return REQ_CONTINUE;
	}

	/* A pending r takes the next key as the replacement character. */
	if (e->vi_rpending) {
		e->vi_rpending = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC)) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		if (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ENTER)
			return vi_do_replace(e, '\n');
		return vi_do_replace(e, seq->ch);
	}

	/* A leading 'Z' expects a second key: ZZ writes and quits, ZQ quits
	 * without writing. */
	if (e->vi_zpending) {
		e->vi_zpending = 0;
		vi_reset_pending(e);
		if (c == 'Z') {			/* write if modified, then quit */
			if (text_dirty(e->t)) {
				if (!e->has_name) {
					snprintf(e->status, sizeof(e->status),
					    "E32: no file name");
					return REQ_CONTINUE;
				}
				if (text_save(e->t, e->path) < 0) {
					snprintf(e->status, sizeof(e->status),
					    "save failed: %s",
					    strerror(errno));
					return REQ_CONTINUE;
				}
			}
			return REQ_FORCE_QUIT;
		}
		if (c == 'Q')			/* quit, discarding changes */
			return REQ_FORCE_QUIT;
		return REQ_CONTINUE;		/* any other key cancels */
	}

	if (ctrl && seq->type == TKBD_KEY) {
		vi_reset_pending(e);
		switch (seq->key) {
		case TKBD_KEY_D:
			vi_move_lines(e, page / 2);
			break;
		case TKBD_KEY_U:
			vi_move_lines(e, -(page / 2));
			break;
		case TKBD_KEY_F:
			vi_move_lines(e, page);
			break;
		case TKBD_KEY_B:
			vi_move_lines(e, -page);
			break;
		case TKBD_KEY_R:
			e->sel_active = 0;
			e->vi_suppress_dot = 1;		/* redo is not a '.' */
			if (text_redo(e->t, &e->cy, &e->cx) != 0)
				snprintf(e->status, sizeof(e->status),
				    "nothing to redo");
			else {
				hl_touch(e, 0);
				vi_clamp(e);
			}
			break;
		default:
			break;
		}
		return REQ_CONTINUE;
	}

	if (seq->type == TKBD_KEY) {
		switch (seq->key) {
		case TKBD_KEY_ESC:
			vi_reset_pending(e);
			return REQ_CONTINUE;
		case TKBD_KEY_F1:
			vi_reset_pending(e);
			return REQ_HELP;
		case TKBD_KEY_LEFT:
			c = 'h';
			break;
		case TKBD_KEY_RIGHT:
			c = 'l';
			break;
		case TKBD_KEY_UP:
			c = 'k';
			break;
		case TKBD_KEY_DOWN:
		case TKBD_KEY_ENTER:
			c = 'j';
			break;
		case TKBD_KEY_HOME:
			c = '0';
			break;
		case TKBD_KEY_END:
			c = '$';
			break;
		case TKBD_KEY_BACKSPACE:
		case TKBD_KEY_BACKSPACE2:
			c = 'h';
			break;
		case TKBD_KEY_DEL:
			c = 'x';
			break;
		case TKBD_KEY_PGUP:
			vi_reset_pending(e);
			vi_move_lines(e, -page);
			return REQ_CONTINUE;
		case TKBD_KEY_PGDN:
			vi_reset_pending(e);
			vi_move_lines(e, page);
			return REQ_CONTINUE;
		default:
			break;
		}
	}

	if (e->vi_gpending) {
		e->vi_gpending = 0;
		if (c == 'g')
			return vi_do_motion(e, 'g');
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}

	if (c >= '1' && c <= '9') {
		e->vi_count = e->vi_count * 10 + (int)(c - '0');
		if (e->vi_count > 1000000)
			e->vi_count = 1000000;
		return REQ_CONTINUE;
	}
	if (c == '0' && e->vi_count > 0) {
		e->vi_count *= 10;
		if (e->vi_count > 1000000)
			e->vi_count = 1000000;
		return REQ_CONTINUE;
	}

	switch (c) {
	case 'h':
	case 'l':
	case 'k':
	case 'j':
	case '0':
	case '^':
	case '$':
	case 'w':
	case 'W':
	case 'b':
	case 'B':
	case 'e':
	case 'E':
	case 'G':
	case '{':
	case '}':
	case '(':
	case ')':
	case '%':
	case 'H':
	case 'M':
	case 'L':
	case '|':
	case ' ':
		return vi_do_motion(e, c);
	case 'f':
	case 'F':
	case 't':
	case 'T':
		e->vi_charsearch = (char)c;	/* wait for the target char */
		return REQ_CONTINUE;
	case ';':
		if (!e->vi_last_fT) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		return vi_do_charsearch(e, e->vi_last_fT, e->vi_last_fT_ch, 1);
	case ',': {
		char rev;

		if (!e->vi_last_fT) {
			vi_reset_pending(e);
			return REQ_CONTINUE;
		}
		switch (e->vi_last_fT) {		/* the opposite direction */
		case 'f': rev = 'F'; break;
		case 'F': rev = 'f'; break;
		case 't': rev = 'T'; break;
		default:  rev = 't'; break;	/* was 'T' */
		}
		return vi_do_charsearch(e, rev, e->vi_last_fT_ch, 1);
	}
	case 'g':
		e->vi_gpending = 1;
		return REQ_CONTINUE;
	case 'Z':
		e->vi_zpending = 1;
		return REQ_CONTINUE;
	case 'm':
	case '`':
	case '\'':
		e->vi_markcmd = (char)c;		/* wait for the letter */
		return REQ_CONTINUE;
	case '"':
		e->vi_regpending = 1;			/* wait for the register */
		return REQ_CONTINUE;
	case 'd':
	case 'c':
	case 'y':
		if (e->vi_op == (char)c) {		/* dd / cc / yy */
			int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;
			int mc = e->vi_count > 0 ? e->vi_count : 1;
			char op = e->vi_op;
			struct vi_mot m = { 0, 0, 1, 0, 1 };

			m.y = e->cy + (size_t)(oc * mc - 1);
			if (m.y >= text_lines(e->t))
				m.y = text_lines(e->t) - 1;
			vi_reset_pending(e);
			return vi_apply_operator(e, op, m);
		}
		e->vi_op = (char)c;
		e->vi_op_count = e->vi_count;
		e->vi_count = 0;
		return REQ_CONTINUE;
	case '>':
	case '<':
		if (e->vi_op == (char)c) {		/* >> / << */
			int oc = e->vi_op_count > 0 ? e->vi_op_count : 1;
			int mc = e->vi_count > 0 ? e->vi_count : 1;
			int dir = (c == '>') ? 1 : -1;
			size_t y2 = e->cy + (size_t)(oc * mc - 1);

			vi_reset_pending(e);
			vi_shift_lines(e, e->cy, y2, dir);
			return REQ_CONTINUE;
		}
		e->vi_op = (char)c;
		e->vi_op_count = e->vi_count;
		e->vi_count = 0;
		return REQ_CONTINUE;
	case 'i':
	case 'a':
		if (e->vi_op) {			/* diw, ci(, ... : await object */
			e->vi_textobj = (char)c;
			return REQ_CONTINUE;
		}
		vi_reset_pending(e);
		vi_enter_insert_cmd(e, c);
		return REQ_CONTINUE;
	case 'A':
	case 'I':
	case 'o':
	case 'O':
		vi_reset_pending(e);
		vi_enter_insert_cmd(e, c);
		return REQ_CONTINUE;
	case 'v':
	case 'V':
		vi_reset_pending(e);
		e->vi_visual = (char)c;
		e->sel_active = 1;
		e->ay = e->cy;			/* anchor the selection here */
		e->ax = e->cx;
		return REQ_CONTINUE;
	case 'x': {
		int count = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_delete_char(e, count);
		return REQ_CONTINUE;
	}
	case 'p':
		vi_reset_pending(e);
		vi_put(e, 1);
		return REQ_CONTINUE;
	case 'P':
		vi_reset_pending(e);
		vi_put(e, 0);
		return REQ_CONTINUE;
	case '.':
		vi_reset_pending(e);
		vi_dot_replay(e);
		return REQ_CONTINUE;
	case 'J': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_join_lines(e, cnt);
		return REQ_CONTINUE;
	}
	case '~': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;

		vi_reset_pending(e);
		vi_toggle_case(e, cnt);
		return REQ_CONTINUE;
	}
	case 'r':
		e->vi_rpending = 1;		/* wait for the new character */
		return REQ_CONTINUE;
	case 'R':
		vi_reset_pending(e);
		e->vi_overtype = 1;		/* Replace mode: typing overwrites */
		vi_enter_insert_cmd(e, 'R');
		return REQ_CONTINUE;
	case 'D':
	case 'C': {
		char op = (c == 'D') ? 'd' : 'c';
		size_t len = text_line_len(e->t, e->cy);

		vi_reset_pending(e);
		return vi_edit_span(e, op, e->cy, len);
	}
	case 's': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;
		size_t len = 0;
		const char *s = text_line(e->t, e->cy, &len);
		size_t x = e->cx;
		int i;

		for (i = 0; i < cnt && x < len; i++)
			x += rune_len_at(s, len, x);
		vi_reset_pending(e);
		return vi_edit_span(e, 'c', e->cy, x);
	}
	case 'S': {
		int cnt = e->vi_count > 0 ? e->vi_count : 1;
		struct vi_mot m = { 0, 0, 1, 0, 1 };

		m.y = e->cy + (size_t)(cnt - 1);
		if (m.y >= text_lines(e->t))
			m.y = text_lines(e->t) - 1;
		vi_reset_pending(e);
		return vi_apply_operator(e, 'c', m);
	}
	case 'u':
		vi_reset_pending(e);
		e->sel_active = 0;
		e->vi_suppress_dot = 1;			/* undo is not a '.' */
		if (text_undo(e->t, &e->cy, &e->cx) != 0)
			snprintf(e->status, sizeof(e->status),
			    "nothing to undo");
		else {
			hl_touch(e, 0);
			vi_clamp(e);
		}
		return REQ_CONTINUE;
	case 'n':
	case 'N': {
		int dir = e->vi_search_dir < 0 ? -1 : 1;

		if (c == 'N')				/* repeat the other way */
			dir = -dir;
		vi_reset_pending(e);
		if (e->last_find[0])
			vi_search_run(e, e->last_find, dir);
		else
			snprintf(e->status, sizeof(e->status),
			    "no previous search");
		return REQ_CONTINUE;
	}
	case '*':
		vi_reset_pending(e);
		vi_search_word(e, 1);
		return REQ_CONTINUE;
	case '#':
		vi_reset_pending(e);
		vi_search_word(e, -1);
		return REQ_CONTINUE;
	case ':':
		vi_reset_pending(e);
		return REQ_VI_COLON;
	case '/':
		vi_reset_pending(e);
		e->vi_search_dir = 1;
		return REQ_VI_SEARCH;
	case '?':
		vi_reset_pending(e);
		e->vi_search_dir = -1;
		return REQ_VI_SEARCH;
	default:
		vi_reset_pending(e);
		return REQ_CONTINUE;
	}
}

/* Handle one key in vi insert mode. Esc returns to normal mode, backing the
 * cursor up one rune the way vi does. */
static enum req
vi_insert_key(struct editor *e, const struct tkbd_seq *seq)
{
	unsigned char buf[8];
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;
	int n;

	if (seq->type != TKBD_KEY)
		return REQ_CONTINUE;

	switch (seq->key) {
	case TKBD_KEY_ESC:
		e->mode = MODE_NORMAL;
		e->vi_overtype = 0;
		text_undo_group_end(e->t);	/* close the insert session */
		if (e->cx > 0) {
			size_t len = 0;
			const char *s = text_line(e->t, e->cy, &len);

			e->cx -= prev_rune_len(s, e->cx);
		}
		vi_clamp(e);
		return REQ_CONTINUE;
	case TKBD_KEY_ENTER:
		do_newline(e);
		return REQ_CONTINUE;
	case TKBD_KEY_TAB:
		do_insert(e, "\t", 1);
		return REQ_CONTINUE;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		do_backspace(e);
		return REQ_CONTINUE;
	case TKBD_KEY_DEL:
		do_delete(e);
		return REQ_CONTINUE;
	case TKBD_KEY_LEFT:
		move_left(e);
		return REQ_CONTINUE;
	case TKBD_KEY_RIGHT:
		move_right(e);
		return REQ_CONTINUE;
	case TKBD_KEY_UP:
		if (e->cy > 0) {
			e->cy--;
			clamp_col(e);
		}
		return REQ_CONTINUE;
	case TKBD_KEY_DOWN:
		if (e->cy + 1 < text_lines(e->t)) {
			e->cy++;
			clamp_col(e);
		}
		return REQ_CONTINUE;
	case TKBD_KEY_HOME:
		e->cx = 0;
		return REQ_CONTINUE;
	case TKBD_KEY_END:
		e->cx = text_line_len(e->t, e->cy);
		return REQ_CONTINUE;
	default:
		break;
	}

	if (!ctrl && seq->ch != TKBD_CH_NONE && seq->ch >= 0x20 &&
	    seq->ch != 0x7f) {
		n = utf8_encode(buf, seq->ch);
		if (n > 0) {
			if (e->vi_overtype) {	/* R mode: overwrite the rune */
				size_t len = 0;
				const char *s = text_line(e->t, e->cy, &len);

				if (e->cx < len)
					text_delete(e->t, e->cy, e->cx,
					    rune_len_at(s, len, e->cx));
			}
			do_insert(e, (char *)buf, (size_t)n);
		}
	}
	return REQ_CONTINUE;
}

/* Leave visual mode, dropping the selection. */
static void
vi_leave_visual(struct editor *e)
{
	e->vi_visual = 0;
	e->sel_active = 0;
	vi_reset_pending(e);
}

/* The motion an operator sees for the current visual selection: the anchor is
 * the far end, the cursor the near end. Charwise is inclusive of both cells;
 * linewise spans whole lines. vi_apply_operator orders the two ends. */
static struct vi_mot
vi_visual_span(struct editor *e)
{
	struct vi_mot m = { e->ay, e->ax, 0, 0, 1 };

	if (e->vi_visual == 'V')
		m.line = 1;
	else
		m.incl = 1;
	return m;
}

/* Handle one key in visual mode. Operators act on the selection and return to
 * normal mode; the visual keys and Esc leave it; everything else (motions,
 * counts, searches) runs through the normal handler and, with no operator
 * pending, just moves the cursor, so the selection tracks it. */
static enum req
vi_visual_key(struct editor *e, const struct tkbd_seq *seq)
{
	uint32_t c = seq->ch;
	int ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;

	/* A pending i/a takes the next key as the object name (viw, va(): the
	 * object becomes the selection, cursor on its last rune. */
	if (e->vi_textobj) {
		char kind = e->vi_textobj;
		size_t sy, sx, ey, ex, y, x;

		e->vi_textobj = 0;
		if (ctrl || seq->ch == TKBD_CH_NONE ||
		    (seq->type == TKBD_KEY && seq->key == TKBD_KEY_ESC) ||
		    !vi_text_object(e, kind, c, &sy, &sx, &ey, &ex) ||
		    (sy == ey && sx == ex))
			return REQ_CONTINUE;
		e->ay = sy;
		e->ax = sx;
		y = ey;
		x = ex;
		if (vi_step_back(e, &y, &x)) {	/* end is exclusive; step in */
			e->cy = y;
			e->cx = x;
		}
		vi_clamp(e);
		return REQ_CONTINUE;
	}

	if (e->vi_charsearch)		/* resolve an f/F/t/T target as a motion */
		return vi_normal_key(e, seq);

	if (seq->type == TKBD_KEY) {
		if (seq->key == TKBD_KEY_ESC) {
			vi_leave_visual(e);
			return REQ_CONTINUE;
		}
		if (seq->key == TKBD_KEY_DEL)
			c = 'x';		/* Delete removes the selection */
	}

	if (ctrl && seq->type == TKBD_KEY) {
		if (seq->key == TKBD_KEY_R)	/* no redo while selecting */
			return REQ_CONTINUE;
		return vi_normal_key(e, seq);	/* Ctrl-D/U/F/B scroll */
	}

	switch (c) {
	case 'v':
	case 'V':
		if (e->vi_visual == (char)c)
			vi_leave_visual(e);	/* same key toggles off */
		else
			e->vi_visual = (char)c;	/* switch charwise <-> linewise */
		return REQ_CONTINUE;
	case 'o':
	case 'O': {			/* jump to the other end of the selection */
		size_t ty = e->cy, tx = e->cx;

		e->cy = e->ay;
		e->cx = e->ax;
		e->ay = ty;
		e->ax = tx;
		vi_clamp(e);
		return REQ_CONTINUE;
	}
	case 'd':
	case 'x':
	case 'y': {
		struct vi_mot m = vi_visual_span(e);
		char op = (c == 'y') ? 'y' : 'd';

		vi_reset_pending(e);
		vi_apply_operator(e, op, m);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	case 'c':
	case 's': {
		struct vi_mot m = vi_visual_span(e);

		vi_reset_pending(e);
		vi_apply_operator(e, 'c', m);	/* deletes, then enters INSERT */
		e->vi_visual = 0;
		e->sel_active = 0;
		return REQ_CONTINUE;
	}
	case 'p':
	case 'P': {			/* replace the selection with the register */
		struct vi_mot m = vi_visual_span(e);
		char *reg = NULL;
		size_t reglen = e->clip_len;
		int reglw = e->clip_linewise;

		if (e->clip && reglen > 0) {
			reg = malloc(reglen);
			if (reg)
				memcpy(reg, e->clip, reglen);
		}
		vi_reset_pending(e);
		/* One undo step for the delete and the put together. Deleting
		 * fills the register with the removed text, so put back the
		 * saved register before pasting it in. */
		text_undo_group_begin(e->t);
		vi_apply_operator(e, 'd', m);
		if (reg) {
			clip_set(e, reg, reglen);	/* takes ownership */
			e->clip_linewise = reglw;
			vi_put(e, 0);
		}
		text_undo_group_end(e->t);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	case 'i':
	case 'a':			/* select the text object under cursor */
		e->vi_textobj = (char)c;
		return REQ_CONTINUE;
	case '>':
	case '<': {			/* shift the selected lines one level */
		size_t lo = e->cy < e->ay ? e->cy : e->ay;
		size_t hi = e->cy < e->ay ? e->ay : e->cy;

		vi_reset_pending(e);
		vi_shift_lines(e, lo, hi, c == '>' ? 1 : -1);
		vi_leave_visual(e);
		return REQ_CONTINUE;
	}
	/* Editing commands that have no selection form yet must not leak to
	 * the normal handler mid-selection; swallow them. */
	case 'A':
	case 'I':
	case 'u':
	case 'r':
	case 'Z':
	case '~':
	case 'J':
	case 'D':
	case 'C':
	case 'S':
		return REQ_CONTINUE;
	default:
		break;
	}

	/* Anything else is a motion (or count, or search): move the cursor and
	 * let the selection follow. */
	return vi_normal_key(e, seq);
}

/* Route a key to the handler for the current mode. */
static enum req
vi_dispatch_key(struct editor *e, const struct tkbd_seq *seq)
{
	if (e->mode == MODE_INSERT)
		return vi_insert_key(e, seq);
	if (e->vi_visual)
		return vi_visual_key(e, seq);
	return vi_normal_key(e, seq);
}

/* True when no command is in flight: normal mode with nothing pending and no
 * register armed. A command begins and ends at these rest points, which is
 * where the '.' recorder starts a recording and commits it. */
static int
vi_at_rest(const struct editor *e)
{
	return e->mode == MODE_NORMAL && !e->vi_visual && !e->vi_op &&
	    e->vi_count == 0 && !e->vi_gpending && !e->vi_charsearch &&
	    !e->vi_textobj && !e->vi_markcmd && !e->vi_regpending &&
	    !e->vi_zpending && !e->vi_reg_fresh;
}

/* Append one key to a recording log, growing it as needed. */
static void
vi_keylog_push(struct vi_keylog *log, const struct tkbd_seq *seq)
{
	if (log->len >= log->cap) {
		int ncap = log->cap ? log->cap * 2 : 16;
		struct tkbd_seq *nev = realloc(log->ev,
		    (size_t)ncap * sizeof(*nev));

		if (!nev)
			return;			/* drop: the repeat may truncate */
		log->ev = nev;
		log->cap = ncap;
	}
	log->ev[log->len++] = *seq;
}

/* Copy the just-recorded command into the '.' log. */
static void
vi_dot_commit(struct editor *e)
{
	struct vi_keylog *d = &e->vi_dot, *s = &e->vi_rec;

	if (s->len == 0)
		return;
	if (d->cap < s->len) {
		struct tkbd_seq *nev = realloc(d->ev,
		    (size_t)s->len * sizeof(*nev));

		if (!nev)
			return;
		d->ev = nev;
		d->cap = s->len;
	}
	memcpy(d->ev, s->ev, (size_t)s->len * sizeof(*s->ev));
	d->len = s->len;
}

/* Replay the last change recorded for '.'. */
static void
vi_dot_replay(struct editor *e)
{
	int i, n = e->vi_dot.len;

	if (n == 0) {
		snprintf(e->status, sizeof(e->status), "nothing to repeat");
		return;
	}
	e->vi_cmd_open = 0;		/* keep this repeat out of the recording */
	e->vi_replaying = 1;
	for (i = 0; i < n; i++) {
		struct tkbd_seq seq = e->vi_dot.ev[i];

		vi_dispatch(e, &seq);
	}
	e->vi_replaying = 0;
}

enum req
vi_dispatch(struct editor *e, const struct tkbd_seq *seq)
{
	int at_rest_before;
	enum req r;

	if (e->vi_replaying)		/* a '.' replay records nothing */
		return vi_dispatch_key(e, seq);

	at_rest_before = vi_at_rest(e);
	if (at_rest_before) {		/* a fresh command starts here */
		e->vi_rec.len = 0;
		e->vi_cmd_open = 1;
		e->vi_suppress_dot = 0;
		e->vi_cmd_rev = text_revision(e->t);
	}
	if (e->vi_cmd_open)
		vi_keylog_push(&e->vi_rec, seq);

	r = vi_dispatch_key(e, seq);

	/* Back at rest: if the buffer changed and the command is repeatable,
	 * it becomes the new '.'. */
	if (e->vi_cmd_open && vi_at_rest(e)) {
		e->vi_cmd_open = 0;
		if (!e->vi_suppress_dot &&
		    text_revision(e->t) != e->vi_cmd_rev)
			vi_dot_commit(e);
	}
	return r;
}

/* True when p equals any of the NULL-terminated list of names. */
static int
ex_match(const char *p, const char *const *names)
{
	for (; *names; names++)
		if (strcmp(p, *names) == 0)
			return 1;
	return 0;
}

/* A delimiter is a printable non-space, non-alphanumeric character, so that a
 * word command like ':syntax' is not mistaken for ':s/.../'. */
static int
is_ex_delim(char d)
{
	if (d == '\0' || d == ' ')
		return 0;
	return !((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
	    (d >= '0' && d <= '9'));
}

/* A ':s' is a substitute only when the character after the s is a delimiter. */
static int
ex_is_subst(const char *cmd)
{
	return is_ex_delim(cmd[1]);
}

/* A ':g' or ':v' (or ':g!') is global only when a delimiter follows. */
static int
ex_is_global(const char *cmd)
{
	const char *p = cmd + 1;

	if (*cmd == 'g' && *p == '!')
		p++;
	return is_ex_delim(*p);
}

/* Replace occurrences of pat with rep on line y: the first only, or every one
 * when global. Returns the number of substitutions made. The search is a plain
 * byte substring, with no regex or backreferences. */
static int
ex_subst_line(struct editor *e, size_t y, const char *pat, const char *rep,
    int global)
{
	size_t plen = strlen(pat), rlen = strlen(rep), len = 0;
	const char *s = text_line(e->t, y, &len);
	size_t i, o, outlen = 0, matches = 0;
	char *out;

	if (!s || plen == 0)
		return 0;
	for (i = 0; i < len; ) {			/* size the result */
		if (i + plen <= len && memcmp(s + i, pat, plen) == 0 &&
		    (matches == 0 || global)) {
			outlen += rlen;
			i += plen;
			matches++;
		} else {
			outlen++;
			i++;
		}
	}
	if (matches == 0)
		return 0;
	out = malloc(outlen ? outlen : 1);
	if (!out)
		return 0;
	for (i = 0, o = 0, matches = 0; i < len; ) {	/* build it */
		if (i + plen <= len && memcmp(s + i, pat, plen) == 0 &&
		    (matches == 0 || global)) {
			memcpy(out + o, rep, rlen);
			o += rlen;
			i += plen;
			matches++;
		} else {
			out[o++] = s[i++];
		}
	}
	text_delete(e->t, y, 0, len);
	if (outlen)
		text_insert(e->t, y, 0, out, outlen);
	free(out);
	hl_touch(e, y);
	return (int)matches;
}

/* Run a :[range]s/pat/rep/[g] over lines [lo,hi]. The delimiter is the char
 * after the s; an empty pattern reuses the last search string. */
static enum req
ex_substitute(struct editor *e, size_t lo, size_t hi, const char *cmd)
{
	char delim = cmd[1];
	char pat[256], rep[256];
	const char *p = cmd + 2, *use;
	size_t n = 0, y;
	int global = 0, subs = 0, lines = 0;

	while (*p && *p != delim) {			/* pattern */
		if (n < sizeof(pat) - 1)
			pat[n++] = *p;
		p++;
	}
	pat[n] = '\0';
	if (*p == delim)
		p++;
	n = 0;
	while (*p && *p != delim) {			/* replacement */
		if (n < sizeof(rep) - 1)
			rep[n++] = *p;
		p++;
	}
	rep[n] = '\0';
	if (*p == delim)
		p++;
	for (; *p; p++)					/* flags */
		if (*p == 'g')
			global = 1;

	use = pat[0] ? pat : e->last_find;
	if (!use || !use[0]) {
		snprintf(e->status, sizeof(e->status),
		    "E35: no previous regular expression");
		return REQ_CONTINUE;
	}
	if (pat[0])
		snprintf(e->last_find, sizeof(e->last_find), "%s", pat);

	if (hi >= text_lines(e->t))
		hi = text_lines(e->t) - 1;
	text_undo_group_begin(e->t);
	for (y = lo; y <= hi; y++) {
		int k = ex_subst_line(e, y, use, rep, global);

		if (k > 0) {
			subs += k;
			lines++;
			e->cy = y;
		}
	}
	text_undo_group_end(e->t);

	if (subs == 0)
		snprintf(e->status, sizeof(e->status),
		    "pattern not found: %.60s", use);
	else {
		e->cx = first_nonblank(e, e->cy);
		vi_clamp(e);
		snprintf(e->status, sizeof(e->status),
		    "%d substitution%s on %d line%s", subs,
		    subs == 1 ? "" : "s", lines, lines == 1 ? "" : "s");
	}
	return REQ_CONTINUE;
}

/* Run :[range]g/pat/cmd -- apply cmd to each line matching pat (or, for :v and
 * :g!, each line not matching). The range defaults to the whole file. The
 * supported commands are d (delete) and s (substitute). Matching lines are
 * collected first so the command can shift line numbers safely. */
static enum req
ex_global(struct editor *e, size_t lo, size_t hi, int had_range,
    const char *cmd, int invert)
{
	const char *p = cmd + 1, *sub, *use;
	char delim, pat[256];
	size_t n = 0, y, *rows, nrows = 0, i;

	if (*cmd == 'g' && *p == '!') {
		invert = 1;
		p++;
	}
	delim = *p;
	if (!is_ex_delim(delim)) {
		snprintf(e->status, sizeof(e->status), "E146: missing pattern");
		return REQ_CONTINUE;
	}
	p++;
	while (*p && *p != delim) {
		if (n < sizeof(pat) - 1)
			pat[n++] = *p;
		p++;
	}
	pat[n] = '\0';
	if (*p == delim)
		p++;
	while (*p == ' ')
		p++;
	sub = p;				/* command to run on each match */

	use = pat[0] ? pat : e->last_find;
	if (!use || !use[0]) {
		snprintf(e->status, sizeof(e->status),
		    "E35: no previous regular expression");
		return REQ_CONTINUE;
	}
	if (pat[0])
		snprintf(e->last_find, sizeof(e->last_find), "%s", pat);

	if (!had_range) {			/* :g defaults to the whole file */
		lo = 0;
		hi = text_lines(e->t) - 1;
	}
	if (hi >= text_lines(e->t))
		hi = text_lines(e->t) - 1;

	rows = malloc((hi - lo + 1) * sizeof(*rows));
	if (!rows)
		return REQ_CONTINUE;
	for (y = lo; y <= hi; y++) {
		size_t llen = 0;
		const char *s = text_line(e->t, y, &llen);
		int match = s && strstr(s, use) != NULL;

		if (match ^ invert)		/* keep matches, or non-matches */
			rows[nrows++] = y;
	}

	text_undo_group_begin(e->t);
	if (sub[0] == 'd' && (sub[1] == '\0' || sub[1] == ' ')) {
		for (i = nrows; i > 0; i--)	/* delete bottom-up */
			vi_delete_lines(e, rows[i - 1], rows[i - 1]);
	} else if (sub[0] == 's' && ex_is_subst(sub)) {
		for (i = 0; i < nrows; i++)	/* substitute keeps line count */
			ex_substitute(e, rows[i], rows[i], sub);
	} else {
		text_undo_group_end(e->t);
		free(rows);
		snprintf(e->status, sizeof(e->status),
		    "unsupported :g command: %.40s", sub);
		return REQ_CONTINUE;
	}
	text_undo_group_end(e->t);

	snprintf(e->status, sizeof(e->status), "%zu line%s matched", nrows,
	    nrows == 1 ? "" : "s");
	free(rows);
	e->cx = first_nonblank(e, e->cy);
	vi_clamp(e);
	return REQ_CONTINUE;
}

/* Parse one ex line address at *pp into *out (a 1-based line number), starting
 * from the current line cur. Handles '.', '$', a number, a mark ('x), and any
 * run of +N/-N offsets. Returns 1 when an address was read, 0 when there was
 * none, or -1 on an error (an undefined mark). *pp is advanced past it. */
static int
ex_addr(struct editor *e, char **pp, long cur, long *out)
{
	char *p = *pp;
	long base = cur;
	int have = 0;

	while (*p == ' ')
		p++;
	if (*p == '.') {
		base = cur;
		p++;
		have = 1;
	} else if (*p == '$') {
		base = (long)text_lines(e->t);
		p++;
		have = 1;
	} else if (*p == '\'') {			/* 'x -- a mark */
		int idx = p[1] - 'a';

		if (p[1] < 'a' || p[1] > 'z' ||
		    !(e->vi_marks_set & ((uint32_t)1 << idx)))
			return -1;
		base = (long)e->vi_mark_y[idx] + 1;
		p += 2;
		have = 1;
	} else if (*p >= '0' && *p <= '9') {
		base = strtol(p, &p, 10);
		have = 1;
	}
	while (*p == '+' || *p == '-') {		/* offsets */
		int sign = (*p == '+') ? 1 : -1;
		long n = 1;

		p++;
		if (*p >= '0' && *p <= '9')
			n = strtol(p, &p, 10);
		base += sign * n;
		have = 1;
	}
	*out = base;
	*pp = p;
	return have;
}

/* Parse an optional leading line range at *pp into 0-based inclusive [*lo,*hi],
 * clamped to the buffer and ordered. Returns 1 when a range (or '%') was
 * present, 0 when none was, or -1 on an error. *pp is advanced past it. */
static int
ex_parse_range(struct editor *e, char **pp, size_t *lo, size_t *hi)
{
	char *p = *pp;
	long cur = (long)e->cy + 1;
	long last = (long)text_lines(e->t);
	long a1, a2;
	int r;

	while (*p == ' ')
		p++;
	if (*p == '%') {				/* the whole file */
		p++;
		*lo = 0;
		*hi = (size_t)(last - 1);
		*pp = p;
		return 1;
	}
	r = ex_addr(e, &p, cur, &a1);
	if (r < 0)
		return -1;
	if (r == 0) {
		*pp = p;
		return 0;
	}
	a2 = a1;
	if (*p == ',' || *p == ';') {
		int semi = (*p == ';');
		long c;

		p++;
		if (semi)				/* ; moves . to the first */
			cur = a1;
		r = ex_addr(e, &p, cur, &c);
		if (r < 0)
			return -1;
		if (r > 0)
			a2 = c;
		else
			a2 = cur;
	}
	if (a1 < 1)
		a1 = 1;
	if (a2 < 1)
		a2 = 1;
	if (a1 > last)
		a1 = last;
	if (a2 > last)
		a2 = last;
	if (a1 > a2) {
		long t = a1;

		a1 = a2;
		a2 = t;
	}
	*lo = (size_t)(a1 - 1);
	*hi = (size_t)(a2 - 1);
	*pp = p;
	return 1;
}

/* Read the file named in cmd (":r path" or ":read path") into the buffer,
 * inserting its contents on the line below line "at". The cursor lands on
 * the first inserted line, matching vi's :r. */
static enum req
ex_read_file(struct editor *e, size_t at, const char *cmd)
{
	const char *fn = cmd;
	struct text *nt;
	char *bytes;
	size_t nlines, i, total, off;

	while (*fn && *fn != ' ')		/* skip the command word */
		fn++;
	while (*fn == ' ')
		fn++;
	if (*fn == '\0') {
		snprintf(e->status, sizeof(e->status), "E32: no file name");
		return REQ_CONTINUE;
	}
	nt = text_new();
	if (!nt) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return REQ_CONTINUE;
	}
	if (text_load(nt, fn) < 0) {
		snprintf(e->status, sizeof(e->status),
		    "E484: cannot open %.80s", fn);
		text_free(nt);
		return REQ_CONTINUE;
	}

	/* Join the file's lines with newlines, led by one newline so the
	 * text opens on a fresh line below "at". */
	nlines = text_lines(nt);
	total = 1;
	for (i = 0; i < nlines; i++)
		total += text_line_len(nt, i);
	total += nlines - 1;			/* separators between lines */
	bytes = malloc(total);
	if (!bytes) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		text_free(nt);
		return REQ_CONTINUE;
	}
	bytes[0] = '\n';
	off = 1;
	for (i = 0; i < nlines; i++) {
		size_t ll = 0;
		const char *lp = text_line(nt, i, &ll);

		if (lp && ll) {
			memcpy(bytes + off, lp, ll);
			off += ll;
		}
		if (i + 1 < nlines)
			bytes[off++] = '\n';
	}

	if (at >= text_lines(e->t))
		at = text_lines(e->t) - 1;
	e->cy = at;
	e->cx = text_line_len(e->t, at);
	text_undo_group_begin(e->t);
	insert_bytes(e, bytes, off);
	text_undo_group_end(e->t);
	e->cy = at + 1 < text_lines(e->t) ? at + 1 : at;
	e->cx = first_nonblank(e, e->cy);
	e->hl_valid = 0;
	vi_clamp(e);

	snprintf(e->status, sizeof(e->status), "\"%.80s\" %zu line%s", fn,
	    nlines, nlines == 1 ? "" : "s");
	free(bytes);
	text_free(nt);
	return REQ_CONTINUE;
}

/* Run an already-entered ex command line. Returns REQ_FORCE_QUIT when the
 * command asks to leave, otherwise REQ_CONTINUE. lumi edit holds a single
 * buffer, so the "all" variants (:qa, :wqa, :xa) behave like their single
 * forms. Split from vi_colon so it can run without the interactive prompt. */
static enum req
vi_ex_exec(struct editor *e, char *buf)
{
	char *p;

	p = buf;
	while (*p == ' ')
		p++;

	/* An optional leading line range, then a range-aware command. */
	{
		char *after = p;
		size_t lo = 0, hi = 0;
		int rr = ex_parse_range(e, &after, &lo, &hi);

		if (rr < 0) {
			snprintf(e->status, sizeof(e->status),
			    "E16: invalid range");
			return REQ_CONTINUE;
		}
		while (*after == ' ')
			after++;
		if (rr > 0 && *after == '\0') {		/* :N -- go to line */
			e->cy = hi;
			e->cx = first_nonblank(e, e->cy);
			vi_clamp(e);
			return REQ_CONTINUE;
		}
		if (rr == 0)				/* default: the current line */
			lo = hi = e->cy;

		switch (*after) {
		case 's':				/* :s -- substitute */
			if (ex_is_subst(after))
				return ex_substitute(e, lo, hi, after);
			break;			/* :syntax etc.: fall through */
		case 'g':				/* :g / :g! -- global */
		case 'v':				/* :v -- inverse global */
			if (ex_is_global(after))
				return ex_global(e, lo, hi, rr > 0, after,
				    *after == 'v');
			break;
		case 'd':				/* :d -- delete lines */
			if (after[1] == '\0' || after[1] == ' ') {
				vi_yank_lines(e, lo, hi);
				vi_delete_lines(e, lo, hi);
				e->cy = lo < text_lines(e->t) ? lo :
				    text_lines(e->t) - 1;
				e->cx = first_nonblank(e, e->cy);
				vi_clamp(e);
				return REQ_CONTINUE;
			}
			break;
		case 'y':				/* :y -- yank lines */
			if (after[1] == '\0' || after[1] == ' ') {
				vi_yank_lines(e, lo, hi);
				e->cy = lo;
				vi_clamp(e);
				return REQ_CONTINUE;
			}
			break;
		case '>':
		case '<':				/* :> / :< -- shift lines */
			vi_shift_lines(e, lo, hi, *after == '>' ? 1 : -1);
			return REQ_CONTINUE;
		case 'r':				/* :[N]r file -- read below */
			if (after[1] == ' ' || after[1] == '\0' ||
			    strncmp(after, "read", 4) == 0)
				return ex_read_file(e, hi, after);
			break;
		default:
			break;
		}
		if (rr > 0) {			/* a range but not a line command */
			snprintf(e->status, sizeof(e->status),
			    "E492: not an editor command: %.60s", after);
			return REQ_CONTINUE;
		}
	}

	if (strncmp(p, "syn", 3) == 0) {	/* :syntax on|off|<name> */
		const char *arg = p;

		while (*arg && *arg != ' ')
			arg++;
		while (*arg == ' ')
			arg++;
		if (strcmp(arg, "off") == 0) {
			e->hl_on = 0;
		} else if (*arg == '\0' || strcmp(arg, "on") == 0) {
			e->hl_on = 1;
			e->hl_valid = 0;	/* recolor from the top */
		} else {
			const struct syntax *sy = syn_for_ext(arg);

			if (!sy) {
				snprintf(e->status, sizeof(e->status),
				    "no syntax for '%.40s'", arg);
				return REQ_CONTINUE;
			}
			e->syn = sy;
			e->hl_on = 1;
			e->hl_valid = 0;
		}
		return REQ_CONTINUE;
	}

	/* Buffer commands. :e opens a file (or reloads the current one), :ls
	 * lists, :bn/:bp cycle, :b N switches, :bd closes. */
	if (strncmp(p, "e ", 2) == 0 || strncmp(p, "edit ", 5) == 0) {
		const char *fn = p + (p[1] == ' ' ? 1 : 4);

		while (*fn == ' ')
			fn++;
		buf_open(e, fn);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "e") == 0 || strcmp(p, "e!") == 0 ||
	    strcmp(p, "edit") == 0 || strcmp(p, "edit!") == 0) {
		struct text *nt;

		if (!e->has_name) {
			snprintf(e->status, sizeof(e->status),
			    "E32: no file name");
			return REQ_CONTINUE;
		}
		nt = text_new();
		if (!nt) {
			snprintf(e->status, sizeof(e->status), "out of memory");
			return REQ_CONTINUE;
		}
		if (text_load(nt, e->path) < 0) {
			snprintf(e->status, sizeof(e->status),
			    "reload failed: %s", strerror(errno));
			text_free(nt);
			return REQ_CONTINUE;
		}
		text_free(e->t);
		e->t = nt;
		e->cy = e->cx = e->top = e->left = 0;
		e->sel_active = 0;
		e->hl_valid = 0;
		snprintf(e->status, sizeof(e->status), "reloaded %.100s",
		    e->path);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "enew") == 0) {
		buf_open(e, NULL);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "ls") == 0 || strcmp(p, "buffers") == 0 ||
	    strcmp(p, "files") == 0) {
		buf_list(e);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "bn") == 0 || strcmp(p, "bnext") == 0) {
		buf_cycle(e, 1);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "bp") == 0 || strcmp(p, "bprev") == 0 ||
	    strcmp(p, "bprevious") == 0 || strcmp(p, "bN") == 0 ||
	    strcmp(p, "bNext") == 0) {
		buf_cycle(e, -1);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "bd") == 0 || strcmp(p, "bd!") == 0 ||
	    strcmp(p, "bdelete") == 0 || strcmp(p, "bdelete!") == 0) {
		size_t bl = strlen(p);
		int force = bl > 0 && p[bl - 1] == '!';

		if (!force && text_dirty(e->t)) {
			snprintf(e->status, sizeof(e->status),
			    "E89: no write since last change (add ! to override)");
			return REQ_CONTINUE;
		}
		if (buf_close(e, e->cur) < 0)
			snprintf(e->status, sizeof(e->status),
			    "cannot close the last buffer");
		return REQ_CONTINUE;
	}
	if ((strncmp(p, "b ", 2) == 0 || strncmp(p, "buffer ", 7) == 0 ||
	    (p[0] == 'b' && p[1] >= '0' && p[1] <= '9'))) {
		const char *np = p + 1;
		long n;

		while (*np && (*np < '0' || *np > '9'))
			np++;
		n = strtol(np, NULL, 10);
		if (n < 1 || n > e->nbuf)
			snprintf(e->status, sizeof(e->status),
			    "E86: no buffer %ld", n);
		else
			buf_switch(e, (int)(n - 1));
		return REQ_CONTINUE;
	}

	if (strcmp(p, "q") == 0) {
		if (text_dirty(e->t)) {
			snprintf(e->status, sizeof(e->status),
			    "E37: no write since last change (:q! overrides)");
			return REQ_CONTINUE;
		}
		return REQ_FORCE_QUIT;
	}
	if (strcmp(p, "q!") == 0)
		return REQ_FORCE_QUIT;
	if (strcmp(p, "w") == 0 || strcmp(p, "w!") == 0 ||
	    strncmp(p, "w ", 2) == 0) {
		const char *fn = p + 1;

		while (*fn == ' ' || *fn == '!')
			fn++;
		if (*fn) {
			snprintf(e->path, sizeof(e->path), "%s", fn);
			e->has_name = 1;
		}
		if (!e->has_name) {
			snprintf(e->status, sizeof(e->status),
			    "E32: no file name");
			return REQ_CONTINUE;
		}
		if (text_save(e->t, e->path) < 0)
			snprintf(e->status, sizeof(e->status),
			    "save failed: %s", strerror(errno));
		else
			snprintf(e->status, sizeof(e->status), "wrote %.120s",
			    e->path);
		return REQ_CONTINUE;
	}
	if (strcmp(p, "wq") == 0 || strcmp(p, "wq!") == 0 ||
	    strcmp(p, "x") == 0 || strcmp(p, "x!") == 0) {
		if (!e->has_name) {
			snprintf(e->status, sizeof(e->status),
			    "E32: no file name");
			return REQ_CONTINUE;
		}
		if (text_save(e->t, e->path) < 0) {
			snprintf(e->status, sizeof(e->status),
			    "save failed: %s", strerror(errno));
			return REQ_CONTINUE;
		}
		return REQ_FORCE_QUIT;
	}

	/* The quit-all, write-all-and-quit, and quit-with-error families. A
	 * trailing '!' forces past unsaved changes. */
	{
		static const char *const qall[] = {
			"qa", "qall", "quita", "quitall", NULL,
		};
		static const char *const wqall[] = {
			"wqa", "wqall", "xa", "xall", NULL,
		};
		static const char *const cquit[] = {
			"cq", "cquit", NULL,
		};
		char base[32];
		int force = 0;
		size_t bl = strlen(p);

		if (bl > 0 && p[bl - 1] == '!') {
			force = 1;
			bl--;
		}
		if (bl < sizeof(base)) {
			memcpy(base, p, bl);
			base[bl] = '\0';

			if (ex_match(base, cquit))
				return REQ_QUIT_ERR;	/* exit nonzero */
			if (ex_match(base, qall)) {
				if (!force && text_dirty(e->t)) {
					snprintf(e->status, sizeof(e->status),
					    "E37: no write since last change"
					    " (add ! to override)");
					return REQ_CONTINUE;
				}
				return REQ_FORCE_QUIT;
			}
			if (ex_match(base, wqall)) {
				if (!e->has_name) {
					snprintf(e->status, sizeof(e->status),
					    "E32: no file name");
					return REQ_CONTINUE;
				}
				if (text_save(e->t, e->path) < 0) {
					snprintf(e->status, sizeof(e->status),
					    "save failed: %s",
					    strerror(errno));
					return REQ_CONTINUE;
				}
				return REQ_FORCE_QUIT;
			}
		}
	}

	snprintf(e->status, sizeof(e->status), "E492: not a command: %.80s", p);
	return REQ_CONTINUE;
}

/* Prompt for a ':' ex command and run it. */
enum req
vi_colon(struct editor *e)
{
	char buf[PATH_MAX];

	buf[0] = '\0';
	if (!prompt_line(e, ":", buf, sizeof(buf)))
		return REQ_CONTINUE;
	return vi_ex_exec(e, buf);
}

/* Read a vi '/' or '?' search line, with an optional offset after a
 * second delimiter, and jump to the match. */
void
vi_search(struct editor *e)
{
	int dir = e->vi_search_dir < 0 ? -1 : 1;
	char q[256];

	q[0] = '\0';
	if (!prompt_line(e, dir < 0 ? "?" : "/", q, sizeof(q))) {
		snprintf(e->status, sizeof(e->status), "search cancelled");
		return;
	}
	vi_search_cmd(e, q, dir);
}

