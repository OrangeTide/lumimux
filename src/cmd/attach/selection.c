/* selection.c : mouse text selection, copy, and paste */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "selection.h"
#include "lu_umask.h"
#include "tio_write.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- selection state ---- */

static int sel_dragging;		/* drag in progress */
static int sel_visible;			/* selection shown (after release) */
static enum sel_mode cur_mode;		/* char, word, or line granularity */
static uint32_t sel_win_id;
static int sel_sr, sel_sc;		/* start (anchor) */
static int sel_er, sel_ec;		/* end (current drag point) */
static int sel_col_min, sel_col_max;	/* window content column bounds */
static int sel_anchor_r, sel_anchor_c0, sel_anchor_c1; /* word/line anchor */

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

/* ---- word boundary detection ---- */

static int
is_word_sep(uint32_t cp)
{
	if (cp == 0 || cp == ' ' || cp == '\t')
		return 1;
	if (cp >= 0x21 && cp <= 0x2F)
		return 1;	/* !"#$%&'()*+,-./ */
	if (cp >= 0x3A && cp <= 0x40)
		return 1;	/* :;<=>?@ */
	if (cp >= 0x5B && cp <= 0x60)
		return 1;	/* [\]^_` */
	if (cp >= 0x7B && cp <= 0x7E)
		return 1;	/* {|}~ */
	return 0;
}

static void
find_word_bounds(const struct vt_cell *screen, int cols,
    int row, int col, int cmin, int cmax, int *out_c0, int *out_c1)
{
	const struct vt_cell *line = &screen[row * cols];
	int c0 = col, c1 = col;

	while (c0 > cmin && !is_word_sep(line[c0 - 1].codepoint))
		c0--;
	while (c1 < cmax && !is_word_sep(line[c1 + 1].codepoint))
		c1++;
	*out_c0 = c0;
	*out_c1 = c1;
}

/* ---- clipboard tool fallback ---- */

static void
clipboard_tool_copy(const char *text, size_t len)
{
	int pfd[2];
	pid_t pid;

	if (len == 0)
		return;
	if (pipe(pfd) < 0)
		return;

	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return;
	}

	if (pid == 0) {
		/* double-fork so the parent can reap immediately */
		pid_t pid2 = fork();

		if (pid2 != 0)
			_exit(0);

		close(pfd[1]);
		dup2(pfd[0], STDIN_FILENO);
		close(pfd[0]);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		lu_umask_restore();

		if (getenv("WAYLAND_DISPLAY"))
			execlp("wl-copy", "wl-copy", (char *)NULL);
		execlp("xclip", "xclip", "-selection", "clipboard",
		    (char *)NULL);
		execlp("xsel", "xsel", "--clipboard", "--input",
		    (char *)NULL);
		execlp("pbcopy", "pbcopy", (char *)NULL);
		_exit(1);
	}

	close(pfd[0]);
	write(pfd[1], text, len);
	close(pfd[1]);
	waitpid(pid, NULL, 0);
}

/* ---- public API ---- */

void
sel_begin(uint32_t win_id, int row, int col, int win_x, int win_w)
{
	sel_dragging = 1;
	sel_visible = 0;
	cur_mode = SEL_MODE_CHAR;
	sel_win_id = win_id;
	sel_sr = row;
	sel_sc = col;
	sel_er = row;
	sel_ec = col;
	sel_col_min = win_x;
	sel_col_max = win_x + win_w - 1;
}

void
sel_begin_word(uint32_t win_id, int row, int col, int win_x, int win_w,
    const struct vt_cell *screen, int scr_cols)
{
	int c0, c1;

	sel_dragging = 1;
	sel_visible = 0;
	cur_mode = SEL_MODE_WORD;
	sel_win_id = win_id;
	sel_col_min = win_x;
	sel_col_max = win_x + win_w - 1;

	find_word_bounds(screen, scr_cols, row, col,
	    sel_col_min, sel_col_max, &c0, &c1);

	sel_sr = row;
	sel_sc = c0;
	sel_er = row;
	sel_ec = c1;
	sel_anchor_r = row;
	sel_anchor_c0 = c0;
	sel_anchor_c1 = c1;
}

void
sel_begin_line(uint32_t win_id, int row, int win_x, int win_w)
{
	sel_dragging = 1;
	sel_visible = 0;
	cur_mode = SEL_MODE_LINE;
	sel_win_id = win_id;
	sel_col_min = win_x;
	sel_col_max = win_x + win_w - 1;

	sel_sr = row;
	sel_sc = sel_col_min;
	sel_er = row;
	sel_ec = sel_col_max;
	sel_anchor_r = row;
	sel_anchor_c0 = sel_col_min;
	sel_anchor_c1 = sel_col_max;
}

void
sel_update(int row, int col)
{
	sel_er = row;
	sel_ec = col;
}

void
sel_update_word(int row, int col,
    const struct vt_cell *screen, int scr_cols)
{
	int c0, c1;

	find_word_bounds(screen, scr_cols, row, col,
	    sel_col_min, sel_col_max, &c0, &c1);

	if (row < sel_anchor_r ||
	    (row == sel_anchor_r && c0 < sel_anchor_c0)) {
		sel_sr = sel_anchor_r;
		sel_sc = sel_anchor_c1;
		sel_er = row;
		sel_ec = c0;
	} else {
		sel_sr = sel_anchor_r;
		sel_sc = sel_anchor_c0;
		sel_er = row;
		sel_ec = c1;
	}
}

void
sel_update_line(int row)
{
	if (row < sel_anchor_r) {
		sel_sr = sel_anchor_r;
		sel_sc = sel_col_max;
		sel_er = row;
		sel_ec = sel_col_min;
	} else {
		sel_sr = sel_anchor_r;
		sel_sc = sel_col_min;
		sel_er = row;
		sel_ec = sel_col_max;
	}
}

enum sel_mode
sel_get_mode(void)
{
	return cur_mode;
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
		if (r < 0)
			continue;
		start = (r == r0) ? c0 : sel_col_min;
		end = (r == r1) ? c1 : sel_col_max;
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

	clipboard_tool_copy(copy_buf, copy_len);
}

void
sel_clear(void)
{
	sel_dragging = 0;
	sel_visible = 0;
	cur_mode = SEL_MODE_CHAR;
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
		start = (r == r0) ? c0 : sel_col_min;
		end = (r == r1) ? c1 : sel_col_max;
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

void
sel_clipboard_sync(void)
{
	clipboard_tool_copy(copy_buf, copy_len);
}
