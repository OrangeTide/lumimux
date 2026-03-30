/* window.c : window management (PTY + VT state) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "window.h"
#include "pty.h"
#include "vt_state.h"
#include "vt_parse.h"
#include "vt_ops.h"
#include "rune_width.h"
#include "xmalloc.h"

#include <stdlib.h>
#include <string.h>

struct window {
	uint32_t id;
	int master_fd;
	int child_pid;
	struct vt_state *vt;
	struct vt_parse *parser;
	char *title;
};

struct window *
window_new(const char *shell, int rows, int cols)
{
	struct window *w;

	w = xcalloc(1, sizeof(*w));

	w->master_fd = pty_open(&w->child_pid, shell);
	if (w->master_fd < 0) {
		free(w);
		return NULL;
	}

	pty_resize(w->master_fd, rows, cols);

	w->vt = vt_state_new(rows, cols, 0);
	if (!w->vt) {
		pty_close(w->master_fd);
		free(w);
		return NULL;
	}
	vt_state_set_reply_fd(w->vt, w->master_fd);

	w->parser = vt_parse_new(vt_ops_default(), w->vt);
	if (!w->parser) {
		vt_state_free(w->vt);
		pty_close(w->master_fd);
		free(w);
		return NULL;
	}

	return w;
}

void
window_free(struct window *w)
{
	if (!w)
		return;
	vt_parse_free(w->parser);
	vt_state_free(w->vt);
	pty_close(w->master_fd);
	free(w->title);
	free(w);
}

int
window_master_fd(const struct window *w)
{
	return w->master_fd;
}

int
window_child_pid(const struct window *w)
{
	return w->child_pid;
}

struct vt_state *
window_vt(const struct window *w)
{
	return w->vt;
}

struct vt_parse *
window_parser(const struct window *w)
{
	return w->parser;
}

uint32_t
window_id(const struct window *w)
{
	return w->id;
}

const char *
window_title(const struct window *w)
{
	return w->title ? w->title : "";
}

void
window_set_id(struct window *w, uint32_t id)
{
	w->id = id;
}

void
window_set_title(struct window *w, const char *title)
{
	free(w->title);
	w->title = title ? xstrdup(title) : NULL;
}

void
window_feed(struct window *w, const char *data, size_t len)
{
	vt_parse_feed(w->parser, data, len);
}

int
window_resize(struct window *w, int rows, int cols)
{
	pty_resize(w->master_fd, rows, cols);
	return vt_state_resize(w->vt, rows, cols);
}

void
window_dump(struct window *w, vt_dump_fn emit, void *ctx)
{
	vt_state_dump(w->vt, emit, ctx);
}
