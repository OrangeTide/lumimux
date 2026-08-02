/* sessdir_control.h : session write token and client roster */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef SESSDIR_CONTROL_H
#define SESSDIR_CONTROL_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Shared attach needs two things that are session-wide rather than
 * per-window: who holds the keyboard, and who is attached.
 *
 * The keyboard is an advisory lock on <session>/control.lock. Each mserver
 * also refuses input from a client it did not grant the keyboard to, but
 * that decision is per window, and a client attaches to every window in
 * the session: two clients could each win a different one. The lock is
 * what makes the answer the same everywhere. It is held for the lifetime
 * of the holding process, so the kernel releases it if that process dies
 * and no crash can leave a session with nobody able to type.
 *
 * The roster is a directory per client under <session>/clients/, which is
 * how "who else is here" survives the fact that no process owns the
 * session as a whole.
 */

#define SESSDIR_CLIENT_MAX	16

struct sessdir_client {
	unsigned long	client_id;	/* the client's own id (its pid) */
	pid_t		pid;
	char		name[64];	/* "user@host", display only */
	char		who[24];	/* "uid:1001" or "key:name" for a
					 * broker-relayed client; empty for
					 * the tier-0 attach.c case, which is
					 * always the owner's own uid */
	char		role[8];	/* "write" or "view" */
	char		mode[8];	/* screen, turbo, minimal, or "-" when
					 * a broker registered on the client's
					 * behalf and cannot know it */
	char		coupling[8];	/* "mirror" or "free" for a view-role
					 * tier-0 client (step 10); "-" for a
					 * write-role client or a broker
					 * registration, neither of which the
					 * concept applies to */
	int		rows, cols;
	long		since;		/* attach time, seconds */
	int		pending;	/* 12-d-c: an "ask" match admitted but
					 * not yet approved -- sent no replay
					 * and no output until it is */
};

/* Take the session's write token, or fail because someone else has it.
 *
 * The lock is kept until sessdir_token_release() or process exit. Records
 * the holder's id, pid, and name in the file so another client can say who
 * has the keyboard. Returns 0 on success, -1 if it is held or on error.
 */
int sessdir_token_acquire(const char *session, unsigned long client_id,
    const char *name);

/* Drop the token if this process holds it. Idempotent. */
void sessdir_token_release(void);

/* Nonzero if this process holds the token. */
int sessdir_token_held(void);

/* Drop this process's reference to the token without releasing the lock.
 *
 * For use immediately after fork(). flock() belongs to the open file
 * description, which fork shares, so a child would otherwise hold its
 * parent's lock and go on holding it after the parent let go. Closing the
 * child's descriptor drops only the child's reference: the parent's own
 * descriptor keeps the lock. Calling this in the parent would make it
 * forget a lock it still holds, which is why it is not the same call as
 * sessdir_token_release().
 */
void sessdir_token_forget(void);

/* Who holds the token, for display.
 *
 * Reads the name and pid recorded by the holder. A holder that died left
 * its line behind, so the pid is checked for liveness and a dead one
 * reports as no holder. Returns 0 when a live holder was found, -1
 * otherwise.
 */
int sessdir_token_holder(const char *session, char *name, size_t namesz,
    pid_t *pid);

/* Add or update this client's roster entry. Returns 0 or -1. */
int sessdir_client_register(const char *session,
    const struct sessdir_client *c);

/* Remove a roster entry. Missing is not an error. */
void sessdir_client_unregister(const char *session, unsigned long client_id);

/* List roster entries, up to max. Returns the count, or -1 on error. */
int sessdir_client_list(const char *session, struct sessdir_client *out,
    int max);

/* ---- share control messages ----
 *
 * Passing the keyboard between clients needs one client to tell another
 * something, and no process owns the session to route it. A single line in
 * <session>/control.msg does it: every client reads it when the session
 * directory changes and acts on the verbs addressed to it.
 *
 * The sequence number is what makes a client act once. It rises with each
 * post, and a client remembers the last one it saw, so re-reading the file
 * (a rescan, a poll, a second event for the same write) does nothing.
 */

