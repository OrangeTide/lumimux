/* cfg.h : gitconfig-style configuration file parser */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef CFG_H
#define CFG_H

struct cfg;

/* Create an empty config. */
struct cfg *cfg_new(void);

/* Free a config and all stored values. */
void cfg_free(struct cfg *c);

/*
 * Load a config file, merging into the existing config.
 * Supports:
 *   [section]            -- section header
 *   [section "subsect"]  -- subsection header
 *   key = value          -- key within current section
 *   section.key = value  -- dotted shorthand (no section header needed)
 *   # comment            -- hash comments
 *   ; comment            -- semicolon comments
 *
 * Later values for the same key overwrite earlier ones.
 * Returns 0 on success, -1 on error.
 */
int cfg_load(struct cfg *c, const char *path);

/*
 * Retrieve a value by dotted key (e.g., "core.shell", "bind.prefix.c").
 * Returns NULL if the key is not set.
 */
const char *cfg_get(const struct cfg *c, const char *key);

/*
 * Callback-style iteration. Calls fn(key, value, arg) for each entry.
 * If fn returns non-zero, iteration stops and that value is returned.
 * Returns 0 if all entries were visited.
 */
int cfg_each(const struct cfg *c,
    int (*fn)(const char *key, const char *value, void *arg), void *arg);

#endif /* CFG_H */
