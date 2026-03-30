/* vt_cell.h : terminal cell structure (codepoint, attributes, color) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef VT_CELL_H
#define VT_CELL_H

#include <stdint.h>

enum vt_attr {
	VT_ATTR_BOLD      = 1 << 0,
	VT_ATTR_UNDERLINE = 1 << 1,
	VT_ATTR_REVERSE   = 1 << 2,
	VT_ATTR_ITALIC    = 1 << 3,
	VT_ATTR_BLINK     = 1 << 4,
	VT_ATTR_UNDERCURL = 1 << 5,
	VT_ATTR_DIM       = 1 << 6,
	VT_ATTR_HIDDEN    = 1 << 7,
	VT_ATTR_STRIKE    = 1 << 8,
	VT_ATTR_PREDICTED = 1 << 9,	/* speculative local echo */
};

enum vt_color_type {
	VT_COLOR_DEFAULT,	/* terminal default fg/bg */
	VT_COLOR_INDEXED,	/* 0-255 palette */
	VT_COLOR_RGB,		/* 24-bit true color */
};

struct vt_color {
	enum vt_color_type type;
	union {
		uint8_t  index;		/* VT_COLOR_INDEXED */
		struct {
			uint8_t r, g, b;
		} rgb;			/* VT_COLOR_RGB */
	};
};

struct vt_cell {
	uint32_t	codepoint;
	struct vt_color	fg;
	struct vt_color	bg;
	uint16_t	attrs;
	uint8_t		width;		/* 1 or 2 for wide chars */
};

void vt_cell_clear(struct vt_cell *c);

#endif /* VT_CELL_H */
