/* test_access.c : unit tests for cross-user session access control */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "sessdir.h"
#include "sessdir_access.h"

#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define CHECK(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", \
		    __FILE__, __LINE__, (msg)); \
		fail_count++; \
	} \
} while (0)

static char test_dir[] = "/tmp/test_access_XXXXXX";

static void
setup(void)
{
	if (!mkdtemp(test_dir)) {
		perror("mkdtemp");
		exit(1);
	}
	setenv("XDG_RUNTIME_DIR", test_dir, 1);
}

static void
teardown(void)
{
	char cmd[4096];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_dir);
	system(cmd);
}

/* write <session>/access with the given content and mode. */
static void
write_acl(const char *session, const char *content, mode_t mode)
{
	char *dir, path[PATH_MAX];
	int fd;

	sessdir_session_create(session);
	dir = sessdir_session_path(session);
	CHECK(dir != NULL, "sessdir_session_path");
	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/access", dir);
	free(dir);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	CHECK(fd >= 0, "open access file");
	if (fd < 0)
		return;
	if (content)
		(void)!write(fd, content, strlen(content));
	close(fd);
	chmod(path, mode);	/* explicit: creat()'s mode is umask-filtered */
}

/* ---- tests ---- */

static void
test_no_acl_owner_only(void)
{
	struct sessdir_access_who owner, stranger;
	struct sessdir_access_decision dec;

	sessdir_session_create("noacl");

	memset(&owner, 0, sizeof(owner));
	owner.uid = getuid();
	memset(&stranger, 0, sizeof(stranger));
	stranger.uid = getuid() + 12345;	/* almost certainly not us */

	sessdir_access_check("noacl", getuid(), &owner, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_WRITE, "owner granted write with no acl");

	sessdir_access_check("noacl", getuid(), &stranger, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY, "stranger denied with no acl");
}

static void
test_wrong_mode_falls_back_to_owner_only(void)
{
	struct sessdir_access_who owner, stranger;
	struct sessdir_access_decision dec;
	char acl[128];

	memset(&owner, 0, sizeof(owner));
	owner.uid = getuid();
	memset(&stranger, 0, sizeof(stranger));
	stranger.uid = getuid() + 12345;

	/* world-readable: must be refused even though it would otherwise
	 * grant the stranger write access. */
	snprintf(acl, sizeof(acl), "* write\n");
	write_acl("badmode", acl, 0644);

	sessdir_access_check("badmode", getuid(), &stranger, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY,
	    "wrong-mode acl falls back to owner-only, denying a stranger");
	sessdir_access_check("badmode", getuid(), &owner, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_WRITE,
	    "wrong-mode acl still grants the owner via fallback");
}

static void
test_subject_matching(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;
	struct passwd *pw;
	char acl[512];
	gid_t mygid = getgid();

	pw = getpwuid(getuid());
	CHECK(pw != NULL, "getpwuid(getuid()) resolves");

	snprintf(acl, sizeof(acl),
	    "# comment line\n"
	    "\n"
	    "owner              write\n"
	    "user:%s            view    ask\n"
	    "group:%lu          view\n"
	    "key:bob@laptop     view    ask\n"
	    "*                  deny\n",
	    pw ? pw->pw_name : "nobody", (unsigned long)mygid);
	write_acl("subj", acl, 0600);

	memset(&who, 0, sizeof(who));
	who.uid = getuid();
	sessdir_access_check("subj", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_WRITE, "owner subject matches first");
	CHECK(strcmp(dec.subject, "owner") == 0, "owner subject label");

	/* a different uid, but sharing our primary group: skips "owner"
	 * (uid differs) and the "user:" line (name differs), matches
	 * "group:", not the trailing "*". */
	memset(&who, 0, sizeof(who));
	who.uid = getuid() + 12345;
	who.gid = mygid;
	sessdir_access_check("subj", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_VIEW && !dec.ask,
	    "group subject grants view without ask");

	/* a netchan key identity. */
	memset(&who, 0, sizeof(who));
	who.is_key = 1;
	snprintf(who.key, sizeof(who.key), "bob@laptop");
	sessdir_access_check("subj", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_VIEW && dec.ask,
	    "key subject grants view with ask");

	/* an unrelated identity falls through to the trailing wildcard. */
	memset(&who, 0, sizeof(who));
	who.uid = getuid() + 99999;
	who.gid = mygid + 99999;
	sessdir_access_check("subj", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY, "unmatched identity hits '*' deny");
	CHECK(strcmp(dec.subject, "*") == 0, "wildcard subject label");
}

