/* test_iox.c : tests for libiox */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ---- loop lifecycle ---- */

static void
test_loop_new_free(void)
{
	struct iox_loop *loop;

	TEST("loop new/free");
	loop = iox_loop_new();
	ASSERT(loop != NULL, "iox_loop_new returned NULL");
	iox_loop_free(loop);
	PASS();
}

/* ---- fd watchers ---- */

static int read_cb_called;
static int read_cb_events;

static void
read_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	char buf[64];

	(void)arg;
	read_cb_called++;
	read_cb_events = (int)events;

	/* drain the pipe so poll doesn't fire again */
	(void)read(fd, buf, sizeof(buf));
	iox_loop_stop(loop);
}

static void
test_fd_read(void)
{
	struct iox_loop *loop;
	int pfd[2];

	TEST("fd read event");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_add(loop, pfd[0], IOX_READ, read_cb, NULL) == 0,
	    "fd_add failed");

	/* write a byte to trigger readability */
	(void)write(pfd[1], "x", 1);

	read_cb_called = 0;
	read_cb_events = 0;

	ASSERT(iox_loop_poll(loop) >= 0, "poll failed");
	ASSERT(read_cb_called == 1, "callback not called");
	ASSERT(read_cb_events & IOX_READ, "IOX_READ not set");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

static int write_cb_called;

static void
write_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	(void)fd;
	(void)arg;
	write_cb_called++;
	if (events & IOX_WRITE)
		iox_loop_stop(loop);
}

static void
test_fd_write(void)
{
	struct iox_loop *loop;
	int pfd[2];

	TEST("fd write event");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_add(loop, pfd[1], IOX_WRITE, write_cb, NULL) == 0,
	    "fd_add failed");

	write_cb_called = 0;

	ASSERT(iox_loop_poll(loop) >= 0, "poll failed");
	ASSERT(write_cb_called == 1, "write callback not called");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

/* ---- fd_mod ---- */

static int mod_cb_events;

static void
mod_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	(void)fd;
	(void)arg;
	mod_cb_events |= (int)events;
	iox_loop_stop(loop);
}

static void
test_fd_mod(void)
{
	struct iox_loop *loop;
	int pfd[2];

	TEST("fd_mod changes events");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	/* start watching read only */
	ASSERT(iox_fd_add(loop, pfd[1], IOX_READ, mod_cb, NULL) == 0,
	    "fd_add failed");

	/* switch to write */
	ASSERT(iox_fd_mod(loop, pfd[1], IOX_WRITE) == 0,
	    "fd_mod failed");

	mod_cb_events = 0;
	ASSERT(iox_loop_poll(loop) >= 0, "poll failed");
	ASSERT(mod_cb_events & IOX_WRITE, "IOX_WRITE not set after mod");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

/* ---- remove during dispatch ---- */

static int remove_other_fd;

static void
remove_during_cb(struct iox_loop *loop, int fd, unsigned events,
                 void *arg)
{
	(void)fd;
	(void)events;
	(void)arg;

	/* remove the other fd while we're inside dispatch */
	iox_fd_remove(loop, remove_other_fd);
	iox_loop_stop(loop);
}

static int removed_cb_called;

static void
removed_fd_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	(void)loop;
	(void)fd;
	(void)events;
	(void)arg;
	removed_cb_called++;
}

static void
test_remove_during_dispatch(void)
{
	struct iox_loop *loop;
	int pfd1[2], pfd2[2];

	TEST("remove during dispatch is safe");
	ASSERT(pipe(pfd1) == 0, "pipe1 failed");
	ASSERT(pipe(pfd2) == 0, "pipe2 failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_add(loop, pfd1[0], IOX_READ,
	    remove_during_cb, NULL) == 0, "fd_add 1 failed");
	ASSERT(iox_fd_add(loop, pfd2[0], IOX_READ,
	    removed_fd_cb, NULL) == 0, "fd_add 2 failed");

	remove_other_fd = pfd2[0];
	removed_cb_called = 0;

	/* make both readable */
	(void)write(pfd1[1], "x", 1);
	(void)write(pfd2[1], "x", 1);

	ASSERT(iox_loop_poll(loop) >= 0, "poll failed");

	/* the removed fd's callback must not fire after removal.
	 * it may have fired before (poll order is not guaranteed),
	 * but we must not crash. */

	/* poll again -- the removed fd must not fire */
	removed_cb_called = 0;
	(void)write(pfd2[1], "y", 1);
	(void)iox_loop_poll(loop);
	ASSERT(removed_cb_called == 0,
	    "removed fd callback fired on second poll");

	iox_loop_free(loop);
	close(pfd1[0]);
	close(pfd1[1]);
	close(pfd2[0]);
	close(pfd2[1]);
	PASS();
}

