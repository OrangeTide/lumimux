/* prefix_menu.h : guided prefix-key menu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PREFIX_MENU_H
#define PREFIX_MENU_H

struct iox_loop;
struct tkbd_seq;

void menu_show(void);
void menu_input(struct iox_loop *loop, const struct tkbd_seq *seq);

#endif /* PREFIX_MENU_H */
