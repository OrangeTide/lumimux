/* session_picker.h : session picker submenu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SESSION_PICKER_H
#define SESSION_PICKER_H

struct iox_loop;
struct tkbd_seq;

void session_picker_show(void);
void session_picker_input(struct iox_loop *loop, const struct tkbd_seq *seq);

#endif /* SESSION_PICKER_H */
