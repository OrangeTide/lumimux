/* test_vt_torture.c : randomized torture test for libvt */

/*
 * Drives the parser and terminal state with a deterministic stream of
 * random and crafted byte sequences, then checks a set of structural
 * invariants after every operation.  It exists to be run under the
 * sanitizers and the coverage build, where a crash, a memory error, a
 * signed-overflow report, or a tripped invariant marks a defect.
 *
 * The run is reproducible: every byte comes from a seeded xorshift
 * generator.  On an invariant failure the seed and iteration are printed
 * so the exact run can be replayed.
 *
 *   ./test_vt_torture [seed] [iterations]
 *
 * Build it under whatever variant you want to exercise, for example:
 *
 *   make SANITIZE=address,undefined test_vt_torture
 *   make VARIANT=coverage CFLAGS='-O0 -g --coverage' \
 *       LDFLAGS=--coverage test_vt_torture
 *
 * then run the binary (optionally with a large iteration count) and read
 * the sanitizer output or the gcov/gcovr report.
 */

#include "vt_parse.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "vt_state.h"
#include "vt_ops.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_ROWS	50
#define MAX_COLS	120
#define MAX_SCROLLBACK	200

/* reproduction context, printed when an invariant fails */
static uint64_t	rng_state;
static uint64_t	run_seed;
static long	iteration;

