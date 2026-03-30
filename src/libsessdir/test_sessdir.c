/* test_sessdir.c : unit tests for session directory management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "sessdir_state.h"
#include "sessdir_watch.h"

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

/* use a temp directory as XDG_RUNTIME_DIR so tests don't pollute
 * the real session directory */
static char test_dir[] = "/tmp/test_sessdir_XXXXXX";

static void
setup(void)
{
	if (!mkdtemp(test_dir)) {
		perror("mkdtemp");
		exit(1);
	}
	setenv("XDG_RUNTIME_DIR", test_dir, 1);
}

/* recursive rm -r (simple, only one level of nesting expected) */
static void
rmdir_r(const char *path)
{
	char cmd[4096];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	system(cmd);
}

static void
teardown(void)
{
	rmdir_r(test_dir);
}

/* ---- directory operations tests ---- */

static void
test_base(void)
{
	char *base = sessdir_base();

	printf("  sessdir_base ... ");
	CHECK(base != NULL, "sessdir_base returned NULL");
	if (base) {
		struct stat sb;

		CHECK(stat(base, &sb) == 0, "base dir should exist");
		CHECK(S_ISDIR(sb.st_mode), "base should be a directory");
		free(base);
	}
	printf("ok\n");
}

static void
test_session_create(void)
{
	char *path;
	struct stat sb;

	printf("  sessdir_session_create ... ");
	CHECK(sessdir_session_create("test1") == 0, "create failed");
	path = sessdir_session_path("test1");
	CHECK(path != NULL, "session_path returned NULL");
	if (path) {
		CHECK(stat(path, &sb) == 0, "session dir should exist");
		CHECK(S_ISDIR(sb.st_mode), "should be a directory");
		free(path);
	}
	/* idempotent */
	CHECK(sessdir_session_create("test1") == 0, "second create failed");
	printf("ok\n");
}

static void
test_server_create_destroy(void)
{
	char *path;
	struct stat sb;

	printf("  sessdir_server_create/destroy ... ");
	sessdir_session_create("test2");
	CHECK(sessdir_server_create("test2", 12345) == 0, "create failed");

	path = sessdir_server_path("test2", 12345);
	CHECK(path != NULL, "server_path returned NULL");
	if (path) {
		CHECK(stat(path, &sb) == 0, "server dir should exist");
		free(path);
	}

	sessdir_server_destroy("test2", 12345);
	path = sessdir_server_path("test2", 12345);
	if (path) {
		CHECK(stat(path, &sb) != 0, "server dir should be gone");
		free(path);
	}
	printf("ok\n");
}

static void
test_write_read_file(void)
{
	char *val;

	printf("  sessdir_write/read_file ... ");
	sessdir_session_create("test3");
	sessdir_server_create("test3", 99999);

	CHECK(sessdir_write_file("test3", 99999, "title",
	    "my window") == 0, "write failed");
	CHECK(sessdir_write_file("test3", 99999, "pty",
	    "/dev/pts/7") == 0, "write pty failed");

	val = sessdir_read_file("test3", 99999, "title");
	CHECK(val != NULL, "read returned NULL");
	if (val) {
		CHECK(strcmp(val, "my window") == 0,
		    "title mismatch");
		free(val);
	}

	val = sessdir_read_file("test3", 99999, "pty");
	CHECK(val != NULL, "read pty returned NULL");
	if (val) {
		CHECK(strcmp(val, "/dev/pts/7") == 0,
		    "pty mismatch");
		free(val);
	}

	sessdir_server_destroy("test3", 99999);
	printf("ok\n");
}

static void
test_list_sessions(void)
{
	char *names[16];
	int n, i;

	printf("  sessdir_list_sessions ... ");
	sessdir_session_create("alpha");
	sessdir_session_create("beta");

	n = sessdir_list_sessions(names, 16);
	CHECK(n >= 2, "should find at least 2 sessions");
	for (i = 0; i < n; i++)
		free(names[i]);
	printf("ok\n");
}

static void
test_list_servers(void)
{
	pid_t pids[16];
	int n;

	printf("  sessdir_list_servers ... ");
	sessdir_session_create("test4");
	sessdir_server_create("test4", 11111);
	sessdir_server_create("test4", 22222);
	sessdir_server_create("test4", 33333);

	n = sessdir_list_servers("test4", pids, 16);
	CHECK(n == 3, "should find 3 servers");

	sessdir_server_destroy("test4", 11111);
	sessdir_server_destroy("test4", 22222);
	sessdir_server_destroy("test4", 33333);
	printf("ok\n");
}

static void
test_server_alive(void)
{
	printf("  sessdir_server_alive ... ");
	CHECK(sessdir_server_alive(getpid()) == 1,
	    "own PID should be alive");
	CHECK(sessdir_server_alive(99999999) == 0,
	    "bogus PID should be dead");
	printf("ok\n");
}

static void
test_cleanup_stale(void)
{
	int removed;

	printf("  sessdir_cleanup_stale ... ");
	sessdir_session_create("test5");
	/* create directories for PIDs that don't exist */
	sessdir_server_create("test5", 99999991);
	sessdir_server_create("test5", 99999992);

	removed = sessdir_cleanup_stale("test5");
	CHECK(removed == 2, "should remove 2 stale entries");

	/* verify they're gone */
	{
		pid_t pids[16];
		int n = sessdir_list_servers("test5", pids, 16);

		CHECK(n == 0, "no servers should remain");
	}
	printf("ok\n");
}

/* ---- state file tests ---- */

