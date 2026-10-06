/* test_draw_term.c : headless tests for the terminal draw backend */

#include "draw_term.h"

#include "draw.h"
#include "tkbd.h"

#include <stdio.h>
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

/* byte-safe substring search (output may embed NUL) */
static int
contains(const char *hay, size_t hlen, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t i;

	if (nlen == 0 || nlen > hlen)
		return 0;
	for (i = 0; i + nlen <= hlen; i++)
		if (memcmp(hay + i, needle, nlen) == 0)
			return 1;
	return 0;
}

static const struct vt_color def = { .type = VT_COLOR_DEFAULT };

static struct tkbd_seq captured;
static int cap_hit;

static void
on_key(void *u, const struct tkbd_seq *seq)
{
	(void)u;
	captured = *seq;
	cap_hit = 1;
}

static void
test_output(void)
{
	FILE *fp;
	int out_fd, in[2];
	struct draw_term *t;
	struct draw *d;
	char buf[65536];
	ssize_t n;

	TEST("driver emits escapes, text, OSC, and bell");
	fp = tmpfile();
	ASSERT(fp != NULL, "tmpfile failed");
	out_fd = fileno(fp);
	ASSERT(pipe(in) == 0, "pipe failed");

	t = draw_term_new(in[0], out_fd, "xterm");
	ASSERT(t != NULL, "draw_term_new failed");
	d = draw_new(draw_term_driver(), t);
	ASSERT(d != NULL, "draw_new failed");

	draw_begin(d);
	draw_text(d, 0, 0, "Hi", def, def, 0);
	draw_present(d);
	draw_set_clipboard(d, "hi", 2);
	draw_set_title(d, "T");
	draw_bell(d);
	draw_end(d);

	lseek(out_fd, 0, SEEK_SET);
	n = read(out_fd, buf, sizeof(buf));
	ASSERT(n > 0, "no output captured");

	ASSERT(contains(buf, (size_t)n, "\033"), "no escape sequences");
	ASSERT(contains(buf, (size_t)n, "Hi"), "drawn text missing");
	ASSERT(contains(buf, (size_t)n, "\033]52;c;aGk="),
	    "OSC 52 clipboard missing");
	ASSERT(contains(buf, (size_t)n, "\033]2;T"), "OSC 2 title missing");
	ASSERT(contains(buf, (size_t)n, "\a"), "bell missing");

	draw_free(d);
	close(in[0]);
	close(in[1]);
	fclose(fp);
	PASS();
}

static void
test_poll(void)
{
	FILE *fp;
	int out_fd, in[2];
	struct draw_term *t;
	struct draw *d;
	int ev;

	TEST("poll decodes an input key");
	fp = tmpfile();
	ASSERT(fp != NULL, "tmpfile failed");
	out_fd = fileno(fp);
	ASSERT(pipe(in) == 0, "pipe failed");

	t = draw_term_new(in[0], out_fd, "xterm");
	ASSERT(t != NULL, "draw_term_new failed");
	d = draw_new(draw_term_driver(), t);
	ASSERT(d != NULL, "draw_new failed");

	cap_hit = 0;
	ASSERT(write(in[1], "A", 1) == 1, "write to pipe failed");
	ev = draw_poll(d, 200, on_key, NULL);
	ASSERT(ev == 1, "expected one event");
	ASSERT(cap_hit, "callback not invoked");
	ASSERT(captured.ch == 'A' || captured.key == 'A', "key not 'A'");

	draw_free(d);
	close(in[0]);
	close(in[1]);
	fclose(fp);
	PASS();
}

static void
test_poll_timeout(void)
{
	FILE *fp;
	int out_fd, in[2];
	struct draw_term *t;
	struct draw *d;
	int ev;

	TEST("poll returns 0 on timeout with no input");
	fp = tmpfile();
	ASSERT(fp != NULL, "tmpfile failed");
	out_fd = fileno(fp);
	ASSERT(pipe(in) == 0, "pipe failed");

	t = draw_term_new(in[0], out_fd, "xterm");
	d = draw_new(draw_term_driver(), t);
	ASSERT(d != NULL, "draw_new failed");

	ev = draw_poll(d, 20, on_key, NULL);
	ASSERT(ev == 0, "expected no events on timeout");

	draw_free(d);
	close(in[0]);
	close(in[1]);
	fclose(fp);
	PASS();
}

int
main(void)
{
	printf("libdraw_term tests:\n");
	test_output();
	test_poll();
	test_poll_timeout();
	printf("test_draw_term: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
