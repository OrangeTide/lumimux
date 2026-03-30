/* sessdir.h : session directory management for micro-server architecture */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSDIR_H
#define SESSDIR_H

#include <sys/types.h>

/* base directory for all sessions: $XDG_RUNTIME_DIR/lumi/ or
 * /tmp/lumi-<uid>/. returns malloc'd path, caller frees.
 * creates the directory if it does not exist. */
char *sessdir_base(void);

/* path to a named session directory. returns malloc'd path.
 * does not create the directory. */
char *sessdir_session_path(const char *name);

/* path to a server's PID directory within a session. returns malloc'd. */
char *sessdir_server_path(const char *session, pid_t pid);

/* create session directory. idempotent. returns 0 on success, -1 on error. */
int sessdir_session_create(const char *name);

/* create server PID directory within a session.
 * returns 0 on success, -1 on error. */
int sessdir_server_create(const char *session, pid_t pid);

/* remove server PID directory and its contents. */
void sessdir_server_destroy(const char *session, pid_t pid);

/* write a descriptor file (socket, pty, title) in a server directory.
 * overwrites if it already exists. returns 0 on success, -1 on error. */
int sessdir_write_file(const char *session, pid_t pid,
    const char *name, const char *content);

/* read a descriptor file. returns malloc'd content, or NULL on error.
 * trailing newline is stripped. */
char *sessdir_read_file(const char *session, pid_t pid, const char *name);

/* list session names under the base directory.
 * fills names[] with malloc'd strings, up to max entries.
 * returns number of sessions found, or -1 on error. */
int sessdir_list_sessions(char **names, int max);

/* list server PIDs in a session directory.
 * fills pids[], up to max entries.
 * returns number of servers found, or -1 on error. */
int sessdir_list_servers(const char *session, pid_t *pids, int max);

/* check if a server process is alive. returns 1 if alive, 0 if dead. */
int sessdir_server_alive(pid_t pid);

/* remove PID directories for dead server processes.
 * returns number of stale entries removed. */
int sessdir_cleanup_stale(const char *session);

#endif /* SESSDIR_H */
