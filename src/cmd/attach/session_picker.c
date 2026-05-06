/* session_picker.c : session picker submenu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "session_picker.h"
#include "attach_ui.h"

#include "sessdir.h"
#include "tui_list.h"
#include "tkbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SESS_MAX 32

struct sess_info {
	char	name[64];
	int	count;		/* number of servers in session */
	int	current;	/* 1 if this is the active session */
};

int session_picker_visible;

static struct sess_info sess_items[SESS_MAX];
static int sess_count;
static int sess_sel;
static struct tui_list sess_list;

/* ---- row drawing callback ---- */

static void
sess_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	struct sess_info *items = ctx;
	struct sess_info *si = &items[index];
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	struct vt_color kfg = selected ? th->sel_key_fg : th->key_fg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;
	char countstr[16];
	int countlen, c, j;

	(void)width;

	countlen = snprintf(countstr, sizeof(countstr), "%d", si->count);
	c = col;

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* current session marker */
	tui_pad_put(p, row, c++, si->current ? '*' : ' ', kfg, bg,
	    attrs, TUI_OPAQUE);

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* session name */
	c = tui_pad_puts(p, row, c, si->name, fg, bg, attrs, TUI_OPAQUE);

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* window count in brackets */
	tui_pad_put(p, row, c++, '[', fg, bg, attrs, TUI_OPAQUE);
	for (j = countlen; j < 2; j++)
		tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
	tui_pad_puts(p, row, c, countstr, kfg, bg, attrs, TUI_OPAQUE);
	c += countlen;
	tui_pad_put(p, row, c++, ']', fg, bg, attrs, TUI_OPAQUE);

	while (c < col + width - 1)
		tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
	tui_pad_put(p, row, c, ' ', fg, bg, attrs, TUI_OPAQUE);
}

/* ---- picker UI ---- */

static void
sess_draw(void)
{
	struct winsize ws;
	struct tui_pad *p;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	sess_list.count = sess_count;
	sess_list.sel = sess_sel;

	p = tui_stack_top(&overlay);
	if (!p) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	}

	tui_list_draw(p, &sess_list, theme, sess_row_cb,
	    sess_items, "Sessions", ws.ws_row, ws.ws_col);
	overlay_visible = 1;
	overlay_render();

	sess_sel = sess_list.sel;
}

static void
sess_hide(int back_to_menu)
{
	session_picker_visible = 0;

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

void
session_picker_show(void)
{
	char *names[SESS_MAX];
	int n, i;
	const char *cur;
	struct tui_pad *p;

	n = sessdir_list_sessions(names, SESS_MAX);
	if (n <= 0)
		return;

	cur = micro_current_session();
	sess_count = 0;
	sess_sel = 0;
	for (i = 0; i < n; i++) {
		struct sess_info *si = &sess_items[sess_count];
		pid_t pids[SESS_MAX];
		int sc;
		size_t len;

		len = strlen(names[i]);
		if (len >= sizeof(si->name))
			len = sizeof(si->name) - 1;
		memcpy(si->name, names[i], len);
		si->name[len] = '\0';

		sc = sessdir_list_servers(names[i], pids, SESS_MAX);
		si->count = sc > 0 ? sc : 0;
		si->current = (cur && strcmp(names[i], cur) == 0);
		if (si->current)
			sess_sel = sess_count;
		free(names[i]);
		sess_count++;
	}

	if (sess_count == 0)
		return;

	session_picker_visible = 1;

	p = tui_stack_push(&overlay);
	if (!p)
		return;
	overlay_visible = 1;
	sess_draw();
}

static void
sess_select(struct iox_loop *loop)
{
	if (sess_sel >= 0 && sess_sel < sess_count) {
		const char *name = sess_items[sess_sel].name;

		sess_hide(0);
		if (!sess_items[sess_sel].current)
			micro_switch_session(loop, name);
	} else {
		sess_hide(0);
	}
}

void
session_picker_input(struct iox_loop *loop, const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
		if (sess_sel > 0)
			sess_sel--;
		else
			sess_sel = sess_count - 1;
		sess_draw();
		return;
	case TKBD_KEY_DOWN:
		if (sess_sel < sess_count - 1)
			sess_sel++;
		else
			sess_sel = 0;
		sess_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		sess_select(loop);
		return;
	case TKBD_KEY_LEFT:
		sess_hide(1);
		return;
	case TKBD_KEY_ESC:
		sess_hide(0);
		return;
	default:
		break;
	}

	sess_hide(0);
}
