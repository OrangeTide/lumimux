/* draw.h : backend-neutral drawing surface */

#ifndef DRAW_H
#define DRAW_H

#include "vt_cell.h"
#include "tkbd.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A draw surface is a full-screen cell grid an app paints into, plus logical
 * cursor state and a bound driver. Drawing calls fill cells; they never emit
 * bytes. draw_present hands the grid to the driver, which diffs and flushes.
 * The surface keeps only the current frame: differencing is the driver's job.
 *
 * The terminal driver lives in libdraw_term; a graphical driver could take its
 * place without any app change. See doc/DRAW.md for the design.
 */

struct draw;
struct draw_driver;

/* An event from draw_wait. */
enum draw_event_type {
	DRAW_EVENT_NONE,	/* nothing happened */
	DRAW_EVENT_KEY,		/* a key or mouse event, in .key */
	DRAW_EVENT_RESIZE,	/* the terminal resized; the surface is already resized */
	DRAW_EVENT_RESUME,	/* returned from suspend; the caller should repaint */
	DRAW_EVENT_EOF,		/* input closed or errored */
};

struct draw_event {
	enum draw_event_type	type;
	struct tkbd_seq		key;	/* valid when type == DRAW_EVENT_KEY */
};

/* Continuation marker for the trailing cell of a wide (width 2) glyph. It
 * mirrors TUI_CODEPOINT_CONT in libtui, kept local so the surface does not
 * depend on a higher layer. Drivers skip continuation cells (width 0); the
 * renderer keys off width, so this value is informational. */
#define DRAW_CELL_CONT 0xFFFEu

enum draw_cursor_shape {
	DRAW_CURSOR_DEFAULT,	/* the terminal's configured cursor */
	DRAW_CURSOR_BLOCK,
	DRAW_CURSOR_BAR,
	DRAW_CURSOR_UNDERLINE,
};

/* Create a surface bound to a driver. The initial size is queried from the
 * driver (falling back to 24x80 if the driver cannot report one). ctx is the
 * driver's own state, passed to every driver method. Returns NULL on failure.
 * draw_free calls the driver's free, so the surface owns the driver. */
struct draw *draw_new(const struct draw_driver *drv, void *ctx);
void draw_free(struct draw *d);

/* Bracket a drawing session. begin enters (raw mode + alt screen for the
 * terminal driver); end leaves. They may be called more than once: an app
 * suspending (SIGTSTP) or shelling out calls draw_end, then draw_begin on
 * return. Both forward to the driver. */
void draw_begin(struct draw *d);
void draw_end(struct draw *d);

/* Report the cached size (no driver call). Either pointer may be NULL. */
void draw_size(struct draw *d, int *rows, int *cols);

/* Reshape the grid, typically from the app's SIGWINCH handler. The grid is
 * reset to default cells and every row marked dirty. */
void draw_resize(struct draw *d, int rows, int cols);

/* Reset every cell to the default (a space with default colors). */
void draw_clear(struct draw *d);

/* Write one cell. A width-2 codepoint also sets the next cell to a
 * continuation. Out-of-range coordinates are ignored. */
void draw_cell(struct draw *d, int r, int c, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Write a UTF-8 string starting at (r, c). Rune widths are honored, control
 * bytes render as a space, and a wide rune that would straddle the right edge
 * is dropped. Returns the column after the last cell written. */
int draw_text(struct draw *d, int r, int c, const char *utf8,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Fill n single-width cells with cp starting at (r, c). */
void draw_fill(struct draw *d, int r, int c, int n, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Cursor state carried to the driver at present time. */
void draw_cursor(struct draw *d, int r, int c);
void draw_cursor_vis(struct draw *d, int on);
void draw_cursor_shape(struct draw *d, enum draw_cursor_shape shape);

/* Hand the current frame to the driver (diff + flush) and clear the
 * per-row dirty marks for the next frame. */
void draw_present(struct draw *d);

/*
 * Wait for the next event and return its type. This is the high-level input
 * call an app's loop pumps: besides keys it also reports a terminal resize
 * and a resume from suspend, handling SIGWINCH and SIGTSTP through the driver
 * so the app installs no signal handlers of its own. On DRAW_EVENT_RESIZE the
 * surface has already resized itself; on DRAW_EVENT_RESUME it has already left
 * and re-entered the terminal around the stop. The caller repaints on either.
 * Blocks until an event is available (or DRAW_EVENT_EOF).
 */
int draw_wait(struct draw *d, struct draw_event *ev);

/*
 * Lower-level input: an event queue on the Win32 GetMessage and PeekMessage
 * model. The driver drains decoded key events into the surface's queue; these
 * calls pull them out one at a time. draw_wait is built on draw_next_event and
 * adds resize and suspend handling; most apps use draw_wait.
 */

/* Remove and return the next event (GetMessage). If the queue is empty it
 * refills from the driver, blocking up to timeout_ms. Returns 1 when *out is
 * filled, 0 on timeout with no event, or -1 on driver error or EOF. */
int draw_next_event(struct draw *d, int timeout_ms, struct tkbd_seq *out);

/* Return the next event without removing it (PeekMessage). Does not block; it
 * refills from the driver only with a zero timeout. Returns 1 when *out is
 * filled, 0 when no event is ready, or -1 on driver error or EOF. */
int draw_peek_event(struct draw *d, struct tkbd_seq *out);

/* Low-level: forward one poll to the driver, dispatching each decoded event
 * to on_event. draw_next_event and draw_peek_event are built on this; most
 * apps use those. Returns events dispatched, -1 on error or EOF, or 0 when
 * the driver has no input source. */
int draw_poll(struct draw *d, int timeout_ms,
    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u);

/* Optional side channels. Each forwards to the driver if it provides the
 * method and is a no-op otherwise, so an app can call them unconditionally. */
void draw_set_clipboard(struct draw *d, const char *utf8, size_t n);
void draw_set_title(struct draw *d, const char *utf8);
void draw_bell(struct draw *d);

#endif /* DRAW_H */
