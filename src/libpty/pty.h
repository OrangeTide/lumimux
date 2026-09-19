/* pty.h : pseudo-terminal allocation and management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PTY_H
#define PTY_H

/** Open a new PTY pair and fork a child process.
 *
 * The slave is born at rows x cols, so the child sees the right size before
 * it execs rather than through a resize that races its startup.
 *
 * argv is the NULL-terminated program and arguments to run (argv[0] is the
 * program). Pass NULL to run the user's login shell.
 *
 * Returns the master fd on success (child gets the slave).
 * Stores the child PID in *child_pid. Returns -1 on failure.
 */
int pty_open(int *child_pid, char *const argv[], int rows, int cols);
int pty_resize(int master_fd, int rows, int cols, int xpixel, int ypixel);
void pty_close(int master_fd);

#endif /* PTY_H */
