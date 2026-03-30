/* attach_ui.h : shared UI state for attach subcommand */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef ATTACH_UI_H
#define ATTACH_UI_H

#include "app.h"
#include "keys.h"
#include "status.h"
#include "tui_pad.h"
#include "tui_theme.h"
#include "txl.h"
#include "vt_state.h"

/* sentinel for tkbd_seq.ch: "no character present" (e.g. arrow keys).
 * set before calling tkbd_parse so ch==0 (Ctrl-Space) is distinguishable
 * from "key produced no character". */
#define TKBD_CH_NONE	0x7FFFFFFFU

struct iox_loop;

/* UI mode */
enum client_mode {
	CLIENT_MODE_SCREEN,	/* GNU Screen-like single window */
	CLIENT_MODE_TURBO,	/* Turbo Vision overlapping windows */
	CLIENT_MODE_MINIMAL,	/* bare passthrough, no status/mouse */
};

/* core subsystem handles */
extern enum client_mode client_mode;
extern struct vt_state *vt;
extern struct txl *txl;
extern struct keys *keybinds;
extern struct status *statusbar;
extern const struct tui_theme *theme;
extern int status_visible;
extern int content_rows, content_cols;

/* overlay system */
extern struct tui_stack overlay;
extern struct tui_backend *tb;
extern int overlay_visible;

/* component visibility (cross-referenced for back-navigation) */
extern int menu_visible;
extern int picker_visible;
extern int apps_menu_visible;
extern int color_picker_visible;
extern const struct app *active_app;
extern struct app_ctx app_context;

void overlay_render(void);
void overlay_pop(void);
void overlay_erase_all(void);

/* callback for full-screen repaint after overlay dismiss (turbo mode).
 * set by attach.c so overlay_pop can trigger recomposite instead of
 * erasing from vt->buf (which is wrong in turbo mode -- vt->buf is a
 * single window's buffer, not the composited screen). */
extern void (*overlay_repaint_fn)(void);

void render_status_line(int fd, int rows, int cols);

int mconn_focused_fd(void);
void micro_select_window(uint32_t id);
void dispatch_action(struct iox_loop *loop, enum keys_action action);

/* per-window theme helpers (implemented in attach.c, used by color_picker) */
void color_picker_set_theme(uint32_t id, const struct tui_theme *t);
const struct tui_theme *color_picker_get_theme(uint32_t id);

#endif /* ATTACH_UI_H */
