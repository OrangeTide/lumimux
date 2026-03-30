/* tui_menu.h : vertical key+label menu widget */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_MENU_H
#define TUI_MENU_H

#include "tui_pad.h"
#include "tui_theme.h"

#define TUI_MENU_MAX 32

/* Per-item flags. */
#define TUI_MENU_SUBMENU	(1 << 0)	/* show > indicator */

struct tui_menu_item {
	char	keys[16];	/* key binding label: "c/C", "0-9" */
	char	label[24];	/* action label: "New window" */
	int	action;		/* application-defined action id */
	int	flags;		/* TUI_MENU_SUBMENU, etc. */
};

struct tui_menu {
	struct tui_menu_item	items[TUI_MENU_MAX];
	int			count;
	int			sel;	/* highlighted index */
};

void tui_menu_measure(const struct tui_menu *m, int *w, int *h);

void tui_menu_draw(struct tui_pad *p, const struct tui_menu *m,
    const struct tui_theme *theme, const char *title,
    const char *footer, int screen_rows, int screen_cols);

#endif /* TUI_MENU_H */
