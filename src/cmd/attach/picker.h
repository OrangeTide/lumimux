/* picker.h : window picker submenu */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PICKER_H
#define PICKER_H

#include <stdint.h>

struct tkbd_seq;

void win_list_clear(void);
void win_list_add(uint32_t id, uint32_t pid, const char *title, int active);
uint32_t win_list_pid_at(int index);
int win_list_active_id(uint32_t *out_id);
int win_list_count(void);
uint32_t win_list_id_at(int index);
const char *win_list_title_at(int index);
void win_list_set_title(uint32_t pid, const char *title);
void win_list_format_status(void);
void picker_show(void);
void picker_input(const struct tkbd_seq *seq);

#endif /* PICKER_H */
