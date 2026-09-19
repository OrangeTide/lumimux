/* files.c : lumi files -- TUI file browser */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "tkbd.h"
#include "tui_theme.h"
#include "tui_out.h"
#include "utf8.h"
#include "vt_cell.h"
#include "rune_width.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *progname = "lumi-files";

/* Set by the SIGWINCH handler; polled from the input loop's idle tick. */
static volatile sig_atomic_t resized;

/* Result of handling one key: keep going, quit, or open the selection in
 * an external pager or editor (performed by the main loop). */
enum action {
	ACT_CONTINUE,
	ACT_QUIT,
	ACT_PAGE,
	ACT_EDIT,
	ACT_WIN_PAGE,		/* open in a new session window (pager) */
	ACT_WIN_EDIT,		/* open in a new session window (editor) */
	ACT_DELETE,		/* delete the selection, after a confirm */
	ACT_RENAME,		/* rename the selection */
	ACT_MKDIR,		/* create a directory */
};

struct entry {
	char		*name;		/* owned */
	int		is_dir;
	int		is_updir;	/* the synthetic ".." entry */
	long long	size;
};

struct browser {
	char		cwd[PATH_MAX];
	struct entry	*ents;
	int		n_ents;
	int		cap_ents;
	int		sel;		/* index of highlighted entry */
	int		scroll;		/* index of first visible entry */
	int		pv_scroll;	/* first visible preview line */
	int		show_hidden;	/* include dotfiles in the listing */
	int		rows;		/* terminal size, refreshed on resize */
	int		cols;
	int		in_session;
	char		status[160];	/* transient footer message */
};

/****************************************************************
 * Directory model
 ****************************************************************/

static void
entries_free(struct browser *b)
{
	int i;

	for (i = 0; i < b->n_ents; i++)
		free(b->ents[i].name);
	free(b->ents);
	b->ents = NULL;
	b->n_ents = b->cap_ents = 0;
}

static int
entries_push(struct browser *b, const char *name, int is_dir,
    int is_updir, long long size)
{
	struct entry *e;

	if (b->n_ents >= b->cap_ents) {
		int cap = b->cap_ents ? b->cap_ents * 2 : 64;
		struct entry *p = realloc(b->ents, (size_t)cap * sizeof(*p));

		if (!p)
			return -1;
		b->ents = p;
		b->cap_ents = cap;
	}
	e = &b->ents[b->n_ents];
	e->name = strdup(name);
	if (!e->name)
		return -1;
	e->is_dir = is_dir;
	e->is_updir = is_updir;
	e->size = size;
	b->n_ents++;
	return 0;
}

/* Directories before files; the ".." entry pinned first; otherwise a
 * case-insensitive name order with a stable byte-order tie-break. */
static int
entry_cmp(const void *pa, const void *pb)
{
	const struct entry *a = pa, *b = pb;
	int c;

	if (a->is_updir != b->is_updir)
		return a->is_updir ? -1 : 1;
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	c = strcasecmp(a->name, b->name);
	if (c != 0)
		return c;
	return strcmp(a->name, b->name);
}

/* Load b->cwd into b->ents. On success returns 0; on failure leaves the
 * entry list empty and returns -1 with errno set. */
static int
load_dir(struct browser *b)
{
	DIR *d;
	struct dirent *de;

	entries_free(b);

	d = opendir(b->cwd);
	if (!d)
		return -1;

	if (strcmp(b->cwd, "/") != 0)
		(void)entries_push(b, "..", 1, 1, 0);

	while ((de = readdir(d)) != NULL) {
		char path[PATH_MAX];
		struct stat st;
		int is_dir = 0;
		long long size = 0;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (de->d_name[0] == '.' && !b->show_hidden)
			continue;		/* dotfiles hidden unless toggled */

		if (snprintf(path, sizeof(path), "%s/%s", b->cwd,
		    de->d_name) < (int)sizeof(path) &&
		    stat(path, &st) == 0) {
			is_dir = S_ISDIR(st.st_mode);
			size = (long long)st.st_size;
		}
		if (entries_push(b, de->d_name, is_dir, 0, size) < 0)
			break;
	}
	closedir(d);

	qsort(b->ents, (size_t)b->n_ents, sizeof(*b->ents), entry_cmp);
	return 0;
}

/* Move into dir, reloading the listing. On success the selection is set to
 * want_name when found, else 0. On failure the current view is kept and a
 * status message is set. */
