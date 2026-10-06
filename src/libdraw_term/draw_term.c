/* draw_term.c : terminal backend for the draw surface */

#include "draw_term.h"

#include "draw.h"
#include "draw_driver.h"

#include "tio.h"
#include "tio_write.h"
#include "txl.h"
#include "render.h"
#include "tkbd.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct draw_term {
	int		in_fd, out_fd;
	struct txl	*txl;		/* owned */
	struct render	*rd;		/* owned */
	int		rows, cols;	/* renderer's current size */
	int		need_full;	/* next present must redraw everything */
	int		cur_shape;	/* last DECSCUSR shape emitted */
	int		mouse;		/* mouse reporting enabled */

	char		inbuf[1024];	/* input bytes carried between polls */
	size_t		inlen;

	/* poll dispatch trampoline state */
	void		(*ev_cb)(void *u, const struct tkbd_seq *seq);
	void		*ev_u;
	int		ev_count;

	int		sig_installed;	/* SIGWINCH/SIGTSTP handlers set */
	struct sigaction sav_winch, sav_tstp;
};

/*
 * Terminal signals are process-global, so the handlers set file-scope flags.
 * There is one terminal driver in practice; a second would share these, which
 * is harmless. signals() reports and clears them.
 */
static volatile sig_atomic_t term_sig_resized;
static volatile sig_atomic_t term_sig_suspended;

static void
term_on_winch(int sig)
{
	(void)sig;
	term_sig_resized = 1;
}

static void
term_on_tstp(int sig)
{
	(void)sig;
	term_sig_suspended = 1;
}

/* ---- small output helpers ---- */

static void
emit(struct draw_term *t, const char *s)
{
	if (s)
		tio_write(t->out_fd, s, strlen(s));
}

static void
emit_mode(struct draw_term *t, int set, int mode)
{
	char buf[32];
	int n = set ? txl_decset(buf, sizeof(buf), mode)
	    : txl_decrst(buf, sizeof(buf), mode);

	if (n > 0)
		tio_write(t->out_fd, buf, (size_t)n);
}

/* Steady cursor shapes via DECSCUSR (CSI Ps SP q). */
static const char *
shape_seq(int shape)
{
	switch (shape) {
	case DRAW_CURSOR_BAR:
		return "\033[6 q";
	case DRAW_CURSOR_UNDERLINE:
		return "\033[4 q";
	case DRAW_CURSOR_BLOCK:
		return "\033[2 q";
	case DRAW_CURSOR_DEFAULT:
	default:
		return "\033[0 q";
	}
}

/* ---- driver methods ---- */

static int
term_size(void *ctx, int *rows, int *cols)
{
	struct draw_term *t = ctx;
	struct winsize ws;

	if (ioctl(t->out_fd, TIOCGWINSZ, &ws) < 0)
		return -1;
	if (ws.ws_row <= 0 || ws.ws_col <= 0)
		return -1;
	*rows = ws.ws_row;
	*cols = ws.ws_col;
	return 0;
}

/* Catch SIGWINCH and SIGTSTP for the lifetime of the session. Installed once
 * on the first begin and left in place across suspend and shell-out re-entry;
 * the originals are restored in free. */
static void
term_install_signals(struct draw_term *t)
{
	struct sigaction sa;

	if (t->sig_installed)
		return;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;		/* no SA_RESTART: let poll see EINTR */
	sa.sa_handler = term_on_winch;
	sigaction(SIGWINCH, &sa, &t->sav_winch);
	sa.sa_handler = term_on_tstp;
	sigaction(SIGTSTP, &sa, &t->sav_tstp);
	t->sig_installed = 1;
}

