/* runtime_dir.h : safe creation of the per-user runtime directory */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef RUNTIME_DIR_H
#define RUNTIME_DIR_H

#include <sys/types.h>

/* Create the runtime directory at path if it is missing, then verify that
 * it is safe to keep session sockets and control files in.
 *
 * The check matters for the /tmp/lumi-<uid> fallback used when
 * XDG_RUNTIME_DIR is unset: /tmp is world writable, so another user can
 * win the race to create that directory and own everything placed inside
 * it afterwards.  mkdir() reporting EEXIST is not evidence that the
 * directory is ours.
 *
 * The directory must be a real directory rather than a symlink to one,
 * owned by the calling user, and carry no group or other permission bits.
 * A directory we own with loose bits is tightened to 0700 rather than
 * refused, since that is a umask accident rather than an attack.
 *
 * Returns 0 when the directory is present and safe, -1 otherwise, with a
 * diagnostic on stderr naming the path.
 */
int lu_runtime_dir_ensure(const char *path);

/* Same checks as lu_runtime_dir_ensure(), but for a directory that is
 * meant to be reachable by other uids on purpose -- a cross-user broker
 * endpoint's directory, which needs group or world traverse bits rather
 * than 0700. mode is created and enforced exactly (set, not just loosened
 * or tightened toward), since owning the directory already means being
 * able to do anything a stricter mode would have blocked anyway. Ownership
 * and the not-a-symlink check are identical to lu_runtime_dir_ensure().
 */
int lu_runtime_dir_ensure_mode(const char *path, mode_t mode);

#endif /* RUNTIME_DIR_H */
