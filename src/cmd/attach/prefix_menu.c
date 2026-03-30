/* prefix_menu.c : guided prefix-key menu (DESQview-style) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "prefix_menu.h"
#include "attach_ui.h"
#include "picker.h"
#include "apps_menu.h"


#include "tui_menu.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MENU_MAX_ITEMS 32

struct menu_item {
	char keys[16];
	char label[24];
	enum keys_action action;
};

static struct menu_item menu_items[MENU_MAX_ITEMS];
static int menu_count;
static int menu_sel;
static int menu_acc = -1;
static struct tui_menu tui_menu_state;

static const char *
action_label(enum keys_action action)
{
	switch (action) {
	case KEYS_ACTION_SEND_PREFIX:	return "Send prefix";
	case KEYS_ACTION_NEW_WINDOW:	return "New window";
	case KEYS_ACTION_NEXT_WINDOW:	return "Next window";
	case KEYS_ACTION_PREV_WINDOW:	return "Prev window";
	case KEYS_ACTION_SELECT_0:	return "Select window";
	case KEYS_ACTION_KILL_WINDOW:	return "Kill window";
	case KEYS_ACTION_DETACH:	return "Detach";
	case KEYS_ACTION_WINDOW_LIST:	return "Window list";
	case KEYS_ACTION_STATUS_TOGGLE:	return "Toggle status";
	case KEYS_ACTION_APPS_MENU:	return "Quick Apps";
	case KEYS_ACTION_SPLIT_H:	return "Split horiz";
	case KEYS_ACTION_SPLIT_V:	return "Split vert";
	case KEYS_ACTION_NEXT_PANE:	return "Next pane";
	case KEYS_ACTION_PREV_PANE:	return "Prev pane";
	case KEYS_ACTION_CLOSE_PANE:	return "Close pane";
	case KEYS_ACTION_RESIZE_PANE:	return "Resize pane";
	case KEYS_ACTION_WINDOW_COLORS:	return "Window colors";
	default:			return NULL;
	}
}

static int
format_key_label(char *dst, int dstsz, uint8_t byte)
{
	if (byte == 0)
		return snprintf(dst, (size_t)dstsz, "C-@");
	if (byte == 0x09)
		return snprintf(dst, (size_t)dstsz, "Tab");
	if (byte == 0x0D)
		return snprintf(dst, (size_t)dstsz, "Enter");
	if (byte == 0x1B)
		return snprintf(dst, (size_t)dstsz, "Esc");
	if (byte < 0x20)
		return snprintf(dst, (size_t)dstsz, "C-%c", byte + 'a' - 1);
	if (byte == ' ')
		return snprintf(dst, (size_t)dstsz, "Space");
	if (byte == 0x7f)
		return snprintf(dst, (size_t)dstsz, "BS");
	if (byte >= 0x20 && byte < 0x7f)
		return snprintf(dst, (size_t)dstsz, "%c", byte);
	return snprintf(dst, (size_t)dstsz, "0x%02x", byte);
}

static int
menu_find_action(enum keys_action action)
{
	int i;

	for (i = 0; i < menu_count; i++) {
		if (menu_items[i].action == action)
			return i;
	}
	return -1;
}

static void
menu_build(void)
{
	int byte;
	int seen_select = 0;
	int idx;
	char kl[8];

	menu_count = 0;

	for (byte = 0; byte < 256 && menu_count < MENU_MAX_ITEMS; byte++) {
		enum keys_action action;
		const char *lbl;

		action = keys_get_binding(keybinds, (uint8_t)byte);
		if (action == KEYS_ACTION_NONE ||
		    action == KEYS_ACTION_CONSUMED)
			continue;

		if (action >= KEYS_ACTION_SELECT_0 &&
		    action <= KEYS_ACTION_SELECT_9) {
			if (!seen_select) {
				seen_select = 1;
				idx = menu_count++;
				snprintf(menu_items[idx].keys,
				    sizeof(menu_items[idx].keys), "0-9");
				snprintf(menu_items[idx].label,
				    sizeof(menu_items[idx].label),
				    "Select window");
				menu_items[idx].action =
				    KEYS_ACTION_SELECT_0;
			}
			continue;
		}

		lbl = action_label(action);
		if (!lbl)
			continue;

		idx = menu_find_action(action);
		if (idx >= 0) {
			size_t klen = strlen(menu_items[idx].keys);

			format_key_label(kl, (int)sizeof(kl),
			    (uint8_t)byte);
			if (klen + 1 + strlen(kl) <
			    sizeof(menu_items[idx].keys)) {
				menu_items[idx].keys[klen] = '/';
				strcpy(menu_items[idx].keys + klen + 1,
				    kl);
			}
			continue;
		}

		idx = menu_count++;
		format_key_label(menu_items[idx].keys,
		    (int)sizeof(menu_items[idx].keys), (uint8_t)byte);
		snprintf(menu_items[idx].label,
		    sizeof(menu_items[idx].label), "%s", lbl);
		menu_items[idx].action = action;
	}

}

static void
menu_draw(void)
{
	struct winsize ws;
	struct tui_pad *p;
	int i;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
		return;

	tui_menu_state.count = menu_count;
	tui_menu_state.sel = menu_sel;
	for (i = 0; i < menu_count; i++) {
		memcpy(tui_menu_state.items[i].keys, menu_items[i].keys,
		    sizeof(menu_items[i].keys));
		memcpy(tui_menu_state.items[i].label, menu_items[i].label,
		    sizeof(menu_items[i].label));
		tui_menu_state.items[i].action = menu_items[i].action;
		tui_menu_state.items[i].flags =
		    (menu_items[i].action == KEYS_ACTION_WINDOW_LIST ||
		    menu_items[i].action == KEYS_ACTION_SELECT_0 ||
		    menu_items[i].action == KEYS_ACTION_APPS_MENU)
		    ? TUI_MENU_SUBMENU : 0;
	}

	if (!overlay_visible) {
		p = tui_stack_push(&overlay);
		if (!p)
			return;
	} else {
		p = tui_stack_top(&overlay);
		if (!p)
			return;
	}

	tui_menu_draw(p, &tui_menu_state, theme, "lumi", "ESC",
	    ws.ws_row, ws.ws_col);
	overlay_visible = 1;
	overlay_render();
}

static void
menu_hide(void)
{
	menu_visible = 0;
	overlay_pop();
}

void
menu_show(void)
{
	menu_build();
	if (menu_count == 0)
		return;
	menu_sel = 0;
	menu_acc = -1;
	menu_visible = 1;
	menu_draw();
}

static void
menu_activate_sel(struct iox_loop *loop)
{
	enum keys_action action = menu_items[menu_sel].action;

	if (action == KEYS_ACTION_WINDOW_LIST ||
	    action == KEYS_ACTION_SELECT_0) {
		keys_reset(keybinds);
		if (menu_acc >= 0) {
			menu_hide();
			micro_select_window((uint32_t)menu_acc);
		} else {
			menu_visible = 0;
			picker_show();
		}
		return;
	}
	if (action == KEYS_ACTION_APPS_MENU) {
		keys_reset(keybinds);
		menu_visible = 0;
		apps_menu_show();
		return;
	}
	keys_reset(keybinds);
	menu_hide();
	dispatch_action(loop, action);
}

void
menu_input(struct iox_loop *loop, const struct tkbd_seq *seq)
{
	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
		if (menu_sel > 0)
			menu_sel--;
		else
			menu_sel = menu_count - 1;
		menu_draw();
		return;
	case TKBD_KEY_DOWN:
		if (menu_sel < menu_count - 1)
			menu_sel++;
		else
			menu_sel = 0;
		menu_draw();
		return;
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
		menu_activate_sel(loop);
		return;
	case TKBD_KEY_LEFT:
	case TKBD_KEY_ESC:
		keys_reset(keybinds);
		menu_hide();
		return;
	default:
		break;
	}

	/* Alt+digit accumulator for window IDs >= 10 */
	if (seq->mod & TKBD_MOD_ALT && seq->ch >= '0' && seq->ch <= '9') {
		if (menu_acc < 0)
			menu_acc = 0;
		menu_acc = menu_acc * 10 + (seq->ch - '0');
		if (menu_acc > 99999)
			menu_acc = 99999;
		return;
	}

	/* check if typed key is a bound action */
	if (seq->ch < 256) {
		enum keys_action action;

		action = keys_get_binding(keybinds, (uint8_t)seq->ch);
		if (action == KEYS_ACTION_SEND_PREFIX) {
			/* prefix key while menu is visible -- restart
			 * prefix sequence so the next key determines the
			 * action.  The main loop sees keys state PREFIX
			 * and handles the timeout. */
			menu_visible = 0;
			overlay_pop();
			keys_reset(keybinds);
			keys_feed(keybinds, (uint8_t)seq->ch);
			return;
		}
		if (action == KEYS_ACTION_WINDOW_LIST) {
			keys_reset(keybinds);
			if (menu_acc >= 0) {
				menu_hide();
				micro_select_window((uint32_t)menu_acc);
			} else {
				menu_visible = 0;
				picker_show();
			}
			return;
		}
		if (action != KEYS_ACTION_NONE) {
			keys_reset(keybinds);
			menu_hide();
			dispatch_action(loop, action);
			return;
		}
	}

	/* single-character hotkey match */
	if (seq->ch >= 0x20 && seq->ch < 0x7f) {
		int i;
		char ch = (char)seq->ch;

		for (i = 0; i < menu_count; i++) {
			if (menu_items[i].keys[0] == ch &&
			    menu_items[i].keys[1] == '\0') {
				enum keys_action action =
				    menu_items[i].action;

				keys_reset(keybinds);
				if (action == KEYS_ACTION_APPS_MENU) {
					menu_visible = 0;
					apps_menu_show();
				} else if (action ==
				    KEYS_ACTION_WINDOW_LIST ||
				    action == KEYS_ACTION_SELECT_0) {
					menu_visible = 0;
					picker_show();
				} else {
					menu_hide();
					dispatch_action(loop, action);
				}
				return;
			}
		}
	}

	keys_reset(keybinds);
	menu_hide();
}
