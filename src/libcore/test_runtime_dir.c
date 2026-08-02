/* test_runtime_dir.c : tests for the runtime directory safety check */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "runtime_dir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* ---- helpers ---- */

static char tmpbase[64];

static void
base_create(void)
{
	snprintf(tmpbase, sizeof(tmpbase), "/tmp/lumi-rtdir-XXXXXX");
	if (!mkdtemp(tmpbase)) {
		perror("mkdtemp");
		exit(1);
	}
}

static void
base_destroy(void)
{
	char cmd[128];

	/* the tree is a handful of empty directories and one file */
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpbase);
	if (system(cmd) < 0)
		perror("rm -rf");
}

static void
sub(char *out, size_t outsz, const char *name)
{
	snprintf(out, outsz, "%s/%s", tmpbase, name);
}

static int
mode_of(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return -1;
	return (int)(st.st_mode & 07777);
}

/* ---- tests ---- */

static void
test_creates_private(void)
{
	char path[128];

	TEST("creates a missing directory as 0700");

	sub(path, sizeof(path), "fresh");
	ASSERT(lu_runtime_dir_ensure(path) == 0, "ensure failed");
	ASSERT(mode_of(path) == 0700, "directory is not 0700");

	PASS();
}

static void
test_accepts_existing(void)
{
	char path[128];

	TEST("accepts a directory it already made");

	sub(path, sizeof(path), "again");
	ASSERT(lu_runtime_dir_ensure(path) == 0, "first ensure failed");
	ASSERT(lu_runtime_dir_ensure(path) == 0, "second ensure failed");

	PASS();
}

/* a loose mode on a directory we own is a umask accident, so it is
 * repaired rather than treated as an attack. */
static void
test_tightens_loose_mode(void)
{
	char path[128];

	TEST("tightens a world-writable directory it owns");

	sub(path, sizeof(path), "loose");
	if (mkdir(path, 0777) < 0) {
		FAIL("mkdir failed");
		return;
	}
	if (chmod(path, 0777) < 0) {	/* defeat the umask */
		FAIL("chmod failed");
		return;
	}
	ASSERT(mode_of(path) == 0777, "setup did not produce 0777");

	ASSERT(lu_runtime_dir_ensure(path) == 0, "ensure refused our dir");
	ASSERT(mode_of(path) == 0700, "mode was not tightened");

	PASS();
}

/* the /tmp attack shape: something else is already sitting at the path.
 * mkdir returns EEXIST, and the old code carried on regardless. */
static void
test_rejects_symlink(void)
{
	char path[128], target[128];

	TEST("refuses a symlink to a directory");

	sub(target, sizeof(target), "real");
	sub(path, sizeof(path), "link");
	if (mkdir(target, 0700) < 0) {
		FAIL("mkdir failed");
		return;
	}
	if (symlink(target, path) < 0) {
		FAIL("symlink failed");
		return;
	}

	ASSERT(lu_runtime_dir_ensure(path) == -1, "symlink was accepted");

	PASS();
}

static void
test_rejects_regular_file(void)
{
	char path[128];
	FILE *f;

	TEST("refuses a regular file");

	sub(path, sizeof(path), "file");
	f = fopen(path, "w");
	if (!f) {
		FAIL("fopen failed");
		return;
	}
	fclose(f);

	ASSERT(lu_runtime_dir_ensure(path) == -1, "regular file accepted");

	PASS();
}

/* ---- lu_runtime_dir_ensure_mode() ---- */

static void
test_mode_creates_requested_mode(void)
{
	char path[128];

	TEST("ensure_mode creates a missing directory at the given mode");

	sub(path, sizeof(path), "broker-group");
	ASSERT(lu_runtime_dir_ensure_mode(path, 0710) == 0, "ensure failed");
	ASSERT(mode_of(path) == 0710, "directory is not 0710");

	PASS();
}

static void
test_mode_sets_sticky_world(void)
{
	char path[128];

	TEST("ensure_mode creates a sticky world-traversable directory");

	sub(path, sizeof(path), "broker-world");
	ASSERT(lu_runtime_dir_ensure_mode(path, 01733) == 0, "ensure failed");
	ASSERT(mode_of(path) == 01733, "directory is not 01733");

	PASS();
}

/* a directory made by an earlier, differently-configured broker run should
 * end up at exactly the mode this call asked for, not just be accepted or
 * only ever loosened. */
static void
test_mode_corrects_existing_mode(void)
{
	char path[128];

	TEST("ensure_mode corrects an existing directory to the given mode");

	sub(path, sizeof(path), "broker-remode");
	ASSERT(lu_runtime_dir_ensure_mode(path, 0700) == 0, "first ensure failed");
	ASSERT(mode_of(path) == 0700, "setup did not produce 0700");

	ASSERT(lu_runtime_dir_ensure_mode(path, 0710) == 0,
	    "second ensure failed");
	ASSERT(mode_of(path) == 0710, "mode was not corrected to 0710");

	PASS();
}

static void
test_mode_rejects_symlink(void)
{
	char path[128], target[128];

	TEST("ensure_mode refuses a symlink to a directory");

	sub(target, sizeof(target), "mode-real");
	sub(path, sizeof(path), "mode-link");
	if (mkdir(target, 0710) < 0) {
		FAIL("mkdir failed");
		return;
	}
	if (symlink(target, path) < 0) {
		FAIL("symlink failed");
		return;
	}

	ASSERT(lu_runtime_dir_ensure_mode(path, 0710) == -1,
	    "symlink was accepted");

	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("runtime_dir tests:\n");

	base_create();

	test_creates_private();
	test_accepts_existing();
	test_tightens_loose_mode();
	test_rejects_symlink();
	test_rejects_regular_file();

	test_mode_creates_requested_mode();
	test_mode_sets_sticky_world();
	test_mode_corrects_existing_mode();
	test_mode_rejects_symlink();

	base_destroy();

	/* a directory owned by another uid is the case the check exists
	 * for, and it cannot be built without privileges.  the uid
	 * comparison it relies on is exercised by every test above. */

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
