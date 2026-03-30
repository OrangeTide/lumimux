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

/* remove a server from the window order. if the removed server was
 * focused, focus moves to the next server in order (or 0 if empty).
 * returns 0 on success, -1 on error. */
int sessdir_state_remove_server(struct sessdir_state *st, pid_t pid);

#endif /* SESSDIR_STATE_H */
