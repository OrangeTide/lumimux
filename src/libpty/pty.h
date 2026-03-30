/* pty.h : pseudo-terminal allocation and management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PTY_H
#define PTY_H

/** Open a new PTY pair and fork a child process.
 *
 * Returns the master fd on success (child gets the slave).
 * Stores the child PID in *child_pid. Returns -1 on failure.
 */
int pty_open(int *child_pid, const char *shell);
int pty_resize(int master_fd, int rows, int cols);
void pty_close(int master_fd);

#endif /* PTY_H */
