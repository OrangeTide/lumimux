/* apps_menu.h : quick apps submenu and app lifecycle */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef APPS_MENU_H
#define APPS_MENU_H

struct tkbd_seq;

void apps_menu_show(void);
void apps_menu_input(const struct tkbd_seq *seq);
void app_dismiss(int back_to_apps);

#endif /* APPS_MENU_H */
