/* sessdir_state.h : session state file with flock() concurrency */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSDIR_STATE_H
#define SESSDIR_STATE_H

#include <sys/types.h>

/*
 * Session state is stored in a dotenv/shell-compatible flat file:
 *
 *   FOCUS=12345
 *   WINDOW_ORDER="12345 12351 12360"
 *   WINDOW_NUMS="12345 0 12360"
 *
 * WINDOW_ORDER is a dense layout/z-order list (no gaps). WINDOW_NUMS is a
 * slot map where the slot index is the stable, user-visible window number
 * and the value is the server PID; a 0 marks a spare (a number freed when
 * a window closed). Window numbers are never reassigned when other windows
 * close, so terminated windows leave gaps; a new window takes the lowest
 * free number.
 *
 * Concurrency: flock(LOCK_SH) for reads, flock(LOCK_EX) for writes.
 * Writes use atomic rename (write to temp file, then rename).
 *
 * Values may be unquoted or double-quoted. Inside double quotes,
 * backslash escapes \\ \" \$ \` are recognized. This is compatible
 * with sh, awk, make, and python.
 */

struct sessdir_state;

/* open the state file for a session. the file is created if it does
 * not exist. returns NULL on error. */
struct sessdir_state *sessdir_state_open(const char *session);

/* close the state file and release locks. */
void sessdir_state_close(struct sessdir_state *st);

/* get the focused server PID. returns 0 if no focus is set. */
pid_t sessdir_state_focus(struct sessdir_state *st);

/* get the window order as an array of PIDs.
 * fills out[], up to max entries.
 * returns number of entries, or -1 on error. */
int sessdir_state_order(struct sessdir_state *st, pid_t *out, int max);

/* set the focused server PID. returns 0 on success, -1 on error. */
int sessdir_state_set_focus(struct sessdir_state *st, pid_t pid);

/* add a server to the window order. appended at the end.
 * returns 0 on success, -1 on error. */
int sessdir_state_add_server(struct sessdir_state *st, pid_t pid);

/* remove a server from the window order. the server's stable window
 * number is freed (left as a spare); other windows keep their numbers.
 * if the removed server was focused, focus moves to the next server in
 * order (or 0 if empty). returns 0 on success, -1 on error. */
int sessdir_state_remove_server(struct sessdir_state *st, pid_t pid);

/* get the stable window number assigned to a server PID.
 * returns the number (>= 0), or -1 if the PID has no number. */
int sessdir_state_num(struct sessdir_state *st, pid_t pid);

/* get the stable window-number map: out[number] = PID, 0 marks a spare.
 * fills up to max slots; returns the number of slots, or -1 on error. */
int sessdir_state_nums(struct sessdir_state *st, pid_t *out, int max);

/* swap the stable window numbers of two server PIDs. both must already
 * have numbers. returns 0 on success, -1 on error. */
int sessdir_state_swap_num(struct sessdir_state *st, pid_t a, pid_t b);

#endif /* SESSDIR_STATE_H */
