/* apps_menu.c : quick apps submenu and app lifecycle */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "apps_menu.h"
#include "attach_ui.h"

#include "tui_menu.h"
#include "tui_term.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const struct app *apps[] = {
	&app_calc,
	&app_cal,
	&app_emoji,
	&app_dict,
};

#define APP_COUNT ((int)(sizeof(apps) / sizeof(apps[0])))

static struct tui_menu apps_menu_state;
static int apps_menu_sel;

static void
app_render_cb(struct app_ctx *ctx)
{
	(void)ctx;
	overlay_render();
}

static void
apps_menu_draw(void)
{
	struct winsize ws;
	struct tui_pad *p;
	int i;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	apps_menu_state.count = APP_COUNT;
	apps_menu_state.sel = apps_menu_sel;
	for (i = 0; i < APP_COUNT; i++) {
		snprintf(apps_menu_state.items[i].keys,
		    sizeof(apps_menu_state.items[i].keys),
		    "%s", apps[i]->key);
		snprintf(apps_menu_state.items[i].label,
		    sizeof(apps_menu_state.items[i].label),
		    "%s", apps[i]->name);
		apps_menu_state.items[i].action = i;
		apps_menu_state.items[i].flags = 0;
	}

	p = tui_stack_top(&overlay);
	if (!p) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	}

	tui_menu_draw(p, &apps_menu_state, theme, "Quick Apps", "ESC",
	    ws.ws_row, ws.ws_col);
	overlay_visible = 1;
	overlay_render();
}

void
apps_menu_show(void)
{
	struct tui_pad *p;

	apps_menu_sel = 0;
	apps_menu_visible = 1;

	p = tui_stack_push(&overlay);
	if (!p)
		return;
	overlay_visible = 1;
	apps_menu_draw();
}

static void
apps_menu_hide(int back_to_menu)
{
	apps_menu_visible = 0;

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
app_show(int index)
{
	struct winsize ws;
	struct tui_pad *p;

	if (index < 0 || index >= APP_COUNT)
		return;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	apps_menu_visible = 0;
	active_app = apps[index];

	p = tui_stack_push(&overlay);
	if (!p) {
		active_app = NULL;
		return;
	}
	overlay_visible = 1;

	app_context.stack = &overlay;
	app_context.be = tb;
	app_context.be_ctx = tui_term_ctx(tb);
	app_context.base = vt->buf;
	app_context.theme = theme;
	app_context.input_fd = mconn_focused_fd();
	app_context.screen_rows = ws.ws_row;
	app_context.screen_cols = ws.ws_col;
	app_context.pad = p;
	app_context.render = app_render_cb;

	active_app->show(&app_context);
	active_app->draw(&app_context);
	overlay_render();
}

void
app_dismiss(int back_to_apps)
{
	if (!active_app)
		return;

	active_app->hide(&app_context);
	active_app = NULL;

	if (back_to_apps && overlay_visible) {
		overlay_pop();
		if (overlay_visible) {
			apps_menu_visible = 1;
			overlay_render();
		}
	} else {
		apps_menu_visible = 0;
		menu_visible = 0;
		overlay_erase_all();
	}
}

void
apps_menu_input(const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
		if (apps_menu_sel > 0)
			apps_menu_sel--;
		else
			apps_menu_sel = APP_COUNT - 1;
		apps_menu_draw();
		return;
	case TKBD_KEY_DOWN:
		if (apps_menu_sel < APP_COUNT - 1)
			apps_menu_sel++;
		else
			apps_menu_sel = 0;
		apps_menu_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		app_show(apps_menu_sel);
		return;
	case TKBD_KEY_LEFT:
		apps_menu_hide(1);
		return;
	case TKBD_KEY_ESC:
		apps_menu_hide(0);
		return;
	default:
		break;
	}

	/* hotkey match */
	if (seq->ch > 0 && seq->ch < 0x7f) {
		int i;

		for (i = 0; i < APP_COUNT; i++) {
			if (apps[i]->key[0] == (char)seq->ch &&
			    apps[i]->key[1] == '\0') {
				app_show(i);
				return;
			}
		}
	}

	apps_menu_hide(0);
}
