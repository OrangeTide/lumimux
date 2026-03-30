/* color_picker.h : per-window color customization UI */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef COLOR_PICKER_H
#define COLOR_PICKER_H

#include <stdint.h>

struct tkbd_seq;

void color_picker_show(uint32_t window_id);
void color_picker_input(const struct tkbd_seq *seq);

#endif /* COLOR_PICKER_H */
