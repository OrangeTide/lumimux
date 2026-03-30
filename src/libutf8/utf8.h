/* utf8.h : UTF-8 encoding and decoding */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Maximum bytes per UTF-8 encoded codepoint (RFC 3629: 4 bytes). */
#define UTF8_MAX 4

/* Replacement character returned on decode errors. */
#define UTF8_RUNE_ERROR 0xFFFDu

/* Maximum valid Unicode codepoint. */
#define UTF8_RUNE_MAX 0x10FFFFu

/** Decode one codepoint from a UTF-8 byte sequence.
 *
 * Reads up to len bytes from s, stores the codepoint in *rune, and
 * returns the number of bytes consumed (1-4). On invalid or incomplete
 * input, stores UTF8_RUNE_ERROR in *rune and returns 1 (to advance
 * past the bad byte).
 */
int utf8_decode(uint32_t *rune, const unsigned char *s, size_t len);

/** Encode one codepoint as UTF-8.
 *
 * Writes up to UTF8_MAX bytes into buf and returns the number of bytes
 * written (1-4). Returns 0 if the codepoint is invalid.
 */
int utf8_encode(unsigned char *buf, uint32_t rune);

/** Count bytes needed to encode a codepoint as UTF-8.
 *
 * Returns 1-4 for valid codepoints, 0 for invalid ones.
 */
int utf8_runelen(uint32_t rune);

#endif /* UTF8_H */
