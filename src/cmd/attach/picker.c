/* picker.c : window picker submenu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "picker.h"
#include "attach_ui.h"

#include "utf8.h"
#include "rune_width.h"
#include "tui_list.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WIN_INFO_MAX 32

struct win_info {
	uint32_t id;
	uint32_t pid;
	int active;
	char title[128];
};

#define ACTIVE_MARKER "\xe2\x9c\xb1"	/* U+2731 HEAVY ASTERISK ✱ */
#define TAB_LEFT "\xe2\x95\xad"		/* U+256D ╭ */
#define TAB_RIGHT "\xe2\x95\xae"	/* U+256E ╮ */

static struct win_info win_list[WIN_INFO_MAX];
static int win_count;

/* clickable column ranges of each tab in the last-formatted window-list
 * taskbar text, in display columns, for mouse hit-testing. */
struct win_hit {
	int start, end;		/* [start, end) display columns */
	uint32_t pid;
};
static struct win_hit win_hits[WIN_INFO_MAX];
static int win_hit_count;

#define PICKER_MAX_ITEMS WIN_INFO_MAX

static struct win_info picker_items[PICKER_MAX_ITEMS];
static int picker_count;
static int picker_sel;
static struct tui_list picker_list;

void
win_list_clear(void)
{
	win_count = 0;
}

void
win_list_add(uint32_t id, uint32_t pid, const char *title, int active)
{
	struct win_info *wi;

	if (win_count >= WIN_INFO_MAX)
		return;
	wi = &win_list[win_count++];
	wi->id = id;
	wi->pid = pid;
	wi->active = active;
	if (title) {
		size_t len = utf8_trunc(title, sizeof(wi->title));
		memcpy(wi->title, title, len);
		wi->title[len] = '\0';
	} else {
		wi->title[0] = '\0';
	}
}

int
win_list_active_id(uint32_t *out_id)
{
	int i;

	for (i = 0; i < win_count; i++) {
		if (win_list[i].active) {
			if (out_id)
				*out_id = win_list[i].id;
			return 1;
		}
	}
	return 0;
}

int
win_list_count(void)
{
	return win_count;
}

uint32_t
win_list_id_at(int index)
{
	if (index < 0 || index >= win_count)
		return 0;
	return win_list[index].id;
}

const char *
win_list_title_at(int index)
{
	if (index < 0 || index >= win_count)
		return "";
	return win_list[index].title;
}

uint32_t
win_list_pid_at(int index)
{
	if (index < 0 || index >= win_count)
		return 0;
	return win_list[index].pid;
}

void
win_list_set_title(uint32_t pid, const char *title)
{
	int i;

	for (i = 0; i < win_count; i++) {
		if (win_list[i].pid == pid) {
			if (title) {
				size_t len = utf8_trunc(title,
				    sizeof(win_list[i].title));
				memcpy(win_list[i].title, title, len);
				win_list[i].title[len] = '\0';
			} else {
				win_list[i].title[0] = '\0';
			}
			return;
		}
	}
}

/* visible column width of a chunk of the taskbar text, skipping ANSI
 * CSI escapes and counting multi-byte runes by display width. mirrors
 * libtaskbar's internal ansi_display_width(). */
static int
tab_chunk_width(const char *s, int len)
{
	int i = 0, w = 0;

	while (i < len) {
		if (s[i] == '\033') {
			i++;
			if (i < len && s[i] == '[') {
				i++;
				while (i < len &&
				    (unsigned char)s[i] < 0x40)
					i++;
				if (i < len)
					i++;
			}
		} else if ((unsigned char)s[i] >= 0x80) {
			uint32_t rune;
			int clen;

			clen = utf8_decode(&rune,
			    (const unsigned char *)s + i,
			    (size_t)(len - i));
			w += rune_width(rune);
			i += clen > 0 ? clen : 1;
		} else {
			if (s[i] >= 0x20)
				w++;
			i++;
		}
	}
	return w;
}

/* U+2026 HORIZONTAL ELLIPSIS, one display column, marks an elided title. */
#define TAB_ELLIPSIS "\xe2\x80\xa6"

/* smallest title cap we will shrink to; below this a title is unreadable. */
#define TAB_TITLE_MIN 8

/* display width of a plain (no ANSI) UTF-8 string, in columns. */
static int
title_width(const char *s)
{
	size_t n = strlen(s), i = 0;
	int w = 0;

	while (i < n) {
		uint32_t rune;
		int clen = utf8_decode(&rune,
		    (const unsigned char *)s + i, n - i);

		if (clen <= 0)
			clen = 1;
		w += rune_width(rune);
		i += (size_t)clen;
	}
	return w;
}

/* copy src into dst capped at maxcols display columns; when truncated,
 * the last column is a trailing ellipsis.  returns bytes written. */