/* xorshift64: small, fast, and fully determined by the seed */
static uint64_t
rng_next(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

/* uniform in [0, n) for n > 0 */
static unsigned
rng_below(unsigned n)
{
	return (unsigned)(rng_next() % n);
}

static void
die(const char *msg)
{
	fprintf(stderr,
	    "torture FAIL: %s (seed 0x%llx, iteration %ld)\n",
	    msg, (unsigned long long)run_seed, iteration);
	abort();
}

/*
 * Structural invariants that must hold after any input.  cursor_col may
 * equal cols: that is the autowrap "pending wrap" position reached after
 * writing the last cell of a row.
 */
static void
check_invariants(struct vt_state *st)
{
	struct vt_buf *buf = st->buf;
	int rows, cols;

	if (!buf)
		die("active buffer is NULL");

	rows = vt_buf_rows(buf);
	cols = vt_buf_cols(buf);

	if (rows < 1 || cols < 1)
		die("degenerate buffer geometry");

	if (st->cursor_row < 0 || st->cursor_row >= rows)
		die("cursor_row out of range");
	if (st->cursor_col < 0 || st->cursor_col > cols)
		die("cursor_col out of range");

	if (st->scroll_top < 0 || st->scroll_bot > rows ||
	    st->scroll_top >= st->scroll_bot)
		die("scroll region inverted or out of range");

	if (vt_buf_scrollback_lines(buf) < 0)
		die("negative scrollback line count");

	/* corners must resolve; a NULL here means a short row allocation */
	if (!vt_buf_cell(buf, 0, 0) ||
	    !vt_buf_cell(buf, rows - 1, cols - 1))
		die("corner cell did not resolve");
}

/* ---- input generators ---- */

/* feed a run of raw random bytes, including control and high bytes */
static void
gen_random_bytes(struct vt_parse *p)
{
	char buf[64];
	unsigned i, n = 1 + rng_below(sizeof(buf));

	for (i = 0; i < n; i++)
		buf[i] = (char)rng_below(256);
	vt_parse_feed(p, buf, n);
}

/* feed printable text mixed with wide, combining, and invalid UTF-8 */
static void
gen_text(struct vt_parse *p)
{
	unsigned char buf[256];
	unsigned len = 0, i, n = 1 + rng_below(48);

	for (i = 0; i < n && len + 4 < sizeof(buf); i++) {
		unsigned pick = rng_below(10);

		if (pick < 6) {
			buf[len++] = (unsigned char)(0x20 + rng_below(95));
		} else if (pick < 8) {
			/* a CJK or other wide codepoint */
			uint32_t cp = 0x3000 + rng_below(0x5000);

			len += (unsigned)utf8_encode(buf + len, cp);
		} else if (pick < 9) {
			/* a combining mark (zero width) */
			len += (unsigned)utf8_encode(buf + len, 0x0300);
		} else {
			/* a lone continuation byte: invalid UTF-8 */
			buf[len++] = (unsigned char)(0x80 + rng_below(64));
		}
	}
	vt_parse_feed(p, (const char *)buf, len);
}

/* append a decimal parameter, picking values that stress the parser */
static unsigned
emit_param(char *buf, unsigned len, size_t cap)
{
	static const long values[] = {
		0, 1, 2, 3, 7, 8, 24, 80, 999, 2147483647L, 99999999999L,
	};
	long v = values[rng_below(sizeof(values) / sizeof(values[0]))];
	int w;

	w = snprintf(buf + len, cap - len, "%ld", v);
	if (w > 0 && (size_t)(len + w) < cap)
		len += (unsigned)w;
	return len;
}

/* feed a CSI sequence with a fuzzed prefix, parameters, and final byte */
static void
gen_csi(struct vt_parse *p)
{
	char buf[128];
	unsigned len = 0, i, nparam;

	buf[len++] = '\033';
	buf[len++] = '[';

	/* private / parameter prefix byte (CSI ?, >, <, =) */
	if (rng_below(2)) {
		static const char pfx[] = "?<>=";

		buf[len++] = pfx[rng_below(4)];
	}

	nparam = rng_below(5);
	for (i = 0; i < nparam; i++) {
		if (i)
			buf[len++] = ';';
		/* an empty parameter (default) now and then */
		if (rng_below(4))
			len = emit_param(buf, len, sizeof(buf) - 2);
	}

	/* optional intermediate byte (0x20-0x2F) */
	if (rng_below(3) == 0)
		buf[len++] = (char)(0x20 + rng_below(16));

	/* final byte (0x40-0x7E) */
	buf[len++] = (char)(0x40 + rng_below(0x3F));

	vt_parse_feed(p, buf, len);
}

/* feed an ESC sequence (intermediate plus final) */
static void
gen_esc(struct vt_parse *p)
{
	char buf[4];
	unsigned len = 0;

	buf[len++] = '\033';
	if (rng_below(2))
		buf[len++] = (char)(0x20 + rng_below(16));
	buf[len++] = (char)(0x30 + rng_below(0x4F));
	vt_parse_feed(p, buf, len);
}

/* feed an OSC string; occasionally huge, to cross the growth and cap paths */
static void
gen_osc(struct vt_parse *p)
{
	static const int codes[] = { 0, 1, 2, 4, 8, 9, 10, 52, 777, 1337 };
	char head[32];
	char *body;
	unsigned blen, i, hlen;
	int code = codes[rng_below(sizeof(codes) / sizeof(codes[0]))];

	/* most OSCs are short; 1 in 64 is oversized */
	if (rng_below(64) == 0)
		blen = 1 + rng_below(300000);
	else
		blen = rng_below(64);

	hlen = (unsigned)snprintf(head, sizeof(head), "\033]%d;", code);
	vt_parse_feed(p, head, hlen);

	body = malloc(blen ? blen : 1);
	if (!body)
		die("OSC body allocation failed");
	for (i = 0; i < blen; i++)
		body[i] = (char)(0x20 + rng_below(95));
	vt_parse_feed(p, body, blen);
	free(body);

	/* terminate with BEL or ST, both of which the parser accepts */
	if (rng_below(2))
		vt_parse_feed(p, "\a", 1);
	else
		vt_parse_feed(p, "\033\\", 2);
}

static void
dcs_sink(void *ctx, int introducer, const char *data, size_t len)
{
	volatile char sink = 0;
	size_t i;

	(void)ctx;
	(void)introducer;
	/* touch every byte so a short buffer is caught under asan */
	for (i = 0; i < len; i++)
		sink = (char)(sink ^ data[i]);
	(void)sink;
}

/* feed a DCS/APC/PM/SOS string; occasionally large to grow the buffer */
static void
gen_dcs(struct vt_parse *p)
{
	static const char intro[] = "PX^_";
	char head[3];
	char *body;
	unsigned blen, i;

	head[0] = '\033';
	head[1] = intro[rng_below(4)];
	vt_parse_feed(p, head, 2);

	if (rng_below(128) == 0)
		blen = 1 + rng_below(2000000);
	else
		blen = rng_below(128);

	body = malloc(blen ? blen : 1);
	if (!body)
		die("DCS body allocation failed");
	for (i = 0; i < blen; i++)
		body[i] = (char)(0x20 + rng_below(95));
	vt_parse_feed(p, body, blen);
	free(body);

	vt_parse_feed(p, "\033\\", 2);
}

/* feed a DECSET/DECRST mode toggle for a real mode number */
static void
gen_mode(struct vt_parse *p)
{
	static const int modes[] = {
		1, 6, 7, 12, 25, 1000, 1002, 1003, 1049, 2004,
	};
	char buf[16];
	unsigned len;
	int m = modes[rng_below(sizeof(modes) / sizeof(modes[0]))];

	len = (unsigned)snprintf(buf, sizeof(buf), "\033[?%d%c", m,
	    rng_below(2) ? 'h' : 'l');
	vt_parse_feed(p, buf, len);
}

/* feed a single C0/C1 control byte */
static void
gen_control(struct vt_parse *p)
{
	char c = (char)rng_below(0x20);

	vt_parse_feed(p, &c, 1);
}

/*
 * Dump the current screen and replay it into a fresh state.  Exercises
 * vt_state_dump and feeds its output back through the parser, which must
 * leave the replica structurally sound.
 */
static void
dump_append(void *ctx, const char *data, size_t len)
{
	struct vt_parse *replica = ctx;

	vt_parse_feed(replica, data, len);
}

static void
gen_dump_roundtrip(struct vt_state *st)
{
	struct vt_state *replica = vt_state_new(24, 80, 50);
	struct vt_parse *rp;

	if (!replica)
		die("replica state allocation failed");
	rp = vt_parse_new(vt_ops_default(), replica);
	if (!rp)
		die("replica parser allocation failed");

	vt_state_dump(st, dump_append, rp);
	check_invariants(replica);

	vt_parse_free(rp);
	vt_state_free(replica);
}

int
main(int argc, char **argv)
{
	struct vt_state *st;
	struct vt_parse *p;
	long iterations = 50000;
	int pipefd[2];
	long i;

	run_seed = 0x9e3779b97f4a7c15ULL;
	if (argc > 1)
		run_seed = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		iterations = strtol(argv[2], NULL, 10);
	/* xorshift must not start from zero */
	rng_state = run_seed ? run_seed : 1;

	st = vt_state_new(24, 80, MAX_SCROLLBACK);
	if (!st)
		die("initial state allocation failed");
	p = vt_parse_new(vt_ops_default(), st);
	if (!p)
		die("initial parser allocation failed");

	/*
	 * A non-blocking, never-drained pipe as the reply fd: DSR/DA
	 * replies first succeed, then return EAGAIN once the pipe fills,
	 * exercising both arms of the reply write loop.
	 */
	if (pipe(pipefd) == 0) {
		fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
		vt_state_set_reply_fd(st, pipefd[1]);
	} else {
		pipefd[0] = pipefd[1] = -1;
	}

	vt_state_set_altscreen_scrollback(st, 1);

	for (i = 0; i < iterations; i++) {
		unsigned op = rng_below(100);

		iteration = i;

		if (op < 25)
			gen_text(p);
		else if (op < 45)
			gen_csi(p);
		else if (op < 55)
			gen_random_bytes(p);
		else if (op < 63)
			gen_esc(p);
		else if (op < 71)
			gen_mode(p);
		else if (op < 79)
			gen_control(p);
		else if (op < 85)
			gen_osc(p);
		else if (op < 90)
			gen_dcs(p);
		else if (op < 94)
			vt_state_resize(st, 1 + (int)rng_below(MAX_ROWS),
			    1 + (int)rng_below(MAX_COLS));
		else if (op < 96)
			vt_parse_reset(p);
		else if (op < 98)
			gen_dump_roundtrip(st);
		else if (op < 99)
			vt_state_altscreen_enter(st);
		else
			vt_state_altscreen_leave(st);

		check_invariants(st);

		/* install the DCS sink partway through and drop it later */
		if (i == iterations / 3)
			vt_parse_set_dcs_cb(p, dcs_sink, NULL);
		else if (i == 2 * iterations / 3)
			vt_parse_set_dcs_cb(p, NULL, NULL);
	}

	vt_parse_free(p);
	vt_state_free(st);
	if (pipefd[0] >= 0) {
		close(pipefd[0]);
		close(pipefd[1]);
	}

	printf("test_vt_torture: %ld iterations, seed 0x%llx, 0 failures\n",
	    iterations, (unsigned long long)run_seed);
	return 0;
}
