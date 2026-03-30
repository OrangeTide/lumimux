/* window.h : window management (PTY + VT state) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef WINDOW_H
#define WINDOW_H

#include "vt_state.h"

#include <stdint.h>

struct window;
struct vt_parse;

/* create a window: fork a shell in a new PTY, set up VT state.
 * rows/cols set the initial terminal size.
 * returns NULL on failure. */
struct window *window_new(const char *shell, int rows, int cols);
void window_free(struct window *w);

/* accessors */
int window_master_fd(const struct window *w);
int window_child_pid(const struct window *w);
struct vt_state *window_vt(const struct window *w);
struct vt_parse *window_parser(const struct window *w);
uint32_t window_id(const struct window *w);
const char *window_title(const struct window *w);

/* set the numeric window ID (assigned by session) */
void window_set_id(struct window *w, uint32_t id);

/* set the window title (from OSC sequences) */
void window_set_title(struct window *w, const char *title);

/* feed PTY output through the VT parser */
void window_feed(struct window *w, const char *data, size_t len);

/* resize PTY + VT state */
int window_resize(struct window *w, int rows, int cols);

/* dump screen state for replay */
void window_dump(struct window *w, vt_dump_fn emit, void *ctx);

#endif /* WINDOW_H */