static void
term_begin(void *ctx)
{
	struct draw_term *t = ctx;

	term_install_signals(t);
	tio_raw(t->in_fd);
	emit(t, txl_str(t->txl, TXL_SMCUP));	/* alt screen */
	emit_mode(t, 1, 2004);			/* bracketed paste */
	if (t->mouse) {
		emit_mode(t, 1, 1000);		/* mouse button tracking */
		emit_mode(t, 1, 1002);		/* mouse drag tracking */
		emit_mode(t, 1, 1006);		/* SGR extended coordinates */
	}
	tio_flush(t->out_fd);
	/* the alt screen starts blank and the renderer's tracked cursor is
	 * now stale, so redraw everything on the next present */
	render_invalidate_cursor(t->rd);
	t->need_full = 1;
	t->cur_shape = -1;			/* force a shape emit */
}

static void
term_end(void *ctx)
{
	struct draw_term *t = ctx;

	emit(t, "\033[0 q");			/* default cursor shape */
	emit(t, txl_str(t->txl, TXL_SGR0));
	emit(t, txl_str(t->txl, TXL_CNORM));	/* show cursor */
	if (t->mouse) {
		emit_mode(t, 0, 1006);
		emit_mode(t, 0, 1002);
		emit_mode(t, 0, 1000);
	}
	emit_mode(t, 0, 2004);
	emit(t, txl_str(t->txl, TXL_RMCUP));	/* leave alt screen */
	tio_flush(t->out_fd);
	tio_restore(t->in_fd);
}

static void
term_present(void *ctx, const struct draw_frame *f)
{
	struct draw_term *t = ctx;

	if (f->rows != t->rows || f->cols != t->cols) {
		render_resize(t->rd, f->rows, f->cols);
		t->rows = f->rows;
		t->cols = f->cols;
		t->need_full = 1;
	}
	if (f->cur_shape != t->cur_shape) {
		emit(t, shape_seq(f->cur_shape));
		t->cur_shape = f->cur_shape;
	}
	if (t->need_full) {
		render_cells_full(t->rd, t->out_fd, f->cells, f->rows, f->cols,
		    f->cur_row, f->cur_col, f->cur_vis);
		t->need_full = 0;
	} else {
		render_cells_diff(t->rd, t->out_fd, f->cells, f->rows, f->cols,
		    f->cur_row, f->cur_col, f->cur_vis, f->row_dirty);
	}
	tio_flush(t->out_fd);
}

/* tkbd_drain trampoline: count events and forward to the user callback. */
static void
term_dispatch(void *ctx, const struct tkbd_seq *seq)
{
	struct draw_term *t = ctx;

	t->ev_count++;
	if (t->ev_cb)
		t->ev_cb(t->ev_u, seq);
}

static int
term_poll(void *ctx, int timeout_ms,
    void (*on_event)(void *u, const struct tkbd_seq *seq), void *u)
{
	struct draw_term *t = ctx;
	struct pollfd pfd;
	int pr;

	t->ev_cb = on_event;
	t->ev_u = u;
	t->ev_count = 0;

	pfd.fd = t->in_fd;
	pfd.events = POLLIN;
	do {
		pr = poll(&pfd, 1, timeout_ms);
	} while (pr < 0 && errno == EINTR);
	if (pr < 0)
		return -1;
	if (pr == 0) {
		/* timed out: release any held lone ESC as the ESC key */
		if (t->inlen > 0) {
			size_t used = tkbd_drain(t->inbuf, t->inlen, 1,
			    term_dispatch, t);

			if (used > 0 && used < t->inlen)
				memmove(t->inbuf, t->inbuf + used,
				    t->inlen - used);
			t->inlen -= used;
		}
		return t->ev_count;
	}

	/* input ready: append and drain complete sequences */
	{
		ssize_t n = read(t->in_fd, t->inbuf + t->inlen,
		    sizeof(t->inbuf) - t->inlen);
		size_t used;

		if (n < 0)
			return (errno == EINTR) ? t->ev_count : -1;
		if (n == 0)
			return -1;		/* EOF */
		t->inlen += (size_t)n;
		used = tkbd_drain(t->inbuf, t->inlen, 0, term_dispatch, t);
		if (used > 0 && used < t->inlen)
			memmove(t->inbuf, t->inbuf + used, t->inlen - used);
		t->inlen -= used;
	}
	return t->ev_count;
}

