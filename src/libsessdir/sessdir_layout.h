/* sessdir_layout.h : per-session window layout persistence */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSDIR_LAYOUT_H
#define SESSDIR_LAYOUT_H

#include <stdint.h>

/*
 * Layout file: <session>/layout  (dotenv format)
 *
 * Turbo mode stores per-window geometry keyed by stable window number:
 *
 *   MODE=turbo
 *   VERSION=2
 *   FOCUS=0
 *   WIN_0="2 2 40 12"
 *   WIN_3="4 3 40 12"
 *
 * Screen mode stores the split tree as a preorder traversal:
 *
 *   MODE=screen
 *   VERSION=2
 *   FOCUS=0
 *   TREE="v128 h128 0 1 3"
 *
 * Tree tokens:  v<pos> = vertical split (split_pos/256)
 *               h<pos> = horizontal split
 *               <N>    = leaf showing window number N
 *
 * Window numbers are the stable, user-visible numbers the tab bar shows,
 * the slot indices of WINDOW_NUMS in the state file. A number stays with
 * its window for the window's whole life and is never reassigned when
 * other windows close, so a saved layout keeps pointing at the windows it
 * described however the window set changes in between; a number whose
 * window has exited simply fails to resolve. (Version 1 files keyed
 * panes by position in WINDOW_ORDER, which another file owns and which
 * shifts when a window closes, so a pane could silently repoint at a
 * different window or at none. They carry no VERSION line and are
 * ignored.)
 */

#define SESSDIR_LAYOUT_VERSION	2

#define SESSDIR_LAYOUT_MAX_WINS	32

/* ---- turbo layout ---- */

struct sessdir_turbo_win {
	int	x, y, w, h;
	int	valid;		/* nonzero if this entry was populated */
};

struct sessdir_turbo_layout {
	int				focus;	/* window number, or -1 */
	struct sessdir_turbo_win	wins[SESSDIR_LAYOUT_MAX_WINS];
						/* indexed by window number */
	int				nwins;	/* one past the highest valid slot */
};

/* ---- screen layout (split tree) ---- */

enum sessdir_tree_type {
	SESSDIR_TREE_LEAF,
	SESSDIR_TREE_SPLIT_H,	/* top/bottom */
	SESSDIR_TREE_SPLIT_V,	/* left/right */
};

struct sessdir_tree_node {
	enum sessdir_tree_type	type;
	int			split_pos;	/* numerator/256 for splits */
	int			win_num;	/* window number for leaves */
	struct sessdir_tree_node *a, *b;	/* children for splits */
};

struct sessdir_screen_layout {
	int				focus;	/* window number, or -1 */
	struct sessdir_tree_node	*root;	/* caller must free via
						   sessdir_tree_free() */
};

void sessdir_tree_free(struct sessdir_tree_node *n);

/* ---- layout mode tag ---- */

enum sessdir_layout_mode {
	SESSDIR_LAYOUT_NONE,
	SESSDIR_LAYOUT_TURBO,
	SESSDIR_LAYOUT_SCREEN,
};

/* probe which mode the layout file contains.
 * returns SESSDIR_LAYOUT_NONE if no file, unrecognized, or an old version. */
enum sessdir_layout_mode sessdir_layout_mode(const char *session);

/* ---- load ---- */

/* load turbo layout. returns 0 on success, -1 on error, wrong mode, or an
 * old version. */
int sessdir_layout_load_turbo(const char *session,
    struct sessdir_turbo_layout *out);

/* load screen layout. returns 0 on success, -1 on error, wrong mode, or an
 * old version.
 * caller must free out->root via sessdir_tree_free(). */
int sessdir_layout_load_screen(const char *session,
    struct sessdir_screen_layout *out);

/* ---- generation (13-d) ----
 *
 * Each save bumps a monotonic counter, written via temp-plus-rename so a
 * reader never sees a torn write. A writer compares this against the
 * generation it last saw before saving its own change, so it can catch up
 * on a newer generation it has not applied yet instead of blindly
 * overwriting it -- see attach.c's mirror_publish() (12B share.display).
 */

/* returns 0 if there is no file, no GEN= line, or either could not be
 * read -- a save always writes 1 or higher, so 0 never collides with a
 * real generation. */
unsigned long sessdir_layout_generation(const char *session);

/* ---- save ---- */

/* save turbo layout, bumping the generation. returns 0 on success, -1 on
 * error. */
int sessdir_layout_save_turbo(const char *session,
    const struct sessdir_turbo_layout *layout);

/* save screen layout, bumping the generation. returns 0 on success, -1
 * on error. */
int sessdir_layout_save_screen(const char *session,
    const struct sessdir_screen_layout *layout);

#endif /* SESSDIR_LAYOUT_H */
