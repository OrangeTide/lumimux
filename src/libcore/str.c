/* str.c : string utilities */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "str.h"

#include <string.h>

size_t
str_strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t srclen = strlen(src);

	if (dstsize > 0) {
		size_t copylen = srclen < dstsize - 1 ? srclen : dstsize - 1;
		memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}
	return srclen;
}

size_t
str_strlcat(char *dst, const char *src, size_t dstsize)
{
	size_t dstlen = strlen(dst);
	size_t srclen = strlen(src);

	if (dstlen < dstsize) {
		size_t remain = dstsize - dstlen;
		size_t copylen = srclen < remain - 1 ? srclen : remain - 1;
		memcpy(dst + dstlen, src, copylen);
		dst[dstlen + copylen] = '\0';
	}
	return dstlen + srclen;
}

size_t
str_sanitize(char *dst, const char *src, size_t dstsize)
{
	size_t n = 0;

	if (dstsize == 0)
		return 0;
	for (; src && *src && n + 1 < dstsize; src++, n++) {
		unsigned char c = (unsigned char)*src;

		/* C0 controls, DEL, and the C1 range a terminal may act on */
		if (c < 0x20 || c == 0x7f)
			dst[n] = '?';
		else
			dst[n] = (char)c;
	}
	dst[n] = '\0';
	return n;
}