static void
navigate_to(struct browser *b, const char *path, const char *want_name)
{
	char resolved[PATH_MAX];
	struct browser saved;
	int i;

	if (!realpath(path, resolved)) {
		snprintf(b->status, sizeof(b->status), "%.120s: %s",
		    path, strerror(errno));
		return;
	}

	/* keep the old listing until the new one loads, so a failed
	 * opendir (permission denied) leaves the view intact */
	saved = *b;
	b->ents = NULL;
	b->n_ents = b->cap_ents = 0;
	snprintf(b->cwd, sizeof(b->cwd), "%s", resolved);

	if (load_dir(b) < 0) {
		snprintf(saved.status, sizeof(saved.status), "%.120s: %s",
		    resolved, strerror(errno));
		entries_free(b);
		*b = saved;		/* restore previous directory */
		return;
	}

	entries_free(&saved);		/* free the old listing */

	b->sel = 0;
	b->scroll = 0;
	b->pv_scroll = 0;
	b->status[0] = '\0';
	if (want_name) {
		for (i = 0; i < b->n_ents; i++) {
			if (strcmp(b->ents[i].name, want_name) == 0) {
				b->sel = i;
				break;
			}
		}
	}
}

static void
go_up(struct browser *b)
{
	char parent[PATH_MAX];
	const char *base;
	char *slash;

	if (strcmp(b->cwd, "/") == 0)
		return;

	/* remember the directory we are leaving so it starts selected */
	base = strrchr(b->cwd, '/');
	base = base ? base + 1 : b->cwd;

	snprintf(parent, sizeof(parent), "%s", b->cwd);
	slash = strrchr(parent, '/');
	if (slash == parent)
		parent[1] = '\0';	/* parent is "/" */
	else if (slash)
		*slash = '\0';

	navigate_to(b, parent, base);
}

/* Reload the current directory in place, keeping a sensible selection. When
 * want is non-NULL that name is selected if present; otherwise the previously
 * selected name is followed, falling back to the same index (clamped). Used
 * after toggling hidden files and after a mutating operation. */
static void
reload_dir(struct browser *b, const char *want)
{
	char name[NAME_MAX + 1];
	int old = b->sel;

	name[0] = '\0';
	if (want)
		snprintf(name, sizeof(name), "%s", want);
	else if (b->sel >= 0 && b->sel < b->n_ents)
		snprintf(name, sizeof(name), "%s", b->ents[b->sel].name);

	if (load_dir(b) < 0) {
		snprintf(b->status, sizeof(b->status), "%.120s: %s",
		    b->cwd, strerror(errno));
		return;
	}

	b->sel = old < b->n_ents ? old : b->n_ents - 1;
	if (b->sel < 0)
		b->sel = 0;
	if (name[0]) {
		int i;

		for (i = 0; i < b->n_ents; i++)
			if (strcmp(b->ents[i].name, name) == 0) {
				b->sel = i;
				break;
			}
	}
	b->scroll = 0;
	b->pv_scroll = 0;
}

static enum action
enter_sel(struct browser *b)
{
	struct entry *e;
	char path[PATH_MAX];

	if (b->sel < 0 || b->sel >= b->n_ents)
		return ACT_CONTINUE;
	e = &b->ents[b->sel];

	if (e->is_updir) {
		go_up(b);
		return ACT_CONTINUE;
	}
	if (!e->is_dir)
		return ACT_PAGE;		/* view the file in a pager */

	if (snprintf(path, sizeof(path), "%s/%s", b->cwd, e->name)
	    >= (int)sizeof(path)) {
		snprintf(b->status, sizeof(b->status), "path too long");
		return ACT_CONTINUE;
	}
	navigate_to(b, path, NULL);
	return ACT_CONTINUE;
}

/****************************************************************
 * Rendering
 ****************************************************************/

static void
human_size(char *buf, size_t sz, long long n)
{
	static const char units[] = { 'B', 'K', 'M', 'G', 'T', 'P' };
	double v = (double)n;
	int u = 0;

	while (v >= 1024.0 && u < (int)sizeof(units) - 1) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(buf, sz, "%lld", n);
	else
		snprintf(buf, sz, "%.1f%c", v, units[u]);
}

static void
draw_bar(struct tui_out *ob, const struct tui_theme *t, const char *text,
    int width)
{
	tui_out_sgr(ob, &t->title_fg, &t->border_bg, 1);
	tui_out_field(ob, text, width);
	tui_out_sgr_reset(ob);
}

/* Draw one list row into exactly `width` columns: a leading gutter, the
 * name (with a trailing '/' for directories), and a right-aligned size for
 * files. Colors come from the theme; the selected row and directories are
 * emphasized. */
