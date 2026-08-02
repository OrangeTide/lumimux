/* share_menu.h : attached clients and passing the keyboard */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef SHARE_MENU_H
#define SHARE_MENU_H

struct tkbd_seq;

extern int share_menu_visible;

void share_menu_show(void);
void share_menu_input(const struct tkbd_seq *seq);

/*
 * Implemented in attach.c, which owns the session name, this client's id,
 * and the connections the role has to be announced on.
 */
unsigned long share_self_id(void);
int share_self_has_keyboard(void);
unsigned long share_pending_request(void);
void share_menu_grant(unsigned long target, const char *who);
void share_menu_deny(unsigned long target);
void share_menu_request(void);
void share_menu_kick(unsigned long target, const char *who);
void share_menu_toggle_lock(void);
int share_menu_locked(void);

#endif /* SHARE_MENU_H */
