/* edit.c : lumi edit -- modeless text editor */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "editor.h"
#include "text.h"
#include "tkbd.h"
#include "draw.h"
#include "draw_term.h"
#include "tui_theme.h"
#include "utf8.h"
#include "rune_width.h"
#include "vt_cell.h"
#include "syntax.h"
#include "cfg.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *progname = "lumi-edit";


/* One parsed compiler/make diagnostic (see the build section). */
struct build_err {
	char	path[512];	/* file the diagnostic names */
	long	line;		/* 1-based line, 0 if none */
	long	col;		/* 1-based column, 0 if none */
	char	msg[200];	/* the message text */
};

/****************************************************************
 * Keymap -- the table a modal (vi) personality would swap out
 ****************************************************************/

enum cmd {
	CMD_NONE,
	CMD_INSERT,		/* self-insert the typed character */
	CMD_TAB,
	CMD_NEWLINE,
	CMD_BACKSPACE,
	CMD_DELETE,
	CMD_LEFT,
	CMD_RIGHT,
	CMD_UP,
	CMD_DOWN,
	CMD_HOME,
	CMD_END,
	CMD_PGUP,
	CMD_PGDN,
	CMD_UNDO,
	CMD_REDO,
	CMD_COPY,
	CMD_CUT,
	CMD_PASTE,
	CMD_SEND,		/* send selection/line to another pane */
	CMD_FIND,		/* prompt for a string and jump to it */
	CMD_GOTO,		/* prompt for a line number and jump to it */
	CMD_HELP,		/* show the key bindings */
	CMD_SAVE,
	CMD_QUIT,
};

struct keybind {
	uint16_t	key;		/* TKBD_KEY_* to match */
	uint8_t		ctrl;		/* require the Ctrl modifier */
	enum cmd	cmd;
};

static const struct keybind keymap[] = {
	{ TKBD_KEY_Q,		1, CMD_QUIT },
	{ TKBD_KEY_S,		1, CMD_SAVE },
	{ TKBD_KEY_Z,		1, CMD_UNDO },
	{ TKBD_KEY_Y,		1, CMD_REDO },
	{ TKBD_KEY_C,		1, CMD_COPY },
	{ TKBD_KEY_X,		1, CMD_CUT },
	{ TKBD_KEY_V,		1, CMD_PASTE },
	{ TKBD_KEY_G,		1, CMD_SEND },
	{ TKBD_KEY_F,		1, CMD_FIND },
	{ TKBD_KEY_L,		1, CMD_GOTO },
	{ TKBD_KEY_F1,		0, CMD_HELP },
	{ TKBD_KEY_LEFT,	0, CMD_LEFT },
	{ TKBD_KEY_RIGHT,	0, CMD_RIGHT },
	{ TKBD_KEY_UP,		0, CMD_UP },
	{ TKBD_KEY_DOWN,	0, CMD_DOWN },
	{ TKBD_KEY_HOME,	0, CMD_HOME },
	{ TKBD_KEY_END,		0, CMD_END },
	{ TKBD_KEY_PGUP,	0, CMD_PGUP },
	{ TKBD_KEY_PGDN,	0, CMD_PGDN },
	{ TKBD_KEY_ENTER,	0, CMD_NEWLINE },
	{ TKBD_KEY_BACKSPACE,	0, CMD_BACKSPACE },
	{ TKBD_KEY_BACKSPACE2,	0, CMD_BACKSPACE },
	{ TKBD_KEY_DEL,		0, CMD_DELETE },
	{ TKBD_KEY_TAB,		0, CMD_TAB },
};

#define KEYMAP_COUNT ((int)(sizeof(keymap) / sizeof(keymap[0])))

static enum cmd
key_to_cmd(const struct tkbd_seq *seq)
{
	int i;

	if (seq->type != TKBD_KEY)
		return CMD_NONE;

	for (i = 0; i < KEYMAP_COUNT; i++) {
		int want_ctrl = keymap[i].ctrl;
		int has_ctrl = (seq->mod & TKBD_MOD_CTRL) != 0;

		if (keymap[i].key == seq->key && want_ctrl == has_ctrl)
			return keymap[i].cmd;
	}

	/* an ordinary printable character self-inserts */
	if (!(seq->mod & TKBD_MOD_CTRL) && seq->ch != TKBD_CH_NONE &&
	    seq->ch >= 0x20 && seq->ch != 0x7f)
		return CMD_INSERT;

	return CMD_NONE;
}

/****************************************************************
 * UTF-8 and display-column helpers
 ****************************************************************/

/* Display columns spanned by the first nbytes of s, expanding tabs to the
 * next TAB_WIDTH stop and honoring rune widths. */
int
disp_cols(const char *s, size_t nbytes)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;

	while (i < nbytes) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, nbytes - i);
		int w;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}
		col += w;
		i += (size_t)n;
	}
	return col;
}

/* Byte length of the UTF-8 rune ending just before byte offset cx. */
size_t
prev_rune_len(const char *s, size_t cx)
{
	size_t n = 1;

	while (n < cx && ((unsigned char)s[cx - n] & 0xc0) == 0x80)
		n++;
	return n;
}

/* Byte length of the UTF-8 rune starting at byte offset cx. */
size_t
rune_len_at(const char *s, size_t len, size_t cx)
{
	uint32_t r;
	int n;

	if (cx >= len)
		return 0;
	n = utf8_decode(&r, (const unsigned char *)s + cx, len - cx);
	return n > 0 ? (size_t)n : 1;
}

/****************************************************************
 * Syntax highlighting
 ****************************************************************/

/* Foreground color for each highlight style. SYN_TEXT and SYN_OPERATOR fall
 * back to the terminal default foreground so ordinary code is left alone. An
 * [edit.syntax] section in lumi.conf overrides any of these at startup (see
 * load_syntax_colors). */
#define SYN_IDX(n) { .type = VT_COLOR_INDEXED, { .index = (n) } }
static struct vt_color syn_color[SYN_STYLE_COUNT] = {
	[SYN_TEXT] = { .type = VT_COLOR_DEFAULT },
	[SYN_COMMENT] = SYN_IDX(244),
	[SYN_KEYWORD] = SYN_IDX(3),
	[SYN_TYPE] = SYN_IDX(6),
	[SYN_CONSTANT] = SYN_IDX(5),
	[SYN_STRING] = SYN_IDX(2),
	[SYN_OPERATOR] = { .type = VT_COLOR_DEFAULT },
	[SYN_FUNCTION] = SYN_IDX(4),
	[SYN_PREPROC] = SYN_IDX(1),
};
#undef SYN_IDX

/* The [edit.syntax] key that names each highlight style. */
static const struct {
	const char	*name;
	enum syn_style	style;
} syn_style_names[] = {
	{ "text",     SYN_TEXT },
	{ "comment",  SYN_COMMENT },
	{ "keyword",  SYN_KEYWORD },
	{ "type",     SYN_TYPE },
	{ "constant", SYN_CONSTANT },
	{ "string",   SYN_STRING },
	{ "operator", SYN_OPERATOR },
	{ "function", SYN_FUNCTION },
	{ "preproc",  SYN_PREPROC },
};
#define SYN_STYLE_NAMES_COUNT \
	((int)(sizeof(syn_style_names) / sizeof(syn_style_names[0])))

/* Map an [edit.syntax] key to a highlight style, or -1 when unknown. */
static int
syn_style_by_name(const char *name)
{
	int i;

	for (i = 0; i < SYN_STYLE_NAMES_COUNT; i++)
		if (strcmp(syn_style_names[i].name, name) == 0)
			return (int)syn_style_names[i].style;
	return -1;
}

/* Parse value ("default", "0"-"255", or "#rrggbb") into the palette slot named
 * by "name". Returns 0 when applied, -1 on an unknown name or a bad color. */
static int
syn_apply(struct vt_color *palette, const char *name, const char *value)
{
	struct vt_color c;
	int st = syn_style_by_name(name);

	if (st < 0 || tui_theme_parse_color(value, &c) != 0)
		return -1;
	palette[st] = c;
	return 0;
}

/* The extension of a path (after the last '.'), or "" when there is none. */
static const char *
file_ext(const char *path)
{
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');

	if (!dot || (slash && dot < slash) || dot[1] == '\0')
		return "";
	return dot + 1;
}

/* Mark the highlighter's cached start-states stale from `line` onward: the
 * state entering `line` is unchanged, but everything after it may differ. */
void
hl_touch(struct editor *e, size_t line)
{
	if (e->hl_valid > line + 1)
		e->hl_valid = line + 1;
}

/* Ensure line_state[0..upto) hold correct start-states, tokenizing forward
 * from the last valid line. Cheap once warm; after an edit it recomputes only
 * from the change down to the bottom of the view. */
static void
hl_ensure(struct editor *e, size_t upto)
{
	size_t nl = text_lines(e->t);

	if (!e->syn || !e->hl_on)
		return;
	if (upto > nl)
		upto = nl;
	if (e->line_state_cap < nl) {
		size_t cap = e->line_state_cap ? e->line_state_cap : 64;
		uint16_t *p;

		while (cap < nl)
			cap *= 2;
		p = realloc(e->line_state, cap * sizeof(*p));
		if (!p)
			return;			/* skip highlighting this frame */
		e->line_state = p;
		e->line_state_cap = cap;
	}
	if (e->hl_valid == 0) {
		e->line_state[0] = e->syn->start;
		e->hl_valid = 1;
	}
	if (e->hl_valid > nl)
		e->hl_valid = nl;		/* the buffer lost lines */
	while (e->hl_valid < upto) {
		size_t i = e->hl_valid - 1;
		size_t llen = 0;
		const char *s = text_line(e->t, i, &llen);

		e->line_state[e->hl_valid] =
		    syn_line(e->syn, e->line_state[i], s ? s : "", llen, NULL);
		e->hl_valid++;
	}
}

/* Compute the per-byte styles for line idx into e->hl_buf, or return NULL when
 * highlighting is off or the line has no cached start-state. */
static const uint8_t *
hl_line(struct editor *e, size_t idx, const char *s, size_t llen)
{
	if (!e->syn || !e->hl_on || idx >= e->hl_valid)
		return NULL;
	if (e->hl_buf_cap < llen) {
		size_t cap = e->hl_buf_cap ? e->hl_buf_cap : 128;
		uint8_t *p;

		while (cap < llen)
			cap *= 2;
		p = realloc(e->hl_buf, cap);
		if (!p)
			return NULL;
		e->hl_buf = p;
		e->hl_buf_cap = cap;
	}
	syn_line(e->syn, e->line_state[idx], s ? s : "", llen, e->hl_buf);
	return e->hl_buf;
}

/****************************************************************
 * DOS-style chrome: menu bar, framed window, scrollbars, status
 *
 * The chrome frames the text area, so the editable region is inset by a
 * menu bar and top border above, a bottom border and status bar below, and
 * a border column on each side (the right one doubles as the vertical
 * scrollbar). Geometry is derived from e->rows/e->cols through the helpers
 * below; the text origin is fixed at (CHROME_TOP, CHROME_LEFT).
 ****************************************************************/

#define CHROME_TOP	2	/* first text row (menu bar 0, top border 1) */
#define CHROME_LEFT	1	/* first text column (left border is column 0) */
#define CHROME_BOTTOM	2	/* rows below the text (bottom border + status) */
#define CHROME_RIGHT	1	/* columns right of the text (border/scrollbar) */

static void draw_field(struct draw *d, int row, int col, int width,
    const char *s, struct vt_color fg, struct vt_color bg, uint16_t attrs);

/* Box-drawing and scrollbar glyphs used by the frame. */
#define GL_TL		0x250cu	/* corners */
#define GL_TR		0x2510u
#define GL_BL		0x2514u
#define GL_BR		0x2518u
#define GL_H		0x2500u	/* horizontal / vertical edges */
#define GL_V		0x2502u
#define GL_UP		0x25b2u	/* scrollbar arrows, thumb, track */
#define GL_DOWN		0x25bcu
#define GL_LEFT		0x25c4u
#define GL_RIGHT	0x25bau
#define GL_THUMB	0x2588u
#define GL_TRACK	0x2591u
#define GL_CHECK	0x2022u	/* bullet marking an enabled toggle item */

/* Editor chrome palette. The two presets are the DOS look (blue text area,
 * gray bars) and a monochrome fallback that leans on reverse video. */
struct chrome_pal {
	struct vt_color	content_fg, content_bg;	/* the text area */
	struct vt_color	frame_fg, frame_bg;	/* window border + scrollbars */
	struct vt_color	title_fg;		/* filename in the top border */
	struct vt_color	bar_fg, bar_bg;		/* menu bar + status bar */
	int		reverse_bars;		/* draw the bars in reverse video */
};

#define CIDX(n) { .type = VT_COLOR_INDEXED, { .index = (n) } }
#define CDEF	{ .type = VT_COLOR_DEFAULT }
/* The default DOS look. edit.theme in lumi.conf replaces it at startup with a
 * palette derived from a named tui_theme (see chrome_from_theme). */
static struct chrome_pal chrome_dos = {
	.content_fg = CIDX(15), .content_bg = CIDX(4),
	.frame_fg = CIDX(15), .frame_bg = CIDX(4),
	.title_fg = CIDX(15),
	.bar_fg = CIDX(0), .bar_bg = CIDX(7),
	.reverse_bars = 0,
};
static const struct chrome_pal chrome_plain = {
	.content_fg = CDEF, .content_bg = CDEF,
	.frame_fg = CDEF, .frame_bg = CDEF,
	.title_fg = CDEF,
	.bar_fg = CDEF, .bar_bg = CDEF,
	.reverse_bars = 1,
};
#undef CIDX
#undef CDEF

/* Derive the DOS-chrome palette from a tui_theme. The text area and frame map
 * straight across; a theme has no menu/status-bar color, so the bars invert
 * the content colors to read as a light strip over the editing area. */
static void
chrome_from_theme(const struct tui_theme *t, struct chrome_pal *out)
{
	out->content_fg = t->content_fg;
	out->content_bg = t->content_bg;
	out->frame_fg = t->border_fg;
	out->frame_bg = t->border_bg;
	out->title_fg = t->title_fg;
	out->bar_fg = t->content_bg;
	out->bar_bg = t->content_fg;
	out->reverse_bars = 0;
}

static const struct chrome_pal *
chrome(const struct editor *e)
{
	return e->dos_chrome ? &chrome_dos : &chrome_plain;
}

/* Height of the framed text area (rows minus menu, two borders, status). */
int
text_height(const struct editor *e)
{
	int h = e->rows - CHROME_TOP - CHROME_BOTTOM;

	return h < 1 ? 1 : h;
}

/* Width of the framed text area (cols minus the two border columns). */
static int
text_width(const struct editor *e)
{
	int w = e->cols - CHROME_LEFT - CHROME_RIGHT;

	return w < 1 ? 1 : w;
}

/* Paint one scrollbar cell, choosing the thumb where pos falls in [0,span). */
static void
scrollbar_cell(struct draw *d, int row, int col, int idx, int span,
    int thumb, uint32_t arrow_a, uint32_t arrow_b,
    struct vt_color fg, struct vt_color bg)
{
	uint32_t cp;

	if (idx == 0)
		cp = arrow_a;
	else if (idx == span - 1)
		cp = arrow_b;
	else
		cp = (idx == thumb) ? GL_THUMB : GL_TRACK;
	draw_cell(d, row, col, cp, fg, bg, 0);
}

/* Thumb index within a track of tspan interior cells for a scroll offset. */
static int
thumb_index(size_t off, size_t max_off, int span)
{
	int inner = span - 2;		/* interior between the two arrows */
	int t;

	if (inner < 1 || max_off == 0)
		return 1;
	t = 1 + (int)((off * (size_t)(inner - 1)) / max_off);
	if (t < 1)
		t = 1;
	if (t > span - 2)
		t = span - 2;
	return t;
}

/* Menu bar model: a fixed set of pull-down menus. Each item names an action
 * the main loop carries out; a separator (MA_SEP) is a non-selectable rule. */
enum menu_act {
	MA_NONE, MA_SEP,
	MA_NEW, MA_OPEN, MA_SAVE, MA_SAVE_AS,
	MA_BUF_NEXT, MA_BUF_PREV, MA_BUF_LIST, MA_EXIT,
	MA_UNDO, MA_REDO, MA_CUT, MA_COPY, MA_PASTE,
	MA_FIND, MA_FIND_NEXT, MA_GOTO,
	MA_RUN, MA_COMPILE, MA_MAKE, MA_NEXT_ERR, MA_PREV_ERR,
	MA_SYNTAX, MA_SCHEME, MA_HEX, MA_VI_MODE, MA_MOUSE,
	MA_HELP, MA_ABOUT,
};

struct menu_item {
	const char	*label;
	const char	*accel;		/* modeless shortcut, right-aligned, or "" */
	const char	*vaccel;	/* vi-personality shortcut, or "" to reuse accel */
	enum menu_act	act;
};

struct menu_def {
	const char	*title;
	int		col;		/* start column on the bar (Help: dynamic) */
	const struct menu_item *items;
	int		n;
};

static const struct menu_item mi_file[] = {
	{ "&New",	"",		"",	MA_NEW },
	{ "&Open...",	"",		"",	MA_OPEN },
	{ "&Save",	"Ctrl+S",	":w",	MA_SAVE },
	{ "Save &As...","",		"",	MA_SAVE_AS },
	{ "",		"",		"",	MA_SEP },
	{ "Next &Buffer","F8",		":bn",	MA_BUF_NEXT },
	{ "&Prev Buffer","Shift+F8",	":bp",	MA_BUF_PREV },
	{ "Buffer &List","",		":ls",	MA_BUF_LIST },
	{ "",		"",		"",	MA_SEP },
	{ "E&xit",	"Ctrl+Q",	":q",	MA_EXIT },
};
static const struct menu_item mi_edit[] = {
	{ "&Undo",	"Ctrl+Z",	"u",		MA_UNDO },
	{ "&Redo",	"Ctrl+Y",	"Ctrl+R",	MA_REDO },
	{ "",		"",		"",		MA_SEP },
	{ "Cu&t",	"Ctrl+X",	"dd",		MA_CUT },
	{ "&Copy",	"Ctrl+C",	"yy",		MA_COPY },
	{ "&Paste",	"Ctrl+V",	"p",		MA_PASTE },
};
static const struct menu_item mi_search[] = {
	{ "&Find...",		"Ctrl+F",	"/",	MA_FIND },
	{ "&Repeat Find",	"",		"n",	MA_FIND_NEXT },
	{ "&Go to Line...",	"Ctrl+L",	"G",	MA_GOTO },
};
static const struct menu_item mi_build[] = {
	{ "&Run",	"F5",	"",	MA_RUN },
	{ "&Compile",	"F6",	"",	MA_COMPILE },
	{ "&Make",	"F7",	"",	MA_MAKE },
	{ "",		"",	"",	MA_SEP },
	{ "&Next Error","F4",	"",	MA_NEXT_ERR },
	{ "&Prev Error","Shift+F4", "",	MA_PREV_ERR },
};
static const struct menu_item mi_view[] = {
	{ "&Syntax Highlight",	"",	"",	MA_SYNTAX },
	{ "&Color Scheme",	"",	"",	MA_SCHEME },
	{ "&Hex Dump",		"",	"",	MA_HEX },
};
static const struct menu_item mi_options[] = {
	{ "&Vi Keys",	"F2",	"",	MA_VI_MODE },
	{ "&Mouse",	"",	"",	MA_MOUSE },
};
static const struct menu_item mi_help[] = {
	{ "&Key Bindings",	"F1",	"",	MA_HELP },
	{ "&About",		"",	"",	MA_ABOUT },
};

#define MENU_ITEMS(a) (a), (int)(sizeof(a) / sizeof((a)[0]))
static const struct menu_def MENUS[] = {
	{ "&File",	1,	MENU_ITEMS(mi_file) },
	{ "&Edit",	7,	MENU_ITEMS(mi_edit) },
	{ "&Search",	13,	MENU_ITEMS(mi_search) },
	{ "&Build",	21,	MENU_ITEMS(mi_build) },
	{ "&View",	28,	MENU_ITEMS(mi_view) },
	{ "&Options",	34,	MENU_ITEMS(mi_options) },
	{ "&Help",	0,	MENU_ITEMS(mi_help) },	/* col set dynamically */
};
#undef MENU_ITEMS
#define MENU_COUNT ((int)(sizeof(MENUS) / sizeof(MENUS[0])))
#define MENU_HELP (MENU_COUNT - 1)

