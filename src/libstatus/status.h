/* status.h : status line template expansion */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

struct status;
struct cfg;

struct status *status_new(void);
void status_free(struct status *s);

/* set the format template (default: " ${window}:${title} ${window-list}") */
void status_set_format(struct status *s, const char *fmt);

/* set position: 0=bottom (default), 1=top */
void status_set_position(struct status *s, int top);
int status_get_position(const struct status *s);

/* set a variable by name. value is copied. NULL clears. */
void status_set(struct status *s, const char *name, const char *value);

/* add a shell command allow pattern (fnmatch). */
void status_add_shell_allow(struct status *s, const char *pattern);

/* load format, position, and shell_allow from config [status] section. */
void status_load_cfg(struct status *s, const struct cfg *c);

/*
 * Expand the template into buf, padded/truncated to exactly cols
 * visible characters. Handles $(fill) alignment.
 * Returns number of bytes written (not including NUL).
 */
int status_expand(struct status *s, char *buf, size_t bufsz, int cols);

#endif /* STATUS_H */
