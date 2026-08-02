/* runtime_dir.c : safe creation of the per-user runtime directory */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "runtime_dir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* O_DIRECTORY and O_NOFOLLOW are POSIX.1-2008; keep building where an
 * older header lacks them, at the cost of the symlink guarantee. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

int
lu_runtime_dir_ensure_mode(const char *path, mode_t mode)
{
	struct stat st;
	int fd, rc = -1;

	if (mkdir(path, mode) < 0 && errno != EEXIST) {
		fprintf(stderr, "lumi: cannot create %s: %s\n", path,
		    strerror(errno));
		return -1;
	}

	/* inspect the directory through an fd so the checks below and the
	 * repair cannot be raced by a rename between them.  O_NOFOLLOW
	 * fails on a symlink, which is one of the ways the directory could
	 * be someone else's. */
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lumi: cannot open %s: %s\n", path,
		    strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "lumi: cannot stat %s: %s\n", path,
		    strerror(errno));
		goto out;
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "lumi: %s is not a directory\n", path);
		errno = ENOTDIR;
		goto out;
	}
	if (st.st_uid != getuid()) {
		fprintf(stderr, "lumi: %s is owned by uid %ld, not %ld; "
		    "refusing to use it\n", path, (long)st.st_uid,
		    (long)getuid());
		errno = EACCES;
		goto out;
	}
	if ((st.st_mode & 07777) != (mode & 07777)) {
		/* ours, but not exactly the mode this caller needs (loose
		 * from a umask accident, or tight from a leftover run with a
		 * different mode).  set it exactly: owning the directory
		 * already means being able to do anything a stricter mode
		 * would have blocked anyway. */
		if (fchmod(fd, mode) < 0) {
			fprintf(stderr, "lumi: %s is mode %04o and cannot be "
			    "set to %04o: %s\n", path,
			    (unsigned)(st.st_mode & 07777), (unsigned)mode,
			    strerror(errno));
			goto out;
		}
	}
	rc = 0;
out:
	close(fd);
	return rc;
}

int
lu_runtime_dir_ensure(const char *path)
{
	return lu_runtime_dir_ensure_mode(path, 0700);
}
