/* xmalloc.h : safe memory allocation wrappers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>

void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

#endif /* XMALLOC_H */
