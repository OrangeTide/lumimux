/* picker.c : window picker submenu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "picker.h"
#include "attach_ui.h"

#include "utf8.h"
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
#define TAB_LEFT "\xe2\x95\xb1"		/* U+2571 ╱ */
#define TAB_RIGHT "\xe2\x95\xb2"	/* U+2572 ╲ */

static struct win_info win_list[WIN_INFO_MAX];
static int win_count;

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
win_list_format_status(void)
{
	char buf[1024];
	int pos = 0;
	int i, n, remain;

	for (i = 0; i < win_count; i++) {
		struct win_info *wi = &win_list[i];

		remain = (int)sizeof(buf) - pos;
		if (remain <= 1)
			break;

		if (pos > 0) {
			n = snprintf(buf + pos, (size_t)remain,
			    "\033[40m ");
			if (n > 0 && n < remain)
				pos += n;
			remain = (int)sizeof(buf) - pos;
			if (remain <= 1)
				break;
		}

		if (wi->active) {
			if (wi->title[0])
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[37;40m" TAB_LEFT
				    "\033[30;47m%u"
				    "\033[33;47m" ACTIVE_MARKER
				    "\033[30;47m%s"
				    "\033[37;40m" TAB_RIGHT,
				    wi->id, wi->title);
			else
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[37;40m" TAB_LEFT
				    "\033[30;47m%u"
				    "\033[33;47m" ACTIVE_MARKER
				    "\033[37;40m" TAB_RIGHT,
				    wi->id);
		} else {
			if (wi->title[0])
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[90;40m" TAB_LEFT
				    "\033[37;100m%u %s"
				    "\033[90;40m" TAB_RIGHT,
				    wi->id, wi->title);
			else
				n = snprintf(buf + pos, (size_t)remain,
				    "\033[90;40m" TAB_LEFT
				    "\033[37;100m%u"
				    "\033[90;40m" TAB_RIGHT,
				    wi->id);
		}

		if (n < 0 || n >= remain)
			break;
		pos += n;
	}

	buf[pos] = '\0';
	status_set(statusbar, "window-list", buf);
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
