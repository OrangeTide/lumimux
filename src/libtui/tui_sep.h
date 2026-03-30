/* tui_sep.h : horizontal separator for TUI widgets */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_SEP_H
#define TUI_SEP_H

#include "tui_pad.h"
#include "tui_theme.h"

void tui_sep_draw(struct tui_pad *p, const struct tui_theme *theme,
    int row, int x, int w);

#endif /* TUI_SEP_H */