static void
test_state_empty(void)
{
	struct sessdir_state *st;

	printf("  state empty file ... ");
	sessdir_session_create("st1");
	st = sessdir_state_open("st1");
	CHECK(st != NULL, "state_open failed");
	if (st) {
		CHECK(sessdir_state_focus(st) == 0,
		    "empty state should have focus 0");
		{
			pid_t order[16];
			int n = sessdir_state_order(st, order, 16);

			CHECK(n == 0, "empty state should have 0 windows");
		}
		sessdir_state_close(st);
	}
	printf("ok\n");
}

static void
test_state_add_servers(void)
{
	struct sessdir_state *st;
	pid_t order[16];
	int n;

	printf("  state add servers ... ");
	sessdir_session_create("st2");
	st = sessdir_state_open("st2");
	CHECK(st != NULL, "state_open failed");
	if (!st)
		return;

	CHECK(sessdir_state_add_server(st, 100) == 0, "add 100 failed");
	CHECK(sessdir_state_add_server(st, 200) == 0, "add 200 failed");
	CHECK(sessdir_state_add_server(st, 300) == 0, "add 300 failed");

	/* first server added should become focus */
	CHECK(sessdir_state_focus(st) == 100, "focus should be 100");

	n = sessdir_state_order(st, order, 16);
	CHECK(n == 3, "should have 3 entries");
	CHECK(order[0] == 100, "order[0] should be 100");
	CHECK(order[1] == 200, "order[1] should be 200");
	CHECK(order[2] == 300, "order[2] should be 300");

	/* duplicate add should be no-op */
	CHECK(sessdir_state_add_server(st, 200) == 0, "dup add failed");
	n = sessdir_state_order(st, order, 16);
	CHECK(n == 3, "still 3 after dup add");

	sessdir_state_close(st);
	printf("ok\n");
}

static void
test_state_set_focus(void)
{
	struct sessdir_state *st;

	printf("  state set focus ... ");
	sessdir_session_create("st3");
	st = sessdir_state_open("st3");
	if (!st)
		return;

	sessdir_state_add_server(st, 100);
	sessdir_state_add_server(st, 200);
	sessdir_state_add_server(st, 300);

	CHECK(sessdir_state_set_focus(st, 200) == 0, "set focus failed");
	CHECK(sessdir_state_focus(st) == 200, "focus should be 200");

	sessdir_state_close(st);
	printf("ok\n");
}

static void
test_state_remove_server(void)
{
	struct sessdir_state *st;
	pid_t order[16];
	int n;

	printf("  state remove server ... ");
	sessdir_session_create("st4");
	st = sessdir_state_open("st4");
	if (!st)
		return;

	sessdir_state_add_server(st, 100);
	sessdir_state_add_server(st, 200);
	sessdir_state_add_server(st, 300);

	/* remove middle */
	CHECK(sessdir_state_remove_server(st, 200) == 0,
	    "remove 200 failed");
	n = sessdir_state_order(st, order, 16);
	CHECK(n == 2, "should have 2 entries");
	CHECK(order[0] == 100, "order[0] should be 100");
	CHECK(order[1] == 300, "order[1] should be 300");

	/* remove focused -- focus should move */
	sessdir_state_set_focus(st, 100);
	CHECK(sessdir_state_remove_server(st, 100) == 0,
	    "remove focused failed");
	CHECK(sessdir_state_focus(st) == 300,
	    "focus should move to 300");

	/* remove last */
	CHECK(sessdir_state_remove_server(st, 300) == 0,
	    "remove last failed");
	CHECK(sessdir_state_focus(st) == 0,
	    "focus should be 0 when empty");

	sessdir_state_close(st);
	printf("ok\n");
}

static void
test_state_persistence(void)
{
	struct sessdir_state *st;
	pid_t order[16];
	int n;

	printf("  state persistence across open/close ... ");
	sessdir_session_create("st5");

	/* write state */
	st = sessdir_state_open("st5");
	if (!st)
		return;
	sessdir_state_add_server(st, 400);
	sessdir_state_add_server(st, 500);
	sessdir_state_set_focus(st, 500);
	sessdir_state_close(st);

	/* reopen and verify */
	st = sessdir_state_open("st5");
	CHECK(st != NULL, "reopen failed");
	if (!st)
		return;

	CHECK(sessdir_state_focus(st) == 500,
	    "focus should persist as 500");
	n = sessdir_state_order(st, order, 16);
	CHECK(n == 2, "should have 2 entries");
	CHECK(order[0] == 400, "order[0] should be 400");
	CHECK(order[1] == 500, "order[1] should be 500");

	sessdir_state_close(st);
	printf("ok\n");
}

/* ---- inotify watch test ---- */

static void
test_watch(void)
{
	int wfd;

	printf("  sessdir_watch start/stop ... ");
	sessdir_session_create("wt1");

	wfd = sessdir_watch_start("wt1");
	CHECK(wfd >= 0, "watch_start failed");

	if (wfd >= 0) {
		/* create a server dir -- should generate an event */
		sessdir_server_create("wt1", 77777);

		/* give inotify a moment */
		usleep(10000);

		{
			int flags = sessdir_watch_read(wfd);

			CHECK(flags & SESSDIR_WATCH_CHANGED,
			    "should detect directory creation");
		}

		sessdir_server_destroy("wt1", 77777);
		sessdir_watch_stop(wfd);
	}
	printf("ok\n");
}

/* ---- main ---- */

int
main(void)
{
	setup();

	printf("sessdir tests:\n");

	/* directory operations */
	test_base();
	test_session_create();
	test_server_create_destroy();
	test_write_read_file();
	test_list_sessions();
	test_list_servers();
	test_server_alive();
	test_cleanup_stale();

	/* state file */
	test_state_empty();
	test_state_add_servers();
	test_state_set_focus();
	test_state_remove_server();
	test_state_persistence();

	/* inotify watch */
	test_watch();

	teardown();

	printf("\ntest_sessdir: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
