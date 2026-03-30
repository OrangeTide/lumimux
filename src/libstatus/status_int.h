/* status_int.h : internal interface for status line modules */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef STATUS_INT_H
#define STATUS_INT_H

#include <stddef.h>

struct status;

/* arena allocators (defined in status.c) */
char *status_arena_alloc(struct status *s, int size);
char *status_arena_strdup(struct status *s, const char *str);
char *status_arena_strndup(struct status *s, const char *str, int n);

/* shell command allow-list check (defined in status.c) */
int status_shell_allowed(const struct status *s, const char *cmd);

/* built-in function signature */
typedef char *(*status_func_t)(struct status *, const char *);

struct status_func_entry {
	const char	*name;
	status_func_t	 func;
};

/* function table (defined in status_func.c) */
extern const struct status_func_entry status_func_table[];
extern const int status_func_count;

#endif /* STATUS_INT_H */
