/* test_sessdir.c : unit tests for session directory management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "sessdir.h"
#include "sessdir_state.h"
#include "sessdir_watch.h"
#include "sessdir_control.h"
#include "sessdir_layout.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/* stable window numbers: closing a window leaves a spare, never renumbers
 * the others; a new window takes the lowest free number. */
static void
test_state_window_nums(void)
{
	struct sessdir_state *st;
	pid_t order[16], nums[16];
	int n;

	printf("  state stable window numbers ... ");
	sessdir_session_create("st6");
	st = sessdir_state_open("st6");
	if (!st)
		return;

	sessdir_state_add_server(st, 100);
	sessdir_state_add_server(st, 200);
	sessdir_state_add_server(st, 300);

	CHECK(sessdir_state_num(st, 100) == 0, "100 should be number 0");
	CHECK(sessdir_state_num(st, 200) == 1, "200 should be number 1");
	CHECK(sessdir_state_num(st, 300) == 2, "300 should be number 2");

	/* close the middle window: 100 and 300 keep their numbers, 200's
	 * number becomes a spare. */
	CHECK(sessdir_state_remove_server(st, 200) == 0, "remove 200");
	CHECK(sessdir_state_num(st, 100) == 0, "100 stays number 0");
	CHECK(sessdir_state_num(st, 300) == 2, "300 stays number 2");
	CHECK(sessdir_state_num(st, 200) == -1, "200 has no number");

	n = sessdir_state_nums(st, nums, 16);
	CHECK(n == 3, "nums map keeps the gap (3 slots)");
	CHECK(nums[0] == 100 && nums[1] == 0 && nums[2] == 300,
	    "slot 1 is a spare");

	/* layout order stays dense (no gap) */
	n = sessdir_state_order(st, order, 16);
	CHECK(n == 2 && order[0] == 100 && order[1] == 300,
	    "order compacts to [100 300]");

	/* a new window fills the lowest free number (the spare at 1) */
	sessdir_state_add_server(st, 400);
	CHECK(sessdir_state_num(st, 400) == 1, "400 reuses spare number 1");

	/* with no spare left, the next window extends the map */
	sessdir_state_add_server(st, 500);
	CHECK(sessdir_state_num(st, 500) == 3, "500 gets number 3");

	/* swap number with a neighbor */
	CHECK(sessdir_state_swap_num(st, 100, 500) == 0, "swap 100<->500");
	CHECK(sessdir_state_num(st, 100) == 3, "100 now number 3");
	CHECK(sessdir_state_num(st, 500) == 0, "500 now number 0");
	CHECK(sessdir_state_swap_num(st, 100, 999) == -1,
	    "swap with unknown pid fails");

	/* set_num onto a busy slot swaps the two windows.  after the swap
	 * above the map is [500 400 300 100] with no spares. */
	CHECK(sessdir_state_set_num(st, 100, 2) == 0, "set 100 -> slot 2");
	CHECK(sessdir_state_num(st, 100) == 2, "100 now number 2");
	CHECK(sessdir_state_num(st, 300) == 3, "300 pushed to slot 3");

	/* set_num to a fresh high slot extends the map, sparing the old
	 * slot; map becomes [500 400 100 0 0 0 0 300]. */
	CHECK(sessdir_state_set_num(st, 300, 7) == 0, "set 300 -> slot 7");
	CHECK(sessdir_state_num(st, 300) == 7, "300 now number 7");
	n = sessdir_state_nums(st, nums, 16);
	CHECK(n == 8 && nums[3] == 0, "old slot 3 becomes a spare");

	/* set_num onto a spare slot moves the window, old slot spared */
	CHECK(sessdir_state_set_num(st, 100, 4) == 0, "set 100 -> spare 4");
	CHECK(sessdir_state_num(st, 100) == 4, "100 now number 4");
	n = sessdir_state_nums(st, nums, 16);
	CHECK(nums[2] == 0, "old slot 2 is now a spare");
	CHECK(sessdir_state_num(st, 500) == 0, "500 keeps number 0");

	/* out-of-range and unknown-pid targets fail */
	CHECK(sessdir_state_set_num(st, 100, -1) == -1, "negative fails");
	CHECK(sessdir_state_set_num(st, 999, 0) == -1, "unknown pid fails");

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
	if (wfd < 0 && (errno == EMFILE || errno == ENFILE)) {
		/* the per-user inotify instance limit is exhausted (a busy
		 * desktop can hold them all); this is an environment
		 * condition, not a defect, so skip rather than fail */
		printf("skipped (inotify instances exhausted)\n");
		return;
	}
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

/* ---- write token ---- */

/* One keyboard per session. The lock is what makes the answer the same on
 * every window, so what matters is that a second holder is impossible and
 * that a dead holder does not keep it. */
static void
test_token_exclusive(void)
{
	printf("  token is held by one process at a time ... ");
	sessdir_session_create("tok");

	CHECK(sessdir_token_acquire("tok", 111, "first@host") == 0,
	    "first acquire failed");
	CHECK(sessdir_token_held(), "held flag not set");

	/* a second process, since flock is per open file description and a
	 * second acquire in this process would just be ours again */
	{
		pid_t pid = fork();
		int status = 0;

		if (pid == 0) {
			/* fork shares the open file description, so drop the
			 * inherited reference first: this has to be a real
			 * outside attempt, not the parent's own lock seen
			 * again */
			sessdir_token_forget();
			_exit(sessdir_token_acquire("tok", 222, "second@host")
			    == 0 ? 1 : 0);
		}
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "a second process took a held token");
	}

	{
		char who[64] = "";
		pid_t holder = 0;

		CHECK(sessdir_token_holder("tok", who, sizeof(who),
		    &holder) == 0, "holder not reported");
		CHECK(strcmp(who, "first@host") == 0, "wrong holder name");
		CHECK(holder == getpid(), "wrong holder pid");
	}

	sessdir_token_release();
	CHECK(!sessdir_token_held(), "held flag still set after release");
	CHECK(sessdir_token_holder("tok", NULL, 0, NULL) < 0,
	    "released token still reports a holder");

	/* and it can be taken again */
	CHECK(sessdir_token_acquire("tok", 333, "third@host") == 0,
	    "reacquire after release failed");
	sessdir_token_release();
	printf("ok\n");
}

