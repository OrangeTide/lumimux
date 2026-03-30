/* utf8.c : UTF-8 encoding and decoding */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "utf8.h"

int
utf8_decode(uint32_t *rune, const unsigned char *s, size_t len)
{
	uint32_t v;
	int need;

	if (len == 0) {
		*rune = UTF8_RUNE_ERROR;
		return 0;
	}

	/* Single byte (ASCII) */
	if (s[0] < 0x80) {
		*rune = s[0];
		return 1;
	}

	/* Determine expected sequence length from lead byte */
	if ((s[0] & 0xE0) == 0xC0) {
		v = s[0] & 0x1F;
		need = 2;
	} else if ((s[0] & 0xF0) == 0xE0) {
		v = s[0] & 0x0F;
		need = 3;
	} else if ((s[0] & 0xF8) == 0xF0) {
		v = s[0] & 0x07;
		need = 4;
	} else {
		/* Invalid lead byte (continuation or 0xFE/0xFF) */
		*rune = UTF8_RUNE_ERROR;
		return 1;
	}

	if ((size_t)need > len) {
		*rune = UTF8_RUNE_ERROR;
		return 1;
	}

	/* Read continuation bytes */
	for (int i = 1; i < need; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*rune = UTF8_RUNE_ERROR;
			return 1;
		}
		v = (v << 6) | (s[i] & 0x3F);
	}

	/* Reject overlong encodings */
	if (need == 2 && v < 0x80)
		goto bad;
	if (need == 3 && v < 0x800)
		goto bad;
	if (need == 4 && v < 0x10000)
		goto bad;

	/* Reject surrogates (U+D800..U+DFFF) and out-of-range */
	if (v >= 0xD800 && v <= 0xDFFF)
		goto bad;
	if (v > UTF8_RUNE_MAX)
		goto bad;

	*rune = v;
	return need;

bad:
	*rune = UTF8_RUNE_ERROR;
	return 1;
}

int
utf8_encode(unsigned char *buf, uint32_t rune)
{
	if (rune <= 0x7F) {
		buf[0] = rune;
		return 1;
	}
	if (rune <= 0x7FF) {
		buf[0] = 0xC0 | (rune >> 6);
		buf[1] = 0x80 | (rune & 0x3F);
		return 2;
	}
	if (rune <= 0xFFFF) {
		/* Reject surrogates */
		if (rune >= 0xD800 && rune <= 0xDFFF)
			return 0;
		buf[0] = 0xE0 | (rune >> 12);
		buf[1] = 0x80 | ((rune >> 6) & 0x3F);
		buf[2] = 0x80 | (rune & 0x3F);
		return 3;
	}
	if (rune <= UTF8_RUNE_MAX) {
		buf[0] = 0xF0 | (rune >> 18);
		buf[1] = 0x80 | ((rune >> 12) & 0x3F);
		buf[2] = 0x80 | ((rune >> 6) & 0x3F);
		buf[3] = 0x80 | (rune & 0x3F);
		return 4;
	}
	return 0;
}

int
utf8_runelen(uint32_t rune)
{
	if (rune <= 0x7F)
		return 1;
	if (rune <= 0x7FF)
		return 2;
	if (rune <= 0xFFFF) {
		if (rune >= 0xD800 && rune <= 0xDFFF)
			return 0;
		return 3;
	}
	if (rune <= UTF8_RUNE_MAX)
		return 4;
	return 0;
}
