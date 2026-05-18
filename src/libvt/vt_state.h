/* vt_state.h : terminal emulation state */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef VT_STATE_H
#define VT_STATE_H

#include "vt_buf.h"
#include "vt_cell.h"

#include <stddef.h>
#include <stdint.h>

/* render target selection (indexes into vt_state target array) */
enum vt_target {
	VT_TARGET_PRIMARY = 0,	/* normal screen buffer (has scrollback) */
	VT_TARGET_ALT,		/* alternate screen (no scrollback) */
	VT_TARGET_SCROLLBACK,	/* scratch buffer for scrollback viewer */
	VT_TARGET_COUNT,
};

/* terminal mode flags (DECSET/DECRST and ESC-based modes) */
#define VT_MODE_AUTOWRAP	(1u << 0)
#define VT_MODE_CURSOR_VIS	(1u << 1)
#define VT_MODE_ORIGIN		(1u << 2)
#define VT_MODE_INSERT		(1u << 3)
#define VT_MODE_ALTSCREEN	(1u << 4)
#define VT_MODE_BRACKETPASTE	(1u << 5)
#define VT_MODE_DECCKM		(1u << 6)
#define VT_MODE_DECKPAM		(1u << 7)
#define VT_MODE_MOUSE		(1u << 8)

/* saved cursor state (DECSC/DECRC) */
struct vt_saved_cursor {
	int		row;
	int		col;
	uint16_t	attrs;
	struct vt_color	fg;
	struct vt_color	bg;
};

struct vt_state {
	struct vt_buf	*buf;		/* active render target */
	struct vt_buf	*targets[VT_TARGET_COUNT]; /* render target array */
	enum vt_target	active_target;	/* which target buf points to */

	int		cursor_row;
	int		cursor_col;
	struct vt_saved_cursor saved;

	/* scroll region (0-based, inclusive top, exclusive bottom) */
	int		scroll_top;
	int		scroll_bot;

	unsigned	modes;

	/* current SGR state applied to new cells */
	uint16_t	attrs;
	struct vt_color	fg;
	struct vt_color	bg;

	/* cursor shape (DECSCUSR: 0=default, 1-6) */
	int		cursor_shape;

	/* character set (G0/G1) */
	int		charset;	/* 0 = G0, 1 = G1 */
	int		g0_set;		/* 0 = ASCII, 1 = line drawing */
	int		g1_set;

	/* tab stops */
	uint8_t		*tabstops;	/* one byte per column */

	/* reply fd for DSR/DA responses (-1 = disabled) */
	int		reply_fd;

	/* child mouse tracking mode (0 = off, 1000/1002/1003) */
	int		mouse_mode;

	/* keyboard enhancement protocols */
	int		kitty_kbd_flags;	/* CSI > flags u (0 = off) */
	int		modify_other_keys;	/* CSI > 4 ; Pm m (0 = off) */

	/* window title set by OSC 0/2 */
	char		*title;
};

struct vt_state *vt_state_new(int rows, int cols, int scrollback);
void vt_state_free(struct vt_state *st);
int vt_state_resize(struct vt_state *st, int rows, int cols);

/* render target selection */
void vt_state_set_target(struct vt_state *st, enum vt_target tgt);

/* alt screen management */
void vt_state_altscreen_enter(struct vt_state *st);
void vt_state_altscreen_leave(struct vt_state *st);

/* cursor operations */
void vt_state_cursor_save(struct vt_state *st);
void vt_state_cursor_restore(struct vt_state *st);
void vt_state_cursor_clamp(struct vt_state *st);

/* set the fd for DSR/DA reply writes (-1 to disable) */
void vt_state_set_reply_fd(struct vt_state *st, int fd);

/* window title (set by OSC 0/2, NULL if never set) */
const char *vt_state_title(const struct vt_state *st);

/* write a character at cursor, advance cursor */
void vt_state_putchar(struct vt_state *st, uint32_t cp, int width);

/* tab stops */
void vt_state_tab_reset(struct vt_state *st);
void vt_state_tab_set(struct vt_state *st, int col);
void vt_state_tab_clear(struct vt_state *st, int col);
int vt_state_tab_next(struct vt_state *st, int col);
int vt_state_tab_prev(struct vt_state *st, int col);

/* synthesize escape sequences that reconstruct the current screen.
 * calls emit(ctx, data, len) for each chunk of output.
 * used to replay state to a newly attached client. */
typedef void (*vt_dump_fn)(void *ctx, const char *data, size_t len);
void vt_state_dump(struct vt_state *st, vt_dump_fn emit, void *ctx);

#endif /* VT_STATE_H */