static void
test_first_match_wins(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;
	char acl[256];

	/* a specific rule before a permissive wildcard: the specific one
	 * must win even though the wildcard would also match. */
	snprintf(acl, sizeof(acl), "user:%lu deny\n* write\n",
	    (unsigned long)getuid());
	write_acl("order", acl, 0600);

	memset(&who, 0, sizeof(who));
	who.uid = getuid();
	sessdir_access_check("order", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY,
	    "first matching subject wins over a later, more permissive one");
}

static void
test_implicit_deny(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;

	write_acl("implicit", "owner write\n", 0600);

	memset(&who, 0, sizeof(who));
	who.uid = getuid() + 12345;
	sessdir_access_check("implicit", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY,
	    "no matching line denies via the implicit final rule");
	CHECK(strcmp(dec.subject, "(implicit deny)") == 0,
	    "implicit deny subject label");
}

static void
test_malformed_line_denies(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;
	char acl[256];

	/* the matching line has a garbage role: must deny, not fall
	 * through to the permissive wildcard below it. */
	snprintf(acl, sizeof(acl), "user:%lu bogus\n* write\n",
	    (unsigned long)getuid());
	write_acl("malformed", acl, 0600);

	memset(&who, 0, sizeof(who));
	who.uid = getuid();
	sessdir_access_check("malformed", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY,
	    "a malformed matching line denies rather than falling through");

	/* an unparsable subject matches everyone (fail closed): even an
	 * unrelated uid is denied by it before reaching the wildcard. */
	write_acl("malformed2", "not-a-real-subject write\n* write\n", 0600);
	memset(&who, 0, sizeof(who));
	who.uid = getuid() + 55555;
	sessdir_access_check("malformed2", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY,
	    "an unparsable subject denies everyone rather than matching no one");
}

static void
test_live_reload(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;

	memset(&who, 0, sizeof(who));
	who.uid = getuid() + 12345;

	write_acl("reload", "* deny\n", 0600);
	sessdir_access_check("reload", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_DENY, "reload: denied before the edit");

	/* no reload call of any kind -- the module re-reads the file on
	 * every check, so an on-disk edit takes effect immediately. */
	write_acl("reload", "* write\n", 0600);
	sessdir_access_check("reload", getuid(), &who, &dec);
	CHECK(dec.role == SESSDIR_ACCESS_WRITE, "reload: granted after the edit");
}

static void
test_audit_log(void)
{
	struct sessdir_access_who who;
	struct sessdir_access_decision dec;
	char *dir, path[PATH_MAX], line[512];
	FILE *f;
	struct stat st;

	sessdir_session_create("audit");
	memset(&who, 0, sizeof(who));
	who.uid = getuid();

	dec.role = SESSDIR_ACCESS_VIEW;
	dec.ask = 0;
	snprintf(dec.subject, sizeof(dec.subject), "owner");
	sessdir_access_audit("audit", &who, &dec, SESSDIR_ACCESS_WRITE,
	    SESSDIR_ACCESS_VIEW);

	dir = sessdir_session_path("audit");
	CHECK(dir != NULL, "sessdir_session_path for audit log");
	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/access.log", dir);
	free(dir);

	CHECK(stat(path, &st) == 0, "access.log created");
	CHECK((st.st_mode & 0777) == 0600, "access.log is mode 0600");

	f = fopen(path, "r");
	CHECK(f != NULL, "access.log opens");
	if (f) {
		CHECK(fgets(line, sizeof(line), f) != NULL,
		    "access.log has a line");
		CHECK(strstr(line, "requested=write") != NULL,
		    "access.log records the requested role");
		CHECK(strstr(line, "granted=view") != NULL,
		    "access.log records the granted role");
		CHECK(strstr(line, "subject=owner") != NULL,
		    "access.log records the matched subject");
		fclose(f);
	}
}

int
main(void)
{
	setup();

	printf("access tests:\n");

	test_no_acl_owner_only();
	test_wrong_mode_falls_back_to_owner_only();
	test_subject_matching();
	test_first_match_wins();
	test_implicit_deny();
	test_malformed_line_denies();
	test_live_reload();
	test_audit_log();

	teardown();

	printf("\ntest_access: %d tests, %d failures\n", test_count,
	    fail_count);
	return fail_count > 0 ? 1 : 0;
}
