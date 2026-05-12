/* wm.h : overlapping window manager compositor */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef WM_H
#define WM_H

#include "vt_cell.h"

#include <stdint.h>

struct vt_state;
struct tui_theme;

#define WM_MAX_WINDOWS	32
#define WM_TITLE_MAX	128

/* hit-test result areas */
enum wm_hit {
	WM_HIT_NONE,		/* background / no window */
	WM_HIT_CONTENT,	/* window content area */
	WM_HIT_TITLE,		/* title bar (drag to move) */
	WM_HIT_BORDER,		/* border edge (resize) */
	WM_HIT_CLOSE,		/* close button */
	WM_HIT_MINIMIZE,	/* minimize button */
	WM_HIT_MAXIMIZE,	/* maximize button */
	WM_HIT_SCROLLBAR,	/* scrollbar track/thumb/arrows */
};

/* drag state */
enum wm_drag {
	WM_DRAG_IDLE,
	WM_DRAG_MOVING,
	WM_DRAG_RESIZING,
	WM_DRAG_SCROLLING,
};

/* window state flags (bitmask) */
#define WM_WIN_SCROLL_LOCK	0x01	/* output frozen */
#define WM_WIN_INPUT_LOCK	0x02	/* keystrokes discarded */
#define WM_WIN_DEAD		0x04	/* child process has exited */

struct wm_window {
	uint32_t	id;
	int		x, y;		/* content origin on screen */
	int		w, h;		/* content size (excluding frame) */
	int		z;		/* z-order, higher = on top */
	int		focused;
	int		minimized;
	int		maximized;
	int		saved_x, saved_y;	/* geometry before maximize */
	int		saved_w, saved_h;
	uint32_t	flags;
	float		scroll_pos;	/* 0.0 = top, 1.0 = bottom (live) */
	float		scroll_len;	/* thumb size ratio, 0 = no scrollbar */
	const struct tui_theme *win_theme; /* per-window override, NULL = global */
	struct vt_state	*vt;		/* not owned */
	char		title[WM_TITLE_MAX];
};

struct wm;

/* lifecycle */
struct wm *wm_new(int rows, int cols);
void wm_free(struct wm *wm);
void wm_resize(struct wm *wm, int rows, int cols);

/* window management */
int wm_add(struct wm *wm, uint32_t id, struct vt_state *vt,
    int x, int y, int w, int h);
void wm_remove(struct wm *wm, uint32_t id);
struct wm_window *wm_find(struct wm *wm, uint32_t id);
int wm_count(const struct wm *wm);
struct wm_window *wm_window_at(struct wm *wm, int index);

/* z-order and focus */
void wm_focus(struct wm *wm, uint32_t id);
void wm_raise(struct wm *wm, uint32_t id);

/* window manipulation */
void wm_move(struct wm *wm, uint32_t id, int x, int y);
void wm_set_title(struct wm *wm, uint32_t id, const char *title);
void wm_set_flags(struct wm *wm, uint32_t id, uint32_t flags);
void wm_set_theme(struct wm *wm, uint32_t id, const struct tui_theme *t);
const struct tui_theme *wm_get_theme(struct wm *wm, uint32_t id);
void wm_set_scroll(struct wm *wm, uint32_t id, float pos, float len);

/* convert a screen row on the scrollbar track to a scroll position [0,1] */
float wm_scroll_pos_from_row(const struct wm *wm, uint32_t id, int row);

/* minimize/maximize */
void wm_minimize(struct wm *wm, uint32_t id);
void wm_unminimize(struct wm *wm, uint32_t id);
int wm_toggle_maximize(struct wm *wm, uint32_t id);

/* compositing -- paints all visible windows into the screen buffer */
void wm_composite(struct wm *wm, const struct tui_theme *theme);
/* compare screen against previous frame and set row_dirty flags.
 * call after wm_composite and any post-composite modifications. */
void wm_update_dirty(struct wm *wm);
const struct vt_cell *wm_screen(const struct wm *wm);
struct vt_cell *wm_screen_mut(struct wm *wm);
const uint8_t *wm_row_dirty(const struct wm *wm);
int wm_rows(const struct wm *wm);
int wm_cols(const struct wm *wm);

/* hit testing -- returns window ID via out_id, area via out_area */
int wm_hit_test(const struct wm *wm, int row, int col,
    uint32_t *out_id, enum wm_hit *out_area);

/* mouse interaction -- returns action taken */
enum wm_drag wm_drag_state(const struct wm *wm);
uint32_t wm_drag_id(const struct wm *wm);

/* begin drag from a mouse press at (row, col).
 * returns the drag type started, or WM_DRAG_IDLE if no drag.
 * for WM_HIT_CLOSE, the window is not closed here -- the caller
 * receives out_id and handles it (e.g. send IPC close). */
enum wm_drag wm_mouse_press(struct wm *wm, int row, int col,
    uint32_t *out_id, enum wm_hit *out_area);

/* update an active drag to new mouse position.
 * returns 1 if the window was moved/resized (needs recomposite), 0 if not. */
int wm_mouse_drag(struct wm *wm, int row, int col);

/* end the current drag. returns the ID of the window that was dragged,
 * or 0 with *resized=0 if no drag was active.
 * sets *resized to 1 if a resize occurred (caller must resize PTY). */
uint32_t wm_mouse_release(struct wm *wm, int *resized);

/* arrange all non-minimized windows into a grid that fills the screen.
 * returns 0 on success, -1 if no eligible windows. */
int wm_arrange_grid(struct wm *wm);

/* minimum content dimensions for resize */
#define WM_MIN_WIDTH	4
#define WM_MIN_HEIGHT	2

#endif /* WM_H */
