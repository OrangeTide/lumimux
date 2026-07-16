/* taskbar.h : taskbar template expansion */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TASKBAR_H
#define TASKBAR_H

#include <stddef.h>

struct taskbar;
struct cfg;

struct taskbar *taskbar_new(void);
void taskbar_free(struct taskbar *s);

/* set the format template (default: " ${window}:${title} ${window-list}") */
void taskbar_set_format(struct taskbar *s, const char *fmt);

/* set position: 0=bottom (default), 1=top */
void taskbar_set_position(struct taskbar *s, int top);
int taskbar_get_position(const struct taskbar *s);

/* set a variable by name. value is copied. NULL clears. */
void taskbar_set(struct taskbar *s, const char *name, const char *value);

/* add a shell command allow pattern (fnmatch). */
void taskbar_add_shell_allow(struct taskbar *s, const char *pattern);

/* load format, position, and shell_allow from config [taskbar] section. */
void taskbar_load_cfg(struct taskbar *s, const struct cfg *c);

/*
 * Expand the template into buf, padded/truncated to exactly cols
 * visible characters. Handles $(fill) alignment.
 * Returns number of bytes written (not including NUL).
 */
int taskbar_expand(struct taskbar *s, char *buf, size_t bufsz, int cols);

#endif /* TASKBAR_H */
