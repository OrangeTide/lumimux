/* sessdir_watch.h : inotify watcher for session directory changes */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSDIR_WATCH_H
#define SESSDIR_WATCH_H

/*
 * Watch a session directory for server appear/disappear events.
 * Returns an fd suitable for iox_fd_add() with IOX_READ.
 *
 * When the fd becomes readable, call sessdir_watch_read() to drain
 * the events and get a bitmask of what changed.
 */

/* start watching a session directory. returns inotify fd, or -1 on error. */
int sessdir_watch_start(const char *session);

/* stop watching and close the inotify fd. */
void sessdir_watch_stop(int watch_fd);

/* drain pending inotify events. returns bitmask of changes. */
#define SESSDIR_WATCH_CHANGED	0x01
int sessdir_watch_read(int watch_fd);

#endif /* SESSDIR_WATCH_H */
