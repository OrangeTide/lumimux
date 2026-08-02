/* ipc.h : Unix domain socket helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <sys/types.h>

/* defined on platforms where ipc_peer_cred() can report the credentials of
 * the far end of a connected Unix domain socket.  code that authorizes a
 * peer by uid or gid must be compiled out where this is undefined: there is
 * no safe fallback, so the caller refuses the peer instead. */
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || \
    (defined(__sun) && defined(__SVR4))
#define IPC_HAVE_PEER_CRED 1
#endif

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

/* report the credentials the kernel recorded for the peer of a connected
 * Unix domain socket.  they are captured at connect() time and cannot be
 * forged by the peer, not even by a later exec of a setuid binary.
 * uid, gid, and pid may each be NULL.  *pid is set to -1 where the platform
 * does not report one, and is safe to display but never to decide with:
 * pids are reused, so authorization must use the uid or the gid.
 * returns 0 on success, -1 on error (errno ENOTSUP where unsupported). */
int ipc_peer_cred(int fd, uid_t *uid, gid_t *gid, pid_t *pid);

/* close a socket fd and optionally unlink the path */
void ipc_close(int fd);

/* unlink a socket path */
void ipc_unlink(const char *path);

/* connect to session, send an empty message, close.
 * cmd_name is used in error messages (e.g. "lumi-kill").
 * returns 0 on success, -1 on error. */
int ipc_command(const char *name, uint32_t msg_type, const char *cmd_name);

#endif /* IPC_H */