/* A menu title or item label may mark its mnemonic with '&' before the chosen
 * letter (DOS style: the highlighted key that selects the entry). A literal
 * ampersand is written "&&". These helpers read such a string. */

/* Display width of a label, not counting the '&' mnemonic markers. */
static int
menu_disp_w(const char *s)
{
	int w = 0;

	while (*s) {
		if (*s == '&' && s[1] == '&') {
			s += 2;
			w++;
		} else if (*s == '&' && s[1]) {
			s++;		/* marker: no column of its own */
		} else {
			s++;
			w++;
		}
	}
	return w;
}

/* The lowercased mnemonic letter of a label, or 0 when it has none. */
static int
menu_mnemonic(const char *s)
{
	for (; *s; s++) {
		if (*s == '&' && s[1] == '&')
			s++;			/* literal "&&", skip both */
		else if (*s == '&' && s[1])
			return tolower((unsigned char)s[1]);
	}
	return 0;
}

/* Draw a label, dropping the '&' markers and underlining the mnemonic letter.
 * Returns the column after the last cell written. */
static int
draw_menu_label(struct draw *d, int r, int c, const char *s,
    struct vt_color fg, struct vt_color bg, uint16_t at)
{
	char buf[2] = { 0, 0 };

	for (; *s; s++) {
		uint16_t a = at;

		if (*s == '&' && s[1] == '&')
			s++;			/* "&&" -> literal '&' */
		else if (*s == '&' && s[1]) {
			a |= VT_ATTR_UNDERLINE;
			s++;
		}
		buf[0] = *s;
		c = draw_text(d, r, c, buf, fg, bg, a);
	}
	return c;
}

/* Top-level menu whose title mnemonic is lc (a lowercased letter), or -1. */
static int
menu_title_by_mnemonic(int lc)
{
	int i;

	for (i = 0; i < MENU_COUNT; i++)
		if (menu_mnemonic(MENUS[i].title) == lc)
			return i;
	return -1;
}

/* Selectable item of menu m whose mnemonic is lc, or -1. Separators never
 * match. */
static int
menu_item_by_mnemonic(int m, int lc)
{
	int i;

	for (i = 0; i < MENUS[m].n; i++)
		if (MENUS[m].items[i].act != MA_SEP &&
		    menu_mnemonic(MENUS[m].items[i].label) == lc)
			return i;
	return -1;
}

/* Bar column of menu i; Help is right-aligned. */
static int
menu_col(const struct editor *e, int i)
{
	if (i == MENU_HELP)
		return e->cols - 5;
	return MENUS[i].col;
}

/* Which top-level menu title column x falls on, or -1. */
static int
menu_hit(const struct editor *e, int x)
{
	int i;

	for (i = 0; i < MENU_COUNT; i++) {
		int c = menu_col(e, i);

		if (x >= c && x < c + menu_disp_w(MENUS[i].title))
			return i;
	}
	return -1;
}

static void
draw_menubar(struct editor *e, const struct chrome_pal *p, int active)
{
	uint16_t at = p->reverse_bars ? VT_ATTR_REVERSE : 0;
	int i;

	draw_fill(e->d, 0, 0, e->cols, ' ', p->bar_fg, p->bar_bg, at);
	for (i = 0; i < MENU_COUNT; i++) {
		uint16_t a = (i == active) ? (at ^ VT_ATTR_REVERSE) : at;
		int col = menu_col(e, i);

		if (col < 0 || col >= e->cols)
			continue;
		draw_menu_label(e->d, 0, col, MENUS[i].title, p->bar_fg,
		    p->bar_bg, a);
	}
}

/* Clamped left column of menu i's drop-down box (kept on screen). */
static int
dropdown_x(const struct editor *e, int mi, int boxw)
{
	int x = menu_col(e, mi);

	if (x + boxw > e->cols)
		x = e->cols - boxw;
	if (x < 0)
		x = 0;
	return x;
}

/* Accelerator to display for an item under the active personality. The
 * modeless Ctrl+ chords do not reach the editor in the vi personalities, so
 * show the vi keys that carry out the same action instead. An empty vaccel
 * means the modeless accel applies in both (e.g. the F1/F2 function keys). */
static const char *
item_accel(const struct editor *e, const struct menu_item *it)
{
	if (e->mode != MODE_MODELESS && it->vaccel[0])
		return it->vaccel;
	return it->accel;
}

/* Inner width of menu i's drop-down (widest "label  accel"). */
static int
dropdown_width(const struct editor *e, int mi)
{
	const struct menu_def *m = &MENUS[mi];
	int i, w = 0;

	for (i = 0; i < m->n; i++) {
		const struct menu_item *it = &m->items[i];
		const char *accel = item_accel(e, it);
		int lw = menu_disp_w(it->label);

		if (accel[0])
			lw += 2 + (int)strlen(accel);
		if (lw > w)
			w = lw;
	}
	return w + 2;		/* one space of padding on each side */
}

/* Toggle state of a menu action: 1 on, 0 off, -1 when it is not a toggle. */
static int
menu_checked(const struct editor *e, enum menu_act act)
{
	switch (act) {
	case MA_SYNTAX:
		return e->hl_on ? 1 : 0;
	case MA_SCHEME:
		return e->dos_chrome ? 1 : 0;
	case MA_VI_MODE:
		return e->mode != MODE_MODELESS ? 1 : 0;
	case MA_MOUSE:
		return e->mouse_on ? 1 : 0;
	default:
		return -1;
	}
}

static void
draw_dropdown(struct editor *e, int mi, int sel)
{
	const struct chrome_pal *p = chrome(e);
	const struct menu_def *m = &MENUS[mi];
	struct draw *d = e->d;
	struct vt_color fg = p->bar_fg, bg = p->bar_bg;
	uint16_t base = p->reverse_bars ? VT_ATTR_REVERSE : 0;
	int w = dropdown_width(e, mi);
	int boxw = w + 2;
	int x = dropdown_x(e, mi, boxw);
	int y = 1;			/* top border sits under the bar */
	int i;

	/* top and bottom border */
	draw_cell(d, y, x, GL_TL, fg, bg, base);
	draw_cell(d, y, x + boxw - 1, GL_TR, fg, bg, base);
	draw_cell(d, y + m->n + 1, x, GL_BL, fg, bg, base);
	draw_cell(d, y + m->n + 1, x + boxw - 1, GL_BR, fg, bg, base);
	for (i = 0; i < w; i++) {
		draw_cell(d, y, x + 1 + i, GL_H, fg, bg, base);
		draw_cell(d, y + m->n + 1, x + 1 + i, GL_H, fg, bg, base);
	}

	for (i = 0; i < m->n; i++) {
		const struct menu_item *it = &m->items[i];
		const char *accel;
		int row = y + 1 + i;
		uint16_t at = base;

		draw_cell(d, row, x, GL_V, fg, bg, base);
		draw_cell(d, row, x + boxw - 1, GL_V, fg, bg, base);
		if (it->act == MA_SEP) {
			int c;

			for (c = 0; c < w; c++)
				draw_cell(d, row, x + 1 + c, GL_H, fg, bg,
				    base);
			continue;
		}
		if (i == sel)
			at = base ^ VT_ATTR_REVERSE;	/* highlight bar */
		draw_fill(d, row, x + 1, w, ' ', fg, bg, at);
		if (menu_checked(e, it->act) == 1)
			draw_cell(d, row, x + 1, GL_CHECK, fg, bg, at);
		draw_menu_label(d, row, x + 2, it->label, fg, bg, at);
		accel = item_accel(e, it);
		if (accel[0])
			draw_text(d, row, x + 1 + w - 1 - (int)strlen(accel),
			    accel, fg, bg, at);
	}
}

/* Draw a bordered, filled box of w by h cells at (x, y). Used by the modal
 * dialogs. */
static void
draw_box(struct draw *d, int x, int y, int w, int h,
    struct vt_color fg, struct vt_color bg, uint16_t at)
{
	int r, c;

	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			uint32_t cp = ' ';

			if (r == 0)
				cp = c == 0 ? GL_TL :
				    c == w - 1 ? GL_TR : GL_H;
			else if (r == h - 1)
				cp = c == 0 ? GL_BL :
				    c == w - 1 ? GL_BR : GL_H;
			else if (c == 0 || c == w - 1)
				cp = GL_V;
			draw_cell(d, y + r, x + c, cp, fg, bg, at);
		}
}

/* Top-left corner that centers a boxw by boxh overlay in the screen, clamped
 * to stay on screen. Used by the modal dialogs at open and on resize. */
static void
center_box(const struct editor *e, int boxw, int boxh, int *x, int *y)
{
	*x = (e->cols - boxw) / 2;
	*y = (e->rows - boxh) / 2;
	if (*x < 0)
		*x = 0;
	if (*y < 0)
		*y = 0;
}

/* The bar palette the modal overlays draw with (menu bar colors, reverse
 * video in the plain scheme). */
static void
dialog_palette(const struct editor *e, struct vt_color *fg, struct vt_color *bg,
    uint16_t *base)
{
	const struct chrome_pal *p = chrome(e);

	*fg = p->bar_fg;
	*bg = p->bar_bg;
	*base = p->reverse_bars ? VT_ATTR_REVERSE : 0;
}

static void
draw_frame(struct editor *e, const struct chrome_pal *p)
{
	struct draw *d = e->d;
	struct vt_color fg = p->frame_fg, bg = p->frame_bg;
	int top = CHROME_TOP - 1;	/* top border row, under the menu bar */
	int bot = e->rows - CHROME_BOTTOM;	/* bottom border row */
	int sb = e->cols - CHROME_RIGHT;	/* right border / vertical bar */
	int th = text_height(e);
	int i;
	size_t nlines = text_lines(e->t);
	size_t max_top = nlines > (size_t)th ? nlines - (size_t)th : 0;
	size_t curlen = 0;
	const char *cur = text_line(e->t, e->cy, &curlen);
	int curw = cur ? disp_cols(cur, curlen) : 0;
	int tw = text_width(e);
	size_t max_left = curw > tw ? (size_t)(curw - tw) : 0;
	int vthumb = thumb_index(e->top, max_top, th);
	int hthumb;
	const char *name = e->has_name ? e->path : "Untitled";
	char title[80];
	int tlen, tstart;

	if (bot < top)
		bot = top;

	/* top and bottom borders */
	draw_fill(d, top, 0, e->cols, GL_H, fg, bg, 0);
	draw_fill(d, bot, 0, e->cols, GL_H, fg, bg, 0);
	draw_cell(d, top, 0, GL_TL, fg, bg, 0);
	draw_cell(d, top, sb, GL_TR, fg, bg, 0);
	draw_cell(d, bot, 0, GL_BL, fg, bg, 0);
	draw_cell(d, bot, sb, GL_BR, fg, bg, 0);

	/* centered filename on the top border, bracketed by spaces; a buffer
	 * index is prefixed when more than one file is open */
	if (e->nbuf > 1)
		tlen = snprintf(title, sizeof(title), " [%d/%d] %s ",
		    e->cur + 1, e->nbuf, name);
	else
		tlen = snprintf(title, sizeof(title), " %s ", name);
	if (tlen > e->cols - 4)
		tlen = e->cols - 4;
	if (tlen > 0) {
		tstart = (e->cols - tlen) / 2;
		if (tstart < 1)
			tstart = 1;
		draw_field(d, top, tstart, tlen, title, p->title_fg, bg,
		    VT_ATTR_BOLD);
	}

	/* left border column and the vertical scrollbar column */
	for (i = 0; i < th; i++) {
		int r = CHROME_TOP + i;

		draw_cell(d, r, 0, GL_V, fg, bg, 0);
		if (th >= 3)
			scrollbar_cell(d, r, sb, i, th, vthumb,
			    GL_UP, GL_DOWN, fg, bg);
		else
			draw_cell(d, r, sb, GL_V, fg, bg, 0);
	}

	/* horizontal scrollbar embedded in the bottom border */
	if (e->cols >= 6) {
		int hspan = e->cols - CHROME_LEFT - CHROME_RIGHT; /* corners off */

		hthumb = thumb_index(e->left, max_left, hspan);
		for (i = 0; i < hspan; i++)
			scrollbar_cell(d, bot, 1 + i, i, hspan, hthumb,
			    GL_LEFT, GL_RIGHT, fg, bg);
	}
}

static void
draw_statusbar(struct editor *e, const struct chrome_pal *p, int cur_col)
{
	int row = e->rows - 1;
	uint16_t at = p->reverse_bars ? VT_ATTR_REVERSE : 0;
	char right[64];
	int rlen;

	draw_fill(e->d, row, 0, e->cols, ' ', p->bar_fg, p->bar_bg, at);

	if (e->status[0]) {
		draw_text(e->d, row, 1, e->status, p->bar_fg, p->bar_bg, at);
	} else {
		const char *mode = "";

		if (e->vi_visual == 'v')
			mode = "-- VISUAL --  ";
		else if (e->vi_visual == 'V')
			mode = "-- VISUAL LINE --  ";
		else if (e->mode == MODE_NORMAL)
			mode = "-- NORMAL --  ";
		else if (e->mode == MODE_INSERT)
			mode = "-- INSERT --  ";
		draw_text(e->d, row, 1, mode, p->bar_fg, p->bar_bg, at);
		draw_text(e->d, row, 1 + (int)strlen(mode), "F1=Help",
		    p->bar_fg, p->bar_bg, at);
	}

	rlen = snprintf(right, sizeof(right), "Line:%zu  Col:%zu%s%s",
	    e->cy + 1, (size_t)cur_col + 1,
	    text_dirty(e->t) ? "  *" : "",
	    e->in_session ? "  [session]" : "");
	if (rlen > 0 && rlen < e->cols - 1)
		draw_text(e->d, row, e->cols - rlen - 1, right,
		    p->bar_fg, p->bar_bg, at);
}

/****************************************************************
 * Rendering
 ****************************************************************/

/* Draw one text line clipped to the display window [left, left+width),
 * expanding tabs. Trailing space pads the field to width. Display columns in
 * [hl_start, hl_end) are shown in reverse video for the selection; pass
 * hl_start >= hl_end for no highlight. */
static void
draw_line(struct draw *d, int row, int col0, const char *s, size_t len,
    int left, int width, int hl_start, int hl_end, const uint8_t *sty,
    struct vt_color base_fg, struct vt_color base_bg)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	int col = 0;		/* display column at the start of this rune */
	int drawn = 0;		/* columns emitted into the window */

	while (i < len && drawn < width) {
		uint32_t r;
		int n = utf8_decode(&r, p + i, len - i);
		int w;
		int rev;
		uint16_t attrs;
		struct vt_color fg;

		if (n <= 0)
			n = 1;
		if (r == '\t')
			w = TAB_WIDTH - (col % TAB_WIDTH);
		else if (r < 0x20 || r == 0x7f)
			w = 1;
		else {
			w = rune_width(r);
			if (w < 0)
				w = 1;
		}

		if (col + w <= left) {
			/* wholly left of the window */
			col += w;
			i += (size_t)n;
			continue;
		}

		/* reverse video marks the selection (its bounds fall on rune
		 * edges, so a whole rune is in or out); syntax colors the fg */
		rev = (hl_start < hl_end && col >= hl_start && col < hl_end);
		attrs = rev ? VT_ATTR_REVERSE : 0;
		fg = sty ? syn_color[sty[i]] : base_fg;

		if (r == '\t' || r < 0x20 || r == 0x7f) {
			/* render as spaces, clipped at both edges */
			int c;

			for (c = 0; c < w; c++) {
				if (col + c < left)
					continue;
				if (drawn >= width)
					break;
				draw_cell(d, row, col0 + drawn, ' ', fg, base_bg,
				    attrs);
				drawn++;
			}
		} else if (col < left) {
			/* a wide rune straddling the left edge: pad */
			int c;

			for (c = 0; c < w && drawn < width; c++) {
				draw_cell(d, row, col0 + drawn, ' ', fg, base_bg,
				    attrs);
				drawn++;
			}
		} else if (drawn + w > width) {
			break;			/* would overflow right edge */
		} else if (w > 0) {
			draw_cell(d, row, col0 + drawn, r, fg, base_bg, attrs);
			drawn += w;
		}
		col += w;
		i += (size_t)n;
	}
	while (drawn < width) {
		draw_cell(d, row, col0 + drawn, ' ', base_fg, base_bg, 0);
		drawn++;
	}
}

/* Adjust top/left so the cursor stays on screen. */
static void
scroll_to_cursor(struct editor *e, int text_h, int text_w)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);
	int cur_col = line ? disp_cols(line, e->cx) : 0;

	if (e->cy < e->top)
		e->top = e->cy;
	if (text_h > 0 && e->cy >= e->top + (size_t)text_h)
		e->top = e->cy - (size_t)text_h + 1;

	if ((size_t)cur_col < e->left)
		e->left = (size_t)cur_col;
	if (text_w > 0 && (size_t)cur_col >= e->left + (size_t)text_w)
		e->left = (size_t)cur_col - (size_t)text_w + 1;
}

/* Order the anchor and cursor so (*y1,*x1) is the start of the selection and
 * (*y2,*x2) the end. */
static void
sel_bounds(const struct editor *e, size_t *y1, size_t *x1,
    size_t *y2, size_t *x2)
{
	if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
		*y1 = e->ay;
		*x1 = e->ax;
		*y2 = e->cy;
		*x2 = e->cx;
	} else {
		*y1 = e->cy;
		*x1 = e->cx;
		*y2 = e->ay;
		*x2 = e->ax;
	}
}

/* Fill row [col, col+width) with a color, then write s over the start of it,
 * so the text sits in a uniformly colored field. Mirrors tui_out_field. */
static void
draw_field(struct draw *d, int row, int col, int width, const char *s,
    struct vt_color fg, struct vt_color bg, uint16_t attrs)
{
	draw_fill(d, row, col, width, ' ', fg, bg, attrs);
	draw_text(d, row, col, s, fg, bg, attrs);
}

/* Prompt for a byte offset (hex, an optional 0x accepted) and move the cursor
 * there. */
static void
hex_goto(struct editor *e)
{
	char buf[32];
	size_t total = hex_total(e->t), off;

	buf[0] = '\0';
	if (!prompt_line(e, "Go to offset: ", buf, sizeof(buf)))
		return;
	off = (size_t)strtoull(buf, NULL, 16);
	if (total == 0)
		off = 0;
	else if (off >= total)
		off = total - 1;
	hex_pos_at(e->t, off, &e->cy, &e->cx);
}

/* The value 0-15 of a hex digit, or -1 if c is not one. */
static int
hex_digit(uint32_t c)
{
	if (c >= '0' && c <= '9')
		return (int)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (int)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (int)(c - 'A' + 10);
	return -1;
}

/* Overwrite the byte under the cursor with v, as one undo step. Refuses when
 * the cursor is on an implied newline or v would introduce one, since changing
 * the line structure is deferred. Returns 1 when a byte was written. */
static int
hex_overwrite(struct editor *e, unsigned char v)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t total = hex_total(e->t);
	size_t cy, cx, len;
	char b = (char)v;

	if (off >= total) {		/* an empty buffer has nothing to edit */
		snprintf(e->status, sizeof(e->status), "no byte to overwrite");
		return 0;
	}
	hex_pos_at(e->t, off, &cy, &cx);
	len = text_line_len(e->t, cy);
	if (cx >= len || v == '\n') {
		snprintf(e->status, sizeof(e->status),
		    "newline bytes are structural (not editable yet)");
		return 0;
	}
	text_undo_group_begin(e->t);
	text_delete(e->t, cy, cx, 1);
	text_insert(e->t, cy, cx, &b, 1);
	text_undo_group_end(e->t);
	e->cy = cy;
	e->cx = cx;
	e->hl_valid = 0;		/* the text view must recolor on return */
	return 1;
}

/* Move the cursor to the next byte after an overwrite, clamped to the end. */
static void
hex_advance(struct editor *e)
{
	size_t total = hex_total(e->t);
	size_t off = hex_offset_of(e->t, e->cy, e->cx) + 1;

	if (total == 0)
		return;
	if (off >= total)
		off = total - 1;
	hex_pos_at(e->t, off, &e->cy, &e->cx);
}

/* Insert the byte v at the cursor, as one undo step, then land on the byte
 * after it. A newline byte splits the line, growing the buffer by a line;
 * any other byte is inserted into the current line. Inserting at an implied
 * newline (cx == line length) appends to the end of the line, which is what
 * the offset there means. */
