/* vt_parse.h : VT escape sequence state machine parser */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef VT_PARSE_H
#define VT_PARSE_H

#include <stddef.h>
#include <stdint.h>

struct vt_parse;

/* callback vtable -- parser emits actions through these */
struct vt_ops {
	void (*print)(void *ctx, uint32_t cp, int width);
	void (*execute)(void *ctx, uint8_t c);	/* C0/C1 controls */
	void (*csi)(void *ctx, const int *params, int nparam,
	    int intermed, int final);
	void (*esc)(void *ctx, int intermed, int final);
	void (*osc)(void *ctx, const char *data, size_t len);
};

struct vt_parse *vt_parse_new(const struct vt_ops *ops, void *ctx);
void vt_parse_free(struct vt_parse *p);
void vt_parse_feed(struct vt_parse *p, const char *data, size_t len);
void vt_parse_reset(struct vt_parse *p);

/* DCS (Device Control String) passthrough callback.
 * invoked when a complete DCS sequence (ESC P ... ST) is received.
 * introducer is the ESC P byte ('P', 'X', '^', or '_').
 * data/len contain bytes between the introducer and the ST terminator. */
typedef void (*vt_parse_dcs_cb)(void *ctx, int introducer,
    const char *data, size_t len);

/* set a DCS passthrough callback with its own context.
 * pass NULL to disable. */
void vt_parse_set_dcs_cb(struct vt_parse *p, vt_parse_dcs_cb cb, void *ctx);

#endif /* VT_PARSE_H */