static void
draw_entry_row(struct tui_out *ob, const struct tui_theme *t,
    const struct entry *e, int width, int selected)
{
	char left[NAME_MAX + 4];
	char right[20];
	int rightw = 0;
	int leftw;

	if (!e->is_dir && !e->is_updir) {
		char size[16];

		human_size(size, sizeof(size), e->size);
		rightw = snprintf(right, sizeof(right), "%s ", size);
		if (rightw < 0)
			rightw = 0;
	} else {
		right[0] = '\0';
	}

	snprintf(left, sizeof(left), " %s%s", e->name,
	    e->is_dir ? "/" : "");

	leftw = width - rightw;
	if (leftw < 0) {
		leftw = width;
		rightw = 0;
	}

	if (selected)
		tui_out_sgr(ob, &t->sel_fg, &t->sel_bg, 1);
	else
		tui_out_sgr(ob, &t->content_fg, &t->content_bg, e->is_dir);
	tui_out_field(ob, left, leftw);
	if (rightw > 0)
		tui_out_field(ob, right, rightw);
	tui_out_sgr_reset(ob);
}

/****************************************************************
 * Preview pane
 ****************************************************************/

#define PV_MAXLINES	160
#define PV_LINEBYTES	512
#define PV_READMAX	65536

struct preview {
	char	lines[PV_MAXLINES][PV_LINEBYTES];
	int	nlines;
	int	dim;			/* render as a note, not file content */
};

static void
pv_note(struct preview *pv, const char *msg)
{
	snprintf(pv->lines[0], PV_LINEBYTES, "%s", msg);
	pv->nlines = 1;
	pv->dim = 1;
}

/* Append one text line, expanding tabs to 8-column stops and stopping at
 * the line buffer bound. Byte-level clipping is fine here; tui_out_field does
 * the display-width truncation when the line is drawn. */
static void
pv_push_line(struct preview *pv, const char *s, size_t len)
{
	char *out;
	size_t o = 0;
	size_t i;
	int col = 0;

	if (pv->nlines >= PV_MAXLINES)
		return;
	out = pv->lines[pv->nlines];

	for (i = 0; i < len && o < PV_LINEBYTES - 1; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '\t') {
			int n = 8 - (col % 8);

			while (n-- > 0 && o < PV_LINEBYTES - 1) {
				out[o++] = ' ';
				col++;
			}
		} else if (c == '\r') {
			continue;
		} else {
			out[o++] = (char)c;
			col++;
		}
	}
	out[o] = '\0';
	pv->nlines++;
}

/* Return nonzero if the first chunk looks like binary (a NUL byte, or a
 * high share of non-text control bytes). */
static int
looks_binary(const unsigned char *buf, size_t n)
{
	size_t i, bad = 0;
	size_t scan = n < 1024 ? n : 1024;

	for (i = 0; i < scan; i++) {
		unsigned char c = buf[i];

		if (c == 0)
			return 1;
		if (c < 0x09 || (c > 0x0d && c < 0x20))
			bad++;
	}
	return scan > 0 && bad * 100 / scan > 30;
}

static void
build_file_preview(struct preview *pv, const char *path, long long size)
{
	unsigned char buf[PV_READMAX];
	ssize_t n;
	int fd;
	size_t start, i;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		pv_note(pv, "(cannot read file)");
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0) {
		pv_note(pv, "(empty file)");
		return;
	}
	if (looks_binary(buf, (size_t)n)) {
		char msg[64], hs[16];

		human_size(hs, sizeof(hs), size);
		snprintf(msg, sizeof(msg), "(binary, %s)", hs);
		pv_note(pv, msg);
		return;
	}

	start = 0;
	for (i = 0; i < (size_t)n && pv->nlines < PV_MAXLINES; i++) {
		if (buf[i] == '\n') {
			pv_push_line(pv, (const char *)buf + start,
			    i - start);
			start = i + 1;
		}
	}
	if (start < (size_t)n && pv->nlines < PV_MAXLINES)
		pv_push_line(pv, (const char *)buf + start,
		    (size_t)n - start);
}

static void
build_dir_preview(struct preview *pv, const char *path)
{
	struct browser tmp;
	int i;

	memset(&tmp, 0, sizeof(tmp));
	snprintf(tmp.cwd, sizeof(tmp.cwd), "%s", path);
	if (load_dir(&tmp) < 0) {
		pv_note(pv, "(cannot read directory)");
		return;
	}
	/* skip the synthetic ".." the listing pins first */
	for (i = 0; i < tmp.n_ents && pv->nlines < PV_MAXLINES; i++) {
		char line[NAME_MAX + 2];

		if (tmp.ents[i].is_updir)
			continue;
		snprintf(line, sizeof(line), "%s%s", tmp.ents[i].name,
		    tmp.ents[i].is_dir ? "/" : "");
		pv_push_line(pv, line, strlen(line));
	}
	if (pv->nlines == 0)
		pv_note(pv, "(empty)");
	entries_free(&tmp);
}

static void
build_preview(struct browser *b, struct preview *pv)
{
	struct entry *e;
	char path[PATH_MAX];

	pv->nlines = 0;
	pv->dim = 0;

	if (b->sel < 0 || b->sel >= b->n_ents) {
		pv_note(pv, "");
		return;
	}
	e = &b->ents[b->sel];
	if (e->is_updir) {
		pv_note(pv, "(parent directory)");
		return;
	}
	if (snprintf(path, sizeof(path), "%s/%s", b->cwd, e->name)
	    >= (int)sizeof(path)) {
		pv_note(pv, "(path too long)");
		return;
	}
	if (e->is_dir)
		build_dir_preview(pv, path);
	else
		build_file_preview(pv, path, e->size);
}

