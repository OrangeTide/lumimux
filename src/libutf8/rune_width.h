/* rune_width.h : Unicode-version-aware character width */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef RUNE_WIDTH_H
#define RUNE_WIDTH_H

#include <stddef.h>
#include <stdint.h>

/* A closed range [first, last] of Unicode codepoints. */
struct rune_range {
	uint32_t first;
	uint32_t last;
};

/* Width tables for a specific Unicode version. */
struct rune_width_table {
	const char *version;
	const struct rune_range *zero;
	size_t zero_count;
	const struct rune_range *wide;
	size_t wide_count;
};

/** Initialize the width lookup from UNICODE_VERSION env var.
 *
 * Call once at startup. If UNICODE_VERSION is unset or unrecognized,
 * the latest built-in table is used. Returns 0 on success, -1 if
 * the requested version was not found (falls back to latest).
 */
int rune_width_init(void);

/** Select a specific Unicode version by string (e.g. "15.1.0").
 *
 * Returns 0 on exact match, -1 if not found (table unchanged).
 */
int rune_width_set(const char *version);

/** Return the display width of a codepoint: 0, 1, or 2.
 *
 * Returns -1 for non-printable control characters (C0/C1 except
 * U+0000 which returns 0).
 */
int rune_width(uint32_t cp);

/** Return the currently selected Unicode version string. */
const char *rune_width_version(void);

/** Return the list of built-in table versions.
 *
 * Stores the count in *count and returns a pointer to the array.
 */
const struct rune_width_table *rune_width_tables(size_t *count);

#endif /* RUNE_WIDTH_H */