static void
hex_insert_byte(struct editor *e, unsigned char v)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t cy, cx;
	char b = (char)v;

	hex_pos_at(e->t, off, &cy, &cx);
	text_undo_group_begin(e->t);
	if (v == '\n')
		text_split(e->t, cy, cx);
	else
		text_insert(e->t, cy, cx, &b, 1);
	text_undo_group_end(e->t);
	e->hl_valid = 0;		/* the text view must recolor on return */
	hex_pos_at(e->t, off + 1, &e->cy, &e->cx);
}

/* Delete the byte under the cursor, as one undo step. A data byte is removed
 * from its line; an implied interior newline joins the following line onto
 * this one. The sole trailing newline of the whole buffer is left alone: it
 * is the final_newline flag, which the structural keys do not touch. Returns
 * 1 when a byte was removed. */
static int
hex_delete_at(struct editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	size_t total = hex_total(e->t);
	size_t cy, cx, len, nlines;

	if (off >= total) {
		snprintf(e->status, sizeof(e->status), "no byte to delete");
		return 0;
	}
	hex_pos_at(e->t, off, &cy, &cx);
	len = text_line_len(e->t, cy);
	nlines = text_lines(e->t);
	if (cx < len) {
		text_undo_group_begin(e->t);
		text_delete(e->t, cy, cx, 1);
		text_undo_group_end(e->t);
	} else if (cy + 1 < nlines) {	/* an implied newline: join the lines */
		text_undo_group_begin(e->t);
		text_join(e->t, cy);
		text_undo_group_end(e->t);
	} else {
		snprintf(e->status, sizeof(e->status),
		    "the trailing newline is not a deletable byte");
		return 0;
	}
	e->hl_valid = 0;
	total = hex_total(e->t);
	if (total == 0) {
		e->cy = e->cx = 0;
	} else {
		if (off >= total)
			off = total - 1;
		hex_pos_at(e->t, off, &e->cy, &e->cx);
	}
	return 1;
}

/* Delete the byte before the cursor (Backspace): step back one offset and
 * delete there, which leaves the cursor on the byte that shifted into place. */
static void
hex_delete_prev(struct editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);

	if (off == 0)
		return;
	hex_pos_at(e->t, off - 1, &e->cy, &e->cx);
	hex_delete_at(e);
}

/* Route a typed byte to the insert or overwrite editor, per the current mode,
 * and advance past it. Overwrite refuses structural positions; insert accepts
 * them (a newline byte splits the line). */
static void
hex_put(struct editor *e, unsigned char v)
{
	if (e->hex_insert)
		hex_insert_byte(e, v);
	else if (hex_overwrite(e, v))
		hex_advance(e);
}

/* Repeat the stored search in direction dir, moving the cursor to the match. */
static void
hex_do_search(struct editor *e, int dir)
{
	size_t from = hex_offset_of(e->t, e->cy, e->cx);
	size_t found;

	if (e->hex_pat_len == 0) {
		snprintf(e->status, sizeof(e->status), "no previous search");
		return;
	}
	if (hex_find(e->t, e->hex_pat, e->hex_pat_len, from, dir, &found)) {
		hex_pos_at(e->t, found, &e->cy, &e->cx);
		snprintf(e->status, sizeof(e->status), "found at %08zx", found);
	} else {
		snprintf(e->status, sizeof(e->status), "pattern not found");
	}
}

/* Prompt for a search pattern and jump to the first match. When ascii is set
 * the query's bytes are searched literally; otherwise it is parsed as hex byte
 * pairs (for example "0a 0d"). The pattern is remembered for n and N. */
static void
hex_search_prompt(struct editor *e, int ascii)
{
	char buf[160];

	buf[0] = '\0';
	e->hex_pending = -1;
	if (ascii) {
		size_t len;

		if (!prompt_line(e, "Find text: ", buf, sizeof(buf)))
			return;
		len = strlen(buf);
		if (len == 0)
			return;
		if (len > sizeof(e->hex_pat))
			len = sizeof(e->hex_pat);
		memcpy(e->hex_pat, buf, len);
		e->hex_pat_len = len;
	} else {
		unsigned char pat[sizeof(e->hex_pat)];
		size_t plen;

		if (!prompt_line(e, "Find hex bytes: ", buf, sizeof(buf)))
			return;
		if (hex_parse_bytes(buf, pat, sizeof(pat), &plen) != 0 ||
		    plen == 0) {
			snprintf(e->status, sizeof(e->status),
			    "enter hex byte pairs, e.g. 0a 0d");
			return;
		}
		memcpy(e->hex_pat, pat, plen);
		e->hex_pat_len = plen;
	}
	e->hex_pat_dir = 1;
	hex_do_search(e, 1);
}

/* The inclusive byte range the selection covers, low to high. With no active
 * selection this is just the byte under the cursor. */
static void
hex_sel_range(struct editor *e, size_t *lo, size_t *hi)
{
	size_t cur = hex_offset_of(e->t, e->cy, e->cx);

	if (e->hex_sel && e->hex_anchor < cur) {
		*lo = e->hex_anchor;
		*hi = cur;
	} else if (e->hex_sel) {
		*lo = cur;
		*hi = e->hex_anchor;
	} else {
		*lo = *hi = cur;
	}
}

/* Copy the selected byte range (or the single byte under the cursor) to the
 * clipboard, which also mirrors to the system clipboard, then clear the
 * selection. */
static void
hex_yank(struct editor *e)
{
	size_t total = hex_total(e->t), lo, hi, n;
	unsigned char *buf;

	if (total == 0) {
		snprintf(e->status, sizeof(e->status), "nothing to yank");
		e->hex_sel = 0;
		return;
	}
	hex_sel_range(e, &lo, &hi);
	if (hi >= total)
		hi = total - 1;
	if (lo > hi)
		lo = hi;
	n = hi - lo + 1;
	buf = malloc(n);
	if (buf == NULL) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}
	hex_gather(e->t, lo, buf, n);
	clip_set(e, (char *)buf, n);
	e->clip_linewise = 0;
	e->hex_sel = 0;
	snprintf(e->status, sizeof(e->status), "yanked %zu byte%s", n,
	    n == 1 ? "" : "s");
}

/* Insert the clipboard bytes at the cursor as one undo step. Newlines in the
 * clipboard split lines, matching how the hex view treats a 0a byte. */
static void
hex_paste(struct editor *e)
{
	size_t off = hex_offset_of(e->t, e->cy, e->cx);

	if (e->clip == NULL || e->clip_len == 0) {
		snprintf(e->status, sizeof(e->status), "clipboard is empty");
		return;
	}
	hex_pos_at(e->t, off, &e->cy, &e->cx);	/* land on a real position */
	text_undo_group_begin(e->t);
	insert_bytes(e, e->clip, e->clip_len);	/* advances the cursor past it */
	text_undo_group_end(e->t);
	e->hl_valid = 0;
	e->hex_sel = 0;
	snprintf(e->status, sizeof(e->status), "pasted %zu byte%s", e->clip_len,
	    e->clip_len == 1 ? "" : "s");
}

/* One key in the hex view. Arrows and paging move the cursor byte; Tab switches
 * the hex and ascii sub-columns; Insert toggles overwrite and insert modes;
 * Delete removes the byte under the cursor and Backspace the one before it;
 * typed hex digits (in the hex column) or printable characters (in the ascii
 * column) overwrite or insert the byte at the cursor per the current mode; g
 * prompts for an offset; / and \ search for a text or a hex-byte pattern and n
 * or N repeat it; w cycles the row width and i toggles the data inspector; v
 * starts or clears a byte selection, y yanks it (or the byte under the cursor)
 * and p pastes the clipboard; and q or Esc returns to the text view (Esc first
 * clears an active selection). Ctrl-S and Ctrl-Q request a save and a quit,
 * which the main loop carries out through the same handling as the text view.
 * The command letters act in the hex column, where they are not byte data.
 * Returns the request for the caller to act on, usually REQ_CONTINUE. */
static enum req
hex_key(struct editor *e, const struct tkbd_seq *seq)
{
	size_t total = hex_total(e->t);
	size_t off = hex_offset_of(e->t, e->cy, e->cx);
	uint32_t ch;
	size_t pg, cols = e->hex_cols ? (size_t)e->hex_cols : 16;
	int page = e->rows - 2, moved = 1;

	if (seq->type != TKBD_KEY)
		return REQ_CONTINUE;
	if (page < 1)
		page = 1;
	pg = (size_t)page * cols;

	/* Save and quit go through the editor's shared request handling, so
	 * the hex view saves and quits exactly as the text view does. */
	if ((seq->mod & TKBD_MOD_CTRL) && seq->key == TKBD_KEY_S) {
		e->hex_pending = -1;
		return REQ_SAVE;
	}
	if ((seq->mod & TKBD_MOD_CTRL) && seq->key == TKBD_KEY_Q) {
		e->hex_pending = -1;
		return REQ_QUIT;
	}

	switch (seq->key) {
	case TKBD_KEY_LEFT:	off = off ? off - 1 : 0; break;
	case TKBD_KEY_RIGHT:	off++; break;
	case TKBD_KEY_UP:	if (off >= cols) off -= cols; break;
	case TKBD_KEY_DOWN:	off += cols; break;
	case TKBD_KEY_PGUP:	off = off >= pg ? off - pg : off % cols; break;
	case TKBD_KEY_PGDN:	off += pg; break;
	case TKBD_KEY_HOME:	off = 0; break;
	case TKBD_KEY_END:	off = total ? total - 1 : 0; break;
	case TKBD_KEY_TAB:
		e->hex_ascii = !e->hex_ascii;
		e->hex_pending = -1;
		return REQ_CONTINUE;
	case TKBD_KEY_INS:
		e->hex_pending = -1;
		e->hex_insert = !e->hex_insert;
		return REQ_CONTINUE;
	case TKBD_KEY_DEL:
		e->hex_pending = -1;
		hex_delete_at(e);
		return REQ_CONTINUE;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		e->hex_pending = -1;
		hex_delete_prev(e);
		return REQ_CONTINUE;
	case TKBD_KEY_ESC:
		if (e->hex_sel) {	/* first Esc drops the selection */
			e->hex_sel = 0;
			e->hex_pending = -1;
			return REQ_CONTINUE;
		}
		e->hex_view = 0;
		return REQ_CONTINUE;
	default:
		moved = 0;
		break;
	}
	if (moved) {			/* any move discards a half-typed byte */
		e->hex_pending = -1;
		if (total == 0)
			off = 0;
		else if (off >= total)
			off = total - 1;
		hex_pos_at(e->t, off, &e->cy, &e->cx);
		return REQ_CONTINUE;
	}

	ch = seq->ch;
	if (e->hex_ascii) {		/* the ascii column takes any printable */
		if (ch >= 0x20 && ch < 0x7f)
			hex_put(e, (unsigned char)ch);
		return REQ_CONTINUE;
	}

	/* the hex column: two digits make a byte; the letters are commands here */
	if (hex_digit(ch) >= 0) {
		if (e->hex_pending < 0) {
			e->hex_pending = hex_digit(ch);
		} else {
			unsigned char v = (unsigned char)
			    ((e->hex_pending << 4) | hex_digit(ch));

			e->hex_pending = -1;
			hex_put(e, v);
		}
		return REQ_CONTINUE;
	}
	if (ch == 'q' || ch == 'Q') {
		e->hex_view = 0;
		return REQ_CONTINUE;
	}
	if (ch == 'g' || ch == 'G') {
		e->hex_pending = -1;
		hex_goto(e);
		return REQ_CONTINUE;
	}
	if (ch == '/') {
		hex_search_prompt(e, 1);
		return REQ_CONTINUE;
	}
	if (ch == '\\') {
		hex_search_prompt(e, 0);
		return REQ_CONTINUE;
	}
	if (ch == 'n') {
		e->hex_pending = -1;
		hex_do_search(e, e->hex_pat_dir);
		return REQ_CONTINUE;
	}
	if (ch == 'N') {
		e->hex_pending = -1;
		hex_do_search(e, -e->hex_pat_dir);
		return REQ_CONTINUE;
	}
	if (ch == 'w') {		/* cycle the row width 8 -> 16 -> 32 */
		e->hex_pending = -1;
		e->hex_cols = e->hex_cols == 8 ? 16 :
		    e->hex_cols == 16 ? 32 : 8;
		snprintf(e->status, sizeof(e->status), "%d bytes per row",
		    e->hex_cols);
		return REQ_CONTINUE;
	}
	if (ch == 'i') {		/* toggle the data-inspector footer */
		e->hex_pending = -1;
		e->hex_inspect = !e->hex_inspect;
		snprintf(e->status, sizeof(e->status), "inspector %s",
		    e->hex_inspect ? "on" : "off");
		return REQ_CONTINUE;
	}
	if (ch == 'v') {		/* start or clear a byte selection */
		e->hex_pending = -1;
		if (e->hex_sel) {
			e->hex_sel = 0;
			snprintf(e->status, sizeof(e->status),
			    "selection cleared");
		} else {
			e->hex_sel = 1;
			e->hex_anchor = off;
			snprintf(e->status, sizeof(e->status),
			    "selecting from %08zx", off);
		}
		return REQ_CONTINUE;
	}
	if (ch == 'y') {		/* yank the selection or the cursor byte */
		e->hex_pending = -1;
		hex_yank(e);
		return REQ_CONTINUE;
	}
	if (ch == 'p') {		/* paste the clipboard bytes at the cursor */
		e->hex_pending = -1;
		hex_paste(e);
	}
	return REQ_CONTINUE;
}

/* Draw the buffer as a hex dump: a menu bar, offset/hex/ascii rows following
 * the cursor byte, and a status line with the offset and total size. */
static void
hex_render(struct editor *e, struct draw *d)
{
	const struct chrome_pal *p = chrome(e);
	uint16_t barat = p->reverse_bars ? VT_ATTR_REVERSE : 0;
	int content_h = e->rows - 2 - (e->hex_inspect ? 1 : 0);
	size_t cols = e->hex_cols ? (size_t)e->hex_cols : 16;
	size_t total = hex_total(e->t);
	size_t curoff = hex_offset_of(e->t, e->cy, e->cx);
	size_t currow = curoff / cols;
	size_t sello = 0, selhi = 0;
	int i, curj = (int)(curoff % cols);
	char st[160];

	if (content_h < 1)
		content_h = 1;
	if (currow < e->hex_top)
		e->hex_top = currow;
	else if (currow >= e->hex_top + (size_t)content_h)
		e->hex_top = currow - (size_t)content_h + 1;

	if (e->hex_sel)
		hex_sel_range(e, &sello, &selhi);

	draw_clear(d);
	for (i = 0; i < content_h; i++) {
		size_t rowoff = (e->hex_top + (size_t)i) * cols;
		unsigned char bytes[32];
		char line[192];
		size_t got, j;

		if (rowoff >= total && !(rowoff == 0 && total == 0)) {
			draw_field(d, 1 + i, 0, e->cols, "", p->content_fg,
			    p->content_bg, 0);
			continue;
		}
		got = hex_gather(e->t, rowoff, bytes, cols);
		hex_format_row(line, sizeof(line), rowoff, bytes, got, (int)cols);
		draw_field(d, 1 + i, 0, e->cols, line, p->content_fg,
		    p->content_bg, 0);
		for (j = 0; e->hex_sel && j < got; j++) {	/* selected bytes */
			size_t boff = rowoff + j;
			char hp[3], ac[2];
			int hc;

			if (boff < sello || boff > selhi)
				continue;
			hc = hex_hexcol((int)j);
			hp[0] = line[hc];
			hp[1] = line[hc + 1];
			hp[2] = '\0';
			ac[0] = line[hex_asciicol((int)j, (int)cols)];
			ac[1] = '\0';
			draw_field(d, 1 + i, hc, 2, hp, p->content_fg,
			    p->content_bg, VT_ATTR_REVERSE);
			draw_field(d, 1 + i, hex_asciicol((int)j, (int)cols), 1,
			    ac, p->content_fg, p->content_bg, VT_ATTR_REVERSE);
		}
		if (e->hex_top + (size_t)i == currow && (size_t)curj < got) {
			char hp[3], ac[2];
			int hc = hex_hexcol(curj);

			hp[0] = e->hex_pending >= 0 && !e->hex_ascii ?
			    "0123456789abcdef"[e->hex_pending] : line[hc];
			hp[1] = line[hc + 1];
			hp[2] = '\0';
			ac[0] = line[hex_asciicol(curj, (int)cols)];
			ac[1] = '\0';
			draw_field(d, 1 + i, hc, 2, hp, p->content_fg,
			    p->content_bg, VT_ATTR_REVERSE);
			draw_field(d, 1 + i, hex_asciicol(curj, (int)cols), 1, ac,
			    p->content_fg, p->content_bg, VT_ATTR_REVERSE);
		}
	}

	if (e->hex_inspect) {		/* decode the bytes under the cursor */
		unsigned char ins[4];
		size_t got = hex_gather(e->t, curoff, ins, sizeof(ins));
		char line[160];

		hex_inspect_line(line, sizeof(line), ins, got);
		draw_fill(d, e->rows - 2, 0, e->cols, ' ', p->bar_fg, p->bar_bg,
		    barat);
		draw_text(d, e->rows - 2, 1, line, p->bar_fg, p->bar_bg, barat);
	}

	draw_menubar(e, p, -1);
	if (e->status[0])		/* a transient message (search, errors) */
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  %.60s",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR", e->status);
	else if (e->hex_sel)		/* a live selection extent */
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  SEL %08zx-%08zx (%zu)",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR", sello, selhi,
		    selhi - sello + 1);
	else
		snprintf(st, sizeof(st),
		    " HEX%s  %08zx / %08zx  [%s %s]  %.22s  (Tab, Ins, /, w, q)",
		    text_dirty(e->t) ? "*" : "", curoff, total,
		    e->hex_ascii ? "ascii" : "hex",
		    e->hex_insert ? "INS" : "OVR",
		    e->has_name ? e->path : "[No Name]");
	draw_fill(d, e->rows - 1, 0, e->cols, ' ', p->bar_fg, p->bar_bg, barat);
	draw_text(d, e->rows - 1, 1, st, p->bar_fg, p->bar_bg, barat);

	draw_cursor_shape(d, DRAW_CURSOR_DEFAULT);
	draw_cursor_vis(d, 1);
	if (currow >= e->hex_top && currow < e->hex_top + (size_t)content_h)
		draw_cursor(d, 1 + (int)(currow - e->hex_top),
		    e->hex_ascii ? hex_asciicol(curj, (int)cols) : hex_hexcol(curj));
}

static void
render_body(struct editor *e, struct draw *d)
{
	const struct chrome_pal *p = chrome(e);
	int text_h = text_height(e);
	int text_w = text_width(e);
	int i;
	size_t len = 0;
	const char *cur = text_line(e->t, e->cy, &len);
	int cur_col;

	if (e->hex_view) {
		hex_render(e, d);
		return;
	}

	scroll_to_cursor(e, text_h, text_w);
	cur_col = cur ? disp_cols(cur, e->cx) : 0;

	hl_ensure(e, e->top + (size_t)text_h);

	draw_clear(d);

	for (i = 0; i < text_h; i++) {
		size_t idx = e->top + (size_t)i;
		size_t llen = 0;
		const char *s = text_line(e->t, idx, &llen);
		int hs = -1, he = -1;
		const uint8_t *sty = NULL;
		int row = CHROME_TOP + i;

		if (e->sel_active && s) {
			size_t y1, x1, y2, x2;

			sel_bounds(e, &y1, &x1, &y2, &x2);
			if (idx >= y1 && idx <= y2) {
				size_t a = (idx == y1) ? x1 : 0;
				size_t b = (idx == y2) ? x2 : llen;

				/* vi visual selects inclusively: charwise covers
				 * the cell under the cursor, linewise whole lines.
				 * The modeless selection (vi_visual == 0) is left
				 * exclusive as before. */
				if (e->vi_visual == 'V') {
					a = 0;
					b = llen;
				} else if (e->vi_visual == 'v' && idx == y2 &&
				    b < llen) {
					b += rune_len_at(s, llen, b);
				}
				hs = disp_cols(s, a);
				he = disp_cols(s, b);
			}
		}

		if (s) {
			sty = hl_line(e, idx, s, llen);
			draw_line(d, row, CHROME_LEFT, s, llen, (int)e->left,
			    text_w, hs, he, sty, p->content_fg, p->content_bg);
		} else {
			draw_line(d, row, CHROME_LEFT, "", 0, 0, text_w, -1, -1,
			    NULL, p->content_fg, p->content_bg);
		}
	}

	draw_menubar(e, p, -1);
	draw_frame(e, p);
	draw_statusbar(e, p, cur_col);

	/* cursor shape follows the mode: a block in normal mode, a bar while
	 * inserting; leave the modeless editor's cursor at its default */
	if (e->mode == MODE_NORMAL)
		draw_cursor_shape(d, DRAW_CURSOR_BLOCK);
	else if (e->mode == MODE_INSERT)
		draw_cursor_shape(d, DRAW_CURSOR_BAR);
	else
		draw_cursor_shape(d, DRAW_CURSOR_DEFAULT);

	draw_cursor_vis(d, 1);		/* a menu overlay may have hidden it */
	draw_cursor(d, CHROME_TOP + (int)(e->cy - e->top),
	    CHROME_LEFT + cur_col - (int)e->left);
}

