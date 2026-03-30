/* xmalloc.c : safe memory allocation wrappers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *
xmalloc(size_t size)
{
	void *p = malloc(size);

	if (!p) {
		fprintf(stderr, "xmalloc: out of memory (%zu bytes)\n",
		        size);
		abort();
	}
	return p;
}

void *
xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);

	if (!p) {
		fprintf(stderr, "xcalloc: out of memory (%zu * %zu)\n",
		        nmemb, size);
		abort();
	}
	return p;
}

void *
xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);

	if (!p && size) {
		fprintf(stderr, "xrealloc: out of memory (%zu bytes)\n",
		        size);
		abort();
	}
	return p;
}

char *
xstrdup(const char *s)
{
	char *p = strdup(s);

	if (!p) {
		fprintf(stderr, "xstrdup: out of memory\n");
		abort();
	}
	return p;
}
