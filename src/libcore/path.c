/* path.c : PATH-like search for sub-command executables */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *
path_search(const char *dirs, const char *prog)
{
	const char *p, *end;
	char buf[4096];

	if (!dirs || !prog)
		return NULL;

	p = dirs;
	while (*p) {
		end = strchr(p, ':');
		if (!end)
			end = p + strlen(p);

		if (end - p > 0) {
			int n = snprintf(buf, sizeof(buf), "%.*s/%s",
			                 (int)(end - p), p, prog);
			if (n > 0 && (size_t)n < sizeof(buf)) {
				if (access(buf, X_OK) == 0)
					return strdup(buf);
			}
		}

		p = *end ? end + 1 : end;
	}

	return NULL;
}