static void
render(struct editor *e, struct draw *d)
{
	render_body(e, d);
	draw_present(d);
}

/* Geometry and palette of a centered modal overlay, handed to its draw and
 * key callbacks each frame. */
struct modal {
	int		x, y, w, h;
	struct vt_color	fg, bg;
	uint16_t	base;
};

/* Run a centered modal box of w by h until its key handler closes it. draw
 * paints the box interior each frame; on_key handles one input event and
 * returns nonzero to close. Both receive the box geometry and palette, plus
 * the caller's ctx. EOF and the editor frame behind the box are handled here.
 */
static void
modal_run(struct editor *e, int w, int h, void *ctx,
    void (*draw)(struct editor *, const struct modal *, void *),
    int (*on_key)(struct editor *, const struct modal *,
        const struct draw_event *, void *))
{
	struct modal m;

	dialog_palette(e, &m.fg, &m.bg, &m.base);
	m.w = w > e->cols ? e->cols : w;
	m.h = h > e->rows ? e->rows : h;
	center_box(e, m.w, m.h, &m.x, &m.y);

	for (;;) {
		struct draw_event ev;

		render_body(e, e->d);
		draw_box(e->d, m.x, m.y, m.w, m.h, m.fg, m.bg, m.base);
		draw(e, &m, ctx);
		draw_cursor_vis(e->d, 0);
		draw_present(e->d);

		switch (draw_wait(e->d, &ev)) {
		case DRAW_EVENT_EOF:
			return;
		case DRAW_EVENT_KEY:
			if (on_key(e, &m, &ev, ctx))
				return;
			break;
		case DRAW_EVENT_RESIZE:
		case DRAW_EVENT_RESUME:
			draw_size(e->d, &e->rows, &e->cols);
			center_box(e, m.w, m.h, &m.x, &m.y);
			break;
		default:
			break;
		}
	}
}

/****************************************************************
 * Editing operations
 ****************************************************************/

void
move_left(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx > 0) {
		e->cx -= prev_rune_len(line, e->cx);
	} else if (e->cy > 0) {
		e->cy--;
		e->cx = text_line_len(e->t, e->cy);
	}
}

void
move_right(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	if (e->cx < len) {
		e->cx += rune_len_at(line, len, e->cx);
	} else if (e->cy + 1 < text_lines(e->t)) {
		e->cy++;
		e->cx = 0;
	}
}

/* Clamp the cursor to a valid line and column. */
void
clamp_col(struct editor *e)
{
	size_t len;

	if (e->cy >= text_lines(e->t))
		e->cy = text_lines(e->t) - 1;
	len = text_line_len(e->t, e->cy);
	if (e->cx > len)
		e->cx = len;
}

void
do_insert(struct editor *e, const char *bytes, size_t n)
{
	hl_touch(e, e->cy);
	if (text_insert(e->t, e->cy, e->cx, bytes, n) == 0)
		e->cx += n;
}

void
do_backspace(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	hl_touch(e, e->cy > 0 ? e->cy - 1 : 0);
	if (e->cx > 0) {
		size_t rl = prev_rune_len(line, e->cx);

		text_delete(e->t, e->cy, e->cx - rl, rl);
		e->cx -= rl;
	} else if (e->cy > 0) {
		size_t plen = text_line_len(e->t, e->cy - 1);

		text_join(e->t, e->cy - 1);
		e->cy--;
		e->cx = plen;
	}
}

void
do_delete(struct editor *e)
{
	size_t len = 0;
	const char *line = text_line(e->t, e->cy, &len);

	hl_touch(e, e->cy);
	if (e->cx < len)
		text_delete(e->t, e->cy, e->cx, rune_len_at(line, len, e->cx));
	else if (e->cy + 1 < text_lines(e->t))
		text_join(e->t, e->cy);
}

void
do_newline(struct editor *e)
{
	hl_touch(e, e->cy);
	if (text_split(e->t, e->cy, e->cx) == 0) {
		e->cy++;
		e->cx = 0;
	}
}


/* Consume a bracketed-paste payload (PASTE_BEGIN was just read) and insert
 * it literally, so control bytes in the paste never fire editor commands.
 * CR, LF, and CRLF all become one newline. Undo boundaries fence the paste
 * off from the surrounding edits. */
static void
paste_input(struct editor *e)
{
	int saw_cr = 0;

	if (e->sel_active) {		/* a paste replaces the selection */
		size_t y1, x1, y2, x2;

		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
	}
	text_undo_boundary(e->t);	/* separate the paste from prior typing */
	for (;;) {
		struct draw_event ev;
		struct tkbd_seq seq;
		unsigned char buf[8];
		int n;

		if (draw_wait(e->d, &ev) == DRAW_EVENT_EOF)
			break;			/* input closed ends the paste */
		if (ev.type != DRAW_EVENT_KEY) {
			draw_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
			continue;
		if (seq.key == TKBD_KEY_PASTE_END)
			break;

		if (seq.key == TKBD_KEY_ENTER) {
			/* collapse a CR immediately followed by LF */
			if (seq.ch == 0x0a && saw_cr) {
				saw_cr = 0;
				continue;
			}
			do_newline(e);
			saw_cr = (seq.ch == 0x0d);
			continue;
		}
		saw_cr = 0;
		if (seq.key == TKBD_KEY_TAB) {
			do_insert(e, "\t", 1);
			continue;
		}
		if (seq.ch != TKBD_CH_NONE && seq.ch >= 0x20 &&
		    seq.ch != 0x7f) {
			n = utf8_encode(buf, seq.ch);
			if (n > 0)
				do_insert(e, (char *)buf, (size_t)n);
		}
		/* other control and function keys are dropped inside a paste */
	}
	text_undo_boundary(e->t);
}

/* Consume and discard a bracketed-paste payload without inserting it. Used in
 * vi normal mode, where a paste is not text input. */
static void
paste_discard(struct editor *e)
{
	for (;;) {
		struct draw_event ev;
		struct tkbd_seq seq;

		if (draw_wait(e->d, &ev) == DRAW_EVENT_EOF)
			break;
		if (ev.type != DRAW_EVENT_KEY) {
			draw_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;
		}
		seq = ev.key;
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_END)
			break;
	}
}

static int
is_movement(enum cmd cmd)
{
	switch (cmd) {
	case CMD_LEFT:
	case CMD_RIGHT:
	case CMD_UP:
	case CMD_DOWN:
	case CMD_HOME:
	case CMD_END:
	case CMD_PGUP:
	case CMD_PGDN:
		return 1;
	default:
		return 0;
	}
}

/* Copy the selection [y1,x1)-(y2,x2) into a fresh buffer, joining lines with
 * '\n'. Returns NULL on allocation failure. Sets *outlen to the byte count. */
char *
region_text(struct editor *e, size_t y1, size_t x1, size_t y2, size_t x2,
    size_t *outlen)
{
	size_t cap = 0, len = 0, y;
	char *buf = NULL;

	for (y = y1; y <= y2; y++) {
		size_t llen = 0;
		const char *s = text_line(e->t, y, &llen);
		size_t a = (y == y1) ? x1 : 0;
		size_t b = (y == y2) ? x2 : llen;
		size_t seg;

		if (b > llen)			/* guard a stale/over-long end */
			b = llen;
		seg = (b > a) ? b - a : 0;
		size_t need = len + seg + 1;	/* room for a joining newline */

		if (need > cap) {
			char *nb = realloc(buf, need + 64);

			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap = need + 64;
		}
		if (seg && s) {
			memcpy(buf + len, s + a, seg);
			len += seg;
		}
		if (y < y2)
			buf[len++] = '\n';
	}
	if (!buf)
		buf = malloc(1);	/* empty selection: a valid 0-byte clip */
	*outlen = len;
	return buf;
}

/* Delete the selection [y1,x1)-(y2,x2) and leave the cursor at its start. */
void
delete_region(struct editor *e, size_t y1, size_t x1, size_t y2, size_t x2)
{
	hl_touch(e, y1);
	if (y1 == y2) {
		text_delete(e->t, y1, x1, x2 - x1);
	} else {
		size_t first_len = 0;
		size_t k;

		text_line(e->t, y1, &first_len);
		text_delete(e->t, y1, x1, first_len - x1);
		text_delete(e->t, y2, 0, x2);
		for (k = y1 + 1; k < y2; k++) {	/* clear fully-covered lines */
			size_t ml = 0;

			text_line(e->t, k, &ml);
			text_delete(e->t, k, 0, ml);
		}
		for (k = y1; k < y2; k++)	/* pull each later line up */
			text_join(e->t, y1);
	}
	e->cy = y1;
	e->cx = x1;
}

/* Insert bytes at the cursor, breaking lines on embedded newlines. */
void
insert_bytes(struct editor *e, const char *bytes, size_t len)
{
	size_t i = 0;

	while (i < len) {
		size_t j = i;

		while (j < len && bytes[j] != '\n')
			j++;
		if (j > i)
			do_insert(e, bytes + i, j - i);
		if (j < len)
			do_newline(e);		/* the newline itself */
		i = (j < len) ? j + 1 : j;
	}
}

/* Insert the clipboard (the unnamed register) at the cursor. */
void
insert_clip(struct editor *e)
{
	insert_bytes(e, e->clip, e->clip_len);
}

/* Cap on the raw byte count mirrored to the system clipboard. OSC 52 rides
 * the terminal's input path, and many terminals cap or drop very long
 * sequences, so keep the payload modest; the internal clipboard is unbounded.
 */
#define OSC52_MAX 100000

void
clip_set(struct editor *e, char *bytes, size_t len)
{
	free(e->clip);
	e->clip = bytes;
	e->clip_len = len;
	/* mirror to the system clipboard; the driver emits OSC 52 */
	if (len > 0 && len <= OSC52_MAX)
		draw_set_clipboard(e->d, bytes, len);
}

/* Allocated text of the active selection, with its byte length in *len. The
 * caller owns the buffer. Returns NULL when nothing is selected or on an
 * allocation failure. */
static char *
current_selection_text(struct editor *e, size_t *len)
{
	size_t y1, x1, y2, x2;

	*len = 0;
	if (!e->sel_active)
		return NULL;
	sel_bounds(e, &y1, &x1, &y2, &x2);
	return region_text(e, y1, x1, y2, x2, len);
}

/* Send the selection, or the current line when nothing is selected, to
 * another window of the session as typed input. A carriage return is
 * appended so the line runs in the target shell or REPL. */
static void
do_send(struct editor *e)
{
	const char *session = getenv("LUMI_SESSION");
	char *text = NULL, *payload;
	size_t tlen = 0;

	if (!session) {
		snprintf(e->status, sizeof(e->status), "not in a session");
		return;
	}
	if (e->sel_active) {
		text = current_selection_text(e, &tlen);
	} else {
		size_t ll = 0;
		const char *s = text_line(e->t, e->cy, &ll);

		text = malloc(ll + 1);
		if (text && ll)
			memcpy(text, s, ll);
		tlen = ll;
	}
	if (!text) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}

	payload = malloc(tlen + 1);
	if (!payload) {
		free(text);
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}
	memcpy(payload, text, tlen);
	payload[tlen] = '\r';		/* submit the line in the target */
	free(text);

	switch (lu_send_input(session, -1, payload, tlen + 1)) {
	case LU_SEND_OK:
		snprintf(e->status, sizeof(e->status),
		    "sent %zu bytes to another pane", tlen);
		break;
	case LU_SEND_NO_TARGET:
		snprintf(e->status, sizeof(e->status),
		    "no other window to send to");
		break;
	case LU_SEND_READONLY:
		snprintf(e->status, sizeof(e->status),
		    "target pane is read-only");
		break;
	case LU_SEND_NO_SESSION:
		snprintf(e->status, sizeof(e->status), "session not found");
		break;
	case LU_SEND_ERROR:
		snprintf(e->status, sizeof(e->status), "send failed");
		break;
	}
	free(payload);
	e->sel_active = 0;		/* consume the selection */
}

/* Carry out one command, returning what the main loop must do next. */
static enum req
dispatch(struct editor *e, enum cmd cmd, const struct tkbd_seq *seq)
{
	int page = text_height(e) - 1;
	unsigned char buf[8];
	int n;
	int grouped = 0;		/* an undo group is open for this command */

	if (page < 1)
		page = 1;

	/* any command other than typing ends the current undo run */
	if (cmd != CMD_INSERT && cmd != CMD_TAB)
		text_undo_boundary(e->t);

	/* Shift + a movement key extends a selection from an anchor; an
	 * unshifted movement clears it. */
	if (is_movement(cmd)) {
		if (seq && (seq->mod & TKBD_MOD_SHIFT)) {
			if (!e->sel_active) {
				e->sel_active = 1;
				e->ay = e->cy;
				e->ax = e->cx;
			}
		} else {
			e->sel_active = 0;
		}
	}

	/* Editing over a selection replaces it: drop the selected text first,
	 * then let insertion proceed. Backspace and Delete are satisfied by
	 * that removal alone. */
	if (e->sel_active && (cmd == CMD_INSERT || cmd == CMD_TAB ||
	    cmd == CMD_NEWLINE || cmd == CMD_BACKSPACE || cmd == CMD_DELETE)) {
		size_t y1, x1, y2, x2;

		/* the deletion and any replacement typing are one undo step */
		text_undo_group_begin(e->t);
		grouped = 1;
		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		if (cmd == CMD_BACKSPACE || cmd == CMD_DELETE) {
			text_undo_group_end(e->t);
			return REQ_CONTINUE;
		}
	}

	switch (cmd) {
	case CMD_QUIT:
		return REQ_QUIT;
	case CMD_SAVE:
		return REQ_SAVE;
	case CMD_UNDO:
		e->sel_active = 0;	/* the buffer shifts under the anchor */
		if (text_undo(e->t, &e->cy, &e->cx) != 0)
			snprintf(e->status, sizeof(e->status),
			    "nothing to undo");
		else {
			hl_touch(e, 0);	/* an undo may touch any lines */
			clamp_col(e);
		}
		break;
	case CMD_REDO:
		e->sel_active = 0;
		if (text_redo(e->t, &e->cy, &e->cx) != 0)
			snprintf(e->status, sizeof(e->status),
			    "nothing to redo");
		else {
			hl_touch(e, 0);
			clamp_col(e);
		}
		break;
	case CMD_COPY: {
		size_t rl = 0;
		char *r;

		if (e->sel_active) {
			r = current_selection_text(e, &rl);
			if (r) {
				clip_set(e, r, rl);
				snprintf(e->status, sizeof(e->status),
				    "copied %zu bytes", rl);
			}
		} else {
			size_t ll = 0;
			const char *s = text_line(e->t, e->cy, &ll);

			r = malloc(ll + 1);	/* the line plus its newline */
			if (r) {
				if (ll && s)
					memcpy(r, s, ll);
				r[ll] = '\n';
				clip_set(e, r, ll + 1);
				snprintf(e->status, sizeof(e->status),
				    "copied line");
			}
		}
		break;
	}
	case CMD_CUT: {
		size_t y1, x1, y2, x2, rl = 0;
		char *r;

		if (!e->sel_active) {
			snprintf(e->status, sizeof(e->status),
			    "select text first (Shift+arrows)");
			break;
		}
		text_undo_group_begin(e->t);
		grouped = 1;
		r = current_selection_text(e, &rl);
		if (r)
			clip_set(e, r, rl);
		sel_bounds(e, &y1, &x1, &y2, &x2);
		delete_region(e, y1, x1, y2, x2);
		e->sel_active = 0;
		snprintf(e->status, sizeof(e->status), "cut %zu bytes", rl);
		break;
	}
	case CMD_PASTE:
		if (!e->clip || e->clip_len == 0) {
			snprintf(e->status, sizeof(e->status),
			    "clipboard is empty");
			break;
		}
		text_undo_group_begin(e->t);
		grouped = 1;
		if (e->sel_active) {		/* paste replaces the selection */
			size_t y1, x1, y2, x2;

			sel_bounds(e, &y1, &x1, &y2, &x2);
			delete_region(e, y1, x1, y2, x2);
			e->sel_active = 0;
		}
		insert_clip(e);
		break;
	case CMD_SEND:
		do_send(e);
		break;
	case CMD_FIND:
		return REQ_FIND;
	case CMD_GOTO:
		return REQ_GOTO;
	case CMD_HELP:
		return REQ_HELP;
	case CMD_INSERT:
		n = utf8_encode(buf, seq->ch);
		if (n > 0)
			do_insert(e, (char *)buf, (size_t)n);
		break;
	case CMD_TAB:
		do_insert(e, "\t", 1);
		break;
	case CMD_NEWLINE:
		do_newline(e);
		break;
	case CMD_BACKSPACE:
		do_backspace(e);
		break;
	case CMD_DELETE:
		do_delete(e);
		break;
	case CMD_LEFT:
		move_left(e);
		break;
	case CMD_RIGHT:
		move_right(e);
		break;
	case CMD_UP:
		if (e->cy > 0) {
			e->cy--;
			clamp_col(e);
		}
		break;
	case CMD_DOWN:
		if (e->cy + 1 < text_lines(e->t)) {
			e->cy++;
			clamp_col(e);
		}
		break;
	case CMD_HOME:
		e->cx = 0;
		break;
	case CMD_END:
		e->cx = text_line_len(e->t, e->cy);
		break;
	case CMD_PGUP:
		e->cy = e->cy > (size_t)page ? e->cy - (size_t)page : 0;
		clamp_col(e);
		break;
	case CMD_PGDN:
		e->cy += (size_t)page;
		if (e->cy >= text_lines(e->t))
			e->cy = text_lines(e->t) - 1;
		clamp_col(e);
		break;
	case CMD_NONE:
		break;
	}
	if (grouped)
		text_undo_group_end(e->t);
	return REQ_CONTINUE;
}

/****************************************************************
 * Status-line prompts
 ****************************************************************/

/* Draw the editor frame, then overlay a prompt on the status row and leave
 * the cursor at the end of the typed text. */
static void
draw_prompt(struct editor *e, const char *q, const char *buf)
{
	const struct tui_theme *t = tui_theme_default();
	char line[512];
	int col;
	int status_row = (e->rows > 0 ? e->rows : 24) - 1;

	render(e, e->d);		/* paint the frame under the prompt */
	col = snprintf(line, sizeof(line), "%s%s", q, buf ? buf : "");
	draw_field(e->d, status_row, 0, e->cols, line, t->sel_fg, t->sel_bg,
	    VT_ATTR_BOLD);
	if (col >= e->cols)
		col = e->cols - 1;
	draw_cursor(e->d, status_row, col);
	draw_present(e->d);
}

/* Read a line of text. buf is edited in place, so a caller may pre-fill it
 * with a default (for example the last search). Returns 1 with buf filled, or
 * 0 if cancelled or left empty. */