/****************************************************************
 * Frame
 ****************************************************************/

static void
render(struct browser *b, struct tui_out *ob)
{
	const struct tui_theme *t = tui_theme_default();
	struct preview pv;
	char header[PATH_MAX + 40];
	int list_top = 2;
	int body_h = b->rows - 2;	/* header + footer */
	int two_pane = b->cols >= 64;
	int list_w = b->cols;
	int prev_w = 0;
	int i;

	if (body_h < 1)
		body_h = 1;

	if (two_pane) {
		list_w = b->cols * 2 / 5;
		if (list_w < 24)
			list_w = 24;
		if (list_w > 56)
			list_w = 56;
		prev_w = b->cols - list_w - 1;	/* 1 col separator */
		if (prev_w < 1) {
			two_pane = 0;
			list_w = b->cols;
			prev_w = 0;
		}
	}

	/* keep the selection within the visible window */
	if (b->sel < b->scroll)
		b->scroll = b->sel;
	if (b->sel >= b->scroll + body_h)
		b->scroll = b->sel - body_h + 1;
	if (b->scroll < 0)
		b->scroll = 0;

	if (two_pane) {
		build_preview(b, &pv);
		/* clamp the preview scroll to the built content */
		if (b->pv_scroll > pv.nlines - body_h)
			b->pv_scroll = pv.nlines - body_h;
		if (b->pv_scroll < 0)
			b->pv_scroll = 0;
	}

	tui_out_reset(ob);
	tui_out_puts(ob, "\033[?25l\033[H\033[2J");	/* hide cursor, clear */

	/* header */
	snprintf(header, sizeof(header), " lumi files  %s%s", b->cwd,
	    b->in_session ? "  [session]" : "");
	tui_out_move(ob, 1, 1);
	draw_bar(ob, t, header, b->cols);

	/* body: list on the left, optional preview on the right */
	for (i = 0; i < body_h; i++) {
		int idx = b->scroll + i;

		tui_out_move(ob, list_top + i, 1);
		if (idx >= b->n_ents) {
			tui_out_sgr(ob, &t->content_fg, &t->content_bg, 0);
			tui_out_field(ob, "", list_w);
			tui_out_sgr_reset(ob);
		} else {
			draw_entry_row(ob, t, &b->ents[idx], list_w,
			    idx == b->sel);
		}

		if (two_pane) {
			int pvidx = b->pv_scroll + i;
			const char *line = pvidx < pv.nlines ? pv.lines[pvidx] : "";

			tui_out_sgr(ob, &t->border_fg, &t->border_bg, 0);
			tui_out_puts(ob, t->border[TUI_BORDER_L]);
			tui_out_sgr_reset(ob);
			tui_out_sgr(ob, &t->content_fg, &t->content_bg, 0);
			tui_out_field(ob, line, prev_w);
			tui_out_sgr_reset(ob);
		}
	}

	/* footer: transient status, else key hints */
	tui_out_move(ob, b->rows > 0 ? b->rows : 24, 1);
	if (b->status[0]) {
		char msg[512];

		snprintf(msg, sizeof(msg), " %s", b->status);
		draw_bar(ob, t, msg, b->cols);
	} else {
		draw_bar(ob, t, b->in_session
		    ? " j/k move  Enter open  e edit  o/O win  d/r/m ops  . hidden  q quit"
		    : " j/k move  Enter open  e edit  d/r/m ops  . hidden  ^U/^D preview  q quit",
		    b->cols);
	}

	(void)tui_out_flush(ob, STDOUT_FILENO);
}

/****************************************************************
 * Terminal / lifecycle
 ****************************************************************/

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [path]\n"
	    "\n"
	    "Browse the filesystem in a full-screen terminal UI.\n"
	    "With no path, start in the current directory.\n"
	    "\n"
	    "Keys:\n"
	    "  j / Down      move down\n"
	    "  k / Up        move up\n"
	    "  Enter / l     enter directory, or view a file in $PAGER\n"
	    "  e             edit the selected file in $EDITOR\n"
	    "  o / O         open in a new session window (pager / editor)\n"
	    "  Backspace / h go to parent\n"
	    "  g / G         first / last entry\n"
	    "  Ctrl-U / Ctrl-D  scroll the preview pane\n"
	    "  .             toggle hidden (dot) files\n"
	    "  m             make a directory\n"
	    "  r             rename the selection\n"
	    "  d             delete the selection (asks to confirm)\n"
	    "  q, Esc        quit\n",
	    progname);
}