/* A holder that dies must not keep the keyboard: the kernel drops the
 * lock, and the line it left behind names a pid that is gone. */
static void
test_token_survives_holder_death(void)
{
	pid_t pid;
	int status = 0;

	printf("  a dead holder does not keep the token ... ");
	sessdir_session_create("tokdead");

	pid = fork();
	if (pid == 0) {
		if (sessdir_token_acquire("tokdead", 444, "gone@host") != 0)
			_exit(1);
		_exit(0);		/* exits holding it */
	}
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child could not take the token");

	CHECK(sessdir_token_holder("tokdead", NULL, 0, NULL) < 0,
	    "a dead holder is still reported as holding it");
	CHECK(sessdir_token_acquire("tokdead", 555, "next@host") == 0,
	    "could not take the token from a dead holder");
	sessdir_token_release();
	printf("ok\n");
}

/* ---- client roster ---- */

static void
test_client_roster(void)
{
	struct sessdir_client me, got[SESSDIR_CLIENT_MAX];
	int n;

	printf("  roster entries round-trip ... ");
	sessdir_session_create("ros");

	memset(&me, 0, sizeof(me));
	me.client_id = 4242;
	me.pid = getpid();
	snprintf(me.name, sizeof(me.name), "someone@somewhere");
	snprintf(me.role, sizeof(me.role), "view");
	snprintf(me.mode, sizeof(me.mode), "turbo");
	me.rows = 40;
	me.cols = 132;
	me.since = 1234567;
	CHECK(sessdir_client_register("ros", &me) == 0, "register failed");

	n = sessdir_client_list("ros", got, SESSDIR_CLIENT_MAX);
	CHECK(n == 1, "expected one roster entry");
	if (n == 1) {
		CHECK(got[0].client_id == 4242, "wrong client id");
		CHECK(got[0].pid == getpid(), "wrong pid");
		CHECK(strcmp(got[0].name, "someone@somewhere") == 0,
		    "wrong name");
		CHECK(strcmp(got[0].role, "view") == 0, "wrong role");
		CHECK(strcmp(got[0].mode, "turbo") == 0, "wrong mode");
		CHECK(got[0].rows == 40 && got[0].cols == 132, "wrong size");
		CHECK(got[0].since == 1234567, "wrong attach time");
	}

	/* re-registering updates rather than duplicating */
	snprintf(me.role, sizeof(me.role), "write");
	CHECK(sessdir_client_register("ros", &me) == 0, "re-register failed");
	n = sessdir_client_list("ros", got, SESSDIR_CLIENT_MAX);
	CHECK(n == 1, "re-register duplicated the entry");
	if (n == 1)
		CHECK(strcmp(got[0].role, "write") == 0, "role not updated");

	sessdir_client_unregister("ros", 4242);
	CHECK(sessdir_client_list("ros", got, SESSDIR_CLIENT_MAX) == 0,
	    "entry survived unregister");
	printf("ok\n");
}