int
prompt_line(struct editor *e, const char *q, char *buf, size_t bufsz)
{
	size_t len = strlen(buf);

	for (;;) {
		struct draw_event ev;
		struct tkbd_seq seq;

		draw_prompt(e, q, buf);
		if (draw_wait(e->d, &ev) == DRAW_EVENT_EOF)
			return 0;
		if (ev.type != DRAW_EVENT_KEY) {
			draw_size(e->d, &e->rows, &e->cols);	/* resize/resume */
			continue;			/* loop redraws the prompt */
		}
		seq = ev.key;
		if (seq.type != TKBD_KEY)
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
				len--;		/* drop UTF-8 continuation */
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

/* Save the buffer, prompting for a name if it has none. Returns 0 on a
 * successful save, -1 on failure or when the save was cancelled. */
static int
save_editor(struct editor *e)
{
	if (!e->has_name) {
		char name[PATH_MAX];

		name[0] = '\0';
		if (!prompt_line(e, "Save as: ", name, sizeof(name))) {
			snprintf(e->status, sizeof(e->status), "save cancelled");
			return -1;
		}
		snprintf(e->path, sizeof(e->path), "%s", name);
		e->has_name = 1;
	}

	if (text_save(e->t, e->path) < 0) {
		snprintf(e->status, sizeof(e->status), "save failed: %s",
		    strerror(errno));
		return -1;
	}
	snprintf(e->status, sizeof(e->status), "wrote %.120s", e->path);
	return 0;
}

/* Rightmost occurrence of q in s whose start is in [lo, hi), or NULL. Used by
 * the backward search to take the match nearest the end of a scanned line. */
static const char *
last_match(const char *s, size_t slen, size_t lo, size_t hi, const char *q)
{
	const char *p, *m, *hit = NULL;

	if (lo > slen)
		return NULL;
	p = s + lo;
	while ((m = strstr(p, q)) != NULL) {
		if ((size_t)(m - s) >= hi)
			break;
		hit = m;
		p = m + 1;
	}
	return hit;
}

/* Search for q from the cursor in direction dir (1 forward, -1 backward),
 * wrapping around the buffer, and move the cursor to the match. */
void
do_find_dir(struct editor *e, const char *q, int dir)
{
	size_t nlines = text_lines(e->t);
	size_t i;

	if (!q[0])
		return;

	if (dir >= 0) {
		/* Current line after the cursor, then each following line, then
		 * wrap and finish the start of the current line. */
		for (i = 0; i <= nlines; i++) {
			size_t ln = (e->cy + i) % nlines;
			size_t llen = 0;
			const char *s = text_line(e->t, ln, &llen);
			size_t from = (i == 0) ? e->cx + 1 : 0;
			const char *hit;

			if (!s || from > llen)
				continue;
			hit = strstr(s + from, q);
			if (hit) {
				e->cy = ln;
				e->cx = (size_t)(hit - s);
				e->sel_active = 0;
				snprintf(e->status, sizeof(e->status),
				    "found '%.80s' (line %zu)", q, ln + 1);
				return;
			}
		}
	} else {
		/* Current line before the cursor, then each preceding line, then
		 * wrap and finish the tail of the current line at/after it. */
		for (i = 0; i <= nlines; i++) {
			size_t ln = (e->cy + 2 * nlines - i) % nlines;
			size_t llen = 0;
			const char *s = text_line(e->t, ln, &llen);
			size_t lo = 0, hi;
			const char *hit;

			if (!s)
				continue;
			if (i == 0)
				hi = e->cx;		/* strictly before cursor */
			else if (i == nlines) {
				lo = e->cx;		/* wrap: tail of this line */
				hi = llen + 1;
			} else {
				hi = llen + 1;		/* the whole line */
			}
			hit = last_match(s, llen, lo, hi, q);
			if (hit) {
				e->cy = ln;
				e->cx = (size_t)(hit - s);
				e->sel_active = 0;
				snprintf(e->status, sizeof(e->status),
				    "found '%.80s' (line %zu)", q, ln + 1);
				return;
			}
		}
	}
	snprintf(e->status, sizeof(e->status), "not found: %.80s", q);
}

void
do_find(struct editor *e, const char *q)
{
	do_find_dir(e, q, 1);
}

/* Prompt for a search string (defaulting to the last one, so Enter repeats)
 * and jump to the next match. */
static void
find_prompt(struct editor *e)
{
	char q[256];

	snprintf(q, sizeof(q), "%s", e->last_find);
	if (!prompt_line(e, "Search: ", q, sizeof(q))) {
		snprintf(e->status, sizeof(e->status), "search cancelled");
		return;
	}
	snprintf(e->last_find, sizeof(e->last_find), "%s", q);
	do_find(e, q);
}

/* Prompt for a 1-based line number and move the cursor to that line. A number
 * past the end clamps to the last line. */
static void
goto_prompt(struct editor *e)
{
	char buf[32], *end;
	long ln;

	buf[0] = '\0';
	if (!prompt_line(e, "Go to line: ", buf, sizeof(buf))) {
		snprintf(e->status, sizeof(e->status), "goto cancelled");
		return;
	}
	ln = strtol(buf, &end, 10);
	if (end == buf || ln < 1) {
		snprintf(e->status, sizeof(e->status), "bad line number");
		return;
	}
	if ((size_t)ln > text_lines(e->t))
		ln = (long)text_lines(e->t);
	e->cy = (size_t)ln - 1;
	e->cx = 0;
	e->sel_active = 0;
	clamp_col(e);
	snprintf(e->status, sizeof(e->status), "line %ld", ln);
}

/* The key bindings, as shown by the help screen. Kept next to the keymap so
 * the two stay in step. */
static const struct {
	const char	*keys;
	const char	*desc;
} help_entries[] = {
	{ "arrows",		"Move the cursor" },
	{ "Home / End",		"Start / end of line" },
	{ "PgUp / PgDn",	"Scroll by a screen" },
	{ "Shift+arrows",	"Extend a selection" },
	{ "Enter",		"Split the line" },
	{ "Backspace / Del",	"Delete before / after the cursor" },
	{ "Ctrl-F",		"Find (Enter repeats the last search)" },
	{ "Ctrl-L",		"Go to a line number" },
	{ "Ctrl-C / Ctrl-X",	"Copy (line if none selected) / cut" },
	{ "Ctrl-V",		"Paste the clipboard" },
	{ "Ctrl-G",		"Send selection/line to another pane" },
	{ "Ctrl-Z / Ctrl-Y",	"Undo / redo" },
	{ "Ctrl-S",		"Save (asks for a name if none)" },
	{ "Ctrl-Q",		"Quit (asks if there are unsaved changes)" },
	{ "F5 / F6 / F7",	"Run / compile / make (see Build menu)" },
	{ "F4 / Shift+F4",	"Next / previous build error in this file" },
	{ "F1",			"Show this help" },
	{ "F2",			"Toggle vi keys (modal editing)" },
};

#define HELP_COUNT ((int)(sizeof(help_entries) / sizeof(help_entries[0])))

/* The vi-personality bindings, shown by the help screen while modal editing
 * is on. Kept next to vi_normal_key and vi_colon so the three stay in step. */
static const struct {
	const char	*keys;
	const char	*desc;
} help_entries_vi[] = {
	{ "h j k l / arrows",	"Move the cursor" },
	{ "0 ^ $",		"Line start / first word / line end" },
	{ "w b e / W B E",	"Word forward / back / end" },
	{ "gg / G",		"First / last line" },
	{ "{ } ( )",		"Paragraph / sentence motion" },
	{ "% H M L |",		"Match pair, screen high/mid/low, column" },
	{ "f F t T ; ,",	"Find a char in the line, then repeat" },
	{ "Ctrl-F/B Ctrl-D/U",	"Scroll a page / half a page" },
	{ "i a o I A O",	"Enter insert mode (Esc returns to normal)" },
	{ "x  dd cc yy",	"Delete char, cut / change / yank a line" },
	{ "d c y + motion",	"Operate over a motion" },
	{ "p / P",		"Paste after / before the cursor" },
	{ "u / Ctrl-R",		"Undo / redo" },
	{ "/ text  n",		"Search forward, repeat the last search" },
	{ ":w  :q  :wq / :x",	"Write, quit, write and quit" },
	{ ":q!  ZZ  ZQ",	"Quit discarding, save and quit, quit" },
	{ ":N  :cq",		"Go to a line, quit with an error code" },
	{ "F5 / F6 / F7",	"Run / compile / make (see Build menu)" },
	{ "F4 / Shift+F4",	"Next / previous build error in this file" },
	{ "F1 / F2",		"Show this help / back to modeless keys" },
};

#define HELP_VI_COUNT \
	((int)(sizeof(help_entries_vi) / sizeof(help_entries_vi[0])))

/* Paint a full-screen scrollable view: a bold header row, body lines drawn
 * from `top` (get_line returns the text for an absolute index, or NULL past
 * the end), and a bold footer row. Used by the help and build-output screens.
 */
static void
draw_scroll_view(struct editor *e, const char *header, const char *footer,
    int top, const char *(*get_line)(struct editor *, void *, int), void *ctx)
{
	const struct tui_theme *t = tui_theme_default();
	struct draw *d = e->d;
	int rows = e->rows > 0 ? e->rows : 24;
	int row;

	draw_clear(d);
	draw_field(d, 0, 0, e->cols, header, t->title_fg, t->border_bg,
	    VT_ATTR_BOLD);
	for (row = 1; row < rows - 1; row++) {
		const char *s = get_line(e, ctx, top + row - 1);

		draw_field(d, row, 0, e->cols, s ? s : "", t->content_fg,
		    t->content_bg, 0);
	}
	draw_field(d, rows - 1, 0, e->cols, footer, t->title_fg, t->border_bg,
	    VT_ATTR_BOLD);
	draw_present(d);
}

/* Line provider for the help view: one formatted "keys  description" row from
 * the active personality's table. */
static const char *
help_line(struct editor *e, void *ctx, int idx)
{
	static char line[128];
	int vi = e->mode != MODE_MODELESS;
	int n = vi ? HELP_VI_COUNT : HELP_COUNT;

	(void)ctx;
	if (idx < 0 || idx >= n)
		return NULL;
	snprintf(line, sizeof(line), "  %-20s %s",
	    vi ? help_entries_vi[idx].keys : help_entries[idx].keys,
	    vi ? help_entries_vi[idx].desc : help_entries[idx].desc);
	return line;
}

/* Show the help screen and wait for one key press to dismiss it. */
static void
show_help(struct editor *e)
{
	const char *hdr = e->mode != MODE_MODELESS ?
	    " lumi edit -- vi key bindings" : " lumi edit -- key bindings";

	for (;;) {
		struct draw_event ev;

		draw_scroll_view(e, hdr, " Press any key to return", 0,
		    help_line, NULL);
		switch (draw_wait(e->d, &ev)) {
		case DRAW_EVENT_KEY:
			if (ev.key.type == TKBD_KEY)
				return;		/* any key returns to editing */
			break;
		case DRAW_EVENT_RESIZE:
		case DRAW_EVENT_RESUME:
			draw_size(e->d, &e->rows, &e->cols);
			break;
		case DRAW_EVENT_EOF:
			return;
		default:
			break;
		}
	}
}

/****************************************************************
 * Menu bar actions and the pull-down modal loop
 ****************************************************************/

enum dlg_result { DLG_CANCEL = -1, DLG_NO = 0, DLG_YES = 1 };

/* Result chosen by button index 0/1/2. */
static enum dlg_result
dlg_btn_result(int i)
{
	return i == 0 ? DLG_YES : i == 1 ? DLG_NO : DLG_CANCEL;
}

static const char *const confirm_btn[3] = {
	"[ &Yes ]", "[ &No ]", "[ &Cancel ]"
};

/* State of the save-confirm dialog across its modal frames. */
struct confirm_ctx {
	const char	*msg;
	int		focus;		/* 0 Yes, 1 No, 2 Cancel */
	enum dlg_result	result;		/* what to return; Cancel by default */
	int		bx[3];		/* button columns, set by the draw pass */
	int		brow;		/* button row, set by the draw pass */
};

static void
confirm_draw(struct editor *e, const struct modal *m, void *ctx)
{
	struct confirm_ctx *c = ctx;
	struct draw *d = e->d;
	int msglen = (int)strlen(c->msg);
	int brow_w = 4;			/* two 2-space gaps between 3 buttons */
	int i, cx;

	for (i = 0; i < 3; i++)
		brow_w += menu_disp_w(confirm_btn[i]);
	c->brow = m->y + m->h - 2;
	cx = m->x + (m->w - brow_w) / 2;
	draw_text(d, m->y + 1, m->x + (m->w - msglen) / 2, c->msg,
	    m->fg, m->bg, m->base);
	for (i = 0; i < 3; i++) {
		uint16_t at = (i == c->focus) ?
		    m->base ^ VT_ATTR_REVERSE : m->base;

		c->bx[i] = cx;
		draw_menu_label(d, c->brow, cx, confirm_btn[i], m->fg, m->bg,
		    at);
		cx += menu_disp_w(confirm_btn[i]) + 2;
	}
}

static int
confirm_key(struct editor *e, const struct modal *m,
    const struct draw_event *ev, void *ctx)
{
	struct confirm_ctx *c = ctx;
	int i;

	(void)e;
	(void)m;
	if (ev->key.type == TKBD_MOUSE) {
		if (ev->key.key == TKBD_MOUSE_LEFT &&
		    !(ev->key.mod & TKBD_MOD_MOTION) && ev->key.y == c->brow)
			for (i = 0; i < 3; i++)
				if (ev->key.x >= c->bx[i] && ev->key.x <
				    c->bx[i] + menu_disp_w(confirm_btn[i])) {
					c->result = dlg_btn_result(i);
					return 1;
				}
		return 0;		/* ignore other mouse events */
	}
	if (ev->key.type != TKBD_KEY)
		return 0;
	if (ev->key.ch != TKBD_CH_NONE && ev->key.ch < 128) {
		int lc = tolower((int)ev->key.ch);

		for (i = 0; i < 3; i++)
			if (menu_mnemonic(confirm_btn[i]) == lc) {
				c->result = dlg_btn_result(i);
				return 1;
			}
	}
	switch (ev->key.key) {
	case TKBD_KEY_ESC:
		c->result = DLG_CANCEL;
		return 1;
	case TKBD_KEY_LEFT:
		c->focus = (c->focus + 2) % 3;
		return 0;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_TAB:
		c->focus = (c->focus + 1) % 3;
		return 0;
	case TKBD_KEY_ENTER:
		c->result = dlg_btn_result(c->focus);
		return 1;
	default:
		return 0;
	}
}

/* A centered modal asking whether to save, with Yes/No/Cancel buttons. The
 * arrow keys or Tab move focus, Enter picks the focused button, an underlined
 * mnemonic letter (Y/N/C) chooses directly, Esc cancels, and a click selects a
 * button. */
static enum dlg_result
confirm_save_dialog(struct editor *e, const char *msg)
{
	struct confirm_ctx c = { msg, 0, DLG_CANCEL, { 0, 0, 0 }, 0 };
	int msglen = (int)strlen(msg);
	int brow_w = 4;			/* two 2-space gaps between 3 buttons */
	int i, boxw;

	for (i = 0; i < 3; i++)
		brow_w += menu_disp_w(confirm_btn[i]);
	boxw = (msglen > brow_w ? msglen : brow_w) + 4 + 2;
	modal_run(e, boxw, 5, &c, confirm_draw, confirm_key);
	return c.result;
}

/* Offer to save a dirty buffer before it is replaced or the editor exits.
 * Returns 1 to proceed (saved or discarded), 0 to abort the operation. */
static int
confirm_save(struct editor *e, const char *msg)
{
	if (!text_dirty(e->t))
		return 1;
	switch (confirm_save_dialog(e, msg)) {
	case DLG_CANCEL:
		return 0;
	case DLG_YES:
		return save_editor(e) == 0;
	case DLG_NO:
	default:
		return 1;		/* discard */
	}
}

/* Confirm before replacing the current buffer (New/Open). */
static int
confirm_discard(struct editor *e)
{
	return confirm_save(e, "Save changes to the current file?");
}

/* Reset syntax and cursor state after the buffer is swapped. */
static void
buffer_reset(struct editor *e)
{
	e->cy = e->cx = e->top = e->left = 0;
	e->sel_active = 0;
	e->hl_valid = 0;
}

/* ---- multi-buffer management ------------------------------------------- *
 * The active buffer's per-file state lives in the flat struct editor, which
 * is authoritative for it; the parked buffers live in e->bufs. buf_save mirrors
 * the flat state into a slot, buf_load mirrors a slot back. Only the active
 * buffer is ever read from the flat fields, so a parked slot may lag until the
 * next save. */

/* Copy the active buffer's per-file fields into a slot. */
static void
buf_save(struct editor *e, struct ebuf *b)
{
	b->t = e->t;
	memcpy(b->path, e->path, sizeof(b->path));
	b->has_name = e->has_name;
	b->cy = e->cy;
	b->cx = e->cx;
	b->top = e->top;
	b->left = e->left;
	b->sel_active = e->sel_active;
	b->ay = e->ay;
	b->ax = e->ax;
	b->syn = e->syn;
	b->line_state = e->line_state;
	b->line_state_cap = e->line_state_cap;
	b->hl_valid = e->hl_valid;
	b->hex_view = e->hex_view;
	b->hex_top = e->hex_top;
	memcpy(b->vi_mark_y, e->vi_mark_y, sizeof(b->vi_mark_y));
	memcpy(b->vi_mark_x, e->vi_mark_x, sizeof(b->vi_mark_x));
	b->vi_marks_set = e->vi_marks_set;
}

/* Mirror a slot into the flat editor and drop any in-flight vi command. */
static void
buf_load(struct editor *e, const struct ebuf *b)
{
	e->t = b->t;
	memcpy(e->path, b->path, sizeof(e->path));
	e->has_name = b->has_name;
	e->cy = b->cy;
	e->cx = b->cx;
	e->top = b->top;
	e->left = b->left;
	e->sel_active = b->sel_active;
	e->ay = b->ay;
	e->ax = b->ax;
	e->syn = b->syn;
	e->line_state = b->line_state;
	e->line_state_cap = b->line_state_cap;
	e->hl_valid = b->hl_valid;
	e->hex_view = b->hex_view;
	e->hex_top = b->hex_top;
	memcpy(e->vi_mark_y, b->vi_mark_y, sizeof(e->vi_mark_y));
	memcpy(e->vi_mark_x, b->vi_mark_x, sizeof(e->vi_mark_x));
	e->vi_marks_set = b->vi_marks_set;
	e->vi_visual = 0;
	e->vi_want_col = e->vi_vert_run = e->vi_vert_prev = 0;
	e->hex_ascii = 0;
	e->hex_pending = -1;
	e->hex_insert = 0;
	e->hex_sel = 0;
	vi_reset_pending(e);
}

/* Free the file resources a slot owns (its text and syntax scratch). Used
 * both for a parked slot and, with the flat editor's own pointers, for the
 * active buffer. The diagnostics list is global, not per-buffer, and is
 * freed once at teardown. */
static void
buf_free_fields(struct text *t, uint16_t *line_state)
{
	text_free(t);
	free(line_state);
}

/* Ensure room for one more buffer and return its index (nbuf grows). */
static int
buf_slot(struct editor *e)
{
	if (e->nbuf >= e->bufs_cap) {
		int nc = e->bufs_cap ? e->bufs_cap * 2 : 4;
		struct ebuf *nb = realloc(e->bufs, (size_t)nc * sizeof(*nb));

		if (!nb)
			return -1;
		e->bufs = nb;
		e->bufs_cap = nc;
	}
	return e->nbuf++;
}

void
buf_switch(struct editor *e, int i)
{
	if (i < 0 || i >= e->nbuf || i == e->cur)
		return;
	buf_save(e, &e->bufs[e->cur]);
	e->cur = i;
	buf_load(e, &e->bufs[i]);
}

int
buf_cycle(struct editor *e, int dir)
{
	int i;

	if (e->nbuf <= 1)
		return e->cur;
	i = ((e->cur + dir) % e->nbuf + e->nbuf) % e->nbuf;
	buf_switch(e, i);
	return e->cur;
}

int
buf_open(struct editor *e, const char *path)
{
	struct text *nt;
	int i;

	if (path && path[0]) {			/* already open? just switch */
		for (i = 0; i < e->nbuf; i++) {
			const char *bp = (i == e->cur) ? e->path :
			    e->bufs[i].path;
			int named = (i == e->cur) ? e->has_name :
			    e->bufs[i].has_name;

			if (named && strcmp(bp, path) == 0) {
				buf_switch(e, i);
				return i;
			}
		}
	}
	nt = text_new();
	if (!nt) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return -1;
	}
	if (path && path[0] && text_load(nt, path) < 0 && errno != ENOENT) {
		snprintf(e->status, sizeof(e->status), "open failed: %s",
		    strerror(errno));
		text_free(nt);
		return -1;
	}
	i = buf_slot(e);
	if (i < 0) {
		text_free(nt);
		snprintf(e->status, sizeof(e->status), "out of memory");
		return -1;
	}
	buf_save(e, &e->bufs[e->cur]);		/* park the current buffer */
	e->cur = i;
	e->t = nt;				/* set up the flat new buffer */
	if (path && path[0]) {
		snprintf(e->path, sizeof(e->path), "%s", path);
		e->has_name = 1;
		e->syn = syn_for_ext(file_ext(e->path));
	} else {
		e->path[0] = '\0';
		e->has_name = 0;
		e->syn = NULL;
	}
	e->cy = e->cx = e->top = e->left = 0;
	e->sel_active = 0;
	e->line_state = NULL;
	e->line_state_cap = 0;
	e->hl_valid = 0;
	memset(e->vi_mark_y, 0, sizeof(e->vi_mark_y));
	memset(e->vi_mark_x, 0, sizeof(e->vi_mark_x));
	e->vi_marks_set = 0;
	e->vi_visual = 0;
	vi_reset_pending(e);
	buf_save(e, &e->bufs[i]);		/* keep the slot consistent */
	snprintf(e->status, sizeof(e->status), "%.120s [%d/%d]",
	    e->has_name ? e->path : "new buffer", e->cur + 1, e->nbuf);
	return i;
}

int
buf_close(struct editor *e, int i)
{
	if (e->nbuf <= 1 || i < 0 || i >= e->nbuf)
		return -1;

	if (i == e->cur) {
		int target;

		buf_free_fields(e->t, e->line_state);
		memmove(&e->bufs[i], &e->bufs[i + 1],
		    (size_t)(e->nbuf - i - 1) * sizeof(*e->bufs));
		e->nbuf--;
		target = i < e->nbuf ? i : e->nbuf - 1;
		e->cur = target;
		buf_load(e, &e->bufs[target]);
	} else {
		buf_free_fields(e->bufs[i].t, e->bufs[i].line_state);
		memmove(&e->bufs[i], &e->bufs[i + 1],
		    (size_t)(e->nbuf - i - 1) * sizeof(*e->bufs));
		e->nbuf--;
		if (e->cur > i)
			e->cur--;
	}
	return 0;
}

void
buf_list(struct editor *e)
{
	char *p = e->status;
	size_t rem = sizeof(e->status);
	int i;

	for (i = 0; i < e->nbuf && rem > 1; i++) {
		int active = (i == e->cur);
		const char *name = active ?
		    (e->has_name ? e->path : "[No Name]") :
		    (e->bufs[i].has_name ? e->bufs[i].path : "[No Name]");
		const char *slash = strrchr(name, '/');
		int n;

		if (slash)
			name = slash + 1;
		n = snprintf(p, rem, "%s%d:%s%s", i ? "  " : "", i + 1,
		    active ? "*" : "", name);
		if (n < 0 || (size_t)n >= rem)
			break;
		p += n;
		rem -= (size_t)n;
	}
}

static void
do_new(struct editor *e)
{
	struct text *nt;

	if (!confirm_discard(e))
		return;
	nt = text_new();
	if (!nt) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}
	text_free(e->t);
	e->t = nt;
	e->has_name = 0;
	e->path[0] = '\0';
	e->syn = NULL;
	buffer_reset(e);
	buf_save(e, &e->bufs[e->cur]);
	snprintf(e->status, sizeof(e->status), "new buffer");
}

