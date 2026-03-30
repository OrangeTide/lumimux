/* ipc.h : Unix domain socket helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef IPC_H
#define IPC_H

#include <stdint.h>

/* return the socket directory path (caller must free).
 * uses $XDG_RUNTIME_DIR/lumi/ or /tmp/lumi-<uid>/ as fallback.
 * creates the directory if it doesn't exist. */
char *ipc_socket_dir(void);

/* return the full socket path for a session name (caller must free) */
char *ipc_socket_path(const char *name);

/* create a listening Unix domain socket at path.
 * returns fd on success, -1 on error. */
int ipc_listen(const char *path);

/* accept a connection on a listening socket.
 * returns client fd, -1 on error. */
int ipc_accept(int listen_fd);

/* connect to a Unix domain socket at path.
 * returns fd on success, -1 on error. */
int ipc_connect(const char *path);

/* close a socket fd and optionally unlink the path */
void ipc_close(int fd);

/* unlink a socket path */
void ipc_unlink(const char *path);

/* connect to session, send an empty message, close.
 * cmd_name is used in error messages (e.g. "lumi-kill").
 * returns 0 on success, -1 on error. */
int ipc_command(const char *name, uint32_t msg_type, const char *cmd_name);

#endif /* IPC_H */