/* A client that was killed leaves its entry behind, so "who is attached"
 * has to be filtered by whether the process is still there. */
static void
test_client_prune(void)
{
	struct sessdir_client c, got[SESSDIR_CLIENT_MAX];
	pid_t dead;
	int status = 0;

	printf("  a dead client is pruned from the roster ... ");
	sessdir_session_create("pru");

	dead = fork();
	if (dead == 0)
		_exit(0);
	waitpid(dead, &status, 0);

	memset(&c, 0, sizeof(c));
	c.client_id = (unsigned long)dead;
	c.pid = dead;
	snprintf(c.name, sizeof(c.name), "gone@host");
	snprintf(c.role, sizeof(c.role), "write");
	CHECK(sessdir_client_register("pru", &c) == 0, "register failed");

	/* a live entry alongside it must survive */
	memset(&c, 0, sizeof(c));
	c.client_id = (unsigned long)getpid();
	c.pid = getpid();
	snprintf(c.name, sizeof(c.name), "here@host");
	snprintf(c.role, sizeof(c.role), "view");
	CHECK(sessdir_client_register("pru", &c) == 0, "register failed");

	CHECK(sessdir_client_list("pru", got, SESSDIR_CLIENT_MAX) == 2,
	    "expected two entries before pruning");
	CHECK(sessdir_client_prune("pru") == 1, "expected one prune");
	CHECK(sessdir_client_list("pru", got, SESSDIR_CLIENT_MAX) == 1,
	    "pruning removed the wrong number of entries");
	CHECK(sessdir_client_prune("pru") == 0, "pruned twice");

	/* and the general cleanup does it too, which is what every client
	 * calls on attach */
	sessdir_cleanup_stale("pru");
	CHECK(sessdir_client_list("pru", got, SESSDIR_CLIENT_MAX) == 1,
	    "cleanup_stale removed a live client");

	sessdir_client_unregister("pru", (unsigned long)getpid());
	printf("ok\n");
}

/* A name written by another client is drawn on this client's screen. A
 * peer that calls itself an escape sequence must not be able to paint on
 * someone else's display, so the roster strips anything a terminal would
 * act on. */