static void
do_open(struct editor *e)
{
	char path[PATH_MAX];
	struct text *nt;

	if (!confirm_discard(e))
		return;
	path[0] = '\0';
	if (!prompt_line(e, "Open file: ", path, sizeof(path)))
		return;
	nt = text_new();
	if (!nt) {
		snprintf(e->status, sizeof(e->status), "out of memory");
		return;
	}
	if (text_load(nt, path) < 0 && errno != ENOENT) {
		snprintf(e->status, sizeof(e->status), "open failed: %s",
		    strerror(errno));
		text_free(nt);
		return;
	}
	text_free(e->t);
	e->t = nt;
	snprintf(e->path, sizeof(e->path), "%s", path);
	e->has_name = 1;
	e->syn = syn_for_ext(file_ext(e->path));
	buffer_reset(e);
	buf_save(e, &e->bufs[e->cur]);
	snprintf(e->status, sizeof(e->status), "opened %.100s", path);
}

static void
do_save_as(struct editor *e)
{
	char path[PATH_MAX];

	path[0] = '\0';
	if (e->has_name)
		snprintf(path, sizeof(path), "%s", e->path);
	if (!prompt_line(e, "Save as: ", path, sizeof(path)))
		return;
	snprintf(e->path, sizeof(e->path), "%s", path);
	e->has_name = 1;
	e->syn = syn_for_ext(file_ext(e->path));
	e->hl_valid = 0;
	(void)save_editor(e);
}

/* Switch between the modeless and vi personalities (also on F2). */
static void
toggle_vi(struct editor *e)
{
	e->vi_visual = 0;
	if (e->mode == MODE_MODELESS) {
		e->mode = MODE_NORMAL;
		e->sel_active = 0;
		vi_reset_pending(e);
		vi_clamp(e);
		snprintf(e->status, sizeof(e->status),
		    "-- NORMAL -- (F2 returns to modeless)");
	} else {
		e->mode = MODE_MODELESS;
		e->sel_active = 0;
		vi_reset_pending(e);
		snprintf(e->status, sizeof(e->status),
		    "modeless mode (F2 for vi keys)");
	}
}

/* The About box content, one centered line per row plus a button. */
struct about_ctx {
	const char *const	*lines;
	int			nlines;
	const char		*btn;
};

static void
about_draw(struct editor *e, const struct modal *m, void *ctx)
{
	struct about_ctx *a = ctx;
	struct draw *d = e->d;
	int i, brow;

	for (i = 0; i < a->nlines; i++) {
		int lw = (int)strlen(a->lines[i]);

		draw_text(d, m->y + 1 + i, m->x + (m->w - lw) / 2, a->lines[i],
		    m->fg, m->bg, m->base);
	}
	brow = m->y + m->h - 2;
	draw_text(d, brow, m->x + (m->w - (int)strlen(a->btn)) / 2, a->btn,
	    m->fg, m->bg, m->base ^ VT_ATTR_REVERSE);
}

static int
about_key(struct editor *e, const struct modal *m,
    const struct draw_event *ev, void *ctx)
{
	(void)e;
	(void)m;
	(void)ctx;
	if (ev->key.type == TKBD_KEY)
		return 1;		/* any key dismisses */
	/* a fresh left click dismisses; ignore the release that trailed the
	 * click which opened this dialog */
	if (ev->key.type == TKBD_MOUSE && ev->key.key == TKBD_MOUSE_LEFT &&
	    !(ev->key.mod & TKBD_MOD_MOTION))
		return 1;
	return 0;
}

/* A centered modal About box, dismissed by any key or a click. */
static void
show_about(struct editor *e)
{
	static const char *const lines[] = {
		"lumi edit",
		"a lumimux full-screen editor",
		"version " LUMI_VERSION,
	};
	struct about_ctx a = {
		lines, (int)(sizeof(lines) / sizeof(lines[0])), "[ OK ]"
	};
	int inner, boxw, i;

	inner = (int)strlen(a.btn);
	for (i = 0; i < a.nlines; i++) {
		int lw = (int)strlen(lines[i]);

		if (lw > inner)
			inner = lw;
	}
	inner += 4;			/* two spaces of padding each side */
	boxw = inner + 2;		/* left and right border */
	modal_run(e, boxw, a.nlines + 4, &a, about_draw, about_key);
}

/****************************************************************
 * Build commands -- compile / make / run, SciTE-style
 *
 * A build verb (compile, make, run) maps to a shell command chosen by the
 * file's extension. Commands live in lumi.conf as [build "<ext>"] sections
 * with a [build] default, and a small built-in table covers common types
 * when the config is silent. Inside a session the command is sent to the
 * adjacent pane (the same target as Ctrl-G) so its output is visible there;
 * outside one the editor suspends, runs it inline, and waits for a key.
 ****************************************************************/

/* Load lumi.conf (honoring XDG_CONFIG_HOME) so [build] sections are available.
 * A missing file is not an error; e->cfg stays NULL and built-in defaults are
 * used. */
static void
load_build_config(struct editor *e)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char path[PATH_MAX];

	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/lumi/lumi.conf", xdg);
	else if (home && *home)
		snprintf(path, sizeof(path), "%s/.config/lumi/lumi.conf", home);
	else
		return;

	e->cfg = cfg_new();
	if (e->cfg && cfg_load(e->cfg, path) != 0) {
		cfg_free(e->cfg);	/* no readable file: fall back to defaults */
		e->cfg = NULL;
	}
}

/* Apply any [edit.syntax] color overrides from lumi.conf onto the highlight
 * palette. Unset keys keep the built-in color; a bad value is ignored. */
static void
load_syntax_colors(struct editor *e)
{
	char key[64];
	int i;

	if (!e->cfg)
		return;
	for (i = 0; i < SYN_STYLE_NAMES_COUNT; i++) {
		const char *v;

		snprintf(key, sizeof(key), "edit.syntax.%s",
		    syn_style_names[i].name);
		v = cfg_get(e->cfg, key);
		if (v)
			(void)syn_apply(syn_color, syn_style_names[i].name, v);
	}
}

/* Built-in fallback commands, used when lumi.conf names none. An empty ext is
 * the default for any file type. Extensions are matched in lower case. */
static const struct {
	const char	*ext;
	const char	*verb;
	const char	*cmd;
} build_defaults[] = {
	{ "",		"make",		"make" },
	{ "c",		"compile",	"cc -Wall -c $(filename)" },
	{ "c",		"run",		"./$(filebase)" },
	{ "h",		"compile",	"cc -Wall -c $(filename)" },
	{ "cc",		"compile",	"c++ -Wall -c $(filename)" },
	{ "cpp",	"compile",	"c++ -Wall -c $(filename)" },
	{ "sh",		"run",		"sh $(filename)" },
	{ "bas",	"run",		"lumi basic $(file)" },
};
#define BUILD_DEFAULTS_COUNT \
	((int)(sizeof(build_defaults) / sizeof(build_defaults[0])))

/* Lower-case the current file's extension into buf (no leading dot). */
static void
build_ext(struct editor *e, char *buf, size_t sz)
{
	const char *ext = file_ext(e->path);
	size_t i;

	for (i = 0; ext[i] && i < sz - 1; i++)
		buf[i] = (char)tolower((unsigned char)ext[i]);
	buf[i] = '\0';
}

/* The command template for a verb on the current file, or NULL when none is
 * configured. lumi.conf wins over the built-in table, and a [build "<ext>"]
 * key wins over the [build] default within each. */
static const char *
build_cmd(struct editor *e, const char *ext, const char *verb)
{
	char key[128];
	const char *v;
	int i;

	if (e->cfg) {
		snprintf(key, sizeof(key), "build.%s.%s", ext, verb);
		if ((v = cfg_get(e->cfg, key)))
			return v;
		snprintf(key, sizeof(key), "build.%s", verb);
		if ((v = cfg_get(e->cfg, key)))
			return v;
	}
	for (i = 0; i < BUILD_DEFAULTS_COUNT; i++)
		if (strcmp(build_defaults[i].ext, ext) == 0 &&
		    strcmp(build_defaults[i].verb, verb) == 0)
			return build_defaults[i].cmd;
	for (i = 0; i < BUILD_DEFAULTS_COUNT; i++)
		if (build_defaults[i].ext[0] == '\0' &&
		    strcmp(build_defaults[i].verb, verb) == 0)
			return build_defaults[i].cmd;
	return NULL;
}

/* Whether to save the buffer before building. build.<ext>.save or build.save
 * may be false/no/0 to skip it; the default is to save. */
static int
build_save_wanted(struct editor *e, const char *ext)
{
	char key[128];
	const char *v = NULL;

	if (e->cfg) {
		snprintf(key, sizeof(key), "build.%s.save", ext);
		v = cfg_get(e->cfg, key);
		if (!v)
			v = cfg_get(e->cfg, "build.save");
	}
	if (!v)
		return 1;
	return !(strcmp(v, "no") == 0 || strcmp(v, "false") == 0 ||
	    strcmp(v, "0") == 0);
}

/* Append s to out[*len], truncating at outsz. */
static void
build_append(char *out, size_t *len, size_t outsz, const char *s)
{
	while (*s && *len < outsz - 1)
		out[(*len)++] = *s++;
	out[*len] = '\0';
}

/* Expand $(file), $(filedir), $(filebase), $(filename), and $(fileext) in
 * tmpl into out. Unknown $(...) tokens are copied through unchanged. The file
 * path is made absolute first so $(file) and $(filedir) still resolve after a
 * cd into the file's directory in another pane. */
static void
build_expand(struct editor *e, const char *tmpl, char *out, size_t outsz)
{
	char abspath[PATH_MAX];
	const char *slash, *name, *dot;
	char dir[PATH_MAX], base[PATH_MAX];
	size_t len = 0;

	if (e->path[0] == '/') {
		snprintf(abspath, sizeof(abspath), "%s", e->path);
	} else {
		char cwd[PATH_MAX];
		int n = -1;

		if (getcwd(cwd, sizeof(cwd)))
			n = snprintf(abspath, sizeof(abspath), "%s/%s", cwd,
			    e->path);
		if (n < 0 || n >= (int)sizeof(abspath))
			snprintf(abspath, sizeof(abspath), "%s", e->path);
	}

	slash = strrchr(abspath, '/');
	name = slash ? slash + 1 : abspath;
	dot = strrchr(name, '.');

	if (slash == abspath)
		snprintf(dir, sizeof(dir), "/");
	else if (slash)
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - abspath),
		    abspath);
	else
		snprintf(dir, sizeof(dir), ".");
	if (dot && dot != name)
		snprintf(base, sizeof(base), "%.*s", (int)(dot - name), name);
	else
		snprintf(base, sizeof(base), "%s", name);

	out[0] = '\0';
	while (*tmpl && len < outsz - 1) {
		if (tmpl[0] == '$' && tmpl[1] == '(') {
			const char *end = strchr(tmpl + 2, ')');

			if (end) {
				size_t n = (size_t)(end - (tmpl + 2));

				if (n == 4 && !strncmp(tmpl + 2, "file", 4))
					build_append(out, &len, outsz, abspath);
				else if (n == 7 && !strncmp(tmpl + 2, "filedir", 7))
					build_append(out, &len, outsz, dir);
				else if (n == 8 && !strncmp(tmpl + 2, "filebase", 8))
					build_append(out, &len, outsz, base);
				else if (n == 8 && !strncmp(tmpl + 2, "filename", 8))
					build_append(out, &len, outsz, name);
				else if (n == 7 && !strncmp(tmpl + 2, "fileext", 7))
					build_append(out, &len, outsz,
					    file_ext(abspath));
				else
					build_append(out, &len, outsz, "");
				if (n == 4 || n == 7 || n == 8) {
					tmpl = end + 1;
					continue;
				}
				/* unknown token: fall through to copy '$' */
			}
		}
		out[len++] = *tmpl++;
		out[len] = '\0';
	}
}

/* Wrap a raw path in single quotes for /bin/sh, escaping embedded quotes. */
static void
build_shquote(const char *s, char *out, size_t outsz)
{
	size_t len = 0;

	build_append(out, &len, outsz, "'");
	for (; *s && len < outsz - 5; s++) {
		if (*s == '\'')
			build_append(out, &len, outsz, "'\\''");
		else {
			out[len++] = *s;
			out[len] = '\0';
		}
	}
	build_append(out, &len, outsz, "'");
}

/* Discard the diagnostics from a previous build. */
static void
build_clear_errors(struct editor *e)
{
	e->n_errs = 0;
	e->err_cur = -1;
}

/* Record one parsed diagnostic. Silently drops any past a fixed ceiling so a
 * runaway build cannot exhaust memory. */
static void
build_add_err(struct editor *e, const char *path, long line, long col,
    const char *msg)
{
	struct build_err *er;

	if (e->n_errs >= 2000)
		return;
	if (e->n_errs >= e->errs_cap) {
		int ncap = e->errs_cap ? e->errs_cap * 2 : 32;
		struct build_err *n = realloc(e->errs,
		    (size_t)ncap * sizeof(*n));

		if (!n)
			return;
		e->errs = n;
		e->errs_cap = ncap;
	}
	er = &e->errs[e->n_errs++];
	snprintf(er->path, sizeof(er->path), "%s", path);
	er->line = line;
	er->col = col;
	snprintf(er->msg, sizeof(er->msg), "%s", msg);
}

/* A build's captured output, indexed by line as it streams in. The raw bytes
 * grow in one buffer (newlines intact, so parse_diagnostics can read it), and
 * "start" holds each line's byte offset. Offsets, not pointers, survive the
 * buffer being realloc'd while more output arrives. */
struct build_lines {
	char	*buf;		/* the whole output, NUL-terminated */
	size_t	len, cap;
	size_t	*start;		/* byte offset of each line's first char */
	int	nlines, lcap;
	size_t	scan;		/* bytes already scanned for newlines */
	int	need_start;	/* the next byte begins a new line */
	int	capped;		/* output hit the size cap and was truncated */
};

static void
build_lines_init(struct build_lines *b)
{
	memset(b, 0, sizeof(*b));
	b->need_start = 1;		/* the first byte opens line 0 */
}

static void
build_lines_free(struct build_lines *b)
{
	free(b->buf);
	free(b->start);
}

/* Append n raw bytes and extend the line index over the new data. Capped
 * output past 4 MB is dropped by the caller, so this never has to shrink. */
static void
build_lines_append(struct build_lines *b, const char *data, size_t n)
{
	size_t i;

	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap : 8192;
		char *nb;

		while (b->len + n + 1 > nc)
			nc *= 2;
		nb = realloc(b->buf, nc);
		if (!nb)
			return;
		b->buf = nb;
		b->cap = nc;
	}
	memcpy(b->buf + b->len, data, n);
	b->len += n;
	b->buf[b->len] = '\0';

	for (i = b->scan; i < b->len; i++) {
		if (b->need_start) {
			if (b->nlines >= b->lcap) {
				int lc = b->lcap ? b->lcap * 2 : 128;
				size_t *ns = realloc(b->start,
				    (size_t)lc * sizeof(*ns));

				if (!ns) {
					/* Leave scan at i so this line is
					 * retried on the next append rather
					 * than swallowed into the previous
					 * one. */
					b->scan = i;
					return;
				}
				b->start = ns;
				b->lcap = lc;
			}
			b->start[b->nlines++] = i;
			b->need_start = 0;
		}
		if (b->buf[i] == '\n')
			b->need_start = 1;
	}
	b->scan = b->len;
}

/* Line provider for the build-output view. Copies one line into a static
 * buffer, dropping the trailing newline (and a CR, for CRLF output). */
static const char *
build_out_line(struct editor *e, void *ctx, int idx)
{
	static char line[1024];
	const struct build_lines *b = ctx;
	size_t s, end, n;

	(void)e;
	if (idx < 0 || idx >= b->nlines)
		return NULL;
	s = b->start[idx];
	end = (idx + 1 < b->nlines) ? b->start[idx + 1] : b->len;
	if (end > s && b->buf[end - 1] == '\n')
		end--;
	if (end > s && b->buf[end - 1] == '\r')
		end--;
	n = end - s;
	if (n >= sizeof(line))
		n = sizeof(line) - 1;
	memcpy(line, b->buf + s, n);
	line[n] = '\0';
	return line;
}

/* Parse one output line for a gcc/clang-style "path:line[:col]: message"
 * diagnostic and record it. Lines that do not match are ignored. */
static void
parse_one_diag(struct editor *e, const char *line)
{
	const char *c, *num, *msg;
	char path[512];
	long lno, col = 0;
	char *end;
	size_t plen;

	/* the first ':' that is followed by a digit marks the end of the path */
	for (c = line; (c = strchr(c, ':')) != NULL; c++)
		if (isdigit((unsigned char)c[1]))
			break;
	if (!c)
		return;
	plen = (size_t)(c - line);
	if (plen == 0 || plen >= sizeof(path))
		return;
	/* A real diagnostic path is a single token. gcc context lines such as
	 * "In file included from foo.c:1:" put words before the name, so a
	 * space in the path means this is not a file:line to jump to. */
	if (memchr(line, ' ', plen) != NULL)
		return;

	num = c + 1;
	lno = strtol(num, &end, 10);
	if (end == num || *end != ':')		/* need at least path:line: */
		return;
	if (isdigit((unsigned char)end[1])) {	/* optional :col */
		char *end2;
		long v = strtol(end + 1, &end2, 10);

		if (*end2 == ':') {
			col = v;
			end = end2;
		}
	}

	memcpy(path, line, plen);
	path[plen] = '\0';
	msg = end;
	if (*msg == ':')
		msg++;
	while (*msg == ' ')
		msg++;
	build_add_err(e, path, lno, col, msg);
}

