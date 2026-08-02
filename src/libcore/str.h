/* str.h : string utilities */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef STR_H
#define STR_H

#include <stddef.h>

size_t str_strlcpy(char *dst, const char *src, size_t dstsize);
size_t str_strlcat(char *dst, const char *src, size_t dstsize);

/* Copy src to dst with anything a terminal would act on removed.
 *
 * Text that arrives from another client (a peer's name, for one) ends up
 * drawn into a status line. Left alone, a peer could paint the screen or
 * move the cursor by choosing what to call itself. Control characters and
 * DEL are replaced by '?'; bytes above 0x7f are kept, so a UTF-8 name
 * survives intact. Always NUL-terminates when dstsize > 0.
 *
 * Returns the length of the result.
 */
size_t str_sanitize(char *dst, const char *src, size_t dstsize);

#endif /* STR_H */