static void
test_client_name_sanitized(void)
{
	struct sessdir_client me, got[SESSDIR_CLIENT_MAX];
	int n;

	printf("  a hostile client name is defanged ... ");
	sessdir_session_create("evil");

	memset(&me, 0, sizeof(me));
	me.client_id = 5150;
	me.pid = getpid();
	snprintf(me.name, sizeof(me.name), "a\033[31mb");
	snprintf(me.role, sizeof(me.role), "view");
	CHECK(sessdir_client_register("evil", &me) == 0, "register failed");

	n = sessdir_client_list("evil", got, SESSDIR_CLIENT_MAX);
	CHECK(n == 1, "expected one entry");
	if (n == 1) {
		CHECK(strchr(got[0].name, '\033') == NULL,
		    "an escape survived the roster");
		CHECK(strcmp(got[0].name, "a?[31mb") == 0,
		    "the name was not defanged as expected");
	}

	/* a raw newline would end the line early and could forge a field:
	 * the writer emits it, the reader must not honor it */
	snprintf(me.name, sizeof(me.name), "x\ny");
	CHECK(sessdir_client_register("evil", &me) == 0, "register failed");
	n = sessdir_client_list("evil", got, SESSDIR_CLIENT_MAX);
	CHECK(n == 1, "newline in a name changed the entry count");
	if (n == 1)
		CHECK(strchr(got[0].name, '\n') == NULL,
		    "a newline survived the roster");

	sessdir_client_unregister("evil", 5150);
	printf("ok\n");
}

/* ---- share control messages ---- */

/* The sequence number is what makes a client act once. Everything else
 * about the message is just fields. */
static void
test_ctl_messages(void)
{
	struct sessdir_ctl m;

	printf("  control messages carry a rising sequence ... ");
	sessdir_session_create("ctl");

	CHECK(sessdir_ctl_read("ctl", &m) < 0, "read an absent message");

	CHECK(sessdir_ctl_post("ctl", SESSDIR_CTL_REQUEST, 0, 77,
	    "asker@host") == 0, "post failed");
	CHECK(sessdir_ctl_read("ctl", &m) == 0, "read failed");
	CHECK(m.seq == 1, "first message should be sequence 1");
	CHECK(m.verb == SESSDIR_CTL_REQUEST, "wrong verb");
	CHECK(m.actor == 77, "wrong actor");
	CHECK(strcmp(m.name, "asker@host") == 0, "wrong name");

	CHECK(sessdir_ctl_post("ctl", SESSDIR_CTL_GRANT, 77, 88,
	    "holder@host") == 0, "second post failed");
	CHECK(sessdir_ctl_read("ctl", &m) == 0, "second read failed");
	CHECK(m.seq == 2, "sequence did not rise");
	CHECK(m.verb == SESSDIR_CTL_GRANT, "wrong verb");
	CHECK(m.target == 77, "wrong target");

	CHECK(sessdir_ctl_post("ctl", SESSDIR_CTL_KICK, 99, 0, "") == 0,
	    "kick post failed");
	CHECK(sessdir_ctl_read("ctl", &m) == 0, "kick read failed");
	CHECK(m.verb == SESSDIR_CTL_KICK && m.target == 99, "wrong kick");
	CHECK(m.actor == 0, "a command should post no actor");
	printf("ok\n");
}

static void
test_share_lock(void)
{
	printf("  the attach lock is a session flag ... ");
	sessdir_session_create("lok");

	CHECK(!sessdir_share_locked("lok"), "a new session should be open");
	CHECK(sessdir_share_set_locked("lok", 1) == 0, "lock failed");
	CHECK(sessdir_share_locked("lok"), "lock did not take");
	CHECK(sessdir_share_set_locked("lok", 1) == 0, "relock failed");
	CHECK(sessdir_share_locked("lok"), "relock cleared it");
	CHECK(sessdir_share_set_locked("lok", 0) == 0, "unlock failed");
	CHECK(!sessdir_share_locked("lok"), "unlock did not take");
	printf("ok\n");
}