static void
on_sigwinch(int sig)
{
	(void)sig;
	resized = 1;
}

static int
get_term_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0)
		return -1;
	*rows = ws.ws_row > 0 ? ws.ws_row : 24;
	*cols = ws.ws_col > 0 ? ws.ws_col : 80;
	return 0;
}

/* Move the selection by delta, clamping to the list bounds. */
static void
move_sel(struct browser *b, int delta)
{
	int n = b->n_ents;

	if (n <= 0)
		return;
	b->sel += delta;
	if (b->sel < 0)
		b->sel = 0;
	if (b->sel >= n)
		b->sel = n - 1;
	b->pv_scroll = 0;		/* a new selection previews from the top */
	b->status[0] = '\0';
}

/* Nonzero when the selection is a regular file (not a directory or ".."). */
static int
sel_is_file(const struct browser *b)
{
	return b->sel >= 0 && b->sel < b->n_ents &&
	    !b->ents[b->sel].is_dir && !b->ents[b->sel].is_updir;
}

/* Return nonzero when seq is a quit request. */
static int
is_quit(const struct tkbd_seq *seq)
{
	if (seq->type != TKBD_KEY)
		return 0;
	if (seq->key == TKBD_KEY_ESC)
		return 1;
	if (seq->ch == 'q' || seq->ch == 'Q')
		return 1;
	if ((seq->mod & TKBD_MOD_CTRL) && seq->key == TKBD_KEY_C)
		return 1;
	return 0;
}

/* Act on one key, returning the action for the main loop to carry out. */
/* Drain a bracketed-paste payload (PASTE_BEGIN was just read). The browser
 * has no text field, so pasted content is discarded rather than acted on as
 * a run of key commands. */
static void
swallow_paste(struct tkbd_stream *s)
{
	for (;;) {
		struct tkbd_seq seq;
		int n;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			continue;
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_END)
			break;
	}
}

static enum action
handle_key(struct browser *b, const struct tkbd_seq *seq)
{
	int page = b->rows - 3;

	if (page < 1)
		page = 1;
	if (is_quit(seq))
		return ACT_QUIT;
	if (seq->type != TKBD_KEY)
		return ACT_CONTINUE;

	/* Ctrl-U / Ctrl-D scroll the preview pane by half a screen. */
	if (seq->mod & TKBD_MOD_CTRL) {
		int half = (b->rows - 3) / 2;

		if (half < 1)
			half = 1;
		if (seq->key == TKBD_KEY_D) {
			b->pv_scroll += half;	/* render clamps to content */
			return ACT_CONTINUE;
		}
		if (seq->key == TKBD_KEY_U) {
			b->pv_scroll -= half;
			if (b->pv_scroll < 0)
				b->pv_scroll = 0;
			return ACT_CONTINUE;
		}
	}

	switch (seq->key) {
	case TKBD_KEY_DOWN:
		move_sel(b, 1);
		break;
	case TKBD_KEY_UP:
		move_sel(b, -1);
		break;
	case TKBD_KEY_PGDN:
		move_sel(b, page);
		break;
	case TKBD_KEY_PGUP:
		move_sel(b, -page);
		break;
	case TKBD_KEY_HOME:
		move_sel(b, -b->n_ents);
		break;
	case TKBD_KEY_END:
		move_sel(b, b->n_ents);
		break;
	case TKBD_KEY_ENTER:
	case TKBD_KEY_RIGHT:
		return enter_sel(b);
	case TKBD_KEY_LEFT:
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		go_up(b);
		break;
	default:
		switch (seq->ch) {
		case 'j':
			move_sel(b, 1);
			break;
		case 'k':
			move_sel(b, -1);
			break;
		case 'l':
			return enter_sel(b);
		case 'h':
			go_up(b);
			break;
		case 'g':
			move_sel(b, -b->n_ents);
			break;
		case 'G':
			move_sel(b, b->n_ents);
			break;
		case 'e':
			if (sel_is_file(b))
				return ACT_EDIT;
			break;
		case 'o':
			if (b->in_session && sel_is_file(b))
				return ACT_WIN_PAGE;
			break;
		case 'O':
			if (b->in_session && sel_is_file(b))
				return ACT_WIN_EDIT;
			break;
		case '.':
			b->show_hidden = !b->show_hidden;
			reload_dir(b, NULL);
			snprintf(b->status, sizeof(b->status), "%s hidden files",
			    b->show_hidden ? "showing" : "hiding");
			break;
		case 'd':
			return ACT_DELETE;
		case 'r':
			return ACT_RENAME;
		case 'm':
			return ACT_MKDIR;
		default:
			break;
		}
		break;
	}
	return ACT_CONTINUE;
}

