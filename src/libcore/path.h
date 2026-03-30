/* path.h : PATH-like search for sub-command executables */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PATH_H
#define PATH_H

/** Search colon-separated directory list for an executable named prog.
 *
 * Returns a malloc'd absolute path on success, NULL if not found.
 * The caller must free the returned string.
 */
char *path_search(const char *dirs, const char *prog);

#endif /* PATH_H */
