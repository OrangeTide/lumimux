/* app.h : client-side application overlay vtable */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef APP_H
#define APP_H

#include "tui_pad.h"
#include "tui_theme.h"
#include "vt_buf.h"

#include <stdint.h>

struct tkbd_seq;

struct app_ctx {
	struct tui_pad		*pad;
	struct tui_stack	*stack;
	const struct tui_backend *be;
	void			*be_ctx;
	struct vt_buf		*base;
	const struct tui_theme	*theme;
	int			input_fd;
	int			screen_rows;
	int			screen_cols;
	void			(*render)(struct app_ctx *ctx);
};

struct app {
	const char	*name;
	const char	*key;		/* hotkey label in apps menu */
	void		(*show)(struct app_ctx *ctx);
	void		(*hide)(struct app_ctx *ctx);
	void		(*draw)(struct app_ctx *ctx);
	int		(*input)(struct app_ctx *ctx,
			    const struct tkbd_seq *seq);
};

/* app descriptors (defined in app_*.c files) */
extern const struct app app_calc;
extern const struct app app_cal;
extern const struct app app_emoji;
extern const struct app app_dict;

#endif /* APP_H */
