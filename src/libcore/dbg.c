/* dbg.c : file-based debug tracing (LUMI_DEBUG=path) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "dbg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *dbg_fp;

void
dbg_init(void)
{
	const char *path;

	path = getenv("LUMI_DEBUG");
	if (!path || !*path)
		return;
	dbg_fp = fopen(path, "a");
}

void
dbg_close(void)
{
	if (dbg_fp) {
		fclose(dbg_fp);
		dbg_fp = NULL;
	}
}

int
dbg_enabled(void)
{
	return dbg_fp != NULL;
}

void
dbg_trace(const char *fmt, ...)
{
	va_list ap;

	if (!dbg_fp)
		return;
	va_start(ap, fmt);
	vfprintf(dbg_fp, fmt, ap);
	va_end(ap);
	fputc('\n', dbg_fp);
	fflush(dbg_fp);
}

void
dbg_hexdump(const char *label, const void *data, size_t len)
{
	const unsigned char *p = data;
	size_t i;

	if (!dbg_fp)
		return;
	fprintf(dbg_fp, "%s (%zu bytes):", label, len);
	for (i = 0; i < len && i < 256; i++)
		fprintf(dbg_fp, " %02x", p[i]);
	if (len > 256)
		fprintf(dbg_fp, " ...");
	fputc('\n', dbg_fp);
	fflush(dbg_fp);
}
