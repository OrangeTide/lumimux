/* splash_draw.h : internal drawing helpers for splash scenes */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SPLASH_DRAW_H
#define SPLASH_DRAW_H

#include "vt_buf.h"
#include "vt_cell.h"

#include <stdint.h>

/* ---- color convenience macros ---- */

#define CLR_IDX(n) \
	((struct vt_color){ .type = VT_COLOR_INDEXED, .index = (n) })

#define CLR_RGB(rv, gv, bv) \
	((struct vt_color){ .type = VT_COLOR_RGB, \
	    .rgb = { (rv), (gv), (bv) } })

#define CLR_DEF \
	((struct vt_color){ .type = VT_COLOR_DEFAULT })

/* ---- cell drawing primitives ---- */

/* Set a cell's codepoint, colors, and attributes. */
void splash_put(struct vt_buf *buf, int row, int col,
    uint32_t cp, struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Set a cell's codepoint and fg, preserving existing bg. */
void splash_put_fg(struct vt_buf *buf, int row, int col,
    uint32_t cp, struct vt_color fg, uint16_t attrs);

/* Fill a rectangular region with a character and colors. */
void splash_fill(struct vt_buf *buf, int r0, int c0, int r1, int c1,
    uint32_t cp, struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Write an ASCII string starting at (row, col).
 * Space characters are transparent (cell not modified). */
void splash_puts(struct vt_buf *buf, int row, int col,
    const char *s, struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Write a UTF-8 string starting at (row, col).
 * Space characters are transparent. Returns columns consumed. */
int splash_puts_utf8(struct vt_buf *buf, int row, int col,
    const char *s, struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* ---- logo rendering ---- */

/* Draw the pipe-art logo and "terminal multiplexer" tagline.
 * Spaces in the font are transparent (scene shows through).
 * Returns the total width in columns. */
int splash_draw_logo(struct vt_buf *buf, int row, int col,
    const char *name, struct vt_color fg);

/* Calculate logo width in columns for a given name. */
int splash_logo_width(const char *name);

/* Calculate total logo height (glyphs + blank + tagline) for a name. */
int splash_logo_height(const char *name);

/* ---- utilities ---- */

/* Simple deterministic LCG. */
uint32_t splash_rand(uint32_t *seed);

/* ---- scene drawing (called from splash_create) ---- */

void splash_scene_space(struct vt_buf *buf, const char *name);
void splash_scene_mountain(struct vt_buf *buf, const char *name);
void splash_scene_beach(struct vt_buf *buf, const char *name);

#endif /* SPLASH_DRAW_H */