/* Parse every line of the captured output into the diagnostics list. */
static void
parse_diagnostics(struct editor *e, const char *out)
{
	const char *p = out;

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
		char line[1024];
		size_t n = linelen < sizeof(line) - 1 ? linelen
		    : sizeof(line) - 1;

		memcpy(line, p, n);
		line[n] = '\0';
		parse_one_diag(e, line);
		if (!eol)
			break;
		p = eol + 1;
	}
}

/* The final path component of s. */
static const char *
build_basename(const char *s)
{
	const char *slash = strrchr(s, '/');

	return slash ? slash + 1 : s;
}

/* Whether a diagnostic path names the file in the buffer. Compared by base
 * name, since a build usually names the file relative to its own directory. */
static int
build_matches_file(struct editor *e, const char *path)
{
	return strcmp(build_basename(path), build_basename(e->path)) == 0;
}

/* Turn a diagnostic's path into one that can be opened. Absolute paths pass
 * through; a relative path is taken against the build's working directory,
 * since a compiler names files relative to where it ran. */
static void
build_resolve_path(struct editor *e, const char *path, char *out, size_t outsz)
{
	if (path[0] == '/' || e->build_dir[0] == '\0')
		snprintf(out, outsz, "%s", path);
	else
		snprintf(out, outsz, "%s/%s", e->build_dir, path);
}

/* Move to the next (dir > 0) or previous (dir < 0) diagnostic, cycling over
 * the whole quickfix list. A diagnostic in another file opens that file
 * (buf_open) before the cursor lands on the line. Returns 1 when one was
 * found, with a status message. */
static int
build_goto(struct editor *e, int dir)
{
	int idx[2000], n = 0, i, pos = -1, target;
	struct build_err *er;
	size_t ln;

	for (i = 0; i < e->n_errs && n < (int)(sizeof(idx) / sizeof(idx[0]));
	    i++)
		if (e->errs[i].line > 0)
			idx[n++] = i;
	if (n == 0) {
		snprintf(e->status, sizeof(e->status), "no build errors");
		return 0;
	}
	for (i = 0; i < n; i++)
		if (idx[i] == e->err_cur) {
			pos = i;
			break;
		}
	if (pos < 0)
		target = dir >= 0 ? 0 : n - 1;
	else {
		target = pos + (dir >= 0 ? 1 : -1);
		if (target < 0)
			target = n - 1;
		if (target >= n)
			target = 0;
	}
	e->err_cur = idx[target];
	er = &e->errs[e->err_cur];

	/* Open the diagnostic's file when it is not the current buffer. */
	if (!build_matches_file(e, er->path)) {
		char resolved[PATH_MAX];

		build_resolve_path(e, er->path, resolved, sizeof(resolved));
		if (access(resolved, F_OK) != 0 || buf_open(e, resolved) < 0) {
			snprintf(e->status, sizeof(e->status),
			    "error %d/%d: cannot open %.80s", target + 1, n,
			    resolved);
			return 0;
		}
	}

	ln = (size_t)er->line - 1;
	if (ln >= text_lines(e->t))
		ln = text_lines(e->t) ? text_lines(e->t) - 1 : 0;
	e->cy = ln;
	e->cx = er->col > 0 ? (size_t)vi_col_to_byte(e, e->cy, (int)er->col - 1)
	    : 0;
	e->sel_active = 0;
	clamp_col(e);
	snprintf(e->status, sizeof(e->status), "error %d/%d: %.120s",
	    target + 1, n, er->msg);
	return 1;
}

/* Browse the finished output in a scrollable full-screen viewer, starting at
 * line "top", until a key dismisses it. The caller owns b. */
static void
build_browse(struct editor *e, const char *hdr, struct build_lines *b, int top)
{
	int page;

	for (;;) {
		struct draw_event ev;
		int et;

		page = (e->rows > 3 ? e->rows : 24) - 3;
		if (page < 1)
			page = 1;
		draw_scroll_view(e, hdr,
		    " Up/Down/PgUp/PgDn scroll -- q, Esc, or Enter returns",
		    top, build_out_line, b);

		et = draw_wait(e->d, &ev);
		if (et == DRAW_EVENT_EOF)
			break;
		if (et == DRAW_EVENT_RESIZE || et == DRAW_EVENT_RESUME) {
			draw_size(e->d, &e->rows, &e->cols);
			continue;
		}
		if (et != DRAW_EVENT_KEY || ev.key.type != TKBD_KEY)
			continue;
		switch (ev.key.key) {
		case TKBD_KEY_UP:
			if (top > 0)
				top--;
			break;
		case TKBD_KEY_DOWN:
			if (top < b->nlines - 1)
				top++;
			break;
		case TKBD_KEY_PGUP:
			top -= page;
			if (top < 0)
				top = 0;
			break;
		case TKBD_KEY_PGDN:
			top += page;
			if (top > b->nlines - 1)
				top = b->nlines > 0 ? b->nlines - 1 : 0;
			break;
		case TKBD_KEY_HOME:
			top = 0;
			break;
		case TKBD_KEY_END:
			top = b->nlines > page ? b->nlines - page : 0;
			break;
		default:
			return;		/* any other key returns to editing */
		}
	}
}

/* Fork cmd in dir and stream its combined stdout and stderr into b, repainting
 * the viewer as output arrives so the editor never freezes on a slow build.
 * The view follows the tail until the user scrolls up; q (or any non-scroll
 * key) cancels the build and sets *cancelled. On return b holds the whole
 * output and *status is the child's exit code, or -1 on a spawn failure.
 * Output is capped at 4 MB. */
