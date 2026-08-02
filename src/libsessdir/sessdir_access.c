/* sessdir_access.c : cross-user access control for a shared session */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "sessdir_access.h"

#include "sessdir.h"

#include <ctype.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* cap on supplementary groups checked for a "group:" subject; real systems
 * rarely approach this, and it keeps the lookup off the stack-eating
 * NGROUPS_MAX (65536 on Linux). */
#define ACCESS_MAX_GROUPS 64

/* max tokens read from one ACL line (subject, role, and a few options);
 * more than this is treated as malformed rather than silently truncated. */
#define ACCESS_MAX_TOKENS 8

static int
session_file(const char *session, const char *name, char *buf, size_t bufsz)
{
	char *dir;
	int n;

	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	n = snprintf(buf, bufsz, "%s/%s", dir, name);
	free(dir);
	return (n < 0 || (size_t)n >= bufsz) ? -1 : 0;
}

const char *
sessdir_access_role_name(int role)
{
	switch (role) {
	case SESSDIR_ACCESS_VIEW:
		return "view";
	case SESSDIR_ACCESS_WRITE:
		return "write";
	default:
		return "deny";
	}
}

/* ---- subject matching ---- */

enum subject_kind {
	SUBJ_BAD = -1,		/* unparsable: matches everyone (fail closed) */
	SUBJ_ANY,
	SUBJ_OWNER,
	SUBJ_USER,
	SUBJ_GROUP,
	SUBJ_KEY,
};

struct subject {
	int	kind;
	char	arg[64];	/* the name/id after "user:"/"group:"/"key:" */
};

static void
parse_subject(const char *tok, struct subject *s)
{
	s->arg[0] = '\0';
	if (strcmp(tok, "*") == 0) {
		s->kind = SUBJ_ANY;
	} else if (strcmp(tok, "owner") == 0) {
		s->kind = SUBJ_OWNER;
	} else if (strncmp(tok, "user:", 5) == 0 && tok[5] != '\0') {
		s->kind = SUBJ_USER;
		snprintf(s->arg, sizeof(s->arg), "%s", tok + 5);
	} else if (strncmp(tok, "group:", 6) == 0 && tok[6] != '\0') {
		s->kind = SUBJ_GROUP;
		snprintf(s->arg, sizeof(s->arg), "%s", tok + 6);
	} else if (strncmp(tok, "key:", 4) == 0 && tok[4] != '\0') {
		s->kind = SUBJ_KEY;
		snprintf(s->arg, sizeof(s->arg), "%s", tok + 4);
	} else {
		s->kind = SUBJ_BAD;
	}
}

/* nonzero if a peer with primary group peer_gid and uid uid belongs to the
 * group named or numbered by gidspec: peer_gid (as reported directly by
 * ipc_peer_cred(), needing no name lookup) covers the primary-group case
 * even for a uid with no local passwd entry; supplementary groups are
 * checked too (up to ACCESS_MAX_GROUPS) when uid does resolve to one. */
static int
uid_in_group(uid_t uid, gid_t peer_gid, const char *gidspec)
{
	struct passwd *pw;
	gid_t want, groups[ACCESS_MAX_GROUPS];
	int ngroups, i;
	char *end;

	want = (gid_t)strtoul(gidspec, &end, 10);
	if (*end != '\0') {
		struct group *gr = getgrnam(gidspec);

		if (!gr)
			return 0;
		want = gr->gr_gid;
	}

	if (peer_gid == want)
		return 1;

	pw = getpwuid(uid);
	if (!pw)
		return 0;
	if (pw->pw_gid == want)
		return 1;

	ngroups = ACCESS_MAX_GROUPS;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
		ngroups = 0;	/* more groups than we check; primary already
				 * ruled out above */
	for (i = 0; i < ngroups; i++) {
		if (groups[i] == want)
			return 1;
	}
	return 0;
}

static int
subject_matches(const struct subject *s, uid_t owner_uid,
    const struct sessdir_access_who *who)
{
	switch (s->kind) {
	case SUBJ_BAD:
		return 1;	/* fail closed: an unreadable rule matches
				 * everyone, so it can only deny, never skip
				 * past to something more permissive */
	case SUBJ_ANY:
		return 1;
	case SUBJ_OWNER:
		return !who->is_key && who->uid == owner_uid;
	case SUBJ_USER: {
		char *end;
		uid_t want;
		struct passwd *pw;

		if (who->is_key)
			return 0;
		want = (uid_t)strtoul(s->arg, &end, 10);
		if (*end == '\0')
			return who->uid == want;
		pw = getpwnam(s->arg);
		return pw != NULL && pw->pw_uid == who->uid;
	}
	case SUBJ_GROUP:
		return !who->is_key &&
		    uid_in_group(who->uid, who->gid, s->arg);
	case SUBJ_KEY:
		return who->is_key && strcmp(who->key, s->arg) == 0;
	default:
		return 0;
	}
}

/* ---- role parsing ---- */

static int
parse_role(const char *tok)
{
	if (strcmp(tok, "write") == 0)
		return SESSDIR_ACCESS_WRITE;
	if (strcmp(tok, "view") == 0)
		return SESSDIR_ACCESS_VIEW;
	if (strcmp(tok, "deny") == 0)
		return SESSDIR_ACCESS_DENY;
	return -1;
}