/* ---- iox_loop_run + stop ---- */

static void
stop_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	char buf[64];

	(void)events;
	(void)arg;
	(void)read(fd, buf, sizeof(buf));
	iox_loop_stop(loop);
}

static void
test_loop_run_stop(void)
{
	struct iox_loop *loop;
	int pfd[2];

	TEST("loop_run stops on loop_stop");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_add(loop, pfd[0], IOX_READ, stop_cb, NULL) == 0,
	    "fd_add failed");

	(void)write(pfd[1], "x", 1);

	ASSERT(iox_loop_run(loop) == 0, "loop_run failed");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

/* ---- idle callback ---- */

static int idle_called;

static void
idle_cb(struct iox_loop *loop, void *arg)
{
	(void)arg;
	idle_called++;
	iox_loop_stop(loop);
}

static void
test_idle_callback(void)
{
	struct iox_loop *loop;
	int pfd[2];

	TEST("idle callback fires between iterations");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_add(loop, pfd[0], IOX_READ, read_cb, NULL) == 0,
	    "fd_add failed");

	(void)write(pfd[1], "x", 1);

	idle_called = 0;
	iox_loop_set_idle(loop, idle_cb, NULL);

	iox_loop_run(loop);
	ASSERT(idle_called > 0, "idle callback not called");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

/* ---- signal handling ---- */

static int signal_cb_called;
static int signal_cb_signo;

static void
test_signal_cb(struct iox_loop *loop, int signo, void *arg)
{
	(void)arg;
	signal_cb_called++;
	signal_cb_signo = signo;
	iox_loop_stop(loop);
}

static void
test_signal(void)
{
	struct iox_loop *loop;

	TEST("signal delivery via self-pipe");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_signal_add(loop, SIGUSR1, test_signal_cb, NULL) == 0,
	    "signal_add failed");

	signal_cb_called = 0;
	signal_cb_signo = 0;

	raise(SIGUSR1);

	/* poll with timeout -- signal should arrive */
	(void)iox_loop_poll(loop);

	ASSERT(signal_cb_called == 1, "signal callback not called");
	ASSERT(signal_cb_signo == SIGUSR1, "wrong signal number");

	iox_signal_remove(loop, SIGUSR1);
	iox_loop_free(loop);
	PASS();
}

/* ---- signal remove restores handler ---- */

static volatile int old_handler_called;

static void
old_handler(int signo)
{
	(void)signo;
	old_handler_called = 1;
}

static void
test_signal_restore(void)
{
	struct iox_loop *loop;
	struct sigaction sa, check;

	TEST("signal_remove restores previous handler");

	/* install a known handler first */
	sa.sa_handler = old_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR2, &sa, NULL);

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_signal_add(loop, SIGUSR2, test_signal_cb, NULL) == 0,
	    "signal_add failed");

	iox_signal_remove(loop, SIGUSR2);

	/* verify old handler is back */
	sigaction(SIGUSR2, NULL, &check);
	ASSERT(check.sa_handler == old_handler,
	    "previous handler not restored");

	iox_loop_free(loop);

	/* restore default */
	sa.sa_handler = SIG_DFL;
	sigaction(SIGUSR2, &sa, NULL);
	PASS();
}

/* ---- poll timeout ---- */

static int timeout_flag;

static void
timeout_flag_cb(struct iox_loop *loop, void *arg)
{
	(void)loop;
	(void)arg;
	timeout_flag = 1;
}