/* OSC 52: set the system clipboard. The payload is base64 of the UTF-8. */
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
term_set_clipboard(void *ctx, const char *utf8, size_t n)
{
	struct draw_term *t = ctx;
	size_t i;

	emit(t, "\033]52;c;");
	for (i = 0; i < n; i += 3) {
		unsigned char a = (unsigned char)utf8[i];
		unsigned char b = i + 1 < n ? (unsigned char)utf8[i + 1] : 0;
		unsigned char c = i + 2 < n ? (unsigned char)utf8[i + 2] : 0;
		char out[4];

		out[0] = b64[a >> 2];
		out[1] = b64[((a & 0x03) << 4) | (b >> 4)];
		out[2] = i + 1 < n ? b64[((b & 0x0f) << 2) | (c >> 6)] : '=';
		out[3] = i + 2 < n ? b64[c & 0x3f] : '=';
		tio_write(t->out_fd, out, 4);
	}
	emit(t, "\033\\");			/* ST terminator */
	tio_flush(t->out_fd);
}

static void
term_set_title(void *ctx, const char *utf8)
{
	struct draw_term *t = ctx;

	emit(t, "\033]2;");
	emit(t, utf8);
	emit(t, "\033\\");
	tio_flush(t->out_fd);
}

static void
term_bell(void *ctx)
{
	struct draw_term *t = ctx;

	tio_write(t->out_fd, "\a", 1);
	tio_flush(t->out_fd);
}

/* Report and clear pending resize and suspend requests. */
static int
term_signals(void *ctx)
{
	int bits = 0;

	(void)ctx;
	if (term_sig_resized) {
		term_sig_resized = 0;
		bits |= DRAW_SIG_RESIZE;
	}
	if (term_sig_suspended) {
		term_sig_suspended = 0;
		bits |= DRAW_SIG_SUSPEND;
	}
	return bits;
}

/* Stop the process with the default disposition, then restore our handler
 * once resumed. The surface has already restored the terminal via end. */
static void
term_suspend(void *ctx)
{
	struct sigaction dfl, sav;

	(void)ctx;
	memset(&dfl, 0, sizeof(dfl));
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(SIGTSTP, &dfl, &sav);
	raise(SIGTSTP);			/* stop here until SIGCONT */
	sigaction(SIGTSTP, &sav, NULL);	/* reinstall the catch handler */
}

static void
term_free(void *ctx)
{
	struct draw_term *t = ctx;

	if (!t)
		return;
	if (t->sig_installed) {
		sigaction(SIGWINCH, &t->sav_winch, NULL);
		sigaction(SIGTSTP, &t->sav_tstp, NULL);
		t->sig_installed = 0;
	}
	render_free(t->rd);
	txl_free(t->txl);
	free(t);
}

static const struct draw_driver term_driver = {
	.begin = term_begin,
	.end = term_end,
	.size = term_size,
	.present = term_present,
	.poll = term_poll,
	.free = term_free,
	.set_clipboard = term_set_clipboard,
	.set_title = term_set_title,
	.bell = term_bell,
	.signals = term_signals,
	.suspend = term_suspend,
};

const struct draw_driver *
draw_term_driver(void)
{
	return &term_driver;
}

void
draw_term_mouse(struct draw_term *t, int on)
{
	t->mouse = on ? 1 : 0;
}

struct draw_term *
draw_term_new(int in_fd, int out_fd, const char *term)
{
	struct draw_term *t;
	struct winsize ws;
	int rows = 24, cols = 80;

	t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->in_fd = in_fd;
	t->out_fd = out_fd;
	t->cur_shape = -1;
	t->txl = txl_new(term);
	if (!t->txl) {
		free(t);
		return NULL;
	}
	if (ioctl(out_fd, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_row > 0 && ws.ws_col > 0) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	}
	t->rd = render_new(rows, cols, t->txl);
	if (!t->rd) {
		txl_free(t->txl);
		free(t);
		return NULL;
	}
	t->rows = rows;
	t->cols = cols;
	return t;
}