static int
title_elide(char *dst, size_t dstsz, const char *src, int maxcols)
{
	size_t n = strlen(src), i = 0, o = 0;
	int w = 0;

	if (title_width(src) <= maxcols) {
		while (i < n && o + 1 < dstsz)
			dst[o++] = src[i++];
		dst[o] = '\0';
		return (int)o;
	}

	/* reserve one column for the ellipsis */
	while (i < n) {
		uint32_t rune;
		int clen = utf8_decode(&rune,
		    (const unsigned char *)src + i, n - i);
		int rw;

		if (clen <= 0)
			clen = 1;
		rw = rune_width(rune);
		if (w + rw > maxcols - 1)
			break;
		if (o + (size_t)clen + sizeof(TAB_ELLIPSIS) >= dstsz)
			break;
		memcpy(dst + o, src + i, (size_t)clen);
		o += (size_t)clen;
		i += (size_t)clen;
		w += rw;
	}
	memcpy(dst + o, TAB_ELLIPSIS, sizeof(TAB_ELLIPSIS) - 1);
	o += sizeof(TAB_ELLIPSIS) - 1;
	dst[o] = '\0';
	return (int)o;
}

/* decimal digits in an unsigned window id. */
static int
id_digits(uint32_t id)
{
	int d = 1;

	while (id >= 10) {
		id /= 10;
		d++;
	}
	return d;
}

/* display width one tab would occupy with its title capped at tmax cols. */
static int
tab_width_est(const struct win_info *wi, int tmax)
{
	int w = 2 + id_digits(wi->id);		/* borders + id */
	int tw = title_width(wi->title);

	if (wi->active)
		w += 1;				/* active marker */
	else if (wi->title[0])
		w += 1;				/* space before title */
	if (wi->title[0])
		w += (tw <= tmax ? tw : tmax);
	return w;
}

/* total width of all tabs plus one-column separators, at title cap tmax. */
static int
tabs_total_width(int tmax)
{
	int i, total = 0;

	for (i = 0; i < win_count; i++)
		total += tab_width_est(&win_list[i], tmax);
	if (win_count > 1)
		total += win_count - 1;
	return total;
}

/* choose the largest per-tab title cap (in display columns) that lets every
 * tab fit within avail columns.  starts from the longest title and halves,
 * never going below TAB_TITLE_MIN. */
static int
win_list_title_cap(int avail)
{
	int i, cap = 0;

	for (i = 0; i < win_count; i++) {
		int tw = title_width(win_list[i].title);

		if (tw > cap)
			cap = tw;
	}
	while (cap > TAB_TITLE_MIN && tabs_total_width(cap) > avail) {
		cap /= 2;
		if (cap < TAB_TITLE_MIN)
			cap = TAB_TITLE_MIN;
	}
	return cap;
}

void
win_list_format_taskbar(void)
{
	char buf[1024];
	int pos = 0;
	int i, n, remain;
	int dcol = 0;
	int avail, cap;
	struct winsize ws;

	win_hit_count = 0;

	avail = 80;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		avail = ws.ws_col;
	cap = win_list_title_cap(avail);

	for (i = 0; i < win_count; i++) {
		struct win_info *wi = &win_list[i];
		char etitle[128];
		int chunk_start;

		if (wi->title[0])
			title_elide(etitle, sizeof(etitle), wi->title, cap);
		else
			etitle[0] = '\0';

		remain = (int)sizeof(buf) - pos;
		if (remain <= 1)
			break;

		if (pos > 0) {
			chunk_start = pos;
			n = snprintf(buf + pos, (size_t)remain,
			    "\033[40m ");
			if (n > 0 && n < remain) {
				dcol += tab_chunk_width(buf + chunk_start, n);
				pos += n;
			}
			remain = (int)sizeof(buf) - pos;
			if (remain <= 1)
				break;
		}

		chunk_start = pos;
		if (wi->active) {
			if (wi->title[0])
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[37;100m" TAB_LEFT
				    "%u"
				    "\033[33;100m" ACTIVE_MARKER
				    "\033[37;100m%s"
				    TAB_RIGHT "\033[0;40m",
				    wi->id, etitle);
			else
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[37;100m" TAB_LEFT
				    "%u"
				    "\033[33;100m" ACTIVE_MARKER
				    "\033[37;100m" TAB_RIGHT
				    "\033[0;40m",
				    wi->id);
		} else {
			if (wi->title[0])
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[90;40m" TAB_LEFT
				    "%u %s"
				    TAB_RIGHT,
				    wi->id, etitle);
			else
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[90;40m" TAB_LEFT
				    "%u"
				    TAB_RIGHT,
				    wi->id);
		}

		if (n < 0 || n >= remain)
			break;
		pos += n;

		if (win_hit_count < WIN_INFO_MAX) {
			struct win_hit *h = &win_hits[win_hit_count++];
			int w = tab_chunk_width(buf + chunk_start, n);

			h->start = dcol;
			h->end = dcol + w;
			h->pid = wi->pid;
			dcol += w;
		}
	}

	buf[pos] = '\0';
	taskbar_set(taskbar, "window-list", buf);
}