/* ---- file trust ---- */

/* refuse the ACL unless it is a regular file, mode 0600, and owned by
 * owner_uid. lstat (not stat) so a symlink cannot borrow another file's
 * trusted ownership. returns 0 if usable, -1 to fall back to owner-only. */
static int
acl_file_ok(const char *path, uid_t owner_uid)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return -1;
	if (!S_ISREG(st.st_mode))
		return -1;
	if (st.st_uid != owner_uid)
		return -1;
	if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
		return -1;
	if ((st.st_mode & S_IRWXU) != (S_IRUSR | S_IWUSR))
		return -1;
	return 0;
}

/* ---- one ACL line ---- */

/* split line in place on whitespace, honoring a '#' comment to end of
 * line. returns the token count, or -1 if more than ACCESS_MAX_TOKENS
 * tokens were present (truncated input is treated as malformed, not
 * silently accepted with the tail dropped). */
static int
tokenize(char *line, char *tok[ACCESS_MAX_TOKENS])
{
	char *p = line;
	int n = 0;
	size_t i;

	for (i = 0; line[i]; i++) {
		if (line[i] == '#' || line[i] == '\n') {
			line[i] = '\0';
			break;
		}
	}

	while (n < ACCESS_MAX_TOKENS) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			return n;
		tok[n++] = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (*p)
			*p++ = '\0';
	}
	while (*p && isspace((unsigned char)*p))
		p++;
	return *p ? -1 : n;
}

int
sessdir_access_check(const char *session, uid_t owner_uid,
    const struct sessdir_access_who *who, struct sessdir_access_decision *out)
{
	char path[PATH_MAX], line[256];
	FILE *f;

	if (!who || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->role = SESSDIR_ACCESS_DENY;

	/* owner-only fallback: no acl, or one this code refuses to trust. */
	if (session_file(session, "access", path, sizeof(path)) < 0 ||
	    acl_file_ok(path, owner_uid) < 0 ||
	    (f = fopen(path, "r")) == NULL) {
		if (!who->is_key && who->uid == owner_uid) {
			out->role = SESSDIR_ACCESS_WRITE;
			snprintf(out->subject, sizeof(out->subject), "owner");
		} else {
			snprintf(out->subject, sizeof(out->subject),
			    "(no acl: owner-only)");
		}
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		char *tok[ACCESS_MAX_TOKENS];
		struct subject subj;
		int ntok = tokenize(line, tok);

		if (ntok == 0)
			continue;		/* blank or comment-only */
		if (ntok < 0) {
			/* too many fields to be sure what was meant; treat
			 * as an unreadable subject so it can only deny */
			subj.kind = SUBJ_BAD;
		} else {
			parse_subject(tok[0], &subj);
		}
		if (!subject_matches(&subj, owner_uid, who))
			continue;

		/* the subject matches this peer: a bad role or option here
		 * denies the match rather than falling through to a line
		 * that might have allowed it. */
		if (ntok < 2) {
			snprintf(out->subject, sizeof(out->subject),
			    "(malformed line)");
			fclose(f);
			return 0;
		}
		{
			int role = parse_role(tok[1]);
			int ask = 0, bad_opt = 0, j;

			for (j = 2; j < ntok; j++) {
				if (strcmp(tok[j], "ask") == 0)
					ask = 1;
				else
					bad_opt = 1;
			}
			if (role < 0 || bad_opt || subj.kind == SUBJ_BAD) {
				snprintf(out->subject, sizeof(out->subject),
				    "(malformed: %.20s)", tok[0]);
				fclose(f);
				return 0;
			}
			out->role = role;
			out->ask = ask;
			snprintf(out->subject, sizeof(out->subject), "%.20s",
			    tok[0]);
		}
		fclose(f);
		return 0;
	}
	fclose(f);

	/* implicit final rule: no match is a deny */
	snprintf(out->subject, sizeof(out->subject), "(implicit deny)");
	return 0;
}

void
sessdir_access_audit(const char *session,
    const struct sessdir_access_who *who,
    const struct sessdir_access_decision *dec, int requested_role,
    int granted_role)
{
	char path[PATH_MAX];
	char idbuf[80];
	FILE *f;
	time_t now;
	struct tm tmv;
	char stamp[32];

	if (!who || session_file(session, "access.log", path, sizeof(path)) < 0)
		return;

	if (who->is_key)
		snprintf(idbuf, sizeof(idbuf), "key:%s", who->key);
	else
		snprintf(idbuf, sizeof(idbuf), "uid:%lu",
		    (unsigned long)who->uid);

	now = time(NULL);
	if (gmtime_r(&now, &tmv) == NULL ||
	    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
		snprintf(stamp, sizeof(stamp), "%ld", (long)now);

	/* "a" mode keeps concurrent writers from interleaving mid-line.
	 * the mode only takes effect on creation, so an already-existing,
	 * wrongly-moded log is left alone rather than silently rewritten. */
	f = fopen(path, "a");
	if (!f)
		return;
	(void)fchmod(fileno(f), 0600);
	fprintf(f, "%s %s subject=%s requested=%s granted=%s\n", stamp, idbuf,
	    dec ? dec->subject : "?",
	    sessdir_access_role_name(requested_role),
	    sessdir_access_role_name(granted_role));
	fclose(f);
}
