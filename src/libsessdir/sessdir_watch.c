/* sessdir_watch.c : directory watcher for session changes */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "sessdir_watch.h"
#include "sessdir.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define USE_KQUEUE 1
#else
#define USE_INOTIFY 1
#endif

#if USE_KQUEUE

#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

int
sessdir_watch_start(const char *session)
{
	char *path;
	int kq, dirfd;
	struct kevent ev;

	path = sessdir_session_path(session);
	if (!path)
		return -1;

	dirfd = open(path, O_RDONLY | O_DIRECTORY);
	free(path);
	if (dirfd < 0)
		return -1;

	kq = kqueue();
	if (kq < 0) {
		int saved = errno;
		close(dirfd);
		errno = saved;
		return -1;
	}

	/* NOTE_WRITE on a directory fires when an entry is added, removed,
	 * or renamed -- not when an existing file's contents change (12-d-b
	 * needs to notice <session>/access being rewritten in place, e.g.
	 * a plain fopen(path, "w") that truncates and rewrites without
	 * touching the directory entry itself). Unlike the inotify branch
	 * below, there is no per-directory kqueue flag that covers child
	 * content writes; catching that would mean watching each file of
	 * interest individually, which this generic directory watcher does
	 * not do. Untested on any BSD/macOS machine, same as the routing
	 * assumption already noted in net_proxy.c's fork-per-client path --
	 * flagged here rather than silently assumed fixed by NOTE_WRITE. */
	EV_SET(&ev, dirfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
	    NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0, NULL);
	if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
		int saved = errno;
		close(kq);
		close(dirfd);
		errno = saved;
		return -1;
	}

	return kq;
}

void
sessdir_watch_stop(int watch_fd)
{
	struct kevent ev;
	struct timespec ts = {0, 0};
	int n;

	if (watch_fd < 0)
		return;

	/* retrieve the dir fd stashed as the kevent ident */
	n = kevent(watch_fd, NULL, 0, &ev, 1, &ts);
	if (n > 0)
		close((int)ev.ident);
	close(watch_fd);
}

int
sessdir_watch_read(int watch_fd)
{
	struct kevent ev;
	struct timespec ts = {0, 0};

	if (kevent(watch_fd, NULL, 0, &ev, 1, &ts) > 0)
		return SESSDIR_WATCH_CHANGED;
	return 0;
}

#elif USE_INOTIFY

#include <sys/inotify.h>

int
sessdir_watch_start(const char *session)
{
	char *path;
	int fd, wd;

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0)
		return -1;

	path = sessdir_session_path(session);
	if (!path) {
		close(fd);
		return -1;
	}

	/* IN_CLOSE_WRITE catches a file being rewritten in place (12-d-b
	 * needs to notice <session>/access changing, which a plain
	 * fopen(path, "w")+fclose() does without ever touching the
	 * directory entry itself, so IN_CREATE/DELETE/MOVED alone would
	 * miss it). */
	wd = inotify_add_watch(fd, path,
	    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
	    IN_CLOSE_WRITE);
	free(path);
	if (wd < 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}

	return fd;
}

void
sessdir_watch_stop(int watch_fd)
{
	if (watch_fd >= 0)
		close(watch_fd);
}

int
sessdir_watch_read(int watch_fd)
{
	char buf[4096];
	ssize_t n;
	int flags = 0;

	for (;;) {
		n = read(watch_fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		flags |= SESSDIR_WATCH_CHANGED;
	}
	return flags;
}

#else
#error "No filesystem watch API available (need inotify or kqueue)"
#endif