/* Mark the tab for `pid` as the current one (and clear the rest), then
 * re-render the taskbar text.  Focus can change after mconn_sync_winlist
 * has already stamped the highlight (e.g. when a window is added or
 * closed), so callers drive the highlight from the live focus at render
 * time.  pid == 0 clears the highlight. */
void
win_list_set_active(uint32_t pid)
{
	int i, changed = 0;

	for (i = 0; i < win_count; i++) {
		int active = (pid != 0 && win_list[i].pid == pid);

		if (win_list[i].active != active) {
			win_list[i].active = active;
			changed = 1;
		}
	}
	if (changed)
		win_list_format_taskbar();
}

/* find the window whose tab occupies display column `col` in the
 * window-list text last built by win_list_format_taskbar(). returns 1 and
 * sets *out_pid on a hit, 0 otherwise. used for mouse clicks on the tab
 * bar; the caller is responsible for knowing that `col` is relative to
 * where the window-list variable landed in the taskbar. */
int
win_list_hit_test(int col, uint32_t *out_pid)
{
	int i;

	for (i = 0; i < win_hit_count; i++) {
		if (col >= win_hits[i].start && col < win_hits[i].end) {
			if (out_pid)
				*out_pid = win_hits[i].pid;
			return 1;
		}
	}
	return 0;
}

/* ---- row drawing callback ---- */

static void
picker_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	struct win_info *items = ctx;
	struct win_info *wi = &items[index];
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	struct vt_color kfg = selected ? th->sel_key_fg : th->key_fg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;
	char idstr[12];
	int idlen, j, c;

	(void)width;

	idlen = snprintf(idstr, sizeof(idstr), "%u", wi->id);
	c = col;

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	for (j = idlen; j < 3; j++)
		tui_pad_put(p, row, c++, ' ', kfg, bg, attrs, TUI_OPAQUE);
	tui_pad_puts(p, row, c, idstr, kfg, bg, attrs, TUI_OPAQUE);
	c += idlen;

	tui_pad_put(p, row, c++, wi->active ? '*' : ' ', fg, bg,
	    attrs, TUI_OPAQUE);

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
	c = tui_pad_puts(p, row, c, wi->title, fg, bg, attrs,
	    TUI_OPAQUE);

	while (c < col + width - 1)
		tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
	tui_pad_put(p, row, c, ' ', fg, bg, attrs, TUI_OPAQUE);
}

/* ---- picker UI ---- */

static void
picker_draw(void)
{
	struct winsize ws;
	struct tui_pad *p;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	picker_list.count = picker_count;
	picker_list.sel = picker_sel;

	p = tui_stack_top(&overlay);
	if (!p) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	}

	tui_list_draw(p, &picker_list, theme, picker_row_cb,
	    picker_items, "Windows", ws.ws_row, ws.ws_col);
	overlay_visible = 1;
	overlay_render();

	picker_sel = picker_list.sel;
}

void
picker_show(void)
{
	struct tui_pad *p;
	int i;

	picker_count = win_count;
	if (picker_count > PICKER_MAX_ITEMS)
		picker_count = PICKER_MAX_ITEMS;
	for (i = 0; i < picker_count; i++)
		picker_items[i] = win_list[i];

	if (picker_count == 0)
		return;

	picker_sel = 0;
	for (i = 0; i < picker_count; i++) {
		if (picker_items[i].active) {
			picker_sel = i;
			break;
		}
	}

	picker_visible = 1;

	p = tui_stack_push(&overlay);
	if (!p)
		return;
	overlay_visible = 1;
	picker_draw();
}

static void
picker_hide(int back_to_menu)
{
	picker_visible = 0;

	if (back_to_menu && overlay_visible) {
		overlay_pop();
		if (overlay_visible) {
			menu_visible = 1;
			overlay_render();
		}
	} else {
		menu_visible = 0;
		overlay_erase_all();
	}
}

static void
picker_select(void)
{
	if (picker_sel >= 0 && picker_sel < picker_count) {
		uint32_t pid = picker_items[picker_sel].pid;

		picker_hide(0);
		micro_select_window(pid);
	} else {
		picker_hide(0);
	}
}

void
picker_input(const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
		if (picker_sel > 0)
			picker_sel--;
		else
			picker_sel = picker_count - 1;
		picker_draw();
		return;
	case TKBD_KEY_DOWN:
		if (picker_sel < picker_count - 1)
			picker_sel++;
		else
			picker_sel = 0;
		picker_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		picker_select();
		return;
	case TKBD_KEY_LEFT:
		picker_hide(1);
		return;
	case TKBD_KEY_ESC:
		picker_hide(0);
		return;
	default:
		break;
	}

	if (seq->ch >= '0' && seq->ch <= '9') {
		int target = (int)(seq->ch - '0');
		int i;

		for (i = 0; i < picker_count; i++) {
			if ((int)picker_items[i].id == target) {
				picker_sel = i;
				picker_select();
				return;
			}
		}
		return;
	}

	picker_hide(0);
}
