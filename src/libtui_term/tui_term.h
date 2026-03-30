/* tui_term.h : terminal backend for libtui */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TUI_TERM_H
#define TUI_TERM_H

#include "tui_pad.h"

struct txl;

struct tui_backend *tui_term_new(struct txl *txl, int fd);
void tui_term_free(struct tui_backend *be);
void *tui_term_ctx(struct tui_backend *be);

#endif /* TUI_TERM_H */
