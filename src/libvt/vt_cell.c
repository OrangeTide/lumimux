/* vt_cell.c : terminal cell structure */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_cell.h"

#include <string.h>

void
vt_cell_clear(struct vt_cell *c)
{
	memset(c, 0, sizeof(*c));
	c->codepoint = ' ';
	c->width = 1;
}

void
vt_cell_erase(struct vt_cell *c, struct vt_color bg)
{
	vt_cell_clear(c);
	c->bg = bg;
}
