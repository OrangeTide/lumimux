/* sessdir_access.h : cross-user access control for a shared session */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef SESSDIR_ACCESS_H
#define SESSDIR_ACCESS_H

#include <sys/types.h>

/*
 * <session>/access is the ACL a broker (lumi-proxy in listen mode,
 * lumi-net-proxy) consults before letting a foreign-uid or networked peer
 * touch a session. See doc/FUTURE.md 12C for the design this implements.
 *
 * File format, mode 0600, owned by the session's own uid:
 *
 *   # subject          role    options
 *   owner              write
 *   user:alice         write   ask
 *   group:devs         view
 *   key:bob@laptop     view    ask
 *   *                  deny
 *
 * Subjects are "owner", "user:<name|uid>", "group:<name|gid>",
 * "key:<name>" (netchan peers only), or "*". The role column is a ceiling,
 * not a grant: a subject with "write" who attaches asking for "view" gets
 * "view". "ask" admits the connection pending approval rather than
 * granting or refusing it outright.
 *
 * First matching subject wins; an unmatched peer is denied (the implicit
 * final rule). A line whose subject cannot be parsed at all is treated as
 * matching every peer, so a typo can only take access away, never grant
 * it. A line whose subject matches but whose role or options cannot be
 * parsed denies that match rather than falling through to a later line
 * that might have allowed it.
 *
 * A missing, unreadable, or wrongly owned/moded file falls back to
 * owner-only rather than erroring open: fail closed, not fail open.
 */

enum sessdir_access_role {
	SESSDIR_ACCESS_DENY = 0,
	SESSDIR_ACCESS_VIEW,
	SESSDIR_ACCESS_WRITE,
};

/* The identity behind a connection: either a local uid/gid (from
 * ipc_peer_cred()) or a netchan keystore key name -- never both. */
struct sessdir_access_who {
	int	is_key;
	uid_t	uid;
	gid_t	gid;
	char	key[64];
};

struct sessdir_access_decision {
	int	role;		/* the ceiling: DENY, VIEW, or WRITE */
	int	ask;		/* nonzero: admit pending, hold for approval */
	char	subject[40];	/* which rule (or fail-closed case) decided
				 * this, for the audit log */
};

/* Evaluate <session>/access for who. owner_uid is the session-owning uid,
 * matched by the "owner" subject; it is the caller's job to skip this
 * check entirely for same-uid tier-0 connections. Always fills *out.
 * Returns 0 normally; -1 only if who or out is NULL. */
int sessdir_access_check(const char *session, uid_t owner_uid,
    const struct sessdir_access_who *who, struct sessdir_access_decision *out);

/* Append one line to <session>/access.log (mode 0600, created if absent):
 * timestamp, identity, the subject sessdir_access_check() recorded,
 * requested role, and granted role (the caller's min(requested, dec->role),
 * DENY on refusal). Best-effort: a logging failure is not itself a reason
 * to deny a connection. */
void sessdir_access_audit(const char *session,
    const struct sessdir_access_who *who,
    const struct sessdir_access_decision *dec, int requested_role,
    int granted_role);

/* Role name for display and logging: "deny", "view", or "write". */
const char *sessdir_access_role_name(int role);

#endif /* SESSDIR_ACCESS_H */