/* Suspend the TUI, run "<prog> <selected file>" through the shell so any
 * flags in $PAGER/$EDITOR are honored, then restore the TUI. The filename
 * is passed as a positional argument so it needs no shell quoting. */
static void
open_file(struct browser *b, struct tkbd_stream *stream, struct tui_out *ob,
    const char *env, const char *fallback)
{
	struct entry *e;
	char path[PATH_MAX];
	char cmd[256];
	const char *prog;
	pid_t pid;

	if (b->sel < 0 || b->sel >= b->n_ents)
		return;
	e = &b->ents[b->sel];
	if (e->is_dir || e->is_updir)
		return;
	if (snprintf(path, sizeof(path), "%s/%s", b->cwd, e->name)
	    >= (int)sizeof(path)) {
		snprintf(b->status, sizeof(b->status), "path too long");
		return;
	}

	prog = getenv(env);
	if (!prog || !*prog)
		prog = fallback;
	snprintf(cmd, sizeof(cmd), "%s \"$1\"", prog);

	/* leave the alternate screen and restore cooked mode for the child */
	tui_out_reset(ob);
	tui_out_puts(ob, "\033[?2004l\033[?25h\033[?1049l");
	(void)tui_out_flush(ob, STDOUT_FILENO);
	tkbd_detach(stream);

	pid = fork();
	if (pid < 0) {
		snprintf(b->status, sizeof(b->status), "fork: %s",
		    strerror(errno));
	} else if (pid == 0) {
		signal(SIGWINCH, SIG_DFL);
		execl("/bin/sh", "sh", "-c", cmd, "sh", path, (char *)NULL);
		_exit(127);
	} else {
		int status;

		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}

	/* re-enter raw mode and the alternate screen; refresh the size in
	 * case the child left the terminal a different shape */
	tkbd_attach(stream, STDIN_FILENO);
	tui_out_reset(ob);
	tui_out_puts(ob, "\033[?1049h\033[?2004h");
	(void)tui_out_flush(ob, STDOUT_FILENO);
	(void)get_term_size(&b->rows, &b->cols);
	b->status[0] = '\0';
}

/* Open the selected file in a new window of the current lumi session, so
 * the browser keeps running in its own window. The command is spawned
 * detached with its standard streams redirected away from this display. */
static void
open_in_window(struct browser *b, int use_editor)
{
	const char *session = getenv("LUMI_SESSION");
	const char *script = use_editor
	    ? "exec ${EDITOR:-vi} \"$1\""
	    : "exec ${PAGER:-less} \"$1\"";
	char path[PATH_MAX];
	pid_t pid;

	if (!session) {
		snprintf(b->status, sizeof(b->status), "not in a session");
		return;
	}
	if (b->sel < 0 || b->sel >= b->n_ents)
		return;
	if (snprintf(path, sizeof(path), "%s/%s", b->cwd,
	    b->ents[b->sel].name) >= (int)sizeof(path)) {
		snprintf(b->status, sizeof(b->status), "path too long");
		return;
	}

	pid = fork();
	if (pid < 0) {
		snprintf(b->status, sizeof(b->status), "fork: %s",
		    strerror(errno));
		return;
	}
	if (pid == 0) {
		char *nw_argv[] = {
			"lumi-new-window", "-s", (char *)session,
			"/bin/sh", "-c", (char *)script, "sh", path, NULL,
		};
		int nul = open("/dev/null", O_RDWR);

		if (nul >= 0) {
			dup2(nul, STDIN_FILENO);
			dup2(nul, STDOUT_FILENO);
			dup2(nul, STDERR_FILENO);
			if (nul > STDERR_FILENO)
				close(nul);
		}
		optind = 1;		/* reset getopt for the reused parser */
		_exit(cmd_new_window_main(8, nw_argv));
	}

	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
	snprintf(b->status, sizeof(b->status), "opened %s in a new window",
	    b->ents[b->sel].name);
}

/****************************************************************
 * Mutating operations (behind a prompt or confirm)
 ****************************************************************/

/* Draw the frame, then overlay a prompt on the footer row with the cursor
 * shown at the end of the typed text. */
static void
draw_prompt(struct browser *b, struct tui_out *ob, const char *q,
    const char *buf)
{
	const struct tui_theme *t = tui_theme_default();
	char line[512];
	int col, row = b->rows > 0 ? b->rows : 24;

	render(b, ob);				/* frame, with the cursor hidden */
	tui_out_reset(ob);
	tui_out_puts(ob, "\033[?25h");		/* show the cursor for input */
	col = snprintf(line, sizeof(line), "%s%s", q, buf ? buf : "");
	tui_out_move(ob, row, 1);
	tui_out_sgr(ob, &t->sel_fg, &t->sel_bg, 1);
	tui_out_field(ob, line, b->cols);
	tui_out_sgr_reset(ob);
	if (col >= b->cols)
		col = b->cols - 1;
	tui_out_move(ob, row, col + 1);
	(void)tui_out_flush(ob, STDOUT_FILENO);
}

