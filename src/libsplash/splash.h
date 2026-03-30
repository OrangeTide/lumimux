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

/* Display the splash, cropped to term_rows x term_cols.
 * The logo (bottom-right) is always visible; overflow scenery is
 * trimmed from the top-left.  Hides the cursor. */
int splash_show(struct vt_buf *buf, int fd, int term_rows, int term_cols);

/* Free a splash canvas. */
void splash_free(struct vt_buf *buf);

#endif /* SPLASH_H */
