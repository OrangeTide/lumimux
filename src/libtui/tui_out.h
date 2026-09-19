/* tui_out.h : terminal output buffer for full-screen TUI programs */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_OUT_H
#define TUI_OUT_H

#include "vt_cell.h"

#include <stddef.h>

/*
 * A growable byte buffer for building one screen frame and writing it in a
 * single call, which avoids the flicker of many small writes. It also
 * carries the escape-sequence and color helpers a direct-rendering
 * full-screen program needs (cursor moves, SGR color runs, and a
 * display-width-correct field writer). Standalone TUI subcommands render
 * through this rather than the overlay pad stack, which is sized for small
 * centered dialogs.
 */

struct tui_out {
	char	*buf;
	size_t	len;
	size_t	cap;
};

/** Discard buffered bytes, keeping the allocation for reuse. */
void tui_out_reset(struct tui_out *o);

/** Release the buffer. NULL is accepted. */
void tui_out_free(struct tui_out *o);

/** Append n raw bytes. */
void tui_out_put(struct tui_out *o, const char *s, size_t n);

/** Append a NUL-terminated string. */
void tui_out_puts(struct tui_out *o, const char *s);

/** Append a formatted string. The result is bounded to a small internal
 *  buffer, so callers pass short control strings, not arbitrary text. */
void tui_out_printf(struct tui_out *o, const char *fmt, ...);

/** Append a cursor-move escape to 1-based (row, col). */
void tui_out_move(struct tui_out *o, int row, int col);

/** Append an SGR sequence selecting fg, bg, and optional bold. */
void tui_out_sgr(struct tui_out *o, const struct vt_color *fg,
    const struct vt_color *bg, int bold);

/** Append an SGR reset. */
void tui_out_sgr_reset(struct tui_out *o);

/** Append s clipped and padded to exactly `cols` display columns: UTF-8 is
 *  decoded and rune widths are honored, control characters render as a
 *  space, and a wide rune that would straddle the right edge is dropped. */
void tui_out_field(struct tui_out *o, const char *s, int cols);

/** Write the buffered bytes to fd, retrying short writes and skipping
 *  EINTR. Does not clear the buffer. Returns 0, or -1 on error. */
int tui_out_flush(struct tui_out *o, int fd);

#endif /* TUI_OUT_H */
