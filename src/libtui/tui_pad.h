/* tui_pad.h : overlay pad with blend modes and backend-driven rendering */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_PAD_H
#define TUI_PAD_H

#include "vt_cell.h"
#include "vt_buf.h"

#include <stddef.h>
#include <stdint.h>

/* ---- blend modes ---- */

enum tui_blend {
	TUI_TRANSPARENT,	/* skip -- show layer below */
	TUI_OPAQUE,		/* draw codepoint + fg + bg + attrs */
	TUI_COLOR_BG,		/* keep underlying char + fg, replace bg */
	TUI_COLOR_FG_BG,	/* keep underlying char, replace fg + bg */
};

/* Continuation marker for the trailing cell(s) of a wide character.
 * The renderer must skip cells that resolve to this codepoint. */
#define TUI_CODEPOINT_CONT 0xFFFEu

/* ---- cell ---- */

struct tui_cell {
	uint32_t	codepoint;
	struct vt_color	fg;
	struct vt_color	bg;
	uint16_t	attrs;		/* VT_ATTR_BOLD, etc. */
	uint8_t		blend;		/* enum tui_blend */
};

/* ---- render backend ---- */

struct tui_backend {
	void (*cell)(void *ctx, int row, int col, uint32_t cp,
	    const struct vt_color *fg, const struct vt_color *bg,
	    uint16_t attrs);
	void (*flush)(void *ctx);
};

/* ---- pad ---- */

#define TUI_PAD_MAX_ROWS 48
#define TUI_PAD_MAX_COLS 80

struct tui_pad {
	struct tui_cell	cells[TUI_PAD_MAX_ROWS][TUI_PAD_MAX_COLS];
	int		w, h;
	int		screen_row, screen_col;
};

void tui_pad_clear(struct tui_pad *p, int w, int h);

void tui_pad_put(struct tui_pad *p, int row, int col,
    uint32_t ch, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend);

/* returns column after last char written */
int tui_pad_puts(struct tui_pad *p, int row, int col,
    const char *s, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend);

void tui_pad_fill(struct tui_pad *p, int row, int col, int n,
    uint32_t ch, struct vt_color fg, struct vt_color bg,
    uint16_t attrs, enum tui_blend blend);

/* ---- pad stack ---- */

#define TUI_STACK_MAX 4

struct tui_stack {
	struct tui_pad	layers[TUI_STACK_MAX];
	int		depth;		/* 0 = empty */
};

void tui_stack_init(struct tui_stack *s);
struct tui_pad *tui_stack_push(struct tui_stack *s);
void tui_stack_pop(struct tui_stack *s);
struct tui_pad *tui_stack_top(struct tui_stack *s);
int tui_stack_depth(const struct tui_stack *s);

void tui_stack_render(const struct tui_stack *s,
    struct vt_buf *base, const struct tui_backend *be, void *ctx);

void tui_stack_erase(const struct tui_stack *s,
    struct vt_buf *base, const struct tui_backend *be, void *ctx,
    int row, int col, int h, int w);

#endif /* TUI_PAD_H */
