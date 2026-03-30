/* str.h : string utilities */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef STR_H
#define STR_H

#include <stddef.h>

size_t str_strlcpy(char *dst, const char *src, size_t dstsize);
size_t str_strlcat(char *dst, const char *src, size_t dstsize);

#endif /* STR_H */