enum sessdir_ctl_verb {
	SESSDIR_CTL_NONE = 0,
	SESSDIR_CTL_REQUEST,	/* actor asks the holder for the keyboard */
	SESSDIR_CTL_GRANT,	/* target is to take the keyboard */
	SESSDIR_CTL_DENY,	/* target's request was refused */
	SESSDIR_CTL_KICK,	/* target is to detach */
	SESSDIR_CTL_APPROVE,	/* target, a pending "ask" client, is admitted */
	SESSDIR_CTL_REJECT,	/* target, a pending "ask" client, is refused */
};

struct sessdir_ctl {
	unsigned long	seq;
	int		verb;
	unsigned long	target;	/* client the verb is addressed to, 0 = all */
	unsigned long	actor;	/* client that posted it, 0 = a command */
	char		name[64];	/* the actor's display name */
};

/* Post a control message, bumping the sequence number. Returns 0 or -1. */
int sessdir_ctl_post(const char *session, int verb, unsigned long target,
    unsigned long actor, const char *name);

/* Read the current control message. Returns 0 when one was read, -1 when
 * there is none. A caller acts only when out->seq is higher than the last
 * one it acted on. */
int sessdir_ctl_read(const char *session, struct sessdir_ctl *out);

/* ---- attach lock ----
 *
 * Marks a session as closed to further clients. Cooperating clients
 * refuse to attach; it keeps someone from joining by accident, and is not
 * a defense against a client that does not care, which is what the broker
 * in a later phase is for.
 */
int sessdir_share_set_locked(const char *session, int locked);
int sessdir_share_locked(const char *session);

/* ---- share mode (12B multi-writer) ----
 *
 * <session>/control holds session-wide settings every mserver and client
 * needs to agree on, starting with whether more than one connection may
 * hold WRITE at once. Distinct from control.lock (the write-token flock)
 * and control.msg (the request/approval mailbox) even though the name is
 * close -- this one is a plain settings file, not a lock or a mailbox.
 *
 * There is no live-update path: a mode change takes effect for the next
 * connection that attaches (mserver.c's role_for() reads it fresh on
 * every call), not for one already connected. Promoting an already-
 * connected VIEW client would need a directory watch in every mserver
 * process (one per window), which is a lot of new inotify usage for a
 * setting that is expected to change rarely; reattaching or lumi share -g
 * both already exist as ways to pick up a mode change immediately.
 */
enum sessdir_share_mode {
	SESSDIR_SHARE_SINGLE_WRITER = 0,	/* default, including "no file" */
	SESSDIR_SHARE_MULTI_WRITER,
};

/* Read the current mode. Never fails outright: a missing or unreadable
 * file, or one with an unrecognized MODE= value, reads as single-writer,
 * the same fail-safe default sessdir_access_check() uses for a missing
 * ACL -- the more restrictive behavior is always the safe default. */
int sessdir_share_mode_get(const char *session);

/* Write the mode (temp-plus-rename, so a reader never sees a half
 * written file). Returns 0 or -1. */
int sessdir_share_mode_set(const char *session, int mode);

/* 13-d: independent is the default -- a client keeps its own layout and
 * focus even in multi-writer mode. shared means every client mirrors one
 * layout and focus and any of them may change it, the same following
 * mechanism step 10 already built for a MIRROR-coupled viewer, just also
 * applied to a WRITE-role client when this is set. Meaningless outside
 * multi-writer mode, since a lone writer has nothing to follow. */
enum sessdir_share_display {
	SESSDIR_SHARE_DISPLAY_INDEPENDENT = 0,	/* default */
	SESSDIR_SHARE_DISPLAY_SHARED,
};

int sessdir_share_display_get(const char *session);
int sessdir_share_display_set(const char *session, int display);

/* Remove roster entries whose process is gone. Returns the count removed.
 * Called from sessdir_cleanup_stale(), which every client runs on attach
 * and on every rescan, so a client that was killed does not linger in the
 * roster. */
int sessdir_client_prune(const char *session);

#endif /* SESSDIR_CONTROL_H */
