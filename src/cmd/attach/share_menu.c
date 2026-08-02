/* share_menu.c : attached clients and passing the keyboard */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Lists everyone attached to the session, marks who has the keyboard, and
 * is where it changes hands. A client with the keyboard grants it; a client
 * without one asks for it.
 */

#include "share_menu.h"
#include "attach_ui.h"

#include "sessdir_control.h"
#include "tui_list.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SHARE_MAX	SESSDIR_CLIENT_MAX

int share_menu_visible;

static struct sessdir_client items[SHARE_MAX];
static int item_count;
static int item_sel;
static struct tui_list share_list;

/* ---- row drawing ---- */

static void
share_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	struct sessdir_client *list = ctx;
	struct sessdir_client *c = &list[index];
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	struct vt_color kfg = selected ? th->sel_key_fg : th->key_fg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;
	int writer = strcmp(c->role, "write") == 0;
	int self = c->client_id == share_self_id();
	int wants = c->client_id == share_pending_request();
	char tail[48];
	int c1 = col;

	tui_pad_put(p, row, c1++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* who has the keyboard, and which of these is us */
	tui_pad_put(p, row, c1++, writer ? '>' : ' ', kfg, bg, attrs,
	    TUI_OPAQUE);
	tui_pad_put(p, row, c1++, self ? '*' : ' ', kfg, bg, attrs,
	    TUI_OPAQUE);
	tui_pad_put(p, row, c1++, ' ', fg, bg, attrs, TUI_OPAQUE);

	c1 = tui_pad_puts(p, row, c1, c->name, fg, bg, attrs, TUI_OPAQUE);

	snprintf(tail, sizeof(tail), "  %s %dx%d%s",
	    writer ? "keyboard" : "watching", c->cols, c->rows,
	    wants ? "  wants the keyboard" : "");
	c1 = tui_pad_puts(p, row, c1, tail, wants ? kfg : fg, bg, attrs,
	    TUI_OPAQUE);

	while (c1 < col + width)
		tui_pad_put(p, row, c1++, ' ', fg, bg, attrs, TUI_OPAQUE);
}

/* ---- drawing and dismissal ---- */

static const char *
share_title(void)
{
	static char title[80];

	snprintf(title, sizeof(title), "Clients%s  %s",
	    share_menu_locked() ? " (locked)" : "",
	    share_self_has_keyboard() ? "enter: give keyboard, k: kick, "
	    "l: lock" : "enter: ask for keyboard, l: lock");
	return title;
}

static void
share_draw(void)
{
	struct winsize ws;
	struct tui_pad *p;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	share_list.count = item_count;
	share_list.sel = item_sel;

	p = tui_stack_top(&overlay);
	if (!p) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	}

	tui_list_draw(p, &share_list, theme, share_row_cb, items,
	    share_title(), ws.ws_row, ws.ws_col);
	overlay_visible = 1;
	overlay_render();

	item_sel = share_list.sel;
}

static void
share_hide(int back_to_menu)
{
	share_menu_visible = 0;

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

/* load the roster, putting the client with the keyboard first so the list
 * reads the way the session works */
static void
share_load(void)
{
	const char *session = micro_current_session();
	int n, i, j;

	item_count = 0;
	item_sel = 0;
	if (!session)
		return;
	n = sessdir_client_list(session, items, SHARE_MAX);
	if (n <= 0)
		return;
	item_count = n;

	for (i = 0; i < item_count; i++) {
		if (strcmp(items[i].role, "write") != 0)
			continue;
		if (i > 0) {
			struct sessdir_client tmp = items[i];

			for (j = i; j > 0; j--)
				items[j] = items[j - 1];
			items[0] = tmp;
		}
		break;
	}

	/* start on the client waiting for an answer, if there is one:
	 * that is what the menu was most likely opened for */
	for (i = 0; i < item_count; i++) {
		if (items[i].client_id == share_pending_request()) {
			item_sel = i;
			break;
		}
	}
}

void
share_menu_show(void)
{
	struct tui_pad *p;

	share_load();
	if (item_count == 0)
		return;

	share_menu_visible = 1;
	p = tui_stack_push(&overlay);
	if (!p)
		return;
	overlay_visible = 1;
	share_draw();
}

/* ---- input ---- */

static void
share_activate(void)
{
	struct sessdir_client *c;

	if (item_sel < 0 || item_sel >= item_count) {
		share_hide(0);
		return;
	}
	c = &items[item_sel];

	if (!share_self_has_keyboard()) {
		/* nothing to give away: ask for it instead */
		share_hide(0);
		share_menu_request();
		return;
	}
	if (c->client_id == share_self_id()) {
		share_hide(0);	/* we already have it */
		return;
	}
	share_hide(0);
	share_menu_grant(c->client_id, c->name);
}

void
share_menu_input(const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
		if (item_sel > 0)
			item_sel--;
		else
			item_sel = item_count - 1;
		share_draw();
		return;
	case TKBD_KEY_DOWN:
		if (item_sel < item_count - 1)
			item_sel++;
		else
			item_sel = 0;
		share_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		share_activate();
		return;
	case TKBD_KEY_LEFT:
		share_hide(1);
		return;
	case TKBD_KEY_ESC:
		share_hide(0);
		return;
	default:
		break;
	}

	switch (seq->ch) {
	case 'k':
		if (item_sel >= 0 && item_sel < item_count &&
		    items[item_sel].client_id != share_self_id()) {
			unsigned long id = items[item_sel].client_id;
			char who[64];

			snprintf(who, sizeof(who), "%s", items[item_sel].name);
			share_hide(0);
			share_menu_kick(id, who);
			return;
		}
		share_hide(0);
		return;
	case 'd':
		/* refuse the request the notice mentioned */
		if (share_pending_request()) {
			unsigned long id = share_pending_request();

			share_hide(0);
			share_menu_deny(id);
			return;
		}
		share_hide(0);
		return;
	case 'l':
		share_menu_toggle_lock();
		share_load();
		share_draw();
		return;
	case 'r':
		share_hide(0);
		share_menu_request();
		return;
	default:
		break;
	}

	share_hide(0);
}