/* Read a y/n answer. Returns 1 for yes, 0 for no, -1 if cancelled. */
static int
confirm_yn(struct browser *b, struct tui_out *ob, struct tkbd_stream *s,
    const char *q)
{
	draw_prompt(b, ob, q, "");
	for (;;) {
		struct tkbd_seq seq;
		int n;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			continue;
		if (seq.ch == 'y' || seq.ch == 'Y')
			return 1;
		if (seq.ch == 'n' || seq.ch == 'N')
			return 0;
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return -1;
	}
}

/* Read a line of text. buf is edited in place, so a caller may pre-fill it
 * with a default. Returns 1 with buf filled, or 0 if cancelled or left
 * empty. */
static int
prompt_line(struct browser *b, struct tui_out *ob, struct tkbd_stream *s,
    const char *q, char *buf, size_t bufsz)
{
	size_t len = strlen(buf);

	for (;;) {
		struct tkbd_seq seq;
		int n;

		draw_prompt(b, ob, q, buf);
		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(s, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (n == 0 || seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_ENTER)
			return len > 0;
		if (seq.key == TKBD_KEY_ESC ||
		    ((seq.mod & TKBD_MOD_CTRL) && seq.key == TKBD_KEY_C))
			return 0;
		if (seq.key == TKBD_KEY_BACKSPACE ||
		    seq.key == TKBD_KEY_BACKSPACE2) {
			while (len > 0 &&
			    ((unsigned char)buf[len - 1] & 0xc0) == 0x80)
				len--;		/* drop UTF-8 continuation bytes */
			if (len > 0)
				len--;
			buf[len] = '\0';
			continue;
		}
		if (!(seq.mod & TKBD_MOD_CTRL) && seq.ch != TKBD_CH_NONE &&
		    seq.ch >= 0x20 && seq.ch != 0x7f) {
			unsigned char enc[8];
			int el = utf8_encode(enc, seq.ch);

			if (el > 0 && len + (size_t)el < bufsz) {
				memcpy(buf + len, enc, (size_t)el);
				len += (size_t)el;
				buf[len] = '\0';
			}
		}
	}
}

/* Build "<cwd>/<name>" into out, or set a status and return -1 if it would
 * overflow. */
static int
sel_path(struct browser *b, const char *name, char *out, size_t outsz)
{
	if ((size_t)snprintf(out, outsz, "%s/%s", b->cwd, name) >= outsz) {
		snprintf(b->status, sizeof(b->status), "path too long");
		return -1;
	}
	return 0;
}

/* Delete the selection after a confirm. Directories must be empty (rmdir). */
static void
do_delete(struct browser *b, struct tui_out *ob, struct tkbd_stream *s)
{
	struct entry *e;
	char path[PATH_MAX], q[NAME_MAX + 32];

	if (b->sel < 0 || b->sel >= b->n_ents)
		return;
	e = &b->ents[b->sel];
	if (e->is_updir) {
		snprintf(b->status, sizeof(b->status), "cannot delete \"..\"");
		return;
	}
	if (sel_path(b, e->name, path, sizeof(path)) < 0)
		return;

	snprintf(q, sizeof(q), "Delete %s%s? (y/n) ", e->name,
	    e->is_dir ? "/" : "");
	if (confirm_yn(b, ob, s, q) != 1) {
		snprintf(b->status, sizeof(b->status), "delete cancelled");
		return;
	}
	if ((e->is_dir ? rmdir(path) : unlink(path)) < 0) {
		snprintf(b->status, sizeof(b->status), "cannot delete %.120s: %s",
		    e->name, strerror(errno));
		return;
	}
	reload_dir(b, NULL);
	snprintf(b->status, sizeof(b->status), "deleted");
}

/* Rename the selection within the current directory. */
static void
do_rename(struct browser *b, struct tui_out *ob, struct tkbd_stream *s)
{
	struct entry *e;
	char oldp[PATH_MAX], newp[PATH_MAX], name[NAME_MAX + 1];

	if (b->sel < 0 || b->sel >= b->n_ents)
		return;
	e = &b->ents[b->sel];
	if (e->is_updir) {
		snprintf(b->status, sizeof(b->status), "cannot rename \"..\"");
		return;
	}
	snprintf(name, sizeof(name), "%s", e->name);	/* pre-fill current */
	if (!prompt_line(b, ob, s, "Rename to: ", name, sizeof(name))) {
		snprintf(b->status, sizeof(b->status), "rename cancelled");
		return;
	}
	if (strchr(name, '/')) {
		snprintf(b->status, sizeof(b->status),
		    "name cannot contain '/'");
		return;
	}
	if (sel_path(b, e->name, oldp, sizeof(oldp)) < 0 ||
	    sel_path(b, name, newp, sizeof(newp)) < 0)
		return;
	if (rename(oldp, newp) < 0) {
		snprintf(b->status, sizeof(b->status), "cannot rename: %s",
		    strerror(errno));
		return;
	}
	reload_dir(b, name);
	snprintf(b->status, sizeof(b->status), "renamed to %.120s", name);
}

/* Create a directory in the current directory. */
static void
do_mkdir(struct browser *b, struct tui_out *ob, struct tkbd_stream *s)
{
	char name[NAME_MAX + 1], path[PATH_MAX];

	name[0] = '\0';
	if (!prompt_line(b, ob, s, "New directory: ", name, sizeof(name))) {
		snprintf(b->status, sizeof(b->status), "mkdir cancelled");
		return;
	}
	if (strchr(name, '/')) {
		snprintf(b->status, sizeof(b->status),
		    "name cannot contain '/'");
		return;
	}
	if (sel_path(b, name, path, sizeof(path)) < 0)
		return;
	if (mkdir(path, 0777) < 0) {
		snprintf(b->status, sizeof(b->status), "cannot create %.120s: %s",
		    name, strerror(errno));
		return;
	}
	reload_dir(b, name);
	snprintf(b->status, sizeof(b->status), "created %.120s/", name);
}

int
cmd_files_main(int argc, char **argv)
{
	struct browser b;
	struct tui_out ob = { 0 };
	struct tkbd_stream stream;
	struct sigaction sa;
	const char *start = ".";
	int rc = 0;

	if (argv[0])
		progname = argv[0];

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			usage();
			return 0;
		}
		start = argv[1];
	}

	rune_width_init();		/* honor UNICODE_VERSION for widths */

	memset(&b, 0, sizeof(b));
	b.in_session = getenv("LUMI_SESSION") != NULL;

	if (!realpath(start, b.cwd)) {
		fprintf(stderr, "%s: %s: %s\n", progname, start,
		    strerror(errno));
		return 1;
	}
	if (load_dir(&b) < 0) {
		fprintf(stderr, "%s: %s: %s\n", progname, b.cwd,
		    strerror(errno));
		return 1;
	}

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "%s: not a terminal\n", progname);
		entries_free(&b);
		return 1;
	}

	if (get_term_size(&b.rows, &b.cols) < 0) {
		b.rows = 24;
		b.cols = 80;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigwinch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;		/* let read()/tkbd_read see EINTR */
	sigaction(SIGWINCH, &sa, NULL);

	if (tkbd_attach(&stream, STDIN_FILENO) < 0) {
		fprintf(stderr, "%s: cannot enter raw mode: %s\n",
		    progname, strerror(errno));
		entries_free(&b);
		return 1;
	}

	/* switch to the alternate screen so the shell scrollback survives, and
	 * enable bracketed paste (DECSET 2004) so a stray paste is swallowed
	 * rather than read as a run of navigation keys */
	tui_out_puts(&ob, "\033[?1049h\033[?2004h");
	(void)tui_out_flush(&ob, STDOUT_FILENO);
	render(&b, &ob);

	for (;;) {
		struct tkbd_seq seq;
		int n;

		if (resized) {
			resized = 0;
			if (get_term_size(&b.rows, &b.cols) == 0)
				render(&b, &ob);
		}

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		n = tkbd_read(&stream, &seq);
		if (n < 0) {
			if (errno == EINTR)
				continue;	/* SIGWINCH: loop re-checks */
			rc = 1;
			break;
		}
		if (n == 0)
			continue;		/* idle tick */

		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_BEGIN) {
			swallow_paste(&stream);
			continue;
		}

		switch (handle_key(&b, &seq)) {
		case ACT_QUIT:
			goto done;
		case ACT_PAGE:
			open_file(&b, &stream, &ob, "PAGER", "less");
			break;
		case ACT_EDIT:
			open_file(&b, &stream, &ob, "EDITOR", "vi");
			break;
		case ACT_WIN_PAGE:
			open_in_window(&b, 0);
			break;
		case ACT_WIN_EDIT:
			open_in_window(&b, 1);
			break;
		case ACT_DELETE:
			do_delete(&b, &ob, &stream);
			break;
		case ACT_RENAME:
			do_rename(&b, &ob, &stream);
			break;
		case ACT_MKDIR:
			do_mkdir(&b, &ob, &stream);
			break;
		case ACT_CONTINUE:
			break;
		}
		render(&b, &ob);
	}
done:

	/* restore: show cursor, leave the alternate screen, stop paste mode */
	tui_out_reset(&ob);
	tui_out_puts(&ob, "\033[?2004l\033[?25h\033[?1049l");
	(void)tui_out_flush(&ob, STDOUT_FILENO);

	tkbd_detach(&stream);
	tui_out_free(&ob);
	entries_free(&b);
	return rc;
}
