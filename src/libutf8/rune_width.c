/* rune_width.c : Unicode-version-aware character width */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "rune_width.h"

#include <stdlib.h>
#include <string.h>

/* Defined in width_tables.c */
extern const struct rune_width_table rune_width_builtin[];
extern const size_t rune_width_builtin_count;

static const struct rune_width_table *current;

/** Binary search for cp in a sorted array of closed ranges. */
static int
range_search(const struct rune_range *table, size_t count, uint32_t cp)
{
	size_t lo = 0, hi = count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (cp > table[mid].last)
			lo = mid + 1;
		else if (cp < table[mid].first)
			hi = mid;
		else
			return 1;
	}
	return 0;
}

static const struct rune_width_table *
find_table(const char *version)
{
	for (size_t i = 0; i < rune_width_builtin_count; i++) {
		if (strcmp(rune_width_builtin[i].version, version) == 0)
			return &rune_width_builtin[i];
	}
	return NULL;
}

static const struct rune_width_table *
latest_table(void)
{
	if (rune_width_builtin_count == 0)
		return NULL;
	return &rune_width_builtin[rune_width_builtin_count - 1];
}

int
rune_width_init(void)
{
	const char *env = getenv("UNICODE_VERSION");

	if (env && *env) {
		const struct rune_width_table *t = find_table(env);
		if (t) {
			current = t;
			return 0;
		}
		/* Requested version not found -- fall back to latest */
		current = latest_table();
		return -1;
	}

	current = latest_table();
	return 0;
}

int
rune_width_set(const char *version)
{
	const struct rune_width_table *t = find_table(version);

	if (!t)
		return -1;
	current = t;
	return 0;
}

int
rune_width(uint32_t cp)
{
	/* Ensure tables are initialized */
	if (!current)
		rune_width_init();

	/* C0 control characters */
	if (cp == 0)
		return 0;
	if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
		return -1;

	/* Check zero-width ranges (combining marks, format chars, etc.) */
	if (current->zero &&
	    range_search(current->zero, current->zero_count, cp))
		return 0;

	/* Check wide/fullwidth ranges (CJK, Hangul, etc.) */
	if (current->wide &&
	    range_search(current->wide, current->wide_count, cp))
		return 2;

	/* Default: single-width */
	return 1;
}

const char *
rune_width_version(void)
{
	if (!current)
		rune_width_init();
	return current ? current->version : "unknown";
}

const struct rune_width_table *
rune_width_tables(size_t *count)
{
	*count = rune_width_builtin_count;
	return rune_width_builtin;
}