static void
test_share_mode(void)
{
	printf("  share mode defaults to single-writer and round-trips ... ");
	sessdir_session_create("modetest");

	CHECK(sessdir_share_mode_get("modetest") == SESSDIR_SHARE_SINGLE_WRITER,
	    "a new session should default to single-writer");
	CHECK(sessdir_share_mode_get("no-such-session") ==
	    SESSDIR_SHARE_SINGLE_WRITER,
	    "a missing session should default to single-writer");

	CHECK(sessdir_share_mode_set("modetest",
	    SESSDIR_SHARE_MULTI_WRITER) == 0, "set multi-writer failed");
	CHECK(sessdir_share_mode_get("modetest") == SESSDIR_SHARE_MULTI_WRITER,
	    "mode did not read back as multi-writer");

	CHECK(sessdir_share_mode_set("modetest",
	    SESSDIR_SHARE_SINGLE_WRITER) == 0, "set single-writer failed");
	CHECK(sessdir_share_mode_get("modetest") ==
	    SESSDIR_SHARE_SINGLE_WRITER,
	    "mode did not read back as single-writer");
	printf("ok\n");
}

static void
test_share_display(void)
{
	printf("  share display defaults to independent, round-trips, and "
	    "does not clobber mode ... ");
	sessdir_session_create("displaytest");

	CHECK(sessdir_share_display_get("displaytest") ==
	    SESSDIR_SHARE_DISPLAY_INDEPENDENT,
	    "a new session should default to independent");

	CHECK(sessdir_share_mode_set("displaytest",
	    SESSDIR_SHARE_MULTI_WRITER) == 0, "set multi-writer failed");
	CHECK(sessdir_share_display_set("displaytest",
	    SESSDIR_SHARE_DISPLAY_SHARED) == 0, "set shared failed");

	/* setting display must not have clobbered the mode set moments
	 * earlier, and vice versa -- both live in the same file */
	CHECK(sessdir_share_mode_get("displaytest") ==
	    SESSDIR_SHARE_MULTI_WRITER,
	    "setting display clobbered the mode");
	CHECK(sessdir_share_display_get("displaytest") ==
	    SESSDIR_SHARE_DISPLAY_SHARED,
	    "display did not read back as shared");

	CHECK(sessdir_share_mode_set("displaytest",
	    SESSDIR_SHARE_SINGLE_WRITER) == 0, "set single-writer failed");
	CHECK(sessdir_share_display_get("displaytest") ==
	    SESSDIR_SHARE_DISPLAY_SHARED,
	    "setting mode clobbered the display");
	printf("ok\n");
}

static void
test_layout_generation(void)
{
	struct sessdir_turbo_layout layout, got;

	printf("  layout saves bump a monotonic generation ... ");
	sessdir_session_create("layoutgen");

	CHECK(sessdir_layout_generation("layoutgen") == 0,
	    "a session with no layout yet should read generation 0");
	CHECK(sessdir_layout_generation("no-such-session") == 0,
	    "a missing session should read generation 0");

	memset(&layout, 0, sizeof(layout));
	layout.focus = -1;
	layout.wins[0].x = 1;
	layout.wins[0].y = 2;
	layout.wins[0].w = 40;
	layout.wins[0].h = 12;
	layout.wins[0].valid = 1;
	layout.nwins = 1;

	CHECK(sessdir_layout_save_turbo("layoutgen", &layout) == 0,
	    "first save failed");
	CHECK(sessdir_layout_generation("layoutgen") == 1,
	    "first save should be generation 1");

	CHECK(sessdir_layout_save_turbo("layoutgen", &layout) == 0,
	    "second save failed");
	CHECK(sessdir_layout_generation("layoutgen") == 2,
	    "second save should be generation 2, not reset");

	CHECK(sessdir_layout_load_turbo("layoutgen", &got) == 0,
	    "load failed");
	CHECK(got.nwins == 1 && got.wins[0].valid && got.wins[0].w == 40,
	    "the GEN= line must not disturb the rest of the format");
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
	test_state_window_nums();

	/* inotify watch */
	test_watch();

	/* write token and client roster */
	test_token_exclusive();
	test_token_survives_holder_death();
	test_client_roster();
	test_client_prune();
	test_client_name_sanitized();
	test_ctl_messages();
	test_share_lock();
	test_share_mode();
	test_share_display();
	test_layout_generation();

	teardown();

	printf("\ntest_sessdir: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
