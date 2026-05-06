/* attach_ui.c : shared UI state and overlay helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "attach_ui.h"

#include "tui_term.h"
#include "tio_write.h"

#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- shared state ---- */

enum client_mode client_mode;
struct vt_state *vt;
struct txl *txl;
struct keys *keybinds;
struct status *statusbar;
const struct tui_theme *theme;
int status_visible = 1;
int content_rows, content_cols;

struct tui_stack overlay;
struct tui_backend *tb;
int overlay_visible;

int menu_visible;
int picker_visible;
int apps_menu_visible;
const struct app *active_app;
struct app_ctx app_context;

void (*overlay_repaint_fn)(void);

/* ---- overlay helpers ---- */

static int prev_ovr_r1 = -1, prev_ovr_c1, prev_ovr_r2, prev_ovr_c2;

static void
overlay_bbox(int *r1, int *c1, int *r2, int *c2)
{
	int i;

	*r1 = 9999;
	*c1 = 9999;
	*r2 = 0;
	*c2 = 0;
	for (i = 0; i < overlay.depth; i++) {
		const struct tui_pad *p = &overlay.layers[i];

		if (p->screen_row < *r1)
			*r1 = p->screen_row;
		if (p->screen_col < *c1)
			*c1 = p->screen_col;
		if (p->screen_row + p->h > *r2)
			*r2 = p->screen_row + p->h;
		if (p->screen_col + p->w > *c2)
			*c2 = p->screen_col + p->w;
	}
}

static void
overlay_merge_prev(int *r1, int *c1, int *r2, int *c2)
{
	if (prev_ovr_r1 >= 0) {
		if (prev_ovr_r1 < *r1) *r1 = prev_ovr_r1;
		if (prev_ovr_c1 < *c1) *c1 = prev_ovr_c1;
		if (prev_ovr_r2 > *r2) *r2 = prev_ovr_r2;
		if (prev_ovr_c2 > *c2) *c2 = prev_ovr_c2;
	}
	prev_ovr_r1 = -1;
}

static void
overlay_repaint_status(void)
{
	struct winsize ws;

	if (status_visible && ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
		render_status_line(STDOUT_FILENO, ws.ws_row, ws.ws_col);
}

void
overlay_render(void)
{
	char cup[24];
	int len;
	int r1, c1, r2, c2;

	if (!vt)
		return;

	if (prev_ovr_r1 >= 0) {
		if (overlay_repaint_fn)
			overlay_repaint_fn();
		else
			tui_stack_erase(&overlay, vt->buf, tb,
			    tui_term_ctx(tb),
			    prev_ovr_r1, prev_ovr_c1,
			    prev_ovr_r2 - prev_ovr_r1,
			    prev_ovr_c2 - prev_ovr_c1);
	}

	tui_stack_render(&overlay, vt->buf, tb, tui_term_ctx(tb));

	overlay_bbox(&r1, &c1, &r2, &c2);
	prev_ovr_r1 = r1;
	prev_ovr_c1 = c1;
	prev_ovr_r2 = r2;
	prev_ovr_c2 = c2;

	len = txl_cup(txl, cup, sizeof(cup),
	    vt->cursor_row, vt->cursor_col);
	if (len > 0) {
		tio_write(STDOUT_FILENO, cup, (size_t)len);
		tio_flush(STDOUT_FILENO);
	}
}

void
overlay_pop(void)
{
	int r1, c1, r2, c2;

	if (overlay.depth == 0)
		return;

	overlay_bbox(&r1, &c1, &r2, &c2);
	tui_stack_pop(&overlay);
	overlay_visible = tui_stack_depth(&overlay) > 0;
	overlay_merge_prev(&r1, &c1, &r2, &c2);

	if (overlay_repaint_fn) {
		/* turbo mode: vt->buf is a single window's buffer, not
		 * the composited screen.  trigger a full recomposite
		 * instead of erasing from the wrong buffer. */
		overlay_repaint_fn();
	} else if (vt) {
		tui_stack_erase(&overlay, vt->buf, tb, tui_term_ctx(tb),
		    r1, c1, r2 - r1, c2 - c1);
	}

	if (!overlay_visible && !overlay_repaint_fn)
		overlay_repaint_status();
}

void
overlay_erase_all(void)
{
	int r1, c1, r2, c2;

	if (overlay.depth == 0)
		return;

	overlay_bbox(&r1, &c1, &r2, &c2);
	overlay_merge_prev(&r1, &c1, &r2, &c2);

	overlay.depth = 0;
	overlay_visible = 0;

	if (overlay_repaint_fn) {
		overlay_repaint_fn();
	} else if (vt) {
		tui_stack_erase(&overlay, vt->buf, tb, tui_term_ctx(tb),
		    r1, c1, r2 - r1, c2 - c1);
	}

	if (!overlay_repaint_fn)
		overlay_repaint_status();
}

/* ---- status line ---- */

void
render_status_line(int fd, int rows, int cols)
{
	static char last_content[1024];
	static int last_row;
	static int last_cols;
	char buf[1536];
	char content[1024];
	int pos = 0;
	int row;
	int slen, max;
	const char *s;
	char cup[24];
	int cuplen;

	if (!status_visible)
		return;

	row = status_get_position(statusbar) ? 1 : rows;

	max = (row == rows) ? cols - 1 : cols;
	status_expand(statusbar, content, sizeof(content), max);

	/* skip if nothing changed since last draw */
	if (row == last_row && cols == last_cols &&
	    strcmp(content, last_content) == 0)
		return;
	memcpy(last_content, content, sizeof(last_content));
	last_row = row;
	last_cols = cols;

	s = txl_str(txl, TXL_SC);
	if (s) {
		slen = (int)strlen(s);
		memcpy(buf + pos, s, (size_t)slen);
		pos += slen;
	}

	cuplen = txl_cup(txl, cup, sizeof(cup), row - 1, 0);
	if (cuplen > 0) {
		memcpy(buf + pos, cup, (size_t)cuplen);
		pos += cuplen;
	}

	{
		char bg[24];
		int bglen = txl_setab(txl, bg, sizeof(bg), 0);

		if (bglen > 0 && pos + bglen < (int)sizeof(buf)) {
			memcpy(buf + pos, bg, (size_t)bglen);
			pos += bglen;
		}
	}

	slen = (int)strlen(content);
	if (pos + slen < (int)sizeof(buf) - 64) {
		memcpy(buf + pos, content, (size_t)slen);
		pos += slen;
	}

	s = txl_str(txl, TXL_SGR0);
	if (s) {
		slen = (int)strlen(s);
		if (pos + slen < (int)sizeof(buf)) {
			memcpy(buf + pos, s, (size_t)slen);
			pos += slen;
		}
	}

	{
		char bg[24];
		int bglen = txl_setab(txl, bg, sizeof(bg), 0);

		if (bglen > 0 && pos + bglen < (int)sizeof(buf)) {
			memcpy(buf + pos, bg, (size_t)bglen);
			pos += bglen;
		}
	}

	if (pos + 3 < (int)sizeof(buf)) {
		buf[pos++] = '\033';
		buf[pos++] = '[';
		buf[pos++] = 'K';
	}

	s = txl_str(txl, TXL_SGR0);
	if (s) {
		slen = (int)strlen(s);
		memcpy(buf + pos, s, (size_t)slen);
		pos += slen;
	}

	s = txl_str(txl, TXL_RC);
	if (s) {
		slen = (int)strlen(s);
		memcpy(buf + pos, s, (size_t)slen);
		pos += slen;
	}

	tio_write(fd, buf, (size_t)pos);
	tio_flush(fd);
}