static void
build_stream(struct editor *e, const char *verb, const char *cmd,
    const char *dir, struct build_lines *b, int *status, int *cancelled)
{
	int pfd[2], follow = 1, top = 0, dirty = 1, done = 0;
	pid_t pid;
	int st = 0;

	*status = -1;
	*cancelled = 0;
	if (pipe(pfd) != 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);

		signal(SIGWINCH, SIG_DFL);
		close(pfd[0]);
		if (chdir(dir) != 0)
			_exit(126);
		if (nul >= 0) {
			dup2(nul, STDIN_FILENO);
			if (nul > 2)
				close(nul);
		}
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		if (pfd[1] > 2)
			close(pfd[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);

	while (!done) {
		char rb[8192];
		ssize_t n;
		struct tkbd_seq key;
		int page, newtop, ev;

		/* drain everything available without blocking */
		for (;;) {
			n = read(pfd[0], rb, sizeof(rb));
			if (n > 0) {
				if (b->len < 4u * 1024 * 1024) {
					build_lines_append(b, rb, (size_t)n);
					dirty = 1;
				} else
					b->capped = 1;
				continue;
			}
			if (n == 0) {			/* child closed the pipe */
				done = 1;
				break;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			done = 1;			/* read error: give up */
			break;
		}
		if (done)
			break;

		draw_size(e->d, &e->rows, &e->cols);
		page = (e->rows > 3 ? e->rows : 24) - 3;
		if (page < 1)
			page = 1;
		if (follow) {
			newtop = b->nlines > page ? b->nlines - page : 0;
			if (newtop != top) {
				top = newtop;
				dirty = 1;
			}
		}
		if (dirty) {
			char hdr[160];

			snprintf(hdr, sizeof(hdr),
			    " %s: running -- %d line%s (q cancels)",
			    verb, b->nlines, b->nlines == 1 ? "" : "s");
			draw_scroll_view(e, hdr,
			    " Up/Down/PgUp/PgDn scroll -- q cancels the build",
			    top, build_out_line, b);
			dirty = 0;
		}

		ev = draw_next_event(e->d, 40, &key);
		if (ev < 0) {			/* terminal input closed */
			kill(pid, SIGTERM);
			*cancelled = 1;
			break;
		}
		if (ev == 0)			/* timed out: poll the pipe again */
			continue;
		if (key.type != TKBD_KEY)
			continue;
		switch (key.key) {
		case TKBD_KEY_UP:
			if (top > 0)
				top--;
			follow = 0;
			dirty = 1;
			break;
		case TKBD_KEY_PGUP:
			top -= page;
			if (top < 0)
				top = 0;
			follow = 0;
			dirty = 1;
			break;
		case TKBD_KEY_HOME:
			top = 0;
			follow = 0;
			dirty = 1;
			break;
		case TKBD_KEY_DOWN:
		case TKBD_KEY_PGDN:
		case TKBD_KEY_END:
			follow = 1;		/* rejoin the tail */
			dirty = 1;
			break;
		default:
			kill(pid, SIGTERM);	/* q, Esc, Enter, ... cancel */
			*cancelled = 1;
			done = 1;
			break;
		}
	}

	/* Collect any bytes already buffered, then stop: EOF and a cancel both
	 * reach here, and waitpid below reaps the child (a cancel makes it exit
	 * on SIGTERM or SIGPIPE). EAGAIN means nothing is waiting, so break
	 * rather than spin. */
	for (;;) {
		char rb[8192];
		ssize_t n = read(pfd[0], rb, sizeof(rb));

		if (n > 0) {
			if (b->len < 4u * 1024 * 1024)
				build_lines_append(b, rb, (size_t)n);
			else
				b->capped = 1;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;			/* EOF, EAGAIN, or error */
	}
	close(pfd[0]);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	*status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Compile or make: stream the command's output into the viewer as it runs,
 * parse the diagnostics when it finishes, then browse the output and jump to
 * the first diagnostic. */
static void
build_captured(struct editor *e, const char *verb, const char *cmd,
    const char *dir)
{
	struct build_lines b;
	int status, cancelled, page, top;
	char hdr[160];

	build_lines_init(&b);
	snprintf(e->status, sizeof(e->status), "running %s...", verb);
	render(e, e->d);		/* show the status before the first frame */

	build_stream(e, verb, cmd, dir, &b, &status, &cancelled);
	if (cancelled) {		/* q during the build: back to editing */
		build_lines_free(&b);
		snprintf(e->status, sizeof(e->status), "%s: cancelled", verb);
		return;
	}

	build_clear_errors(e);
	snprintf(e->build_dir, sizeof(e->build_dir), "%s", dir);
	if (b.buf)
		parse_diagnostics(e, b.buf);

	if (status < 0)
		snprintf(hdr, sizeof(hdr), " %s: failed to run", verb);
	else
		snprintf(hdr, sizeof(hdr),
		    " %s exited %d -- %d diagnostic%s%s (arrows scroll, q returns)",
		    verb, status, e->n_errs, e->n_errs == 1 ? "" : "s",
		    b.capped ? " [output truncated]" : "");

	page = (e->rows > 3 ? e->rows : 24) - 3;
	if (page < 1)
		page = 1;
	top = b.nlines > page ? b.nlines - page : 0;	/* start at the tail */
	build_browse(e, hdr, &b, top);
	build_lines_free(&b);

	if (!build_goto(e, +1))		/* jump to the first error here, if any */
		snprintf(e->status, sizeof(e->status),
		    status == 0 ? "%s: ok" : "%s: exited %d (%d diagnostics)",
		    verb, status, e->n_errs);
}

/* Run a build verb on the current file: compile, make, or run. The working
 * directory defaults to the file's own directory (build.<ext>.dir overrides).
 * Compile and make run captured, with their output and diagnostics shown in a
 * viewer; run is interactive, sent to the adjacent pane in a session or run
 * inline with the editor suspended otherwise. */
static void
run_build(struct editor *e, const char *verb)
{
	char ext[32], cmd[1024], dir[PATH_MAX], qdir[PATH_MAX + 16];
	const char *tmpl, *dtmpl, *session;
	char key[128];

	if (!e->has_name) {
		snprintf(e->status, sizeof(e->status),
		    "name the file first with Save As");
		return;
	}
	build_ext(e, ext, sizeof(ext));
	tmpl = build_cmd(e, ext, verb);
	if (!tmpl) {
		snprintf(e->status, sizeof(e->status),
		    "no %s command for .%s", verb, ext[0] ? ext : "?");
		return;
	}
	if (text_dirty(e->t) && build_save_wanted(e, ext)) {
		if (save_editor(e) != 0)
			return;			/* save_editor set the status */
	}

	build_expand(e, tmpl, cmd, sizeof(cmd));
	snprintf(key, sizeof(key), "build.%s.dir", ext);
	dtmpl = e->cfg ? cfg_get(e->cfg, key) : NULL;
	if (!dtmpl && e->cfg)
		dtmpl = cfg_get(e->cfg, "build.dir");
	if (!dtmpl)
		dtmpl = "$(filedir)";
	build_expand(e, dtmpl, dir, sizeof(dir));
	build_shquote(dir, qdir, sizeof(qdir));

	/* compile and make run captured, so their output can be shown and
	 * their diagnostics parsed; run is interactive and goes to a pane. */
	if (strcmp(verb, "run") != 0) {
		build_captured(e, verb, cmd, dir);
		return;
	}

	session = getenv("LUMI_SESSION");
	if (session) {
		char line[sizeof(cmd) + sizeof(qdir) + 16];
		int n = snprintf(line, sizeof(line), "(cd %s && %s)\r",
		    qdir, cmd);

		if (n < 0 || n >= (int)sizeof(line)) {
			snprintf(e->status, sizeof(e->status),
			    "%s command too long", verb);
			return;
		}
		switch (lu_send_input(session, -1, line, (size_t)n)) {
		case LU_SEND_OK:
			snprintf(e->status, sizeof(e->status),
			    "%s: sent to another pane", verb);
			break;
		case LU_SEND_NO_TARGET:
			snprintf(e->status, sizeof(e->status),
			    "%s: no other pane to send to", verb);
			break;
		case LU_SEND_READONLY:
			snprintf(e->status, sizeof(e->status),
			    "%s: target pane is read-only", verb);
			break;
		default:
			snprintf(e->status, sizeof(e->status),
			    "%s: send failed", verb);
			break;
		}
		return;
	}

	/* No session: suspend the display, run the command in the file's
	 * directory, and wait for a key so the output can be read. */
	draw_end(e->d);
	fprintf(stdout, "\r\n$ %s\r\n", cmd);
	fflush(stdout);
	{
		pid_t pid = fork();

		if (pid < 0) {
			/* fall through to resume with an error status */
		} else if (pid == 0) {
			signal(SIGWINCH, SIG_DFL);
			if (chdir(dir) != 0)
				_exit(126);
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		} else {
			int status;

			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
		}
		fprintf(stdout, "\r\n[%s done -- press Enter]", verb);
		fflush(stdout);
		{
			char c;

			while (read(STDIN_FILENO, &c, 1) == 1 &&
			    c != '\r' && c != '\n')
				;
		}
	}
	draw_begin(e->d);
	draw_size(e->d, &e->rows, &e->cols);
	snprintf(e->status, sizeof(e->status), "%s finished", verb);
}

/* Carry out a chosen menu action. Returns 1 when the editor should quit. */
static int
run_menu_act(struct editor *e, enum menu_act act)
{
	switch (act) {
	case MA_NEW:
		do_new(e);
		break;
	case MA_OPEN:
		do_open(e);
		break;
	case MA_SAVE:
		(void)save_editor(e);
		break;
	case MA_SAVE_AS:
		do_save_as(e);
		break;
	case MA_BUF_NEXT:
		buf_cycle(e, 1);
		break;
	case MA_BUF_PREV:
		buf_cycle(e, -1);
		break;
	case MA_BUF_LIST:
		buf_list(e);
		break;
	case MA_EXIT:
		if (confirm_save(e, "Save changes before exiting?"))
			return 1;
		break;
	case MA_UNDO:
		(void)dispatch(e, CMD_UNDO, NULL);
		break;
	case MA_REDO:
		(void)dispatch(e, CMD_REDO, NULL);
		break;
	case MA_CUT:
		(void)dispatch(e, CMD_CUT, NULL);
		break;
	case MA_COPY:
		(void)dispatch(e, CMD_COPY, NULL);
		break;
	case MA_PASTE:
		(void)dispatch(e, CMD_PASTE, NULL);
		break;
	case MA_FIND:
		find_prompt(e);
		break;
	case MA_FIND_NEXT:
		if (e->last_find[0])
			do_find(e, e->last_find);
		else
			snprintf(e->status, sizeof(e->status),
			    "no previous search");
		break;
	case MA_GOTO:
		goto_prompt(e);
		break;
	case MA_RUN:
		run_build(e, "run");
		break;
	case MA_COMPILE:
		run_build(e, "compile");
		break;
	case MA_MAKE:
		run_build(e, "make");
		break;
	case MA_NEXT_ERR:
		(void)build_goto(e, 1);
		break;
	case MA_PREV_ERR:
		(void)build_goto(e, -1);
		break;
	case MA_SYNTAX:
		e->hl_on = !e->hl_on;
		e->hl_valid = 0;
		snprintf(e->status, sizeof(e->status),
		    "syntax highlight %s", e->hl_on ? "on" : "off");
		break;
	case MA_SCHEME:
		e->dos_chrome = !e->dos_chrome;
		snprintf(e->status, sizeof(e->status), "%s colors",
		    e->dos_chrome ? "DOS" : "plain");
		break;
	case MA_HEX:
		e->hex_view = !e->hex_view;
		e->hex_top = 0;
		e->hex_ascii = 0;
		e->hex_pending = -1;
		e->hex_insert = 0;
		e->hex_sel = 0;
		snprintf(e->status, sizeof(e->status), "%s view",
		    e->hex_view ? "hex" : "text");
		break;
	case MA_VI_MODE:
		toggle_vi(e);
		break;
	case MA_MOUSE:
		e->mouse_on = !e->mouse_on;
		if (e->term)
			draw_term_mouse(e->term, e->mouse_on);
		snprintf(e->status, sizeof(e->status), "mouse %s%s",
		    e->mouse_on ? "on" : "off",
		    e->mouse_on ? "" : " (terminal selection restored)");
		break;
	case MA_HELP:
		show_help(e);
		break;
	case MA_ABOUT:
		show_about(e);
		break;
	case MA_NONE:
	case MA_SEP:
		break;
	}
	return 0;
}

/* Scroll the view from a click or drag on the vertical scrollbar. click_row
 * is the screen row grabbed; arrow_step treats the arrow cells as a one-line
 * nudge (a plain click), while a drag maps the whole bar proportionally. The
 * cursor is pulled back into the new view so it stays visible. */
static void
vbar_set(struct editor *e, int click_row, int arrow_step)
{
	int th = text_height(e);
	size_t nl = text_lines(e->t);
	size_t max_top = nl > (size_t)th ? nl - (size_t)th : 0;
	int i = click_row - CHROME_TOP;
	size_t top;

	if (i < 0)
		i = 0;
	if (i > th - 1)
		i = th - 1;
	if (arrow_step && i == 0)
		top = e->top > 0 ? e->top - 1 : 0;
	else if (arrow_step && i == th - 1)
		top = e->top + 1 > max_top ? max_top : e->top + 1;
	else if (max_top == 0 || th <= 1)
		top = 0;
	else
		top = (size_t)((long)i * (long)max_top / (th - 1));
	if (top > max_top)
		top = max_top;
	e->top = top;
	if (e->cy < e->top)
		e->cy = e->top;
	else if (e->cy >= e->top + (size_t)th)
		e->cy = e->top + (size_t)th - 1;
	clamp_col(e);
}

/* Scroll horizontally from a click or drag on the bottom scrollbar. The reach
 * is bounded by the cursor line's width, matching the drawn thumb. */
static void
hbar_set(struct editor *e, int click_col, int arrow_step)
{
	int tw = text_width(e);
	size_t curlen = 0;
	const char *cur = text_line(e->t, e->cy, &curlen);
	int curw = cur ? disp_cols(cur, curlen) : 0;
	size_t max_left = curw > tw ? (size_t)(curw - tw) : 0;
	int hspan = e->cols - CHROME_LEFT - CHROME_RIGHT;
	int i = click_col - 1;		/* column 1 is bar index 0 */
	size_t left;
	int cc;

	if (hspan < 1)
		hspan = 1;
	if (i < 0)
		i = 0;
	if (i > hspan - 1)
		i = hspan - 1;
	if (arrow_step && i == 0)
		left = e->left > 0 ? e->left - 1 : 0;
	else if (arrow_step && i == hspan - 1)
		left = e->left + 1 > max_left ? max_left : e->left + 1;
	else if (max_left == 0 || hspan <= 1)
		left = 0;
	else
		left = (size_t)((long)i * (long)max_left / (hspan - 1));
	if (left > max_left)
		left = max_left;
	e->left = left;
	cc = cur ? disp_cols(cur, e->cx) : 0;
	if ((size_t)cc < e->left)
		e->cx = vi_col_to_byte(e, e->cy, (int)e->left);
	else if ((size_t)cc >= e->left + (size_t)tw)
		e->cx = vi_col_to_byte(e, e->cy, (int)e->left + tw - 1);
}

/* First selectable item in menu m (skips a leading separator). */
static int
menu_first(int m)
{
	int i;

	for (i = 0; i < MENUS[m].n; i++)
		if (MENUS[m].items[i].act != MA_SEP)
			return i;
	return 0;
}

/* Step the selection within menu m by dir, skipping separators and wrapping. */
static int
menu_step(int m, int sel, int dir)
{
	int n = MENUS[m].n;
	int i;

	for (i = 0; i < n; i++) {
		sel = (sel + dir + n) % n;
		if (MENUS[m].items[sel].act != MA_SEP)
			break;
	}
	return sel;
}

/* Run the menu bar with menu `start` open. Returns the chosen action, or
 * MA_NONE when the user backs out with Esc. */
static enum menu_act
menu_bar_run(struct editor *e, int start)
{
	int cur = start;
	int sel;

	if (cur < 0)
		cur = 0;
	if (cur >= MENU_COUNT)
		cur = MENU_COUNT - 1;
	sel = menu_first(cur);

	for (;;) {
		struct draw_event ev;
		uint32_t ch;

		render_body(e, e->d);
		draw_menubar(e, chrome(e), cur);
		draw_dropdown(e, cur, sel);
		draw_cursor_vis(e->d, 0);
		draw_present(e->d);

		switch (draw_wait(e->d, &ev)) {
		case DRAW_EVENT_EOF:
			return MA_EXIT;
		case DRAW_EVENT_RESIZE:
		case DRAW_EVENT_RESUME:
			draw_size(e->d, &e->rows, &e->cols);
			continue;
		case DRAW_EVENT_KEY:
			break;
		default:
			continue;
		}
		if (ev.key.type == TKBD_MOUSE) {
			int boxw, x0, y0, item;

			if (ev.key.key != TKBD_MOUSE_LEFT)
				continue;	/* ignore drag/release/wheel */
			if (ev.key.y == 0) {	/* click on the bar */
				int m = menu_hit(e, ev.key.x);

				if (m < 0)
					return MA_NONE;	/* off a title: close */
				cur = m;
				sel = menu_first(cur);
				continue;
			}
			/* click inside the open drop-down selects an item */
			boxw = dropdown_width(e, cur) + 2;
			x0 = dropdown_x(e, cur, boxw);
			y0 = 1;			/* top border row */
			item = ev.key.y - (y0 + 1);
			if (ev.key.x > x0 && ev.key.x < x0 + boxw - 1 &&
			    item >= 0 && item < MENUS[cur].n &&
			    MENUS[cur].items[item].act != MA_SEP)
				return MENUS[cur].items[item].act;
			return MA_NONE;		/* click elsewhere closes */
		}
		if (ev.key.type != TKBD_KEY)
			continue;

		/* Alt+letter jumps straight to another menu. */
		ch = ev.key.ch;
		if ((ev.key.mod & TKBD_MOD_ALT) && ch != TKBD_CH_NONE &&
		    ch < 128) {
			int m = menu_title_by_mnemonic(tolower((int)ch));

			if (m >= 0) {
				cur = m;
				sel = menu_first(cur);
			}
			continue;
		}

		switch (ev.key.key) {
		case TKBD_KEY_ESC:
			return MA_NONE;
		case TKBD_KEY_LEFT:
			cur = (cur - 1 + MENU_COUNT) % MENU_COUNT;
			sel = menu_first(cur);
			break;
		case TKBD_KEY_RIGHT:
			cur = (cur + 1) % MENU_COUNT;
			sel = menu_first(cur);
			break;
		case TKBD_KEY_UP:
			sel = menu_step(cur, sel, -1);
			break;
		case TKBD_KEY_DOWN:
			sel = menu_step(cur, sel, 1);
			break;
		case TKBD_KEY_ENTER:
			if (MENUS[cur].items[sel].act != MA_SEP)
				return MENUS[cur].items[sel].act;
			break;
		default:
			/* an item's underlined mnemonic letter selects it */
			if (ch != TKBD_CH_NONE && ch < 128 &&
			    !(ev.key.mod & (TKBD_MOD_CTRL | TKBD_MOD_ALT))) {
				int it = menu_item_by_mnemonic(cur,
				    tolower((int)ch));

				if (it >= 0)
					return MENUS[cur].items[it].act;
			}
			break;
		}
	}
}

/* Which menu a keypress opens, or -1 for none. F10 opens the bar; Alt+letter
 * opens the matching menu. */
static int
menu_trigger(const struct tkbd_seq *seq)
{
	uint32_t ch;

	if (seq->type != TKBD_KEY)
		return -1;
	if (seq->key == TKBD_KEY_F10)
		return 0;
	ch = seq->ch;
	if ((seq->mod & TKBD_MOD_ALT) && ch != TKBD_CH_NONE && ch < 128)
		return menu_title_by_mnemonic(tolower((int)ch));
	return -1;
}

/****************************************************************
 * Terminal / lifecycle
 ****************************************************************/

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [file]\n"
	    "\n"
	    "Edit a text file in a full-screen terminal UI.\n"
	    "\n"
	    "Keys:\n"
	    "  arrows        move the cursor\n"
	    "  Home / End    start / end of line\n"
	    "  PgUp / PgDn   scroll by a screen\n"
	    "  Enter         split the line\n"
	    "  Backspace     delete left; Delete removes right\n"
	    "  Shift-arrows  extend a selection\n"
	    "  Ctrl-C / Ctrl-X  copy / cut (Ctrl-C with no selection copies the line)\n"
	    "  Ctrl-V        paste the internal clipboard\n"
	    "  Ctrl-G        send selection/line to another pane (in a session)\n"
	    "  Ctrl-F        find (Enter repeats the last search)\n"
	    "  Ctrl-L        go to a line number\n"
	    "  Ctrl-Z / Ctrl-Y  undo / redo\n"
	    "  Ctrl-S        save (prompts for a name if the buffer has none)\n"
	    "  Ctrl-Q        quit (prompts if the buffer was modified)\n"
	    "  F5 / F6 / F7  run / compile / make (see the Build menu)\n"
	    "  F4 / Shift+F4 next / previous build error in this file\n"
	    "  F1            show the key bindings\n"
	    "  F2            toggle vi keys (modal editing)\n"
	    "\n"
	    "With vi keys on: NORMAL mode has h j k l, 0 ^ $, w b e, gg G,"
	    " f F t T,\n"
	    "  ; , { } ( ) %% H M L | counts (3j), operators d c y with"
	    " motions (dw,\n"
	    "  d$, dt), cc, dd, yy, x, p, u, and i a A I o O to insert; Esc"
	    " returns to\n"
	    "  NORMAL. ZZ writes and quits, ZQ quits without writing. ':'"
	    " runs w q wq\n"
	    "  q! qa wqa cq and :N; '/' searches and n repeats.\n"
	    "\n"
	    "Files with a known extension (c, sh, ...) are syntax"
	    " highlighted; the vi\n"
	    "command ':syntax off' (or 'on', or a language name) controls"
	    " it.\n"
	    "\n"
	    "Pasted text from the terminal is inserted literally (bracketed"
	    " paste).\n",
	    progname);
}

/* What a left-button press grabbed, tracked across motion events until the
 * button is released. */
enum drag_mode { DRAG_NONE, DRAG_TEXT, DRAG_VBAR, DRAG_HBAR };

/* Handle one mouse event: wheel scroll, scrollbar press/drag, a menu-bar
 * click, and click-and-drag text selection. *drag carries the grab from a
 * press through the following motion events. Returns 1 when a menu action
 * asked to quit, 0 otherwise; the caller repaints. */
static int
handle_mouse(struct editor *e, const struct tkbd_seq *seq, enum drag_mode *drag)
{
	int th = text_height(e), tw = text_width(e);
	int sb = e->cols - CHROME_RIGHT;	 /* vertical scrollbar col */
	int botrow = e->rows - CHROME_BOTTOM;	/* bottom scrollbar row */
	int inx = seq->x - CHROME_LEFT;
	int iny = seq->y - CHROME_TOP;
	int motion = (seq->mod & TKBD_MOD_MOTION) != 0;

	if (seq->key == TKBD_MOUSE_WHEEL_UP ||
	    seq->key == TKBD_MOUSE_WHEEL_DOWN) {
		int d = (seq->key == TKBD_MOUSE_WHEEL_UP) ? -3 : 3;

		if (d < 0)
			e->cy = e->cy > (size_t)-d ? e->cy + (size_t)d : 0;
		else if (e->cy + (size_t)d < text_lines(e->t))
			e->cy += (size_t)d;
		else
			e->cy = text_lines(e->t) - 1;
		clamp_col(e);
		return 0;
	}
	if (seq->key == TKBD_MOUSE_RELEASE) {
		*drag = DRAG_NONE;
		return 0;
	}
	if (seq->key != TKBD_MOUSE_LEFT)
		return 0;

	if (motion) {
		/* continue whatever the press started */
		if (*drag == DRAG_VBAR) {
			vbar_set(e, seq->y, 0);
		} else if (*drag == DRAG_HBAR) {
			hbar_set(e, seq->x, 0);
		} else if (*drag == DRAG_TEXT) {
			size_t nl = text_lines(e->t);
			int cr = iny, cc = inx;
			size_t line;

			if (cr < 0)
				cr = 0;
			if (cr >= th)
				cr = th - 1;
			if (cc < 0)
				cc = 0;
			if (cc >= tw)
				cc = tw - 1;
			line = e->top + (size_t)cr;
			if (line >= nl)
				line = nl ? nl - 1 : 0;
			e->cy = line;
			e->cx = vi_col_to_byte(e, line, (int)e->left + cc);
			e->sel_active = 1;
			clamp_col(e);
		}
		return 0;
	}

	/* a fresh press picks what to drag */
	if (seq->y == 0) {
		int m = menu_hit(e, seq->x);

		if (m >= 0) {
			enum menu_act act = menu_bar_run(e, m);

			if (run_menu_act(e, act))
				return 1;
		}
	} else if (seq->x == sb && iny >= 0 && iny < th) {
		vbar_set(e, seq->y, 1);
		*drag = DRAG_VBAR;
	} else if (seq->y == botrow && seq->x >= 1 &&
	    seq->x <= e->cols - 2) {
		hbar_set(e, seq->x, 1);
		*drag = DRAG_HBAR;
	} else if (iny >= 0 && iny < th && inx >= 0 && inx < tw) {
		size_t nl = text_lines(e->t);
		size_t line = e->top + (size_t)iny;

		if (line >= nl)
			line = nl ? nl - 1 : 0;
		e->cy = line;
		e->cx = vi_col_to_byte(e, line, (int)e->left + inx);
		e->sel_active = 0;	/* anchor for a drag */
		e->ay = e->cy;
		e->ax = e->cx;
		*drag = DRAG_TEXT;
	}
	return 0;
}

/* Carry out a request produced by a key handler, for the views that share the
 * editor's request framework (the text and hex views). Prompts and edits run
 * on the one backing buffer, so save, quit, find, and goto behave identically
 * whichever view raised them. Returns 1 when the editor should exit. */
static int
run_req(struct editor *e, enum req req)
{
	switch (req) {
	case REQ_FIND:
		find_prompt(e);
		break;
	case REQ_GOTO:
		goto_prompt(e);
		break;
	case REQ_HELP:
		show_help(e);
		break;
	case REQ_SAVE:
		(void)save_editor(e);
		break;
	case REQ_QUIT:
		if (confirm_save(e, "Save changes before exiting?"))
			return 1;
		break;
	default:
		break;
	}
	return 0;
}

int
cmd_edit_main(int argc, char **argv)
{
	struct editor e;
	struct draw_term *term;
	int rc = 0;
	enum drag_mode drag = DRAG_NONE;

	if (argv[0])
		progname = argv[0];

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0)) {
		usage();
		return 0;
	}

	rune_width_init();

	memset(&e, 0, sizeof(e));
	e.in_session = getenv("LUMI_SESSION") != NULL;
	load_build_config(&e);
	{			/* edit.theme picks the chrome palette */
		const char *tn = e.cfg ? cfg_get(e.cfg, "edit.theme") : NULL;
		const struct tui_theme *th = tn ? tui_theme_by_name(tn) : NULL;

		if (th)
			chrome_from_theme(th, &chrome_dos);
	}
	load_syntax_colors(&e);
	e.hl_on = 1;
	e.hex_pending = -1;
	e.hex_cols = 16;	/* dump width; the hex view's w key cycles it */
	e.dos_chrome = 1;	/* DOS EDIT look by default; toggleable */
	e.mouse_on = 1;		/* mouse on by default; Options > Mouse toggles */
	e.t = text_new();
	if (!e.t) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}

	if (argc > 1) {
		snprintf(e.path, sizeof(e.path), "%s", argv[1]);
		e.has_name = 1;
		if (text_load(e.t, e.path) < 0 && errno != ENOENT) {
			fprintf(stderr, "%s: %s: %s\n", progname, e.path,
			    strerror(errno));
			text_free(e.t);
			return 1;
		}
		/* a missing file opens as an empty new buffer */
		e.syn = syn_for_ext(file_ext(e.path));
	}

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "%s: not a terminal\n", progname);
		text_free(e.t);
		return 1;
	}

	/* The terminal driver owns raw mode, the alt screen, bracketed paste
	 * (so pasted text arrives wrapped in PASTE_BEGIN/END and never fires
	 * editor commands), and the SIGWINCH and SIGTSTP handling delivered as
	 * draw_wait events. Mouse reporting is on by default (Options > Mouse
	 * turns it off to restore the terminal's own selection). */
	term = draw_term_new(STDIN_FILENO, STDOUT_FILENO, NULL);
	if (!term) {
		fprintf(stderr, "%s: cannot open the terminal\n", progname);
		text_free(e.t);
		return 1;
	}
	e.term = term;
	e.d = draw_new(draw_term_driver(), term);
	if (!e.d) {
		fprintf(stderr, "%s: out of memory\n", progname);
		text_free(e.t);
		return 1;
	}
	draw_size(e.d, &e.rows, &e.cols);
	draw_begin(e.d);
	draw_term_mouse(term, e.mouse_on);

	/* Register the initial file as buffer 0. The flat editor stays the live
	 * copy of the active buffer; e.bufs holds the parked ones. */
	e.err_cur = -1;
	if (buf_slot(&e) < 0) {
		fprintf(stderr, "%s: out of memory\n", progname);
		draw_end(e.d);
		draw_free(e.d);
		text_free(e.t);
		return 1;
	}
	buf_save(&e, &e.bufs[0]);

	snprintf(e.status, sizeof(e.status), "Press F1 for help");
	render(&e, e.d);

	for (;;) {
		struct draw_event ev;
		struct tkbd_seq seq;

		switch (draw_wait(e.d, &ev)) {
		case DRAW_EVENT_EOF:
			rc = 1;
			goto done;
		case DRAW_EVENT_RESIZE:
		case DRAW_EVENT_RESUME:
			draw_size(e.d, &e.rows, &e.cols);
			render(&e, e.d);
			continue;
		case DRAW_EVENT_KEY:
			seq = ev.key;
			break;
		default:
			continue;
		}

		e.status[0] = '\0';	/* clear any transient message */

		/* F10 or Alt+letter opens the menu bar. It takes over input
		 * until an item is chosen or Esc backs out, then the chosen
		 * action runs here (MA_EXIT may end the editor). */
		{
			int mi = menu_trigger(&seq);

			if (mi >= 0) {
				enum menu_act act = menu_bar_run(&e, mi);

				if (run_menu_act(&e, act))
					goto done;
				render(&e, e.d);
				continue;
			}
		}

		/* Mouse: click the bar to open a menu, drag the scrollbars to
		 * scroll, click the text to place the cursor and drag to
		 * select, and the wheel to scroll. */
		if (seq.type == TKBD_MOUSE) {
			if (handle_mouse(&e, &seq, &drag))
				goto done;
			render(&e, e.d);
			continue;
		}

		/* In the hex view, keys drive the hex navigator (menu and mouse
		 * above still work); text editing and personality keys do not
		 * apply. It shares the editor's request handling, so its Ctrl-S
		 * and Ctrl-Q save and quit exactly as the text view does. */
		if (e.hex_view) {
			if (run_req(&e, hex_key(&e, &seq)))
				goto done;
			render(&e, e.d);
			continue;
		}

		/* F2 toggles between the modeless and vi personalities. It is
		 * ignored while inserting so it does not interrupt typing. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_F2 &&
		    !(seq.mod & TKBD_MOD_CTRL) && e.mode != MODE_INSERT) {
			if (e.mode == MODE_MODELESS) {
				e.mode = MODE_NORMAL;
				e.sel_active = 0;
				vi_reset_pending(&e);
				vi_clamp(&e);
				snprintf(e.status, sizeof(e.status),
				    "-- NORMAL -- (F2 returns to modeless)");
			} else {
				e.mode = MODE_MODELESS;
				vi_reset_pending(&e);
				snprintf(e.status, sizeof(e.status),
				    "modeless mode (F2 for vi keys)");
			}
			render(&e, e.d);
			continue;
		}

		/* Build keys work in every personality, so they are handled
		 * here rather than through the modeless/vi keymaps: F5 run,
		 * F6 compile, F7 make. */
		if (seq.type == TKBD_KEY && !(seq.mod & TKBD_MOD_CTRL) &&
		    (seq.key == TKBD_KEY_F5 || seq.key == TKBD_KEY_F6 ||
		     seq.key == TKBD_KEY_F7)) {
			run_build(&e, seq.key == TKBD_KEY_F5 ? "run" :
			    seq.key == TKBD_KEY_F6 ? "compile" : "make");
			render(&e, e.d);
			continue;
		}

		/* F4 jumps to the next build error in this file, Shift+F4 to
		 * the previous one. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_F4 &&
		    !(seq.mod & TKBD_MOD_CTRL)) {
			(void)build_goto(&e, (seq.mod & TKBD_MOD_SHIFT) ? -1 : 1);
			render(&e, e.d);
			continue;
		}

		/* F8 cycles to the next open buffer, Shift+F8 to the previous
		 * one; a no-op while only one file is open. */
		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_F8 &&
		    !(seq.mod & TKBD_MOD_CTRL)) {
			buf_cycle(&e, (seq.mod & TKBD_MOD_SHIFT) ? -1 : 1);
			render(&e, e.d);
			continue;
		}

		if (seq.type == TKBD_KEY && seq.key == TKBD_KEY_PASTE_BEGIN) {
			if (e.mode == MODE_NORMAL)
				paste_discard(&e);
			else
				paste_input(&e);
			render(&e, e.d);
			continue;
		}

		if (e.mode != MODE_MODELESS) {
			switch (vi_dispatch(&e, &seq)) {
			case REQ_VI_COLON:
				switch (vi_colon(&e)) {
				case REQ_FORCE_QUIT:
					goto done;
				case REQ_QUIT_ERR:
					rc = 1;		/* :cq exits nonzero */
					goto done;
				default:
					break;
				}
				break;
			case REQ_VI_SEARCH:
				vi_search(&e);
				break;
			case REQ_HELP:
				show_help(&e);
				break;
			case REQ_FORCE_QUIT:
				goto done;
			default:
				break;
			}
			render(&e, e.d);
			continue;
		}

		if (run_req(&e, dispatch(&e, key_to_cmd(&seq), &seq)))
			goto done;
		render(&e, e.d);
	}
done:

	draw_end(e.d);
	draw_free(e.d);		/* also frees the terminal driver */

	/* Free every buffer's file resources. The active buffer's live pointers
	 * are mirrored back into its slot first, so each is freed exactly once. */
	if (e.nbuf > 0) {
		int i;

		buf_save(&e, &e.bufs[e.cur]);
		for (i = 0; i < e.nbuf; i++)
			buf_free_fields(e.bufs[i].t, e.bufs[i].line_state);
	} else {
		buf_free_fields(e.t, e.line_state);
	}
	free(e.bufs);
	free(e.errs);			/* the one shared diagnostics list */

	if (e.cfg)
		cfg_free(e.cfg);
	free(e.clip);
	for (int i = 0; i < 26; i++)
		free(e.vi_regs[i].bytes);
	free(e.vi_dot.ev);
	free(e.vi_rec.ev);
	free(e.hl_buf);
	return rc;
}
