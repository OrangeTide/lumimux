/* splash.c : ANSI art splash screen -- core, pipe font, output */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash.h"
#include "splash_draw.h"

#include "vt_buf.h"
#include "vt_cell.h"
#include "utf8.h"
#include "xmalloc.h"

#include <string.h>

/* ---- pipe-art font (variable height, baseline-aligned) ---- */

#define GLYPH_MAX_ROWS 5

struct glyph {
	int		width;		/* columns */
	int		height;		/* total rows (0 = undefined glyph) */
	int		baseline;	/* row index of baseline from top */
	const char	*rows[GLYPH_MAX_ROWS];
};

/*
 * ACiD/iCE style box-drawing font.  Lowercase glyphs are 3 rows tall,
 * uppercase are 4 rows tall.  All glyphs share a common baseline so
 * mixed case aligns naturally.  Space characters within glyph rows
 * are treated as transparent when rendered.
 */
static const struct glyph font[128] = {
	/* --- lowercase (height 3, baseline 2) --- */
	['a'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['b'] = { 3, 3, 2, { "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90",      /* ┬─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98" } },  /* ┴─┘ */
	['c'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['d'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac",      /* ┌─┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['e'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\xa4 ",                 /* ├┤  */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['f'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80 ",                 /* ├─  */
			      "\xe2\x94\xb4  " } },                        /* ┴   */
	['g'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82\xe2\x94\x80\xe2\x94\xa4",      /* │─┤ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['h'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['i'] = { 1, 3, 2, { "\xe2\x94\xac",                              /* ┬ */
			      "\xe2\x94\x82",                              /* │ */
			      "\xe2\x94\xb4" } },                          /* ┴ */
	['j'] = { 2, 3, 2, { " \xe2\x94\xac",                             /*  ┬ */
			      "\xe2\x94\x80\xe2\x94\xa4",                  /* ─┤ */
			      "\xe2\x94\x94\xe2\x94\x98" } },              /* └┘ */
	['k'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\x8c",                 /* ┬ ┌ */
			      "\xe2\x94\x9c\xe2\x94\xac\xe2\x94\x98",      /* ├┬┘ */
			      "\xe2\x94\xb4\xe2\x94\x94 " } },             /* ┴└  */
	['l'] = { 3, 3, 2, { "\xe2\x94\xac  ",                            /* ┬   */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98" } },  /* ┴─┘ */
	['m'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90",      /* ┌┬┐ */
			      "\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82",      /* │││ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['n'] = { 3, 3, 2, { "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90",      /* ┬─┐ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x98 \xe2\x94\x98" } },             /* ┘ ┘ */
	['o'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['p'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x98",      /* ├─┘ */
			      "\xe2\x94\xb4  " } },                        /* ┴   */
	['q'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\xa4",      /* └─┤ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['r'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\xac\xe2\x94\x98",      /* ├┬┘ */
			      "\xe2\x94\xb4\xe2\x94\x94 " } },             /* ┴└  */
	['s'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x90",      /* └─┐ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['t'] = { 3, 3, 2, { "\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90",      /* ┌┬┐ */
			      " \xe2\x94\x82 ",                            /*  │  */
			      " \xe2\x94\xb4 " } },                        /*  ┴  */
	['u'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['v'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      " \xe2\x94\x94\xe2\x94\x98" } },             /*  └┘ */
	['w'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82\xe2\x94\xac\xe2\x94\x82",      /* │┬│ */
			      "\xe2\x94\x94\xe2\x94\xb4\xe2\x94\x98" } },  /* └┴┘ */
	['x'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x94\xe2\x94\xac\xe2\x94\x98",      /* └┬┘ */
			      "\xe2\x94\x8c\xe2\x94\xb4\xe2\x94\x90" } },  /* ┌┴┐ */
	['y'] = { 3, 3, 2, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\xa4",      /* └─┤ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['z'] = { 3, 3, 2, { "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90",      /* ──┐ */
			      "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x98",      /* ┌─┘ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80" } },  /* └── */

	/* --- uppercase (height 4, baseline 3) --- */
	['A'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['B'] = { 3, 4, 3, { "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90",      /* ┬─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98" } },  /* ┴─┘ */
	['C'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['D'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac",      /* ┌─┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['E'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\xa4 ",                 /* ├┤  */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['F'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80 ",                 /* ├─  */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\xb4  " } },                        /* ┴   */
	['G'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x82\xe2\x94\x80\xe2\x94\xa4",      /* │─┤ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['H'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",      /* ├─┤ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['I'] = { 1, 4, 3, { "\xe2\x94\xac",                              /* ┬ */
			      "\xe2\x94\x82",                              /* │ */
			      "\xe2\x94\x82",                              /* │ */
			      "\xe2\x94\xb4" } },                          /* ┴ */
	['J'] = { 2, 4, 3, { " \xe2\x94\xac",                             /*  ┬ */
			      " \xe2\x94\x82",                             /*  │ */
			      "\xe2\x94\x80\xe2\x94\xa4",                  /* ─┤ */
			      "\xe2\x94\x94\xe2\x94\x98" } },              /* └┘ */
	['K'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\x90",                 /* ┬ ┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x98",      /* ├─┘ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x90",      /* ├─┐ */
			      "\xe2\x94\xb4 \xe2\x94\x94" } },             /* ┴ └ */
	['L'] = { 3, 4, 3, { "\xe2\x94\xac  ",                            /* ┬   */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98" } },  /* ┴─┘ */
	['M'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90",      /* ┌┬┐ */
			      "\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82",      /* │││ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['N'] = { 3, 4, 3, { "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90",      /* ┬─┐ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x98 \xe2\x94\x98" } },             /* ┘ ┘ */
	['O'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['P'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x98",      /* ├─┘ */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\xb4  " } },                        /* ┴   */
	['Q'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\xa4",      /* └─┤ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['R'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x98",      /* ├─┘ */
			      "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x90",      /* ├─┐ */
			      "\xe2\x94\xb4 \xe2\x94\x94" } },             /* ┴ └ */
	['S'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",      /* ┌─┐ */
			      "\xe2\x94\x9c\xe2\x94\x80 ",                 /* ├─  */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x90",      /* └─┐ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['T'] = { 3, 4, 3, { "\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90",      /* ┌┬┐ */
			      " \xe2\x94\x82 ",                            /*  │  */
			      " \xe2\x94\x82 ",                            /*  │  */
			      " \xe2\x94\xb4 " } },                        /*  ┴  */
	['U'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98" } },  /* └─┘ */
	['V'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      " \xe2\x94\x94\xe2\x94\x98" } },             /*  └┘ */
	['W'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x82 \xe2\x94\x82",                 /* │ │ */
			      "\xe2\x94\x82\xe2\x94\xac\xe2\x94\x82",      /* │┬│ */
			      "\xe2\x94\x94\xe2\x94\xb4\xe2\x94\x98" } },  /* └┴┘ */
	['X'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x94\xe2\x94\xac\xe2\x94\x98",      /* └┬┘ */
			      "\xe2\x94\x8c\xe2\x94\xb4\xe2\x94\x90",      /* ┌┴┐ */
			      "\xe2\x94\xb4 \xe2\x94\xb4" } },             /* ┴ ┴ */
	['Y'] = { 3, 4, 3, { "\xe2\x94\xac \xe2\x94\xac",                 /* ┬ ┬ */
			      "\xe2\x94\x94\xe2\x94\xac\xe2\x94\x98",      /* └┬┘ */
			      " \xe2\x94\x82 ",                            /*  │  */
			      " \xe2\x94\xb4 " } },                        /*  ┴  */
	['Z'] = { 3, 4, 3, { "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90",      /* ──┐ */
			      "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x98",      /* ┌─┘ */
			      "\xe2\x94\x82  ",                            /* │   */
			      "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80" } },  /* └── */

	/* --- punctuation --- */
	[' '] = { 2, 3, 2, { "  ", "  ", "  " } },
	['-'] = { 3, 3, 2, { "   ",
			      "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",      /* ─── */
			      "   " } },
};

/* ---- drawing primitives ---- */

void
splash_put(struct vt_buf *buf, int row, int col, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	struct vt_cell *c;

	c = vt_buf_cell(buf, row, col);
	if (!c)
		return;
	c->codepoint = cp;
	c->fg = fg;
	c->bg = bg;
	c->attrs = attrs;
	c->width = 1;
}

void
splash_put_fg(struct vt_buf *buf, int row, int col, uint32_t cp,
    struct vt_color fg, uint16_t attrs)
{
	struct vt_cell *c;

	c = vt_buf_cell(buf, row, col);
	if (!c)
		return;
	c->codepoint = cp;
	c->fg = fg;
	c->attrs = attrs;
	c->width = 1;
	/* bg preserved */
}

void
splash_fill(struct vt_buf *buf, int r0, int c0, int r1, int c1,
    uint32_t cp, struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	int r, c;

	for (r = r0; r < r1; r++)
		for (c = c0; c < c1; c++)
			splash_put(buf, r, c, cp, fg, bg, attrs);
}

void
splash_puts(struct vt_buf *buf, int row, int col, const char *s,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	int c;

	for (c = 0; s[c]; c++) {
		if (s[c] != ' ')
			splash_put(buf, row, col + c, (uint32_t)(unsigned char)s[c],
			    fg, bg, attrs);
	}
}

int
splash_puts_utf8(struct vt_buf *buf, int row, int col, const char *s,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	const unsigned char *p;
	uint32_t cp;
	int n, c;

	p = (const unsigned char *)s;
	c = col;
	while (*p) {
		n = utf8_decode(&cp, p, 4);
		if (cp != ' ')
			splash_put(buf, row, c, cp, fg, bg, attrs);
		c++;
		p += n;
	}
	return c - col;
}

/* ---- logo rendering ---- */

/* Scan name and compute max ascent/descent from glyph baselines. */
static void
logo_metrics(const char *name, int *max_asc, int *max_desc)
{
	const char *p;

	*max_asc = 0;
	*max_desc = 0;
	for (p = name; *p; p++) {
		int ch = (unsigned char)*p & 0x7f;
		const struct glyph *g = &font[ch];

		if (g->height == 0)
			continue;
		if (g->baseline > *max_asc)
			*max_asc = g->baseline;
		if (g->height - g->baseline - 1 > *max_desc)
			*max_desc = g->height - g->baseline - 1;
	}
}

int
splash_logo_width(const char *name)
{
	int w;
	const char *p;

	w = 0;
	for (p = name; *p; p++) {
		int ch = (unsigned char)*p & 0x7f;
		const struct glyph *g = &font[ch];

		if (g->height == 0)
			continue;
		if (w > 0)
			w++;	/* inter-glyph space */
		w += g->width;
	}
	return w;
}

int
splash_logo_height(const char *name)
{
	int asc, desc;

	logo_metrics(name, &asc, &desc);
	return asc + 1 + desc + 2;	/* glyph rows + blank + tagline */
}

int
splash_draw_logo(struct vt_buf *buf, int row, int col,
    const char *name, struct vt_color fg)
{
	const char *p;
	int x, r, logo_w, logo_rows, tag_len, tag_col;
	int max_asc, max_desc;
	const char *tag = "terminal multiplexer";

	logo_metrics(name, &max_asc, &max_desc);
	logo_rows = max_asc + 1 + max_desc;

	/* draw pipe-art glyphs, baseline-aligned */
	x = col;
	for (p = name; *p; p++) {
		int ch = (unsigned char)*p & 0x7f;
		const struct glyph *g = &font[ch];
		int off;

		if (g->height == 0)
			continue;
		if (x > col)
			x++;	/* inter-glyph space */
		off = max_asc - g->baseline;
		for (r = 0; r < g->height; r++)
			splash_puts_utf8(buf, row + off + r, x,
			    g->rows[r], fg, CLR_DEF, 0);
		x += g->width;
	}

	logo_w = x - col;

	/* draw tagline, centered under the logo (or vice versa) */
	tag_len = (int)strlen(tag);
	if (tag_len <= logo_w)
		tag_col = col + (logo_w - tag_len) / 2;
	else
		tag_col = col - (tag_len - logo_w) / 2;
	splash_puts(buf, row + logo_rows + 1, tag_col, tag,
	    CLR_IDX(8), CLR_DEF, VT_ATTR_DIM);

	return logo_w;
}

/* ---- utilities ---- */

uint32_t
splash_rand(uint32_t *seed)
{
	*seed = *seed * 1103515245u + 12345u;
	return (*seed >> 16) & 0x7fffu;
}

/* ---- public API ---- */

/* Canvas dimensions per scene */
#define SPACE_COLS	80
#define SPACE_ROWS	40
#define MOUNTAIN_COLS	80
#define MOUNTAIN_ROWS	40
#define BEACH_COLS	120
#define BEACH_ROWS	65

struct vt_buf *
splash_create(enum splash_scene scene, const char *name)
{
	struct vt_buf *buf;
	int rows, cols;

	switch (scene) {
	case SPLASH_SPACE:
		rows = SPACE_ROWS;
		cols = SPACE_COLS;
		break;
	case SPLASH_MOUNTAIN:
		rows = MOUNTAIN_ROWS;
		cols = MOUNTAIN_COLS;
		break;
	case SPLASH_BEACH:
		rows = BEACH_ROWS;
		cols = BEACH_COLS;
		break;
	default:
		return NULL;
	}

	buf = vt_buf_new(rows, cols, 0);
	if (!buf)
		return NULL;

	switch (scene) {
	case SPLASH_SPACE:
		splash_scene_space(buf, name);
		break;
	case SPLASH_MOUNTAIN:
		splash_scene_mountain(buf, name);
		break;
	case SPLASH_BEACH:
		splash_scene_beach(buf, name);
		break;
	default:
		break;
	}

	return buf;
}

void
splash_free(struct vt_buf *buf)
{
	vt_buf_free(buf);
}
