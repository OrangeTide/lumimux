/* splash.h : ANSI art splash screen scenes */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef SPLASH_H
#define SPLASH_H

struct vt_buf;

enum splash_scene {
	SPLASH_SPACE,
	SPLASH_MOUNTAIN,
	SPLASH_BEACH,
	SPLASH_COUNT,
};

/* Create a splash scene canvas at its native resolution.
 * name is rendered as a pipe-art logo in the bottom-right corner. */
struct vt_buf *splash_create(enum splash_scene scene, const char *name);

/* Free a splash canvas. */
void splash_free(struct vt_buf *buf);

#endif /* SPLASH_H */
