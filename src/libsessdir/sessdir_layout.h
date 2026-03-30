/* sessdir_layout.h : per-session window layout persistence */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSDIR_LAYOUT_H
#define SESSDIR_LAYOUT_H

#include <stdint.h>

/*
 * Layout file: <session>/layout  (dotenv format)
 *
 * Turbo mode stores per-window geometry keyed by window-order index:
 *
 *   MODE=turbo
 *   FOCUS=0
 *   WIN_0="2 2 40 12"
 *   WIN_1="4 3 40 12"
 *
 * Screen mode stores the split tree as a preorder traversal:
 *
 *   MODE=screen
 *   FOCUS=0
 *   TREE="v128 h128 0 1 2"
 *
 * Tree tokens:  v<pos> = vertical split (split_pos/256)
 *               h<pos> = horizontal split
 *               <N>    = leaf with window-order index N
 *
 * Window-order indices correspond to WINDOW_ORDER in the state file.
 * PIDs change across restarts; indices provide a stable mapping.
 */

#define SESSDIR_LAYOUT_MAX_WINS	32

/* ---- turbo layout ---- */

struct sessdir_turbo_win {
	int	x, y, w, h;
	int	valid;		/* nonzero if this entry was populated */
};

struct sessdir_turbo_layout {
	int				focus;	/* window-order index, or -1 */
	struct sessdir_turbo_win	wins[SESSDIR_LAYOUT_MAX_WINS];
	int				nwins;
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
	int			win_index;	/* window-order index for leaves */
	struct sessdir_tree_node *a, *b;	/* children for splits */
};

struct sessdir_screen_layout {
	int				focus;	/* window-order index, or -1 */
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
 * returns SESSDIR_LAYOUT_NONE if no file or unrecognized. */
enum sessdir_layout_mode sessdir_layout_mode(const char *session);

/* ---- load ---- */

/* load turbo layout. returns 0 on success, -1 on error or wrong mode. */
int sessdir_layout_load_turbo(const char *session,
    struct sessdir_turbo_layout *out);

/* load screen layout. returns 0 on success, -1 on error or wrong mode.
 * caller must free out->root via sessdir_tree_free(). */
int sessdir_layout_load_screen(const char *session,
    struct sessdir_screen_layout *out);

/* ---- save ---- */

/* save turbo layout. returns 0 on success, -1 on error. */
int sessdir_layout_save_turbo(const char *session,
    const struct sessdir_turbo_layout *layout);

/* save screen layout. returns 0 on success, -1 on error. */
int sessdir_layout_save_screen(const char *session,
    const struct sessdir_screen_layout *layout);

#endif /* SESSDIR_LAYOUT_H */
