/* taskbar_int.h : internal interface for taskbar modules */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TASKBAR_INT_H
#define TASKBAR_INT_H

#include <stddef.h>

struct taskbar;

/* arena allocators (defined in taskbar.c) */
char *taskbar_arena_alloc(struct taskbar *s, int size);
char *taskbar_arena_strdup(struct taskbar *s, const char *str);
char *taskbar_arena_strndup(struct taskbar *s, const char *str, int n);

/* shell command allow-list check (defined in taskbar.c) */
int taskbar_shell_allowed(const struct taskbar *s, const char *cmd);

/* built-in function signature */
typedef char *(*taskbar_func_t)(struct taskbar *, const char *);

struct taskbar_func_entry {
	const char	*name;
	taskbar_func_t	 func;
};

/* function table (defined in taskbar_func.c) */
extern const struct taskbar_func_entry taskbar_func_table[];
extern const int taskbar_func_count;

#endif /* TASKBAR_INT_H */