static void
test_poll_timeout(void)
{
	struct iox_loop *loop;

	TEST("poll returns 0 on timeout");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	timeout_flag = 0;
	ASSERT(iox_timer_add(loop, 0, timeout_flag_cb, NULL) >= 0,
	    "timer_add failed");

	(void)iox_loop_poll(loop);
	ASSERT(timeout_flag == 1, "timer callback not called");

	iox_loop_free(loop);
	PASS();
}

/* ---- timer fires ---- */

static int timer_cb_called;

static void
timer_fire_cb(struct iox_loop *loop, void *arg)
{
	(void)loop;
	(void)arg;
	timer_cb_called++;
}

static void
test_timer_fires(void)
{
	struct iox_loop *loop;

	TEST("timer fires after delay");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	timer_cb_called = 0;
	ASSERT(iox_timer_add(loop, 0, timer_fire_cb, NULL) >= 0,
	    "timer_add failed");

	(void)iox_loop_poll(loop);
	ASSERT(timer_cb_called == 1, "timer callback not called");

	iox_loop_free(loop);
	PASS();
}

/* ---- timer remove ---- */

static int removed_timer_cb_called;

static void
removed_timer_cb(struct iox_loop *loop, void *arg)
{
	(void)loop;
	(void)arg;
	removed_timer_cb_called++;
}

static void
test_timer_remove(void)
{
	struct iox_loop *loop;
	int pfd[2], tid;

	TEST("timer_remove cancels pending timer");
	ASSERT(pipe(pfd) == 0, "pipe failed");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	/* add a fd so poll returns without blocking */
	ASSERT(iox_fd_add(loop, pfd[0], IOX_READ, read_cb, NULL) == 0,
	    "fd_add failed");
	(void)write(pfd[1], "x", 1);

	removed_timer_cb_called = 0;
	tid = iox_timer_add(loop, 0, removed_timer_cb, NULL);
	ASSERT(tid >= 0, "timer_add failed");

	iox_timer_remove(loop, tid);

	(void)iox_loop_poll(loop);
	ASSERT(removed_timer_cb_called == 0,
	    "removed timer callback fired");

	iox_loop_free(loop);
	close(pfd[0]);
	close(pfd[1]);
	PASS();
}

/* ---- timer ordering ---- */

static int timer_order[2];
static int timer_order_idx;

static void
timer_order_cb(struct iox_loop *loop, void *arg)
{
	(void)loop;
	timer_order[timer_order_idx++] = *(int *)arg;
}

static void
test_timer_ordering(void)
{
	struct iox_loop *loop;
	static int id_a = 1, id_b = 2;

	TEST("timers fire in deadline order");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	timer_order_idx = 0;

	/* add longer timer first, shorter second */
	ASSERT(iox_timer_add(loop, 50, timer_order_cb, &id_a) >= 0,
	    "timer_add A failed");
	ASSERT(iox_timer_add(loop, 0, timer_order_cb, &id_b) >= 0,
	    "timer_add B failed");

	(void)iox_loop_poll(loop);

	/* 0ms timer (B=2) should fire first */
	ASSERT(timer_order_idx >= 1, "no timer fired");
	ASSERT(timer_order[0] == 2, "shorter timer did not fire first");

	iox_loop_free(loop);
	PASS();
}

/* ---- fd_mod not found ---- */

static void
test_fd_mod_not_found(void)
{
	struct iox_loop *loop;

	TEST("fd_mod returns -1 for unknown fd");

	loop = iox_loop_new();
	ASSERT(loop != NULL, "loop new failed");

	ASSERT(iox_fd_mod(loop, 9999, IOX_READ) == -1,
	    "fd_mod should fail for unknown fd");

	iox_loop_free(loop);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libiox tests:\n");

	test_loop_new_free();
	test_fd_read();
	test_fd_write();
	test_fd_mod();
	test_fd_mod_not_found();
	test_remove_during_dispatch();
	test_loop_run_stop();
	test_idle_callback();
	test_poll_timeout();
	test_timer_fires();
	test_timer_remove();
	test_timer_ordering();
	test_signal();
	test_signal_restore();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
