/* dbg.h : file-based debug tracing (LUMI_DEBUG=path) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef DBG_H
#define DBG_H

#include <stddef.h>

void dbg_init(void);
void dbg_close(void);
int dbg_enabled(void);
void dbg_trace(const char *fmt, ...);
void dbg_hexdump(const char *label, const void *data, size_t len);

#endif /* DBG_H */
