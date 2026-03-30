/* theme_cfg.h : user-defined theme loading from config */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef THEME_CFG_H
#define THEME_CFG_H

struct cfg;
struct tui_theme;

void theme_load_cfg(const struct cfg *c);
const struct tui_theme *theme_find(const char *name);

#endif /* THEME_CFG_H */
