/* attach.c : connect to server and relay terminal I/O */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "attach_ui.h"
#include "multicall.h"
#include "lu_umask.h"
#include "prefix_menu.h"
#include "picker.h"
#include "session_picker.h"
#include "apps_menu.h"
#include "color_picker.h"
#include "selection.h"
#include "theme_cfg.h"

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"
#include "ipc.h"
#include "ipc_msg.h"
#include "ipc_transport.h"
#include "ipc_transport_netchan.h"
#include "nc_udp.h"
#include "lumi_msg.h"
#include "proxy_msg.h"
#include "byte_order.h"
#include "sessdir.h"
#include "sessdir_layout.h"
#include "sessdir_state.h"
#include "sessdir_watch.h"
#include "tio.h"
#include "tio_write.h"
#include "vt_parse.h"
#include "vt_ops.h"
#include "render.h"
#include "rune_width.h"
#include "utf8.h"
#include "tui_term.h"
#include "predict.h"
#include "cfg.h"
#include "log.h"
#include "dbg.h"
#include "tkbd.h"
#include "wm.h"
#include "tile.h"
#include "xmalloc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <time.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static struct render *renderer;
#define CLIENT_WIN_MAX 32

static uint32_t watched_id;	/* window we're subscribed to */
static int watching;		/* nonzero if subscribed */

/* ---- micro-server connection table ---- */

struct mconn {
	int		fd;		/* raw fd, kept for the event loop */
	struct ipc_transport *t;	/* carries messages; NULL when remote */
	pid_t		pid;		/* = window ID */
	int		num;		/* stable window number, -1 if unknown */
	char		title[128];
};

static struct mconn mconns[CLIENT_WIN_MAX];
static int mconn_count;
static int sessdir_watch_fd = -1;
int sessdir_watch_degraded;	/* nonzero: sessdir watch unavailable, so
				 * new windows are not auto-discovered live */
static char *session_name;	/* set in main for micro mode (allocated) */

/* ---- remote proxy state ---- */

static int is_remote;
static int rproxy_fd = -1;
static pid_t rproxy_pid;

/* netchan remote carrier: an alternative to the SSH pipe (rproxy_fd).
 * when remote_netchan is set, the proxy envelope rides an ipc_transport
 * (rnet) over UDP instead of proxy_msg over a raw fd, and a re-armed
 * one-shot timer services netchan while the link is idle. */
static struct ipc_transport *rnet;
static int remote_netchan;
static int net_service_timer = -1;
static int net_registered_fd = -1;	/* rnet's fd currently in the iox loop */
static uint32_t net_last_roam_ms;	/* rate-limit automatic roams */
#define NET_SERVICE_MS	100
#define NET_ROAM_COOLDOWN_MS	1000

/* Returns 1 if name is remote (scp-style), 0 if local.
 * On remote: *user may be NULL, *host and *rsession are set.
 * Pointers into the original string -- caller must not free them. */
static int
parse_session_name(const char *name, const char **user,
    const char **host, const char **rsession)
{
	const char *colon, *at;

	*user = NULL;
	*host = NULL;
	*rsession = NULL;

	/* leading / or . -> local path, not remote */
	if (name[0] == '/' || name[0] == '.')
		return 0;

	colon = strchr(name, ':');
	if (!colon)
		return 0;

	at = strchr(name, '@');
	if (at && at < colon) {
		/* user@host:session */
		static char ubuf[128], hbuf[256];
		size_t ulen = (size_t)(at - name);
		size_t hlen = (size_t)(colon - at - 1);

		if (ulen >= sizeof(ubuf) || hlen >= sizeof(hbuf))
			return 0;
		memcpy(ubuf, name, ulen);
		ubuf[ulen] = '\0';
		memcpy(hbuf, at + 1, hlen);
		hbuf[hlen] = '\0';
		*user = ubuf;
		*host = hbuf;
	} else {
		/* host:session */
		static char hbuf[256];
		size_t hlen = (size_t)(colon - name);

		if (hlen >= sizeof(hbuf))
			return 0;
		memcpy(hbuf, name, hlen);
		hbuf[hlen] = '\0';
		*host = hbuf;
	}

	*rsession = colon[1] ? colon + 1 : "0";
	return 1;
}

/* ---- tiled mode state ---- */

static struct tile *tilemgr;		/* NULL when not tiled */
static struct wm *wmgr;			/* turbo layout, NULL when not turbo */
static int tile_need_full;		/* next render should be full redraw */
static int resize_mode;			/* 1 = hjkl resize mode active */
static int altscreen_scrollback;	/* capture alt-screen into scrollback */
static int scrollback_mode;		/* 1 = viewing scrollback buffer */
static int scrollback_offset;		/* lines scrolled back (0 = live) */
static uint32_t scrollback_win_id;	/* window we entered scrollback on */
static struct taskbar *scrollback_sb;	/* taskbar for scrollback mode */

/* ---- keyboard copy mode (within scrollback) ---- */
static int copy_cur_row, copy_cur_col;	/* copy cursor, screen coords */
static int copy_anchor_row, copy_anchor_col; /* selection mark, screen coords */
static int copy_selecting;		/* 1 = mark set, extending selection */
static enum sel_mode copy_gran;		/* char/line/block granularity */

/* ---- command input line (Ctrl-A :) ---- */
static int cmdline_visible;		/* 1 = command line prompt active */
static char cmd_buf[512];		/* current input text */
static size_t cmd_len;			/* bytes in cmd_buf */
static char cmd_msg[128];		/* transient result/error message */
static int cmdline_row(void);
static void cmdline_render(void);
static void paste_cancel(void);

static int need_render;			/* screen needs repaint before poll */
static int need_taskbar;			/* taskbar needs redraw */

/* ---- mouse selection state ---- */

static int sel_pending;			/* left press, waiting for drag */
static int sel_press_row, sel_press_col;
static uint32_t sel_press_id;
static struct tkbd_seq sel_press_seq;	/* saved press for click-through */

/* double/triple click tracking */
static int sel_click_count;
static int sel_click_row, sel_click_col;
static uint32_t sel_click_id;
static int sel_dblclick_timer_id = -1;

static struct mconn *
mconn_find_by_fd(int fd)
{
	int i;

	for (i = 0; i < mconn_count; i++) {
		if (mconns[i].fd == fd)
			return &mconns[i];
	}
	return NULL;
}

static struct mconn *
mconn_find_by_pid(pid_t pid)
{
	int i;

	for (i = 0; i < mconn_count; i++) {
		if (mconns[i].pid == pid)
			return &mconns[i];
	}
	return NULL;
}

/* forward declarations for micro-server callbacks */
static void on_mserver_read(struct iox_loop *lp, int fd,
    unsigned events, void *arg);
static int turbo_focused_id(uint32_t *out_id);
static int mconn_ipc_send(struct mconn *mc, uint32_t type,
    const void *payload, uint32_t len);
static int mconn_ipc_send_empty(struct mconn *mc, uint32_t type);
static int mconn_ipc_send_size(struct mconn *mc, uint32_t type,
    int rows, int cols);

static struct mconn *
mconn_add(struct iox_loop *lp, pid_t pid, int fd, const char *title)
{
	struct mconn *mc;

	if (mconn_count >= CLIENT_WIN_MAX)
		return NULL;
	mc = &mconns[mconn_count++];
	mc->fd = fd;
	/* wrap the connected fd in a transport for message I/O.  remote
	 * windows carry fd = -1 (multiplexed over the proxy) and have no
	 * per-window transport. */
	mc->t = (fd >= 0) ? ipc_transport_unix_new(fd) : NULL;
	mc->pid = pid;
	mc->num = -1;
	if (title) {
		size_t len = utf8_trunc(title, sizeof(mc->title));
		memcpy(mc->title, title, len);
		mc->title[len] = '\0';
	} else {
		mc->title[0] = '\0';
	}
	if (lp)
		iox_fd_add(lp, fd, IOX_READ, on_mserver_read, mc);
	return mc;
}

static void
mconn_remove(struct iox_loop *lp, pid_t pid)
{
	int i;

	for (i = 0; i < mconn_count; i++) {
		if (mconns[i].pid == pid) {
			if (mconns[i].fd >= 0 && lp)
				iox_fd_remove(lp, mconns[i].fd);
			ipc_transport_free(mconns[i].t);	/* closes fd */
			mconns[i].t = NULL;
			mconns[i] = mconns[--mconn_count];
			return;
		}
	}
}

static void
mconn_disconnect_all(struct iox_loop *lp)
{
	int i;

	for (i = 0; i < mconn_count; i++) {
		mconn_ipc_send_empty(&mconns[i], IPC_MSG_DETACH);
		if (mconns[i].fd >= 0 && lp)
			iox_fd_remove(lp, mconns[i].fd);
		ipc_transport_free(mconns[i].t);	/* closes fd */
		mconns[i].t = NULL;
	}
	mconn_count = 0;
}

/* get the focused/active mserver connection.
 * returns NULL if no connection is active. */
static struct mconn *
mconn_focused(void)
{
	uint32_t id;
	struct mconn *mc;

	if (client_mode == CLIENT_MODE_TURBO) {
		if (turbo_focused_id(&id)) {
			mc = mconn_find_by_pid((pid_t)id);
			if (mc)
				return mc;
		}
	}

	if (tilemgr) {
		id = tile_focused_id(tilemgr);
		if (id)
			return mconn_find_by_pid((pid_t)id);
		return NULL;	/* tiled: no fallback to watched_id */
	}

	/* screen mode fallback via watched_id */
	if (watching)
		return mconn_find_by_pid((pid_t)watched_id);
	return NULL;
}

/* get the transport for the focused/active mserver connection.
 * returns NULL if no connection is active (or the session is remote). */
struct ipc_transport *
mconn_focused_transport(void)
{
	struct mconn *mc = mconn_focused();

	return mc ? mc->t : NULL;
}

/* send IPC message to a specific mserver, routing through proxy
 * envelope when connected to a remote session */
static int
mconn_ipc_send(struct mconn *mc, uint32_t type,
    const void *payload, uint32_t len)
{
	if (is_remote) {
		if (remote_netchan)
			return proxy_msg_xsend(rnet, (uint32_t)mc->pid,
			    type, payload, len);
		return proxy_msg_send(rproxy_fd, (uint32_t)mc->pid,
		    type, payload, len);
	}
	if (!mc->t)
		return -1;
	return ipc_transport_send(mc->t, type, payload, len);
}

static int
mconn_ipc_send_empty(struct mconn *mc, uint32_t type)
{
	return mconn_ipc_send(mc, type, NULL, 0);
}

static int
mconn_ipc_send_size(struct mconn *mc, uint32_t type, int rows, int cols)
{
	struct ipc_size sz;
	uint8_t buf[16];
	int n;

	sz.rows = (uint16_t)rows;
	sz.cols = (uint16_t)cols;
	n = ipc_size_encode(&sz, buf, sizeof(buf));
	if (n < 0)
		return -1;
	return mconn_ipc_send(mc, type, buf, (uint32_t)n);
}

/* send WIN_RESIZE to all connected mservers */
static void
micro_resize_all(int rows, int cols)
{
	int i;

	for (i = 0; i < mconn_count; i++)
		mconn_ipc_send_size(&mconns[i], IPC_MSG_WIN_RESIZE,
		    rows, cols);
}

/* ask every connected mserver to re-send its screen replay.  used after
 * the panes have been sized so each window resyncs at its final size,
 * without relying on the child process to repaint after SIGWINCH (a
 * full-screen alt-screen app that does not redraw would otherwise leave
 * the pane blank until the next unrelated update). */
static void
mconn_request_refresh_all(void)
{
	int i;

	for (i = 0; i < mconn_count; i++)
		mconn_ipc_send_empty(&mconns[i], IPC_MSG_REFRESH);
}

static int mconn_refresh(struct iox_loop *lp);

/* A new mserver registers its session directory slightly before it begins
 * listening, so an immediate rescan can miss it. Retry a few times with a
 * short delay to win that race. This makes new windows appear even when the
 * sessdir watch is unavailable (e.g. inotify instances exhausted). */
#define SPAWN_RESCAN_TRIES	5
#define SPAWN_RESCAN_DELAY_MS	80

struct spawn_rescan {
	int tries_left;
};

static void
spawn_rescan_cb(struct iox_loop *lp, void *arg)
{
	struct spawn_rescan *sr = arg;

	/* mconn_refresh() is idempotent: already-connected windows are
	 * skipped, so racing with the watch (when it works) is harmless. */
	if (mconn_refresh(lp) <= 0 && --sr->tries_left > 0) {
		if (iox_timer_add(lp, SPAWN_RESCAN_DELAY_MS,
		    spawn_rescan_cb, sr) >= 0)
			return;
	}
	free(sr);
}

/* spawn a new mserver for this session */
static void
micro_spawn_window(struct iox_loop *lp, const char *name)
{
	pid_t pid;
	struct spawn_rescan *sr;

	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		char *child_argv[] = {
		    "lumi-mserver", "-s", (char *)name, NULL,
		};
		_exit(multicall_exec_cmd("mserver", 3, child_argv));
	}

	/* parent: the watch picks this up if it is working; also schedule
	 * active rescans so the window still appears when it is not. */
	if (lp && (sr = malloc(sizeof(*sr))) != NULL) {
		sr->tries_left = SPAWN_RESCAN_TRIES;
		if (iox_timer_add(lp, SPAWN_RESCAN_DELAY_MS,
		    spawn_rescan_cb, sr) < 0)
			free(sr);
	}
}

const char *
micro_current_session(void)
{
	return session_name;
}

/* forward declarations for functions defined after turbo section */
static void mconn_sync_winlist(void);
static void micro_cycle_focus(int dir);
static void remove_window(struct iox_loop *lp, uint32_t pid);

/* ---- per-window VT state ---- */

#define SCROLLBACK_LINES 2000

/* per-window layout snapshot: which arrangement algorithm placed this
 * window and where.  used to decide whether a compositor should recapture
 * the window (layout.mode != active mode) or restore it in place. */
struct win_layout {
	enum client_mode mode;
	int		x, y, w, h;
	int		valid;		/* nonzero if populated */
};

struct client_window {
	uint32_t	id;
	struct vt_state	*vt;
	struct vt_parse	*parser;
	struct predict	*pred;
	int		keep_open;	/* -1=default, 0=no, 1=yes */
	int		dead;		/* child process has exited */
	int		scroll_lock;	/* output frozen */
	int		input_lock;	/* keystrokes discarded */
	struct win_layout layout;	/* current mode's placement */
	struct win_layout prev_layout;	/* saved from previous mode */
	char		title_override[128];	/* :title override, "" = none */
};

static struct client_window cwins[CLIENT_WIN_MAX];
static int cwin_count;
static int keep_open_default;		/* global: keep window after child exits */

/* resolve per-window keep_open tri-state against global default */
static int
cwin_should_keep_open(const struct client_window *cw)
{
	if (cw->keep_open >= 0)
		return cw->keep_open;
	return keep_open_default;
}

/* effective on-screen title for a window: a :title override wins over
 * the live VT title, which the running program can change at any time.
 * cw may be NULL (a window we have not materialized locally). */
static const char *
cwin_effective_title(const struct client_window *cw, const char *vt_title)
{
	if (cw && cw->title_override[0])
		return cw->title_override;
	return vt_title;
}

/*
 * Most-recently-used window order.  mru[0] is the current window, mru[1]
 * the one focused before it, and so on.  The list is refreshed once per
 * event-loop iteration from the live focus (mru_note_focus), so every
 * focus path updates it without each having to call in.  It drives the
 * last-window bounce (Ctrl-A Ctrl-A) and the focus fallback when the
 * current window closes.
 */
static uint32_t mru[CLIENT_WIN_MAX];
static int mru_len;

/* move id to the front of the MRU list, inserting it if absent */
static void
mru_touch(uint32_t id)
{
	int i, n;

	if (id == 0 || (mru_len > 0 && mru[0] == id))
		return;
	for (i = 0, n = 0; i < mru_len; i++) {
		if (mru[i] != id)
			mru[n++] = mru[i];
	}
	if (n > CLIENT_WIN_MAX - 1)
		n = CLIENT_WIN_MAX - 1;
	memmove(&mru[1], &mru[0], (size_t)n * sizeof(mru[0]));
	mru[0] = id;
	mru_len = n + 1;
}

/* drop id from the MRU list */
static void
mru_remove(uint32_t id)
{
	int i, n;

	for (i = 0, n = 0; i < mru_len; i++) {
		if (mru[i] != id)
			mru[n++] = mru[i];
	}
	mru_len = n;
}

/* the window focused before the current one, or 0 if there is none */
static uint32_t
mru_prev(void)
{
	return mru_len >= 2 ? mru[1] : 0;
}

/* most-recently-used window that still has a live connection, or 0 */
static uint32_t
mru_recent_live(void)
{
	int i;

	for (i = 0; i < mru_len; i++) {
		if (mconn_find_by_pid((pid_t)mru[i]))
			return mru[i];
	}
	return 0;
}

/* record the live focus at the front of the MRU list */
static void
mru_note_focus(void)
{
	if (watching && watched_id != 0)
		mru_touch(watched_id);
}

static struct client_window *
cwin_find(uint32_t id)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		if (cwins[i].id == id)
			return &cwins[i];
	}
	return NULL;
}

/* return the focused client_window, or NULL */
static struct client_window *
cwin_focused(void)
{
	uint32_t id;

	if (client_mode == CLIENT_MODE_TURBO) {
		if (turbo_focused_id(&id))
			return cwin_find(id);
	}

	if (tilemgr) {
		id = tile_focused_id(tilemgr);
		if (id)
			return cwin_find(id);
		return NULL;
	}
	if (watching)
		return cwin_find(watched_id);
	return NULL;
}

/* DCS passthrough: forward SIXEL and other DCS sequences to the outer
 * terminal when the source pane is fullscreen (single pane, no splits). */
static void
dcs_passthru(void *ctx, int introducer, const char *data, size_t len)
{
	char intro[2];

	(void)ctx;

	/* only pass through when a single pane is visible */
	if (!tilemgr || tile_pane_count(tilemgr) != 1)
		return;
	if (!watching)
		return;

	/* write raw DCS to stdout: ESC <introducer> <data> ESC \ */
	intro[0] = '\033';
	intro[1] = (char)introducer;
	tio_write(STDOUT_FILENO, intro, 2);
	tio_write(STDOUT_FILENO, data, len);
	tio_write(STDOUT_FILENO, "\033\\", 2);
}

/* OSC passthrough: forward desktop-notification OSCs (kitty OSC 99, the
 * simple OSC 9 form, and OSC 777 "notify") from any window to the outer
 * terminal so pop-ups from programs like Claude reach the user. */
static void
osc_passthru(void *ctx, const char *data, size_t len)
{
	int num = 0;
	size_t i;

	(void)ctx;

	if (!watching)
		return;

	/* parse the leading "N;" selector */
	for (i = 0; i < len && data[i] >= '0' && data[i] <= '9'; i++)
		num = num * 10 + (data[i] - '0');
	if (i == 0 || i >= len || data[i] != ';')
		return;

	if (num != 9 && num != 99 && num != 777)
		return;

	/* re-wrap as ESC ] <data> ST for the host terminal */
	tio_write(STDOUT_FILENO, "\033]", 2);
	tio_write(STDOUT_FILENO, data, len);
	tio_write(STDOUT_FILENO, "\033\\", 2);
}

static struct client_window *
cwin_add_sized(uint32_t id, int rows, int cols)
{
	struct client_window *cw;

	if (cwin_count >= CLIENT_WIN_MAX)
		return NULL;
	cw = &cwins[cwin_count];
	cw->id = id;
	cw->vt = vt_state_new(rows, cols, SCROLLBACK_LINES);
	if (!cw->vt)
		return NULL;
	vt_state_set_altscreen_scrollback(cw->vt, altscreen_scrollback);
	cw->parser = vt_parse_new(vt_ops_default(), cw->vt);
	if (!cw->parser) {
		vt_state_free(cw->vt);
		return NULL;
	}
	vt_parse_set_dcs_cb(cw->parser, dcs_passthru, NULL);
	vt_parse_set_osc_cb(cw->parser, osc_passthru, NULL);
	cw->pred = predict_new();
	cw->keep_open = -1;	/* inherit global default */
	cw->dead = 0;
	cw->scroll_lock = 0;
	cw->input_lock = 0;
	cwin_count++;
	return cw;
}

static struct client_window *
cwin_add(uint32_t id)
{
	return cwin_add_sized(id, content_rows, content_cols);
}

static void
cwin_remove(uint32_t id)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		if (cwins[i].id == id) {
			/* drop any layout reference before freeing the vt so
			 * a stale leaf/window cannot dereference a dangling
			 * pointer during a later composite. */
			if (tilemgr)
				tile_forget_vt(tilemgr, cwins[i].vt);
			if (wmgr)
				wm_forget_vt(wmgr, cwins[i].vt);
			predict_free(cwins[i].pred);
			vt_parse_free(cwins[i].parser);
			vt_state_free(cwins[i].vt);
			cwins[i] = cwins[--cwin_count];
			return;
		}
	}
}

static void
cwin_free_all(void)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		if (tilemgr)
			tile_forget_vt(tilemgr, cwins[i].vt);
		if (wmgr)
			wm_forget_vt(wmgr, cwins[i].vt);
		predict_free(cwins[i].pred);
		vt_parse_free(cwins[i].parser);
		vt_state_free(cwins[i].vt);
	}
	cwin_count = 0;
}

static void
cwin_resize_all(int rows, int cols)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		predict_reset(cwins[i].pred, cwins[i].vt);
		vt_state_resize(cwins[i].vt, rows, cols);
	}
}

/* ---- prefix timeout ---- */

static int prefix_timer_id = -1;

static void
on_prefix_timeout(struct keys *k, void *arg)
{
	(void)k;
	(void)arg;

	if (client_mode != CLIENT_MODE_MINIMAL)
		menu_show();
}

static void
on_dblclick_timeout(struct iox_loop *lp, void *arg)
{
	(void)lp;
	(void)arg;

	sel_click_count = 0;
	sel_dblclick_timer_id = -1;
}

static void
on_prefix_timer(struct iox_loop *lp, void *arg)
{
	(void)lp;
	(void)arg;

	prefix_timer_id = -1;
	keys_timeout_expired(keybinds);
}

static void
sync_prefix_timer(struct iox_loop *lp)
{
	int want = keys_get_timeout(keybinds);

	if (want >= 0 && prefix_timer_id < 0)
		prefix_timer_id = iox_timer_add(lp, want,
		    on_prefix_timer, NULL);
	else if (want < 0 && prefix_timer_id >= 0) {
		iox_timer_remove(lp, prefix_timer_id);
		prefix_timer_id = -1;
	}
}

/* ---- turbo mode state ---- */

static int turbo_need_full;	/* next render should be full redraw */

/* cascade offset for new turbo windows */
#define TURBO_CASCADE_DX	2
#define TURBO_CASCADE_DY	1
static int cascade_x = 2, cascade_y = 2;

/* ---- terminal mode forwarding ---- */

#define FORWARD_MODES (VT_MODE_DECCKM | VT_MODE_DECKPAM)
static unsigned synced_modes;
static int synced_cursor_shape;
static int synced_kitty_kbd;
static int synced_modify_other_keys;

static void
emit_mode(int mode, int on)
{
	char buf[16];
	int len;

	if (on)
		len = txl_decset(buf, sizeof(buf), mode);
	else
		len = txl_decrst(buf, sizeof(buf), mode);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
}

static void
emit_cursor_shape(int shape)
{
	char buf[16];
	int len;

	len = snprintf(buf, sizeof(buf), "\033[%d q", shape);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
}

static void
sync_terminal_modes(unsigned new_modes)
{
	unsigned changed = (synced_modes ^ new_modes) & FORWARD_MODES;

	if (!changed)
		return;

	if (changed & VT_MODE_DECCKM)
		emit_mode(1, new_modes & VT_MODE_DECCKM);
	if (changed & VT_MODE_DECKPAM) {
		const char *s = (new_modes & VT_MODE_DECKPAM) ?
		    "\033=" : "\033>";
		tio_write(STDOUT_FILENO, s, 2);
	}
	tio_flush(STDOUT_FILENO);
	synced_modes = new_modes & FORWARD_MODES;
}

static void
sync_cursor_shape(int shape)
{
	if (shape == synced_cursor_shape)
		return;
	emit_cursor_shape(shape);
	tio_flush(STDOUT_FILENO);
	synced_cursor_shape = shape;
}

static void
sync_keyboard_proto(struct vt_state *st)
{
	int flags = st->kitty_kbd_flags;
	int mok = st->modify_other_keys;
	char buf[32];
	int n;
	int dirty = 0;

	if (flags != synced_kitty_kbd) {
		if (flags) {
			n = snprintf(buf, sizeof(buf),
			    "\033[>%du", flags);
		} else {
			n = snprintf(buf, sizeof(buf), "\033[<u");
		}
		tio_write(STDOUT_FILENO, buf, n);
		synced_kitty_kbd = flags;
		dirty = 1;
	}

	if (mok != synced_modify_other_keys) {
		n = snprintf(buf, sizeof(buf),
		    "\033[>4;%dm", mok);
		tio_write(STDOUT_FILENO, buf, n);
		synced_modify_other_keys = mok;
		dirty = 1;
	}

	if (dirty)
		tio_flush(STDOUT_FILENO);
}

static void
reset_terminal_modes(void)
{
	int dirty = synced_modes || synced_cursor_shape;

	if (synced_modes & VT_MODE_DECCKM)
		emit_mode(1, 0);
	if (synced_modes & VT_MODE_DECKPAM) {
		tio_write(STDOUT_FILENO, "\033>", 2);
		dirty = 1;
	}
	if (synced_cursor_shape != 0)
		emit_cursor_shape(0);
	if (synced_kitty_kbd) {
		tio_write(STDOUT_FILENO, "\033[<u", 4);
		synced_kitty_kbd = 0;
		dirty = 1;
	}
	if (synced_modify_other_keys) {
		tio_write(STDOUT_FILENO, "\033[>4;0m", 7);
		synced_modify_other_keys = 0;
		dirty = 1;
	}
	if (dirty)
		tio_flush(STDOUT_FILENO);
	synced_modes = 0;
	synced_cursor_shape = 0;
}

static int cursor_visible = -1; /* -1 = unknown, 0 = hidden, 1 = shown */
static void turbo_cursor(int *row, int *col, int *vis);

/* set outer terminal cursor visibility, emitting only on state change */
static void
set_cursor_vis(int visible)
{
	const char *s;

	if (visible == cursor_visible)
		return;
	if (visible)
		s = txl_str(txl, TXL_CNORM);
	else
		s = txl_str(txl, TXL_CIVIS);
	if (s)
		tio_write(STDOUT_FILENO, s, strlen(s));
	cursor_visible = visible;
}

/* query whether the application wants cursor visible */
static int
app_cursor_vis(void)
{
	if (client_mode == CLIENT_MODE_TURBO) {
		int row, col, vis;

		turbo_cursor(&row, &col, &vis);
		return vis;
	}
	if (tilemgr) {
		int row, col, vis;

		tile_cursor(tilemgr, &row, &col, &vis);
		return vis;
	}
	return 0;
}

static void
update_content_size(int rows, int cols)
{
	content_cols = cols;
	content_rows = taskbar_visible ? rows - 1 : rows;
	if (content_rows < 1)
		content_rows = 1;
}

/* ---- mouse mode ---- */

static int mouse_enabled;

static void
enable_mouse(void)
{
	char buf[32];
	int len, mode;

	/* 1002 = button-motion tracking (needed for drag-select and
	 * turbo window move/resize). all modes need it now. */
	mode = 1002;

	len = txl_decset(buf, sizeof(buf), mode);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
	len = txl_decset(buf, sizeof(buf), 1006);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
	tio_flush(STDOUT_FILENO);
	mouse_enabled = mode;
}

static void
disable_mouse(void)
{
	char buf[32];
	int len;

	if (!mouse_enabled)
		return;
	len = txl_decrst(buf, sizeof(buf), 1006);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
	len = txl_decrst(buf, sizeof(buf), mouse_enabled);
	if (len > 0)
		tio_write(STDOUT_FILENO, buf, (size_t)len);
	tio_flush(STDOUT_FILENO);
	mouse_enabled = 0;
}

/* ---- turbo mode helpers ---- */

/* compute cursor position in turbo mode: focused window's VT cursor
 * offset by window position on screen. hides cursor if no focused window. */
static void
turbo_cursor(int *row, int *col, int *vis)
{
	int i, n;

	n = wm_count(wmgr);
	for (i = 0; i < n; i++) {
		struct wm_window *win = wm_window_at(wmgr, i);

		if (!win || !win->focused || !win->vt)
			continue;
		*row = win->y + win->vt->cursor_row;
		*col = win->x + win->vt->cursor_col;
		*vis = (win->vt->modes & VT_MODE_CURSOR_VIS) ? 1 : 0;
		return;
	}
	*row = 0;
	*col = 0;
	*vis = 0;
}

static void
turbo_render(void)
{
	int crow, ccol, cvis;

	if (sel_active())
		wm_invalidate(wmgr);
	wm_composite(wmgr, theme);
	if (sel_active())
		sel_highlight(wm_screen_mut(wmgr),
		    wm_rows(wmgr), wm_cols(wmgr));
	wm_update_dirty(wmgr);
	turbo_cursor(&crow, &ccol, &cvis);

	if (turbo_need_full) {
		render_cells_full(renderer, STDOUT_FILENO,
		    wm_screen(wmgr), wm_rows(wmgr), wm_cols(wmgr),
		    crow, ccol, cvis);
		turbo_need_full = 0;
	} else {
		render_cells_diff(renderer, STDOUT_FILENO,
		    wm_screen(wmgr), wm_rows(wmgr), wm_cols(wmgr),
		    crow, ccol, cvis, wm_row_dirty(wmgr));
	}
}

/* callback for overlay_pop in turbo mode -- recomposite the full
 * screen instead of erasing from vt->buf (which is a single window's
 * buffer, not the composited screen). */
static void
turbo_repaint(void)
{
	int crow, ccol, cvis;

	turbo_need_full = 1;
	taskbar_invalidate();
	turbo_render();
	if (taskbar_visible)
		render_taskbar(STDOUT_FILENO,
		    content_rows + 1, content_cols);
	turbo_cursor(&crow, &ccol, &cvis);
	render_move_cursor(renderer, STDOUT_FILENO, crow, ccol);
	set_cursor_vis(cvis);
	tio_flush(STDOUT_FILENO);
}

/* ---- tiled mode helpers ---- */

static void
tiled_render(void)
{
	int crow, ccol, cvis;

	tile_composite(tilemgr);
	if (sel_active())
		sel_highlight(tile_screen_mut(tilemgr),
		    tile_rows(tilemgr), tile_cols(tilemgr));
	tile_update_dirty(tilemgr);
	tile_cursor(tilemgr, &crow, &ccol, &cvis);

	if (tile_need_full) {
		render_cells_full(renderer, STDOUT_FILENO,
		    tile_screen(tilemgr), tile_rows(tilemgr),
		    tile_cols(tilemgr), crow, ccol, cvis);
		tile_need_full = 0;
	} else {
		render_cells_diff(renderer, STDOUT_FILENO,
		    tile_screen(tilemgr), tile_rows(tilemgr),
		    tile_cols(tilemgr), crow, ccol, cvis,
		    tile_row_dirty(tilemgr));
	}
}

static void
tiled_repaint(void)
{
	int crow, ccol, cvis;

	tile_need_full = 1;
	taskbar_invalidate();
	tiled_render();
	if (taskbar_visible)
		render_taskbar(STDOUT_FILENO,
		    content_rows + 1, content_cols);
	tile_cursor(tilemgr, &crow, &ccol, &cvis);
	render_move_cursor(renderer, STDOUT_FILENO, crow, ccol);
	set_cursor_vis(cvis);
	tio_flush(STDOUT_FILENO);
}

/* ---- scrollback viewer ---- */

static int copy_win_rect(int *x, int *y, int *w, int *h);

/* return the client_window we entered scrollback on */
static struct client_window *
scrollback_cwin(void)
{
	return cwin_find(scrollback_win_id);
}

/* enter scrollback mode on the focused window */
static void
scrollback_enter(void)
{
	struct client_window *cw;
	struct vt_buf *primary;
	int rows, cols;

	cw = cwin_focused();
	if (!cw || !cw->vt)
		return;

	primary = cw->vt->targets[VT_TARGET_PRIMARY];
	rows = vt_buf_rows(primary);
	cols = vt_buf_cols(primary);

	/* create or resize the scrollback scratch buffer */
	if (!cw->vt->targets[VT_TARGET_SCROLLBACK])
		cw->vt->targets[VT_TARGET_SCROLLBACK] =
		    vt_buf_new(rows, cols, 0);
	else
		vt_buf_resize(cw->vt->targets[VT_TARGET_SCROLLBACK],
		    rows, cols);

	/* create the scrollback taskbar on first use */
	if (!scrollback_sb) {
		scrollback_sb = taskbar_new();
		taskbar_set_format(scrollback_sb,
		    " [scrollback ${offset}/${total} -- q to exit]");
	}

	if (sel_active()) {
		sel_clear();
		turbo_need_full = 1;
		tile_need_full = 1;
	}

	scrollback_mode = 1;
	scrollback_offset = 0;
	scrollback_win_id = cw->id;

	/* place the copy cursor at the top-left of the visible content */
	copy_selecting = 0;
	copy_gran = SEL_MODE_CHAR;
	{
		int wx, wy, ww, wh;

		if (copy_win_rect(&wx, &wy, &ww, &wh) == 0) {
			copy_cur_row = wy;
			copy_cur_col = wx;
		} else {
			copy_cur_row = 0;
			copy_cur_col = 0;
		}
	}

	/* show scrollbar in turbo mode */
	if (client_mode == CLIENT_MODE_TURBO && wmgr) {
		int sb_lines = vt_buf_scrollback_lines(primary);
		int total = sb_lines + rows;
		float len = (total > 0) ?
		    (float)rows / (float)total : 1.0f;

		wm_set_scroll(wmgr, cw->id, 1.0f, len);
	}
}

/* leave scrollback mode, restoring the window's render target */
static void
scrollback_leave(void)
{
	struct client_window *cw;

	if (sel_active())
		sel_clear();

	cw = scrollback_cwin();
	if (cw && cw->vt)
		vt_state_set_target(cw->vt, VT_TARGET_PRIMARY);

	/* hide scrollbar in turbo mode */
	if (client_mode == CLIENT_MODE_TURBO && wmgr)
		wm_set_scroll(wmgr, scrollback_win_id, 0.0f, 0.0f);

	scrollback_mode = 0;
	scrollback_offset = 0;
	scrollback_win_id = 0;
	copy_selecting = 0;

	tile_need_full = 1;
	turbo_need_full = 1;
	need_render = 1;
	need_taskbar = 1;
}

/* resolve the content rectangle of the window we are viewing in
 * scrollback.  returns 0 on success, -1 if it cannot be found. */
static int
copy_win_rect(int *x, int *y, int *w, int *h)
{
	if (client_mode == CLIENT_MODE_TURBO) {
		struct wm_window *win = wmgr ?
		    wm_find(wmgr, scrollback_win_id) : NULL;

		if (!win)
			return -1;
		*x = win->x;
		*y = win->y;
		*w = win->w;
		*h = win->h;
		return 0;
	}
	if (tilemgr &&
	    tile_pane_geometry(tilemgr, scrollback_win_id, x, y, w, h) == 0)
		return 0;
	return -1;
}

/* extend the active copy selection to the copy cursor, honoring the
 * current granularity. */
static void
copy_extend(void)
{
	if (!copy_selecting)
		return;
	if (copy_gran == SEL_MODE_LINE)
		sel_update_line(copy_cur_row);
	else
		sel_update(copy_cur_row, copy_cur_col);
	turbo_need_full = 1;
	tile_need_full = 1;
}

/* scroll the scrollback view by delta lines (positive = older),
 * clamped to the available history. */
static void
copy_scroll(int delta)
{
	struct client_window *cw = scrollback_cwin();
	struct vt_buf *primary;
	int maxoff;

	if (!cw || !cw->vt)
		return;
	primary = cw->vt->targets[VT_TARGET_PRIMARY];
	maxoff = primary ? vt_buf_scrollback_lines(primary) : 0;
	scrollback_offset += delta;
	if (scrollback_offset < 0)
		scrollback_offset = 0;
	if (scrollback_offset > maxoff)
		scrollback_offset = maxoff;
}

/* move the copy cursor one row (dir -1 up, +1 down).  when not
 * selecting, moving past a vertical edge scrolls the view instead. */
static void
copy_move_vert(int dir)
{
	int wx, wy, ww, wh;

	if (copy_win_rect(&wx, &wy, &ww, &wh) != 0)
		return;
	if (dir < 0) {
		if (copy_cur_row > wy)
			copy_cur_row--;
		else if (!copy_selecting)
			copy_scroll(1);
	} else {
		if (copy_cur_row < wy + wh - 1)
			copy_cur_row++;
		else if (!copy_selecting)
			copy_scroll(-1);
	}
	copy_extend();
}

/* move the copy cursor one column (dir -1 left, +1 right),
 * clamped to the window content. */
static void
copy_move_horiz(int dir)
{
	int wx, wy, ww, wh;

	if (copy_win_rect(&wx, &wy, &ww, &wh) != 0)
		return;
	if (dir < 0) {
		if (copy_cur_col > wx)
			copy_cur_col--;
	} else {
		if (copy_cur_col < wx + ww - 1)
			copy_cur_col++;
	}
	copy_extend();
}

/* set (or re-anchor) the selection mark at the copy cursor and begin
 * extending with the given granularity.  switching granularity while
 * selecting keeps the original anchor. */
static void
copy_start(enum sel_mode gran)
{
	int wx, wy, ww, wh;

	if (copy_win_rect(&wx, &wy, &ww, &wh) != 0)
		return;
	if (!copy_selecting) {
		copy_anchor_row = copy_cur_row;
		copy_anchor_col = copy_cur_col;
	}
	copy_gran = gran;
	copy_selecting = 1;
	if (gran == SEL_MODE_LINE)
		sel_begin_line(scrollback_win_id, copy_anchor_row, wx, ww);
	else if (gran == SEL_MODE_BLOCK)
		sel_begin_block(scrollback_win_id, copy_anchor_row,
		    copy_anchor_col, wx, ww);
	else
		sel_begin(scrollback_win_id, copy_anchor_row,
		    copy_anchor_col, wx, ww);
	copy_extend();
	turbo_need_full = 1;
	tile_need_full = 1;
}

/* copy the active selection to the clipboard and leave scrollback. */
static void
copy_yank(void)
{
	const struct vt_cell *scr;
	int nr, nc;

	if (!copy_selecting) {
		scrollback_leave();
		return;
	}
	if (client_mode == CLIENT_MODE_TURBO) {
		scr = wm_screen(wmgr);
		nr = wm_rows(wmgr);
		nc = wm_cols(wmgr);
	} else {
		scr = tile_screen(tilemgr);
		nr = tile_rows(tilemgr);
		nc = tile_cols(tilemgr);
	}
	sel_finish(scr, nr, nc);
	scrollback_leave();
}

/*
 * Prepare the scrollback render target with the correct view and
 * switch the window's active target so the compositor picks it up.
 *
 * offset 0 = live view (same as normal render)
 * offset N = scrolled back N lines from live
 */
static void
scrollback_prepare(void)
{
	struct client_window *cw;
	struct vt_buf *primary, *scratch;
	int rows, sb_lines, vis_rows, total_lines;
	int top;

	cw = scrollback_cwin();
	if (!cw || !cw->vt)
		return;

	primary = cw->vt->targets[VT_TARGET_PRIMARY];
	scratch = cw->vt->targets[VT_TARGET_SCROLLBACK];
	if (!primary || !scratch)
		return;

	vis_rows = vt_buf_rows(primary);
	rows = vt_buf_rows(scratch);
	sb_lines = vt_buf_scrollback_lines(primary);
	total_lines = sb_lines + vis_rows;

	/* clamp offset */
	if (scrollback_offset > total_lines - rows)
		scrollback_offset = total_lines - rows;
	if (scrollback_offset < 0)
		scrollback_offset = 0;

	/* top line in the virtual [scrollback][visible] history */
	top = total_lines - scrollback_offset - rows;
	if (top < 0)
		top = 0;

	/* blit history rows into the scratch buffer */
	vt_buf_copy_scrollback(scratch, primary, 0, top, rows);

	/* point the compositor at the scratch buffer */
	vt_state_set_target(cw->vt, VT_TARGET_SCROLLBACK);

	/* update scrollbar position in turbo mode */
	if (client_mode == CLIENT_MODE_TURBO && wmgr) {
		int max_off = total_lines - vis_rows;
		float pos, len;

		if (max_off > 0)
			pos = 1.0f - (float)scrollback_offset / (float)max_off;
		else
			pos = 1.0f;
		len = (total_lines > 0) ?
		    (float)vis_rows / (float)total_lines : 1.0f;
		wm_set_scroll(wmgr, scrollback_win_id, pos, len);
	}
}

/* update scrollback taskbar variables and render via the taskbar */
static void
scrollback_taskbar_render(void)
{
	struct client_window *cw;
	struct vt_buf *primary;
	struct taskbar *saved;
	char tmp[16];
	int total;

	if (!taskbar_visible || !scrollback_sb)
		return;

	cw = scrollback_cwin();
	total = 0;
	if (cw && cw->vt) {
		primary = cw->vt->targets[VT_TARGET_PRIMARY];
		total = vt_buf_scrollback_lines(primary) +
		    vt_buf_rows(primary);
	}

	snprintf(tmp, sizeof(tmp), "%d", scrollback_offset);
	taskbar_set(scrollback_sb, "offset", tmp);
	snprintf(tmp, sizeof(tmp), "%d", total);
	taskbar_set(scrollback_sb, "total", tmp);

	/* temporarily swap the active taskbar */
	saved = taskbar;
	taskbar = scrollback_sb;
	render_taskbar(STDOUT_FILENO,
	    content_rows + 1, content_cols);
	taskbar = saved;
}

/*
 * Deferred rendering: event handlers set need_render / need_taskbar
 * flags instead of calling render functions directly.  flush_render()
 * is called once per main-loop iteration, just before blocking in poll.
 * This batches multiple events (e.g. several IPC_MSG_OUTPUT messages
 * ready on the same poll cycle) into a single render pass.
 */
static void
flush_render(void)
{
	int did_render = 0;

	/* refresh MRU order from the live focus every iteration, so all
	 * focus paths are captured without each calling in explicitly. */
	mru_note_focus();

	if (!need_render && !need_taskbar)
		return;

	/* Hold the backdrop static while a modal overlay (prefix menu,
	 * command palette, color picker) is visible.  Repainting it would
	 * flush app output over the overlay, which then redraws on top; when
	 * the underlying window updates rapidly (a spinner, a full-screen
	 * app) that reads as the overlay strobing.  The VT is still fed by
	 * the IPC handler, and closing the overlay triggers a full repaint,
	 * so no output is lost, only deferred. */
	if (overlay_visible) {
		need_render = 0;
		need_taskbar = 0;
		return;
	}

	if (need_render) {
		if (scrollback_mode)
			scrollback_prepare();
		if (turbo_need_full || tile_need_full) {
			taskbar_invalidate();
			need_taskbar = 1;
		}
		if (client_mode == CLIENT_MODE_TURBO)
			turbo_render();
		else
			tiled_render();
		need_render = 0;
		did_render = 1;
	}

	/* sync terminal modes from focused VT */
	if (vt && !scrollback_mode) {
		sync_terminal_modes(vt->modes);
		sync_cursor_shape(vt->cursor_shape);
		sync_keyboard_proto(vt);
	}

	if (need_taskbar) {
		/* the command line occupies the taskbar row while open */
		if (cmdline_visible) {
			cmdline_render();
		} else if (taskbar_visible) {
			if (scrollback_mode)
				scrollback_taskbar_render();
			else {
				/* highlight the tab for the live focus: focus
				 * can change after mconn_sync_winlist stamped
				 * the winlist (window add/close), so drive the
				 * highlight from watched_id here. */
				win_list_set_active(watching ? watched_id : 0);
				render_taskbar(STDOUT_FILENO,
				    content_rows + 1, content_cols);
			}
		}
		need_taskbar = 0;
	}

	if (overlay_visible)
		overlay_render();

	if (did_render) {
		int crow = 0, ccol = 0, cvis = 0;

		if (cmdline_visible) {
			/* keep the prompt intact and the caret on it */
			cmdline_render();
			crow = cmdline_row();
			ccol = 1 + (int)cmd_len;
			cvis = 1;
		} else if (scrollback_mode) {
			/* show the copy cursor instead of the app cursor */
			crow = copy_cur_row;
			ccol = copy_cur_col;
			cvis = 1;
		} else if (client_mode == CLIENT_MODE_TURBO) {
			turbo_cursor(&crow, &ccol, &cvis);
		} else if (tilemgr) {
			tile_cursor(tilemgr, &crow, &ccol, &cvis);
		}
		render_move_cursor(renderer, STDOUT_FILENO, crow, ccol);
		set_cursor_vis(cvis);
		tio_flush(STDOUT_FILENO);
	}
}

/* resize callback: resize each pane's VT + tell mserver */
static void
tiled_resize_pane_cb(uint32_t window_id, int w, int h, void *arg)
{
	struct client_window *cw;
	struct mconn *mc;

	(void)arg;

	cw = cwin_find(window_id);
	if (cw)
		vt_state_resize(cw->vt, h, w);

	mc = mconn_find_by_pid((pid_t)window_id);
	if (mc)
		mconn_ipc_send_size(mc, IPC_MSG_WIN_RESIZE, h, w);
}

/* resize a single window's VT to the geometry of the pane it currently
 * occupies.  call after swapping a window into the focused pane so a
 * buffer carried over from turbo mode (often taller than the terminal)
 * is shrunk to fit instead of leaving the live content below the pane. */
static void
screen_fit_window(uint32_t id)
{
	int w, h;

	if (tile_pane_geometry(tilemgr, id, NULL, NULL, &w, &h) == 0)
		tiled_resize_pane_cb(id, w, h, NULL);
}

/* enforce the screen-mode sizing policy: every window fits the area
 * screen mode controls.  windows shown in a pane already get their pane
 * size from tile_each_pane(); resize any remaining off-screen window to
 * the full content area so it is never larger than the terminal. */
static void
screen_fit_all_windows(void)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		if (tile_pane_geometry(tilemgr, cwins[i].id,
		    NULL, NULL, NULL, NULL) == 0)
			continue;	/* sized by tile_each_pane */
		tiled_resize_pane_cb(cwins[i].id, content_cols,
		    content_rows, NULL);
	}
}

/* a visible selection belongs to the window it was made in and is
 * pinned to screen coordinates.  drop it when the foreground window is
 * about to change so it does not linger over another window's content. */
static void
sel_clear_on_focus_change(uint32_t new_id)
{
	if (new_id != watched_id && sel_active()) {
		sel_clear();
		turbo_need_full = 1;
		tile_need_full = 1;
		need_render = 1;
	}

	/* a PASTE_BEGIN with no matching PASTE_END (e.g. the window that
	 * started the paste lost focus, was closed, or the terminating
	 * marker was lost) must not permanently divert the prefix key to
	 * a different window's child process. */
	if (new_id != watched_id)
		paste_cancel();
}

/* sync watched_id / watching / vt globals from tilemgr's focused pane.
 * these globals are used by other files (prefix_menu.c, picker.c, etc.)
 * but tilemgr is the source of truth for focus. */
static void
tile_sync_focus(void)
{
	uint32_t id = tile_focused_id(tilemgr);
	struct client_window *cw;

	if (id) {
		sel_clear_on_focus_change(id);
		watched_id = id;
		watching = 1;
		cw = cwin_find(id);
		if (cw)
			vt = cw->vt;
	}
}

/* sync watched_id / watching / vt globals from wmgr's focused window.
 * analogous to tile_sync_focus() -- wmgr is the source of truth for
 * turbo mode focus, and these globals must track it for the taskbar
 * line, keybinds title, and window list formatting. */
static void
turbo_sync_focus(void)
{
	uint32_t id;
	struct client_window *cw;

	if (turbo_focused_id(&id)) {
		if (scrollback_mode && id != scrollback_win_id)
			scrollback_leave();
		sel_clear_on_focus_change(id);
		watched_id = id;
		watching = 1;
		cw = cwin_find(id);
		if (cw)
			vt = cw->vt;
	}
}

/* resize a window's VT and PTY to match its current wm geometry.
 * call after wm_toggle_maximize or any direct geometry change. */
static void
turbo_sync_size(uint32_t id)
{
	struct wm_window *win;
	struct client_window *cw;
	struct mconn *mc;

	win = wm_find(wmgr, id);
	if (!win)
		return;
	cw = cwin_find(id);
	if (cw)
		vt_state_resize(cw->vt, win->h, win->w);
	mc = mconn_find_by_pid((pid_t)id);
	if (mc)
		mconn_ipc_send_size(mc, IPC_MSG_WIN_RESIZE,
		    win->h, win->w);
}

/* send OSC 2 to set the host terminal's title bar */
static void
sync_host_title(const char *title)
{
	char buf[256];
	int len;

	if (title && title[0])
		len = snprintf(buf, sizeof(buf),
		    "\033]2;lumi - %s:%s\033\\",
		    session_name ? session_name : "", title);
	else
		len = snprintf(buf, sizeof(buf),
		    "\033]2;lumi - %s\033\\",
		    session_name ? session_name : "");
	if (len > 0 && len < (int)sizeof(buf)) {
		tio_write(STDOUT_FILENO, buf, (size_t)len);
		tio_flush(STDOUT_FILENO);
	}
}

/* reset host terminal title (empty OSC 2 restores default) */
static void
reset_host_title(void)
{
	tio_write(STDOUT_FILENO, "\033]2;\033\\", 6);
	tio_flush(STDOUT_FILENO);
}

/* sync the keybinds title context from the focused window's title.
 * called after focus changes so state-dependent bindings activate. */
static void
sync_keybinds_title(void)
{
	struct mconn *mc;

	if (!watching) {
		keys_set_title(keybinds, NULL);
		sync_host_title(NULL);
		return;
	}
	mc = mconn_find_by_pid((pid_t)watched_id);
	keys_set_title(keybinds, mc ? mc->title : NULL);
	sync_host_title(mc ? mc->title : NULL);
}

/* propagate VT title changes to mconn + wm_window immediately,
 * avoiding the round-trip through the sessdir filesystem. */
static void
sync_vt_title(uint32_t win_id, struct vt_state *st)
{
	struct mconn *mc;
	const char *vt_title;
	size_t len;

	if (!st)
		return;
	vt_title = vt_state_title(st);
	if (!vt_title)
		vt_title = "";
	/* a :title override, when set, replaces the program's title */
	vt_title = cwin_effective_title(cwin_find(win_id), vt_title);

	mc = mconn_find_by_pid((pid_t)win_id);
	if (!mc || strcmp(mc->title, vt_title) == 0)
		return;

	len = utf8_trunc(vt_title, sizeof(mc->title));
	memcpy(mc->title, vt_title, len);
	mc->title[len] = '\0';

	if (client_mode == CLIENT_MODE_TURBO && wmgr)
		wm_set_title(wmgr, win_id, vt_title);

	win_list_set_title(win_id, vt_title);
	win_list_format_taskbar();

	if (watching && win_id == watched_id)
		sync_keybinds_title();
}

/* sync wm windows with cwin list: add/remove wm windows to match */
static void
turbo_sync_windows(void)
{
	int i, n;

	n = win_list_count();

	/* add wm windows for cwins that don't have one */
	for (i = 0; i < n; i++) {
		uint32_t id = win_list_pid_at(i);
		struct client_window *cw = cwin_find(id);

		if (!cw)
			continue;
		if (wm_find(wmgr, id))
			continue;

		/* cascade new windows across the screen */
		{
			int w = content_cols / 2;
			int h = content_rows / 2;

			if (w < WM_MIN_WIDTH)
				w = WM_MIN_WIDTH;
			if (h < WM_MIN_HEIGHT)
				h = WM_MIN_HEIGHT;
			wm_add(wmgr, id, cw->vt,
			    cascade_x, cascade_y, w, h);
			wm_set_title(wmgr, id, win_list_title_at(i));

			/* resize client VT and server PTY to match */
			vt_state_resize(cw->vt, h, w);
			{
				struct mconn *smc;

				smc = mconn_find_by_pid((pid_t)id);
				if (smc)
					mconn_ipc_send_size(smc,
					    IPC_MSG_WIN_RESIZE, h, w);
			}

			cascade_x += TURBO_CASCADE_DX;
			cascade_y += TURBO_CASCADE_DY;
			/* wrap cascade */
			if (cascade_x + w + 2 > content_cols)
				cascade_x = 2;
			if (cascade_y + h + 2 > content_rows)
				cascade_y = 2;
		}
	}

	/* remove wm windows that no longer exist on server */
	for (i = wm_count(wmgr) - 1; i >= 0; i--) {
		struct wm_window *win = wm_window_at(wmgr, i);
		int j, found = 0;

		if (!win)
			continue;
		for (j = 0; j < n; j++) {
			if (win_list_pid_at(j) == win->id) {
				found = 1;
				break;
			}
		}
		if (!found)
			wm_remove(wmgr, win->id);
	}

	/* update titles */
	for (i = 0; i < n; i++) {
		uint32_t id = win_list_pid_at(i);
		struct wm_window *win = wm_find(wmgr, id);

		if (win)
			wm_set_title(wmgr, id, win_list_title_at(i));
	}
}

/* map window-order index to PID using sessdir state.
 * fills order[] with up to max PIDs, returns count. */
static int
layout_get_order(const char *session, pid_t *order, int max)
{
	struct sessdir_state *st;
	int n;

	st = sessdir_state_open(session);
	if (!st)
		return 0;
	n = sessdir_state_order(st, order, max);
	sessdir_state_close(st);
	return (n > 0) ? n : 0;
}

/* apply turbo layout: reposition wm windows from saved geometry.
 * call after turbo_sync_windows() has created the wm windows. */
static void
turbo_apply_layout(const char *session)
{
	struct sessdir_turbo_layout layout;
	pid_t order[SESSDIR_LAYOUT_MAX_WINS];
	int norder, i;

	if (sessdir_layout_load_turbo(session, &layout) < 0)
		return;

	norder = layout_get_order(session, order, SESSDIR_LAYOUT_MAX_WINS);
	if (norder == 0)
		return;

	for (i = 0; i < layout.nwins && i < norder; i++) {
		struct wm_window *win;
		struct client_window *cw;

		if (!layout.wins[i].valid)
			continue;
		win = wm_find(wmgr, (uint32_t)order[i]);
		if (!win)
			continue;

		wm_move(wmgr, win->id, layout.wins[i].x, layout.wins[i].y);
		/* resize via wm_find and direct field update, then
		 * resize the VT and notify the mserver */
		win->w = layout.wins[i].w;
		win->h = layout.wins[i].h;
		cw = cwin_find(win->id);
		if (cw) {
			vt_state_resize(cw->vt, win->h, win->w);
			{
				struct mconn *smc;

				smc = mconn_find_by_pid((pid_t)win->id);
				if (smc)
					mconn_ipc_send_size(smc,
					    IPC_MSG_WIN_RESIZE,
					    win->h, win->w);
			}
		}
	}

	/* apply saved focus */
	if (layout.focus >= 0 && layout.focus < norder) {
		uint32_t fid = (uint32_t)order[layout.focus];

		wm_focus(wmgr, fid);
		watched_id = fid;
		watching = 1;
		{
			struct client_window *cw;

			cw = cwin_find(fid);
			if (cw)
				vt = cw->vt;
		}
	}
}

/* recursively convert sessdir_tree_node to tile_node */
static struct tile_node *
screen_build_tile_tree(const struct sessdir_tree_node *sn,
    const pid_t *order, int norder,
    struct client_window *(*find_cw)(uint32_t))
{
	struct tile_node *tn;

	if (!sn)
		return NULL;

	tn = xcalloc(1, sizeof(*tn));

	if (sn->type == SESSDIR_TREE_LEAF) {
		tn->type = TILE_LEAF;
		if (sn->win_index >= 0 && sn->win_index < norder) {
			uint32_t id = (uint32_t)order[sn->win_index];
			struct client_window *cw = find_cw(id);

			tn->window_id = id;
			tn->vt = cw ? cw->vt : NULL;
		}
		return tn;
	}

	tn->type = (sn->type == SESSDIR_TREE_SPLIT_H)
	    ? TILE_SPLIT_H : TILE_SPLIT_V;
	tn->split_pos = sn->split_pos;
	tn->a = screen_build_tile_tree(sn->a, order, norder, find_cw);
	tn->b = screen_build_tile_tree(sn->b, order, norder, find_cw);

	if (!tn->a || !tn->b) {
		/* partial tree -- fall back to default */
		if (tn->a) free(tn->a);
		if (tn->b) free(tn->b);
		free(tn);
		return NULL;
	}
	return tn;
}

/* apply screen layout: build tile tree from saved splits.
 * returns 0 on success, -1 to fall back to default single-pane. */
static int
screen_apply_layout(const char *session)
{
	struct sessdir_screen_layout layout;
	pid_t order[SESSDIR_LAYOUT_MAX_WINS];
	int norder;
	struct tile_node *root;

	if (sessdir_layout_load_screen(session, &layout) < 0)
		return -1;

	norder = layout_get_order(session, order, SESSDIR_LAYOUT_MAX_WINS);
	if (norder == 0) {
		sessdir_tree_free(layout.root);
		return -1;
	}

	root = screen_build_tile_tree(layout.root, order, norder, cwin_find);
	sessdir_tree_free(layout.root);

	if (!root)
		return -1;

	tilemgr = tile_new(content_rows, content_cols);
	if (tile_import_tree(tilemgr, root) < 0) {
		/* root was not consumed on error -- free it */
		free(root);
		return -1;
	}

	/* apply saved focus */
	if (layout.focus >= 0 && layout.focus < norder) {
		uint32_t fid = (uint32_t)order[layout.focus];

		tile_focus(tilemgr, fid);
		watched_id = fid;
		watching = 1;
		{
			struct client_window *cw;

			cw = cwin_find(fid);
			if (cw)
				vt = cw->vt;
		}
	} else {
		tile_focus(tilemgr, (uint32_t)order[0]);
	}

	return 0;
}

/* save current turbo layout to session directory */
static void
turbo_save_layout(const char *session)
{
	struct sessdir_turbo_layout layout;
	pid_t order[SESSDIR_LAYOUT_MAX_WINS];
	int norder, i;

	norder = layout_get_order(session, order, SESSDIR_LAYOUT_MAX_WINS);
	if (norder == 0)
		return;

	memset(&layout, 0, sizeof(layout));
	layout.focus = -1;

	for (i = 0; i < norder; i++) {
		struct wm_window *win;

		win = wm_find(wmgr, (uint32_t)order[i]);
		if (!win)
			continue;
		layout.wins[i].x = win->x;
		layout.wins[i].y = win->y;
		layout.wins[i].w = win->w;
		layout.wins[i].h = win->h;
		layout.wins[i].valid = 1;
		if (i >= layout.nwins)
			layout.nwins = i + 1;
		if (win->focused)
			layout.focus = i;
	}

	sessdir_layout_save_turbo(session, &layout);
}

/* recursively convert tile_node to sessdir_tree_node */
static struct sessdir_tree_node *
screen_export_tree(const struct tile_node *tn,
    const pid_t *order, int norder)
{
	struct sessdir_tree_node *sn;
	int i;

	if (!tn)
		return NULL;

	sn = xcalloc(1, sizeof(*sn));

	if (tn->type == TILE_LEAF) {
		sn->type = SESSDIR_TREE_LEAF;
		sn->win_index = -1;
		for (i = 0; i < norder; i++) {
			if ((uint32_t)order[i] == tn->window_id) {
				sn->win_index = i;
				break;
			}
		}
		return sn;
	}

	sn->type = (tn->type == TILE_SPLIT_H)
	    ? SESSDIR_TREE_SPLIT_H : SESSDIR_TREE_SPLIT_V;
	sn->split_pos = tn->split_pos;
	sn->a = screen_export_tree(tn->a, order, norder);
	sn->b = screen_export_tree(tn->b, order, norder);
	return sn;
}

/* save current screen layout to session directory */
static void
screen_save_layout(const char *session)
{
	struct sessdir_screen_layout layout;
	struct tile_node *root;
	pid_t order[SESSDIR_LAYOUT_MAX_WINS];
	int norder, i;
	uint32_t fid;

	if (!tilemgr)
		return;

	norder = layout_get_order(session, order, SESSDIR_LAYOUT_MAX_WINS);
	if (norder == 0)
		return;

	memset(&layout, 0, sizeof(layout));
	layout.focus = -1;

	fid = tile_focused_id(tilemgr);
	for (i = 0; i < norder; i++) {
		if ((uint32_t)order[i] == fid) {
			layout.focus = i;
			break;
		}
	}

	root = tile_root(tilemgr);
	layout.root = screen_export_tree(root, order, norder);

	sessdir_layout_save_screen(session, &layout);
	sessdir_tree_free(layout.root);
}

/* ---- per-window layout snapshot / restore ---- */

/* save each window's current turbo geometry into its win_layout */
static void
snapshot_turbo_layouts(void)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		struct wm_window *win;

		win = wm_find(wmgr, cwins[i].id);
		if (!win)
			continue;
		cwins[i].layout.mode = CLIENT_MODE_TURBO;
		cwins[i].layout.x = win->x;
		cwins[i].layout.y = win->y;
		cwins[i].layout.w = win->w;
		cwins[i].layout.h = win->h;
		cwins[i].layout.valid = 1;
	}
}

/* save each window's current tile pane geometry into its win_layout */
static void
snapshot_screen_layouts(void)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		int x, y, w, h;

		if (tile_pane_geometry(tilemgr, cwins[i].id,
		    &x, &y, &w, &h) < 0)
			continue;
		cwins[i].layout.mode = CLIENT_MODE_SCREEN;
		cwins[i].layout.x = x;
		cwins[i].layout.y = y;
		cwins[i].layout.w = w;
		cwins[i].layout.h = h;
		cwins[i].layout.valid = 1;
	}
}

/* swap layout <-> prev_layout for all windows */
static void
swap_layouts(void)
{
	int i;

	for (i = 0; i < cwin_count; i++) {
		struct win_layout tmp;

		tmp = cwins[i].layout;
		cwins[i].layout = cwins[i].prev_layout;
		cwins[i].prev_layout = tmp;
	}
}

/* restore a window's turbo position from its layout if it was
 * previously placed by turbo.  returns 1 if restored, 0 if not. */
static int
restore_turbo_win(struct client_window *cw)
{
	struct wm_window *win;

	if (!cw->layout.valid || cw->layout.mode != CLIENT_MODE_TURBO)
		return 0;

	win = wm_find(wmgr, cw->id);
	if (!win)
		return 0;

	wm_move(wmgr, cw->id, cw->layout.x, cw->layout.y);
	win->w = cw->layout.w;
	win->h = cw->layout.h;
	vt_state_resize(cw->vt, win->h, win->w);
	{
		struct mconn *mc;

		mc = mconn_find_by_pid((pid_t)cw->id);
		if (mc)
			mconn_ipc_send_size(mc, IPC_MSG_WIN_RESIZE,
			    win->h, win->w);
	}
	return 1;
}

/* toggle between turbo and screen modes at runtime */
static void
mode_toggle(void)
{
	if (client_mode == CLIENT_MODE_MINIMAL)
		return;

	if (client_mode == CLIENT_MODE_TURBO) {
		snapshot_turbo_layouts();
		turbo_save_layout(session_name);
		swap_layouts();

		client_mode = CLIENT_MODE_SCREEN;

		if (!tilemgr) {
			if (screen_apply_layout(session_name) < 0) {
				tilemgr = tile_new(content_rows,
				    content_cols);
				tile_set_window(tilemgr, 0,
				    watched_id, vt);
				tile_focus(tilemgr, watched_id);
			}
		} else {
			/* tile tree exists from a previous toggle;
			 * assign windows not already in the tree */
			int i;

			for (i = 0; i < cwin_count; i++) {
				if (tile_pane_geometry(tilemgr,
				    cwins[i].id,
				    NULL, NULL, NULL, NULL) == 0)
					continue;
				tile_set_window(tilemgr, 0,
				    cwins[i].id, cwins[i].vt);
			}
		}

		tile_resize(tilemgr, content_rows, content_cols);
		overlay_repaint_fn = tiled_repaint;
		tile_each_pane(tilemgr, tiled_resize_pane_cb, NULL);
		screen_fit_all_windows();
		tile_focus(tilemgr, watched_id);
		tile_sync_focus();
		tile_need_full = 1;
	} else {
		snapshot_screen_layouts();
		if (tilemgr)
			screen_save_layout(session_name);
		swap_layouts();

		client_mode = CLIENT_MODE_TURBO;

		if (!wmgr)
			wmgr = wm_new(content_rows, content_cols);
		else
			wm_resize(wmgr, content_rows, content_cols);

		turbo_sync_windows();

		/* restore previously saved turbo positions */
		{
			int i;

			for (i = 0; i < cwin_count; i++)
				restore_turbo_win(&cwins[i]);
		}

		overlay_repaint_fn = turbo_repaint;
		wm_focus(wmgr, watched_id);
		turbo_sync_focus();
		turbo_need_full = 1;
	}

	need_render = 1;
	need_taskbar = 1;
}

/* find the focused wm_window's ID, return via out_id.
 * returns 1 if found, 0 if no focused window. */
static int
turbo_focused_id(uint32_t *out_id)
{
	int i, n;

	n = wm_count(wmgr);
	for (i = 0; i < n; i++) {
		struct wm_window *win = wm_window_at(wmgr, i);

		if (win && win->focused) {
			if (out_id)
				*out_id = win->id;
			return 1;
		}
	}
	return 0;
}

/* ---- micro-server helpers (need turbo state) ---- */

/* refresh each connection's stable window number from session state.
 * windows keep their number across other windows closing; a closed
 * window's number becomes a spare and is not reused until a new window
 * is created. */
static void
mconn_refresh_nums(void)
{
	struct sessdir_state *st;
	pid_t nums[CLIENT_WIN_MAX];
	int nnums, i, j;

	for (i = 0; i < mconn_count; i++)
		mconns[i].num = -1;

	st = sessdir_state_open(session_name);
	if (!st)
		return;
	nnums = sessdir_state_nums(st, nums, CLIENT_WIN_MAX);
	sessdir_state_close(st);

	for (j = 0; j < nnums; j++) {
		if (nums[j] == 0)
			continue;
		for (i = 0; i < mconn_count; i++) {
			if ((pid_t)mconns[i].pid == nums[j]) {
				mconns[i].num = j;
				break;
			}
		}
	}
}

/* find the connection whose number is adjacent to `from` in direction
 * `dir` (>0 next higher, <0 next lower). spares are skipped. when `wrap`
 * is set and no candidate lies in the direction, wraps to the extreme at
 * the other end. returns the pid, or 0 if none. */
static uint32_t
mconn_adjacent_pid(int from, int dir, int wrap)
{
	uint32_t best_pid = 0, wrap_pid = 0;
	int best = -1, wend = -1, i;

	for (i = 0; i < mconn_count; i++) {
		int n = mconns[i].num;

		if (n < 0)
			continue;

		/* wrap-around extreme: smallest for next, largest for prev */
		if (wend < 0 || (dir > 0 ? n < wend : n > wend)) {
			wend = n;
			wrap_pid = (uint32_t)mconns[i].pid;
		}

		/* nearest candidate strictly in the requested direction */
		if (dir > 0 ? n > from : n < from) {
			if (best < 0 || (dir > 0 ? n < best : n > best)) {
				best = n;
				best_pid = (uint32_t)mconns[i].pid;
			}
		}
	}

	if (best >= 0)
		return best_pid;
	return wrap ? wrap_pid : 0;
}

/* swap the current window's stable number with the next lower (dir<0)
 * or next higher (dir>0) numbered window. does not wrap around. */
static void
micro_swap_num(int dir)
{
	struct sessdir_state *st;
	uint32_t other;
	int i, cur_num = -1;

	if (!watching || mconn_count == 0)
		return;

	mconn_refresh_nums();

	for (i = 0; i < mconn_count; i++) {
		if ((uint32_t)mconns[i].pid == watched_id) {
			cur_num = mconns[i].num;
			break;
		}
	}
	if (cur_num < 0)
		return;

	other = mconn_adjacent_pid(cur_num, dir, 0);
	if (other == 0 || other == watched_id)
		return;

	st = sessdir_state_open(session_name);
	if (st) {
		sessdir_state_swap_num(st, (pid_t)watched_id, (pid_t)other);
		sessdir_state_close(st);
	}

	mconn_sync_winlist();
	need_taskbar = 1;
}

/* renumber the current window to the stable number target.  if target is
 * taken the two windows swap numbers, otherwise the window moves into the
 * free slot.  returns 0 on success, -1 on error. */
static int
micro_set_num(int target)
{
	struct sessdir_state *st;
	int rc;

	if (!watching || mconn_count == 0)
		return -1;

	st = sessdir_state_open(session_name);
	if (!st)
		return -1;
	rc = sessdir_state_set_num(st, (pid_t)watched_id, target);
	sessdir_state_close(st);
	if (rc < 0)
		return -1;

	mconn_sync_winlist();
	need_taskbar = 1;
	return 0;
}

static void
mconn_sync_winlist(void)
{
	int i, k, ord[CLIENT_WIN_MAX], n = 0;

	mconn_refresh_nums();

	/* present windows in ascending stable-number order so the tab bar
	 * reads 0,1,2,... with gaps where numbers are spare; windows with
	 * no number yet sort to the end. */
	for (i = 0; i < mconn_count; i++)
		ord[n++] = i;
	for (i = 1; i < n; i++) {
		int t = ord[i], j = i - 1;
		int tn = mconns[t].num < 0 ? INT_MAX : mconns[t].num;

		while (j >= 0 &&
		    (mconns[ord[j]].num < 0 ? INT_MAX : mconns[ord[j]].num)
		    > tn) {
			ord[j + 1] = ord[j];
			j--;
		}
		ord[j + 1] = t;
	}

	win_list_clear();
	for (k = 0; k < n; k++) {
		struct client_window *cw;
		char *title;
		const char *use, *live;
		char label[140];

		i = ord[k];
		cw = cwin_find((uint32_t)mconns[i].pid);

		/* The client's live VT state is authoritative for the
		 * on-screen title and is at least as fresh as the sessdir
		 * file (the server writes the file from the same stream we
		 * parse, before forwarding it to us). Prefer it, and use the
		 * sessdir file only to seed a window we have not yet parsed a
		 * title for. Reading the stale file unconditionally here is
		 * what made switched-away tabs revert to old titles. */
		title = sessdir_read_file(session_name, mconns[i].pid,
		    "title");
		live = (cw && cw->vt) ? vt_state_title(cw->vt) : NULL;
		use = (live && live[0]) ? live : title;
		use = cwin_effective_title(cw, use);
		if (use) {
			size_t len = utf8_trunc(use,
			    sizeof(mconns[i].title));
			memcpy(mconns[i].title, use, len);
			mconns[i].title[len] = '\0';
		}
		free(title);
		snprintf(label, sizeof(label), "%s%s%s%s",
		    mconns[i].title,
		    (cw && cw->dead) ? " [dead]" : "",
		    (cw && cw->scroll_lock) ? " [scroll-lock]" : "",
		    (cw && cw->input_lock) ? " [input-lock]" : "");
		win_list_add((uint32_t)(mconns[i].num < 0 ? 0 : mconns[i].num),
		    (uint32_t)mconns[i].pid, label,
		    watching && (uint32_t)mconns[i].pid == watched_id);
	}
	win_list_format_taskbar();
	sync_keybinds_title();
}

/* choose the window to focus at attach time: prefer the session's
 * recorded foreground window, then the first window in stable order,
 * then the first discovered connection. */
static uint32_t
mconn_initial_focus(void)
{
	struct sessdir_state *st;
	pid_t focus = 0;
	pid_t order[CLIENT_WIN_MAX];
	int norder = 0, i;

	st = sessdir_state_open(session_name);
	if (st) {
		focus = sessdir_state_focus(st);
		norder = sessdir_state_order(st, order, CLIENT_WIN_MAX);
		sessdir_state_close(st);
	}

	/* recorded foreground window, if still connected */
	if (focus > 0 && mconn_find_by_pid((pid_t)focus))
		return (uint32_t)focus;

	/* otherwise the first window in stable order that is connected */
	for (i = 0; i < norder; i++) {
		if (mconn_find_by_pid(order[i]))
			return (uint32_t)order[i];
	}

	/* fallback: first discovered connection */
	return (uint32_t)mconns[0].pid;
}

/* cycle focus to next/prev mserver window.
 * dir > 0 = next, dir < 0 = prev. */
static void
micro_cycle_focus(int dir)
{
	int i, cur_num = -1, from;

	if (mconn_count == 0)
		return;

	mconn_refresh_nums();

	/* current window's stable number */
	if (watching) {
		for (i = 0; i < mconn_count; i++) {
			if ((uint32_t)mconns[i].pid == watched_id) {
				cur_num = mconns[i].num;
				break;
			}
		}
	}

	/* not watching: start past the appropriate extreme so the first
	 * window in the direction is chosen. */
	if (cur_num < 0)
		from = (dir > 0) ? -1 : INT_MAX;
	else
		from = cur_num;

	{
		uint32_t next = mconn_adjacent_pid(from, dir, 1);

		if (next == 0)
			return;
		sel_clear_on_focus_change(next);
		watched_id = next;
	}
	watching = 1;

	{
		struct client_window *cw = cwin_find(watched_id);

		if (cw)
			vt = cw->vt;
	}

	if (client_mode == CLIENT_MODE_TURBO) {
		wm_unminimize(wmgr, watched_id);
		wm_focus(wmgr, watched_id);
	} else {
		/* swap the focused pane to show the selected window */
		uint32_t old_id = tile_focused_id(tilemgr);

		tile_set_window(tilemgr, old_id, watched_id, vt);
		tile_focus(tilemgr, watched_id);
		screen_fit_window(watched_id);
		tile_need_full = 1;
	}
	need_render = 1;
	need_taskbar = 1;
	sync_keybinds_title();

	mconn_sync_winlist();
}

/* select a window by ID (from picker or numeric select).
 * looks up the mconn by pid; does nothing if not found. */
void
micro_select_window(uint32_t id)
{
	struct mconn *mc;
	struct client_window *cw;

	mc = mconn_find_by_pid((pid_t)id);
	if (!mc)
		return;

	sel_clear_on_focus_change(id);
	watched_id = id;
	watching = 1;

	cw = cwin_find(id);
	if (cw)
		vt = cw->vt;

	if (client_mode == CLIENT_MODE_TURBO) {
		wm_unminimize(wmgr, id);
		wm_focus(wmgr, id);
	} else {
		/* swap the focused pane to show the selected window */
		uint32_t old_id = tile_focused_id(tilemgr);

		tile_set_window(tilemgr, old_id, id, vt);
		tile_focus(tilemgr, id);
		screen_fit_window(id);
		tile_need_full = 1;
	}
	need_render = 1;
	need_taskbar = 1;
	sync_keybinds_title();

	mconn_sync_winlist();
}

/* ---- command input line (Ctrl-A :) ---- */

/* 0-based screen row where the command line and taskbar sit. */
static int
cmdline_row(void)
{
	return taskbar_visible ? content_rows : content_rows - 1;
}

/* draw the prompt line: ':' + input (or the result message) at the
 * bottom row, clearing the rest of the line. */
static void
cmdline_render(void)
{
	const char *body = cmd_msg[0] ? cmd_msg : cmd_buf;
	char lead = cmd_msg[0] ? ' ' : ':';

	/* The raw tio_write() calls below advance the terminal cursor
	 * without the renderer knowing, so its tracked position is stale
	 * by the time we return.  Invalidate first, otherwise a second
	 * cmdline_render() in the same frame finds the tracked position
	 * already at (row, 0), skips the move, and redraws the prompt at
	 * the wrong column -- showing the text twice. */
	render_invalidate_cursor(renderer);
	render_move_cursor(renderer, STDOUT_FILENO, cmdline_row(), 0);
	tio_write(STDOUT_FILENO, "\033[0m", 4);
	tio_write(STDOUT_FILENO, &lead, 1);
	if (body[0])
		tio_write(STDOUT_FILENO, body, strlen(body));
	tio_write(STDOUT_FILENO, "\033[K", 3);
	tio_flush(STDOUT_FILENO);
}

/* parse and run a submitted directive.  on success cmd_msg is left
 * empty (caller hides the line); on error cmd_msg holds a message and
 * the line stays open. */
static void
cmdline_execute(const char *line)
{
	char word[64];
	const char *p = line;
	size_t i;

	cmd_msg[0] = '\0';

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return;

	/* first word = directive */
	for (i = 0; *p && *p != ' ' && *p != '\t' && i < sizeof(word) - 1; i++)
		word[i] = *p++;
	word[i] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;

	if (strcmp(word, "setenv") == 0 || strcmp(word, "unsetenv") == 0) {
		char var[128];
		int unset = (word[0] == 'u');

		for (i = 0; *p && *p != ' ' && *p != '\t' &&
		    i < sizeof(var) - 1; i++)
			var[i] = *p++;
		var[i] = '\0';
		if (!var[0]) {
			snprintf(cmd_msg, sizeof(cmd_msg),
			    "usage: %.40s VAR%s", word,
			    unset ? "" : " value");
			return;
		}
		if (unset) {
			unsetenv(var);
		} else {
			char val[320];
			size_t vlen;

			while (*p == ' ' || *p == '\t')
				p++;
			vlen = strlen(p);
			/* strip one layer of surrounding double quotes */
			if (vlen >= 2 && p[0] == '"' && p[vlen - 1] == '"') {
				p++;
				vlen -= 2;
			}
			if (vlen >= sizeof(val))
				vlen = sizeof(val) - 1;
			memcpy(val, p, vlen);
			val[vlen] = '\0';
			setenv(var, val, 1);
		}
		return;
	}

	if (strcmp(word, "title") == 0) {
		struct client_window *cw = cwin_focused();
		size_t tlen = strlen(p);

		if (!cw) {
			snprintf(cmd_msg, sizeof(cmd_msg), "no window");
			return;
		}
		if (tlen >= 2 && p[0] == '"' && p[tlen - 1] == '"') {
			p++;
			tlen -= 2;
		}
		if (tlen >= sizeof(cw->title_override))
			tlen = sizeof(cw->title_override) - 1;
		/* set a client-side override; an empty argument clears it so
		 * the program's live VT title shows through again. */
		memcpy(cw->title_override, p, tlen);
		cw->title_override[tlen] = '\0';
		sync_vt_title(cw->id, cw->vt);
		mconn_sync_winlist();
		return;
	}

	if (strcmp(word, "number") == 0 || strcmp(word, "renumber") == 0) {
		char *end;
		long target;

		if (!*p) {
			snprintf(cmd_msg, sizeof(cmd_msg),
			    "usage: %.20s N", word);
			return;
		}
		target = strtol(p, &end, 10);
		while (*end == ' ' || *end == '\t')
			end++;
		if (end == p || *end || target < 0) {
			snprintf(cmd_msg, sizeof(cmd_msg),
			    "invalid number: %.80s", p);
			return;
		}
		if (micro_set_num((int)target) < 0)
			snprintf(cmd_msg, sizeof(cmd_msg),
			    "cannot renumber to %ld", target);
		return;
	}

	snprintf(cmd_msg, sizeof(cmd_msg), "unknown command: %.90s", word);
}

static void
cmdline_hide(void)
{
	cmdline_visible = 0;
	cmd_buf[0] = '\0';
	cmd_len = 0;
	cmd_msg[0] = '\0';
	turbo_need_full = 1;
	tile_need_full = 1;
	need_render = 1;
	need_taskbar = 1;
}

static void
cmdline_show(void)
{
	cmdline_visible = 1;
	cmd_buf[0] = '\0';
	cmd_len = 0;
	cmd_msg[0] = '\0';
	cmdline_render();
	render_move_cursor(renderer, STDOUT_FILENO, cmdline_row(), 1);
	set_cursor_vis(1);
	tio_flush(STDOUT_FILENO);
}

static void
cmdline_input(const struct tkbd_seq *seq)
{
	/* a pending result/error message: any key dismisses or edits */
	if (cmd_msg[0]) {
		cmd_msg[0] = '\0';
		if (seq->key == TKBD_KEY_ESC || seq->key == TKBD_KEY_ENTER) {
			cmdline_hide();
			return;
		}
	}

	if (seq->key == TKBD_KEY_ESC) {
		cmdline_hide();
		return;
	}
	if (seq->key == TKBD_KEY_ENTER) {
		cmdline_execute(cmd_buf);
		if (!cmd_msg[0]) {
			cmdline_hide();
			return;
		}
		/* keep the line open to show the message */
		cmdline_render();
		set_cursor_vis(0);
		return;
	}
	if (seq->key == TKBD_KEY_BACKSPACE ||
	    seq->key == TKBD_KEY_BACKSPACE2) {
		/* drop one UTF-8 character (trailing continuation bytes) */
		while (cmd_len > 0 &&
		    (cmd_buf[cmd_len - 1] & 0xC0) == 0x80)
			cmd_len--;
		if (cmd_len > 0)
			cmd_len--;
		cmd_buf[cmd_len] = '\0';
	} else if (seq->len > 0 && (unsigned char)seq->data[0] >= 0x20 &&
	    (unsigned char)seq->data[0] != 0x7F) {
		/* append the raw bytes (handles UTF-8 input) */
		if (cmd_len + (size_t)seq->len < sizeof(cmd_buf)) {
			memcpy(cmd_buf + cmd_len, seq->data, (size_t)seq->len);
			cmd_len += (size_t)seq->len;
			cmd_buf[cmd_len] = '\0';
		}
	} else {
		return;	/* ignore other control keys */
	}

	cmdline_render();
	render_move_cursor(renderer, STDOUT_FILENO, cmdline_row(),
	    1 + (int)cmd_len);
	set_cursor_vis(1);
	tio_flush(STDOUT_FILENO);
}

/* ---- action dispatch ---- */

static void sel_paste(void);

void
dispatch_action(struct iox_loop *loop, enum keys_action action)
{
	uint8_t prefix;

	switch (action) {
	case KEYS_ACTION_SEND_PREFIX:
		prefix = keys_get_prefix(keybinds);
		{
			struct mconn *mc = mconn_focused();
			struct client_window *cw = cwin_focused();

			if (mc && !(cw && cw->input_lock))
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    &prefix, 1);
		}
		break;
	case KEYS_ACTION_NEW_WINDOW:
		micro_spawn_window(loop, session_name);
		break;
	case KEYS_ACTION_NEXT_WINDOW:
		micro_cycle_focus(1);
		break;
	case KEYS_ACTION_PREV_WINDOW:
		micro_cycle_focus(-1);
		break;
	case KEYS_ACTION_LAST_WINDOW:
		{
			uint32_t prev = mru_prev();

			if (prev)
				micro_select_window(prev);
		}
		break;
	case KEYS_ACTION_SWAP_NUM_LOWER:
		micro_swap_num(-1);
		break;
	case KEYS_ACTION_SWAP_NUM_HIGHER:
		micro_swap_num(1);
		break;
	case KEYS_ACTION_SELECT_0:
	case KEYS_ACTION_SELECT_1:
	case KEYS_ACTION_SELECT_2:
	case KEYS_ACTION_SELECT_3:
	case KEYS_ACTION_SELECT_4:
	case KEYS_ACTION_SELECT_5:
	case KEYS_ACTION_SELECT_6:
	case KEYS_ACTION_SELECT_7:
	case KEYS_ACTION_SELECT_8:
	case KEYS_ACTION_SELECT_9:
		{
			int num = (int)(action - KEYS_ACTION_SELECT_0);
			int i;

			mconn_refresh_nums();
			for (i = 0; i < mconn_count; i++) {
				if (mconns[i].num == num) {
					micro_select_window(
					    (uint32_t)mconns[i].pid);
					break;
				}
			}
		}
		break;
	case KEYS_ACTION_KILL_WINDOW:
		{
			struct mconn *mc = mconn_focused();

			if (mc) {
				mconn_ipc_send_empty(mc, IPC_MSG_KILL);
			} else {
				/* dead window: remove directly */
				struct client_window *cw;

				cw = cwin_focused();
				if (cw && cw->dead)
					remove_window(loop, cw->id);
			}
		}
		break;
	case KEYS_ACTION_DETACH:
		mconn_disconnect_all(loop);
		iox_loop_stop(loop);
		break;
	case KEYS_ACTION_WINDOW_LIST:
		if (client_mode != CLIENT_MODE_MINIMAL)
			picker_show();
		break;
	case KEYS_ACTION_TASKBAR_TOGGLE:
		if (client_mode == CLIENT_MODE_MINIMAL)
			break;
		{
			struct winsize ws;

			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0)
				break;
			taskbar_visible = !taskbar_visible;
			update_content_size(ws.ws_row, ws.ws_col);
			render_resize(renderer, content_rows,
			    content_cols);

			if (client_mode == CLIENT_MODE_TURBO) {
				wm_resize(wmgr, content_rows,
				    content_cols);
				for (int i = 0; i < wm_count(wmgr); i++) {
					struct wm_window *w;
					w = wm_window_at(wmgr, i);
					if (w && w->maximized) {
						w->x = 1;
						w->y = 1;
						w->w = content_cols - 2;
						w->h = content_rows - 2;
						turbo_sync_size(w->id);
					}
				}
				turbo_need_full = 1;
			} else {
				tile_resize(tilemgr,
				    content_rows, content_cols);
				tile_each_pane(tilemgr,
				    tiled_resize_pane_cb, NULL);
				tile_need_full = 1;
			}
			mconn_sync_winlist();
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_APPS_MENU:
		if (client_mode != CLIENT_MODE_MINIMAL)
			apps_menu_show();
		break;
	case KEYS_ACTION_SPLIT_H:
	case KEYS_ACTION_SPLIT_V:
		if (client_mode != CLIENT_MODE_SCREEN)
			break;
		{
			enum tile_split dir = (action == KEYS_ACTION_SPLIT_H)
			    ? TILE_SPLIT_H : TILE_SPLIT_V;

			{
				uint32_t fid = tile_focused_id(tilemgr);

				micro_spawn_window(loop, session_name);
				tile_split(tilemgr, fid, dir, 0, NULL);
				tile_each_pane(tilemgr,
				    tiled_resize_pane_cb, NULL);
				tile_need_full = 1;
				need_render = 1;
				need_taskbar = 1;
			}
		}
		break;
	case KEYS_ACTION_NEXT_PANE:
		if (tile_pane_count(tilemgr) > 1) {
			tile_focus_next(tilemgr);
			tile_sync_focus();
			tile_need_full = 1;
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_PREV_PANE:
		if (tile_pane_count(tilemgr) > 1) {
			tile_focus_prev(tilemgr);
			tile_sync_focus();
			tile_need_full = 1;
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_CLOSE_PANE:
		if (tile_pane_count(tilemgr) <= 1)
			break;	/* can't close the only pane */
		{
			uint32_t fid = tile_focused_id(tilemgr);

			tile_close(tilemgr, fid);
			tile_each_pane(tilemgr,
			    tiled_resize_pane_cb, NULL);
			tile_sync_focus();
			tile_need_full = 1;
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_RESIZE_PANE:
		if (tile_pane_count(tilemgr) > 1) {
			/* pane geometry is about to change under the
			 * selection; drop it */
			if (sel_active()) {
				sel_clear();
				tile_need_full = 1;
				need_render = 1;
			}
			resize_mode = 1;
		}
		break;
	case KEYS_ACTION_SCROLLBACK:
		if (!scrollback_mode)
			scrollback_enter();
		need_render = 1;
		need_taskbar = 1;
		break;
	case KEYS_ACTION_WINDOW_COLORS:
		if (client_mode == CLIENT_MODE_TURBO) {
			uint32_t fid;

			if (turbo_focused_id(&fid))
				color_picker_show(fid);
		}
		break;
	case KEYS_ACTION_MINIMIZE:
		if (client_mode == CLIENT_MODE_TURBO) {
			uint32_t fid;

			if (turbo_focused_id(&fid)) {
				wm_minimize(wmgr, fid);
				/* focus next visible window */
				micro_cycle_focus(1);
				need_render = 1;
				need_taskbar = 1;
			}
		}
		break;
	case KEYS_ACTION_MAXIMIZE:
		if (client_mode == CLIENT_MODE_TURBO) {
			uint32_t fid;

			if (turbo_focused_id(&fid)) {
				wm_toggle_maximize(wmgr, fid);
				turbo_sync_size(fid);
				need_render = 1;
			}
		}
		break;
	case KEYS_ACTION_SCROLL_LOCK:
		{
			struct client_window *cw;
			struct mconn *mc;

			cw = cwin_focused();
			if (!cw || cw->dead)
				break;
			cw->scroll_lock = !cw->scroll_lock;
			mc = mconn_focused();
			if (mc) {
				uint8_t pause = cw->scroll_lock ? 1 : 0;

				mconn_ipc_send(mc, IPC_MSG_FLOW_CTRL,
				    &pause, 1);
			}
			if (client_mode == CLIENT_MODE_TURBO) {
				struct wm_window *w;

				w = wm_find(wmgr, cw->id);
				if (w) {
					if (cw->scroll_lock)
						wm_set_flags(wmgr, cw->id,
						    w->flags |
						    WM_WIN_SCROLL_LOCK);
					else
						wm_set_flags(wmgr, cw->id,
						    w->flags &
						    ~WM_WIN_SCROLL_LOCK);
				}
			}
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_INPUT_LOCK:
		{
			struct client_window *cw;

			cw = cwin_focused();
			if (!cw || cw->dead)
				break;
			cw->input_lock = !cw->input_lock;
			if (client_mode == CLIENT_MODE_TURBO) {
				struct wm_window *w;

				w = wm_find(wmgr, cw->id);
				if (w) {
					if (cw->input_lock)
						wm_set_flags(wmgr, cw->id,
						    w->flags |
						    WM_WIN_INPUT_LOCK);
					else
						wm_set_flags(wmgr, cw->id,
						    w->flags &
						    ~WM_WIN_INPUT_LOCK);
				}
			}
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	case KEYS_ACTION_SESSION_LIST:
		if (client_mode != CLIENT_MODE_MINIMAL)
			session_picker_show();
		break;
	case KEYS_ACTION_PASTE:
		sel_paste();
		break;
	case KEYS_ACTION_CLIPBOARD_SYNC:
		sel_clipboard_sync();
		break;
	case KEYS_ACTION_TOGGLE_MODE:
		mode_toggle();
		break;
	case KEYS_ACTION_COMMAND:
		cmdline_show();
		break;
	case KEYS_ACTION_REDISPLAY:
		/* re-request every window's screen and force a full local
		 * repaint, so a pane left stale (e.g. an alt-screen app that
		 * did not repaint) is resynced on demand */
		mconn_request_refresh_all();
		turbo_need_full = 1;
		tile_need_full = 1;
		taskbar_invalidate();
		need_render = 1;
		need_taskbar = 1;
		break;
	case KEYS_ACTION_ARRANGE_GRID:
		if (client_mode == CLIENT_MODE_TURBO && wmgr) {
			int gi;

			/* windows are about to move; drop a stale selection */
			if (sel_active())
				sel_clear();
			wm_arrange_grid(wmgr);
			for (gi = 0; gi < wm_count(wmgr); gi++) {
				struct wm_window *gw;

				gw = wm_window_at(wmgr, gi);
				if (gw && !gw->minimized)
					turbo_sync_size(gw->id);
			}
			turbo_save_layout(session_name);
			turbo_need_full = 1;
			need_render = 1;
			need_taskbar = 1;
		}
		break;
	default:
		break;
	}
}

/* ---- color picker helpers ---- */

int color_picker_visible;

void
color_picker_set_theme(uint32_t id, const struct tui_theme *t)
{
	wm_set_theme(wmgr, id, t);
	need_render = 1;
}

const struct tui_theme *
color_picker_get_theme(uint32_t id)
{
	return wm_get_theme(wmgr, id);
}

/* ---- mouse handling ---- */

static int
focused_wants_mouse(void)
{
	struct client_window *cw = cwin_focused();

	return cw && cw->vt && (cw->vt->modes & VT_MODE_MOUSE);
}

/* forward a saved press event to the PTY as a click-through */
static void
sel_forward_press(void)
{
	struct mconn *mc = mconn_focused();

	if (sel_press_seq.len > 0 && mc && focused_wants_mouse())
		mconn_ipc_send(mc, IPC_MSG_INPUT,
		    sel_press_seq.data, (uint32_t)sel_press_seq.len);
}

static int
focused_wants_bracketed_paste(void)
{
	struct client_window *cw = cwin_focused();

	return cw && cw->vt && (cw->vt->modes & VT_MODE_BRACKETPASTE);
}

/* paste last copied text into focused PTY, wrapping with bracketed
 * paste only when the child has requested it */
static void
sel_paste(void)
{
	const char *buf;
	size_t len;
	struct mconn *mc;

	buf = sel_copy_buf();
	len = sel_copy_len();
	if (len == 0)
		return;
	mc = mconn_focused();
	if (!mc)
		return;

	if (focused_wants_bracketed_paste())
		mconn_ipc_send(mc, IPC_MSG_INPUT, "\033[200~", 6);
	mconn_ipc_send(mc, IPC_MSG_INPUT, buf, (uint32_t)len);
	if (focused_wants_bracketed_paste())
		mconn_ipc_send(mc, IPC_MSG_INPUT, "\033[201~", 6);

	/* the highlighted selection has served its purpose */
	if (sel_active())
		sel_clear();
}

/* feed child output into a window's terminal state.  if the window owns
 * the active selection and this output scrolls its content, clear the
 * now-stale selection so the highlight does not linger over new text. */
static void
cwin_feed_output(struct client_window *cw, uint32_t win_id,
    const char *buf, uint32_t len)
{
	unsigned gen0 = 0;
	int watch;

	watch = sel_active() && sel_owner() == cw->id && cw->vt->buf;
	if (watch)
		gen0 = vt_buf_scroll_gen(cw->vt->buf);

	predict_confirm(cw->pred, cw->vt, buf, len);
	vt_parse_feed(cw->parser, buf, len);
	sync_vt_title(win_id, cw->vt);

	if (watch && vt_buf_scroll_gen(cw->vt->buf) != gen0)
		sel_clear();
}

/* left-click on the taskbar, when it is showing the window-list,
 * switches focus to the clicked tab, the same way the window-select
 * hotkey does. returns 1 if the click was on the taskbar and has been
 * fully handled (hit or miss), 0 if the caller should process it as a
 * normal content-area click. */
static int
handle_tabbar_click(int row, int col, const struct tkbd_seq *seq)
{
	uint32_t pid;
	int wcol;

	if (!taskbar_visible || seq->key != TKBD_MOUSE_LEFT ||
	    (seq->mod & TKBD_MOD_MOTION))
		return 0;
	if (row != (taskbar_get_position(taskbar) ? 0 : content_rows))
		return 0;

	/* the window-list variable is assumed to lead the taskbar format
	 * (the default), preceded only by the fixed watch-degraded
	 * warning; a customized taskbar.format with other text before
	 * ${window-list} will make clicks land on the wrong tab. */
	wcol = col - (sessdir_watch_degraded ?
	    (int)strlen("[!watch] ") : 0);
	if (wcol >= 0 && win_list_hit_test(wcol, &pid))
		micro_select_window(pid);
	return 1;
}

static void
handle_mouse_turbo(struct iox_loop *loop, const struct tkbd_seq *seq)
{
	int row, col;
	uint32_t id;
	enum wm_hit area;

	/* tkbd converts SGR 1-based coords to 0-based */
	row = seq->y;
	col = seq->x;

	if (handle_tabbar_click(row, col, seq))
		return;

	/* motion events (button held + move) */
	if (seq->mod & TKBD_MOD_MOTION) {
		/* text selection: deferred press -> begin drag */
		if (sel_pending) {
			struct wm_window *sw;

			sw = wm_find(wmgr, sel_press_id);
			if (sw) {
				if (row < sw->y)
					row = sw->y;
				else if (row >= sw->y + sw->h)
					row = sw->y + sw->h - 1;
				if (col < sw->x)
					col = sw->x;
				else if (col >= sw->x + sw->w)
					col = sw->x + sw->w - 1;
				sel_begin(sel_press_id, sel_press_row,
				    sel_press_col, sw->x, sw->w);
			} else {
				sel_begin(sel_press_id, sel_press_row,
				    sel_press_col, 0, wm_cols(wmgr));
			}
			sel_update(row, col);
			sel_pending = 0;
			need_render = 1;
			return;
		}
		if (sel_active()) {
			struct wm_window *sw;

			sw = wm_find(wmgr, sel_press_id);
			if (sw) {
				if (row < sw->y)
					row = sw->y;
				else if (row >= sw->y + sw->h)
					row = sw->y + sw->h - 1;
				if (col < sw->x)
					col = sw->x;
				else if (col >= sw->x + sw->w)
					col = sw->x + sw->w - 1;
			}
			switch (sel_get_mode()) {
			case SEL_MODE_WORD:
				sel_update_word(row, col,
				    wm_screen(wmgr), wm_cols(wmgr));
				break;
			case SEL_MODE_LINE:
				sel_update_line(row);
				break;
			default:
				sel_update(row, col);
				break;
			}
			need_render = 1;
			return;
		}

		if (wm_mouse_drag(wmgr, row, col)) {
			/* if resizing, update VT + PTY in real-time */
			if (wm_drag_state(wmgr) == WM_DRAG_RESIZING) {
				struct wm_window *win;
				uint32_t did = wm_drag_id(wmgr);

				win = wm_find(wmgr, did);
				if (win) {
					struct client_window *cw;

					cw = cwin_find(did);
					if (cw)
						vt_state_resize(cw->vt,
						    win->h, win->w);
					{
						struct mconn *dmc;

						dmc = mconn_find_by_pid(
						    (pid_t)did);
						if (dmc)
							mconn_ipc_send_size(
							    dmc,
							    IPC_MSG_WIN_RESIZE,
							    win->h, win->w);
					}
				}
			}
			if (wm_drag_state(wmgr) == WM_DRAG_SCROLLING &&
			    scrollback_mode) {
				struct wm_window *win;
				uint32_t did = wm_drag_id(wmgr);

				win = wm_find(wmgr, did);
				if (win) {
					struct client_window *cw;

					cw = cwin_find(did);
					if (cw && cw->vt) {
						struct vt_buf *p;
						int vis, total, max_off;

						p = cw->vt->targets[
						    VT_TARGET_PRIMARY];
						vis = vt_buf_rows(p);
						total =
						    vt_buf_scrollback_lines(
						    p) + vis;
						max_off = total - vis;
						if (max_off > 0)
							scrollback_offset =
							    (int)((1.0f -
							    win->scroll_pos) *
							    (float)max_off +
							    0.5f);
						else
							scrollback_offset = 0;
						if (scrollback_offset < 0)
							scrollback_offset = 0;
					}
				}
				need_taskbar = 1;
			}
			need_render = 1;
		}
		return;
	}

	/* mouse wheel: scroll history, or forward to child in alt screen */
	if (seq->key == TKBD_MOUSE_WHEEL_UP ||
	    seq->key == TKBD_MOUSE_WHEEL_DOWN) {
		struct client_window *fcw = cwin_focused();

		if (sel_active()) {
			sel_clear();
			turbo_need_full = 1;
		}
		if (fcw && fcw->vt &&
		    fcw->vt->active_target != VT_TARGET_ALT) {
			if (seq->key == TKBD_MOUSE_WHEEL_UP) {
				if (!scrollback_mode)
					scrollback_enter();
				scrollback_offset += 3;
			} else {
				if (scrollback_mode) {
					scrollback_offset -= 3;
					if (scrollback_offset <= 0)
						scrollback_leave();
				}
			}
			need_render = 1;
			need_taskbar = 1;
		} else if (fcw && fcw->vt && focused_wants_mouse()) {
			struct mconn *mc = mconn_focused();

			if (seq->len > 0 && mc)
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    seq->data, (uint32_t)seq->len);
		}
		return;
	}

	if (seq->key == TKBD_MOUSE_LEFT) {
		enum wm_drag drag;

		/* clear previous selection on new click */
		if (sel_active()) {
			sel_clear();
			need_render = 1;
		}

		drag = wm_mouse_press(wmgr, row, col, &id, &area);
		/* wm_mouse_press focuses the clicked window --
		 * sync globals so watched_id/vt track wmgr */
		if (area != WM_HIT_NONE) {
			turbo_sync_focus();
			mconn_sync_winlist();
			need_render = 1;
			need_taskbar = 1;
		}
		if (area == WM_HIT_CLOSE) {
			struct mconn *cmc;

			cmc = mconn_find_by_pid((pid_t)id);
			if (cmc) {
				mconn_ipc_send_empty(cmc,
				    IPC_MSG_KILL);
			} else {
				/* dead window: remove directly */
				struct client_window *cw;

				cw = cwin_find(id);
				if (cw && cw->dead)
					remove_window(loop, id);
			}
			return;
		}
		if (area == WM_HIT_MINIMIZE) {
			wm_minimize(wmgr, id);
			micro_cycle_focus(1);
			need_render = 1;
			need_taskbar = 1;
			return;
		}
		if (area == WM_HIT_MAXIMIZE) {
			wm_toggle_maximize(wmgr, id);
			turbo_sync_size(id);
			need_render = 1;
			return;
		}
		if (area == WM_HIT_SCROLLBAR && scrollback_mode) {
			struct wm_window *win = wm_find(wmgr, id);

			if (win) {
				int fy = win->y - 1;
				int arrow_top = fy + 1;
				int arrow_bot = fy + win->h;

				if (win->h >= 4 &&
				    row == arrow_top) {
					/* up arrow: scroll back one line */
					scrollback_offset++;
				} else if (win->h >= 4 &&
				    row == arrow_bot) {
					/* down arrow: scroll forward */
					if (scrollback_offset > 0)
						scrollback_offset--;
				} else if (drag == WM_DRAG_SCROLLING) {
					/* thumb: drag started by wm */
				} else {
					/* track: page up or down based on
					 * click position vs thumb */
					float click_pos;

					click_pos =
					    wm_scroll_pos_from_row(wmgr,
					    id, row);
					if (click_pos < win->scroll_pos)
						scrollback_offset +=
						    content_rows / 2;
					else
						scrollback_offset -=
						    content_rows / 2;
					if (scrollback_offset < 0)
						scrollback_offset = 0;
				}
				need_render = 1;
				need_taskbar = 1;
			}
			return;
		}
		if (area == WM_HIT_CONTENT) {
			int near;

			near = (id == sel_click_id &&
			    abs(row - sel_click_row) <= 0 &&
			    abs(col - sel_click_col) <= 2);

			if (near && sel_dblclick_timer_id >= 0) {
				sel_click_count++;
				iox_timer_remove(loop, sel_dblclick_timer_id);
			} else {
				sel_click_count = 1;
			}
			sel_dblclick_timer_id = iox_timer_add(loop, 500,
			    on_dblclick_timeout, NULL);
			sel_click_row = row;
			sel_click_col = col;
			sel_click_id = id;

			if (sel_click_count == 2) {
				struct wm_window *sw;

				sw = wm_find(wmgr, id);
				if (sw) {
					const struct vt_cell *scr;

					scr = wm_screen(wmgr);
					sel_begin_word(id, row, col,
					    sw->x, sw->w,
					    scr, wm_cols(wmgr));
				}
				sel_press_id = id;
				turbo_need_full = 1;
				need_render = 1;
				return;
			}
			if (sel_click_count >= 3) {
				struct wm_window *sw;

				sel_click_count = 3;
				sw = wm_find(wmgr, id);
				if (sw)
					sel_begin_line(id, row,
					    sw->x, sw->w);
				sel_press_id = id;
				turbo_need_full = 1;
				need_render = 1;
				return;
			}
			/* single click: defer for drag detection */
			sel_pending = 1;
			sel_press_row = row;
			sel_press_col = col;
			sel_press_id = id;
			sel_press_seq = *seq;
			return;
		}
		if (drag != WM_DRAG_IDLE)
			need_render = 1;
		return;
	}

	if (seq->key == TKBD_MOUSE_RIGHT) {
		sel_paste();
		return;
	}

	if (seq->key == TKBD_MOUSE_RELEASE) {
		/* deferred press with no motion -> click-through */
		if (sel_pending) {
			sel_pending = 0;
			if (focused_wants_mouse()) {
				sel_forward_press();
				{
					struct mconn *mc = mconn_focused();

					if (seq->len > 0 && mc)
						mconn_ipc_send(mc,
						    IPC_MSG_INPUT,
						    seq->data,
						    (uint32_t)seq->len);
				}
			}
			return;
		}

		/* finish active selection */
		if (sel_active()) {
			const struct vt_cell *scr = wm_screen(wmgr);
			int nr = wm_rows(wmgr);
			int nc = wm_cols(wmgr);

			sel_finish(scr, nr, nc);
			need_render = 1;
			return;
		}

		/* scrollbar drag release: convert scroll_pos to offset */
		if (scrollback_mode &&
		    wm_drag_state(wmgr) == WM_DRAG_SCROLLING) {
			struct wm_window *win;
			int resized = 0;

			id = wm_mouse_release(wmgr, &resized);
			win = wm_find(wmgr, id);
			if (win) {
				struct client_window *cw;
				struct vt_buf *primary;
				int vis, total, max_off;

				cw = cwin_find(id);
				if (cw && cw->vt) {
					primary = cw->vt->targets[
					    VT_TARGET_PRIMARY];
					vis = vt_buf_rows(primary);
					total = vt_buf_scrollback_lines(
					    primary) + vis;
					max_off = total - vis;
					if (max_off > 0)
						scrollback_offset =
						    (int)((1.0f -
						    win->scroll_pos) *
						    (float)max_off + 0.5f);
					else
						scrollback_offset = 0;
					if (scrollback_offset < 0)
						scrollback_offset = 0;
				}
			}
			need_render = 1;
			need_taskbar = 1;
			return;
		}

		{
			int resized = 0;

			id = wm_mouse_release(wmgr, &resized);
			if (resized) {
				struct wm_window *win = wm_find(wmgr, id);

				if (win) {
					struct client_window *cw;

					cw = cwin_find(id);
					if (cw)
						vt_state_resize(cw->vt,
						    win->h, win->w);
					{
						struct mconn *rmc;

						rmc = mconn_find_by_pid(
						    (pid_t)id);
						if (rmc)
							mconn_ipc_send_size(
							    rmc,
							    IPC_MSG_WIN_RESIZE,
							    win->h, win->w);
					}
				}
				need_render = 1;
			}
		}
		return;
	}
}

static void
handle_mouse(struct iox_loop *loop, const struct tkbd_seq *seq)
{
	int row, col;

	if (client_mode == CLIENT_MODE_TURBO && !overlay_visible) {
		handle_mouse_turbo(loop, seq);
		return;
	}

	/* overlays don't handle mouse yet */
	if (overlay_visible)
		return;

	row = seq->y;
	col = seq->x;

	if (handle_tabbar_click(row, col, seq))
		return;

	/* text selection in tiled/screen mode. clamp the drag point to the
	 * content area so a stray edge coordinate cannot extend the
	 * selection past the visible screen (mirrors the turbo path). */
	if (seq->mod & TKBD_MOD_MOTION) {
		if (row < 0)
			row = 0;
		else if (row >= content_rows)
			row = content_rows - 1;
		if (col < 0)
			col = 0;
		else if (col >= content_cols)
			col = content_cols - 1;
		if (sel_pending) {
			sel_begin(0, sel_press_row, sel_press_col,
			    0, content_cols);
			sel_update(row, col);
			sel_pending = 0;
			tile_need_full = 1;
			need_render = 1;
			return;
		}
		if (sel_active()) {
			switch (sel_get_mode()) {
			case SEL_MODE_WORD:
				sel_update_word(row, col,
				    tile_screen(tilemgr),
				    tile_cols(tilemgr));
				break;
			case SEL_MODE_LINE:
				sel_update_line(row);
				break;
			default:
				sel_update(row, col);
				break;
			}
			tile_need_full = 1;
			need_render = 1;
			return;
		}
		return;
	}

	/* mouse wheel: scroll history, or forward to child in alt screen */
	if (seq->key == TKBD_MOUSE_WHEEL_UP ||
	    seq->key == TKBD_MOUSE_WHEEL_DOWN) {
		struct client_window *fcw = cwin_focused();

		if (sel_active()) {
			sel_clear();
			tile_need_full = 1;
		}
		if (fcw && fcw->vt &&
		    fcw->vt->active_target != VT_TARGET_ALT) {
			if (seq->key == TKBD_MOUSE_WHEEL_UP) {
				if (!scrollback_mode)
					scrollback_enter();
				scrollback_offset += 3;
			} else {
				if (scrollback_mode) {
					scrollback_offset -= 3;
					if (scrollback_offset <= 0)
						scrollback_leave();
				}
			}
			need_render = 1;
			need_taskbar = 1;
		} else if (fcw && fcw->vt && focused_wants_mouse()) {
			struct mconn *mc = mconn_focused();

			if (seq->len > 0 && mc)
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    seq->data, (uint32_t)seq->len);
		}
		return;
	}

	if (seq->key == TKBD_MOUSE_LEFT) {
		int near;

		if (sel_active()) {
			sel_clear();
			tile_need_full = 1;
			need_render = 1;
		}

		near = (sel_click_id == 0 &&
		    abs(row - sel_click_row) <= 0 &&
		    abs(col - sel_click_col) <= 2);

		if (near && sel_dblclick_timer_id >= 0) {
			sel_click_count++;
			iox_timer_remove(loop, sel_dblclick_timer_id);
		} else {
			sel_click_count = 1;
		}
		sel_dblclick_timer_id = iox_timer_add(loop, 500,
		    on_dblclick_timeout, NULL);
		sel_click_row = row;
		sel_click_col = col;
		sel_click_id = 0;

		if (sel_click_count == 2) {
			const struct vt_cell *scr;
			int nc;

			scr = tile_screen(tilemgr);
			nc = tile_cols(tilemgr);
			sel_begin_word(0, row, col, 0, nc, scr, nc);
			sel_press_id = 0;
			tile_need_full = 1;
			need_render = 1;
			return;
		}
		if (sel_click_count >= 3) {
			sel_click_count = 3;
			sel_begin_line(0, row, 0, content_cols);
			sel_press_id = 0;
			tile_need_full = 1;
			need_render = 1;
			return;
		}
		/* defer press for drag detection */
		sel_pending = 1;
		sel_press_row = row;
		sel_press_col = col;
		sel_press_id = 0;
		sel_press_seq = *seq;
		return;
	}

	if (seq->key == TKBD_MOUSE_RIGHT) {
		sel_paste();
		return;
	}

	if (seq->key == TKBD_MOUSE_RELEASE) {
		if (sel_pending) {
			sel_pending = 0;
			if (focused_wants_mouse()) {
				sel_forward_press();
				{
					struct mconn *mc = mconn_focused();

					if (seq->len > 0 && mc)
						mconn_ipc_send(mc,
						    IPC_MSG_INPUT,
						    seq->data,
						    (uint32_t)seq->len);
				}
			}
			return;
		}
		if (sel_active()) {
			const struct vt_cell *scr;
			int nr, nc;

			scr = tile_screen(tilemgr);
			nr = tile_rows(tilemgr);
			nc = tile_cols(tilemgr);
			sel_finish(scr, nr, nc);
			tile_need_full = 1;
			need_render = 1;
			return;
		}
		return;
	}
}

/* ---- input handling ---- */

static char stdin_buf[4096];
static int stdin_buflen;
static int in_paste;

/* end any in-progress bracketed paste without requiring a PASTE_END
 * marker from the outer terminal.  see sel_clear_on_focus_change(). */
static void
paste_cancel(void)
{
	in_paste = 0;
}

static void
dispatch_input(struct iox_loop *loop, const struct tkbd_seq *seq)
{
	enum keys_action action;

	if (seq->type == TKBD_MOUSE) {
		handle_mouse(loop, seq);
		return;
	}

	/* command input line captures all keyboard input while open */
	if (cmdline_visible) {
		cmdline_input(seq);
		return;
	}

	/* bracketed paste: intercept boundaries from the outer terminal.
	 * while inside a paste, bypass the prefix key machine and forward
	 * content directly to the child PTY. */
	if (seq->key == TKBD_KEY_PASTE_BEGIN) {
		in_paste = 1;
		if (focused_wants_bracketed_paste()) {
			struct mconn *mc = mconn_focused();

			if (mc)
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    "\033[200~", 6);
		}
		return;
	}
	if (seq->key == TKBD_KEY_PASTE_END) {
		paste_cancel();
		if (focused_wants_bracketed_paste()) {
			struct mconn *mc = mconn_focused();

			if (mc)
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    "\033[201~", 6);
		}
		return;
	}
	if (in_paste) {
		if (seq->len > 0) {
			struct mconn *mc = mconn_focused();

			if (mc)
				mconn_ipc_send(mc, IPC_MSG_INPUT,
				    seq->data, (uint32_t)seq->len);
		}
		return;
	}

	/* route through active UI layers */
	if (active_app) {
		int rc = active_app->input(&app_context, seq);

		if (rc == 0)
			app_dismiss(1);
		else if (rc < 0)
			app_dismiss(0);
		return;
	}

	if (color_picker_visible) {
		color_picker_input(seq);
		return;
	}

	if (apps_menu_visible) {
		apps_menu_input(seq);
		return;
	}

	if (session_picker_visible) {
		session_picker_input(loop, seq);
		return;
	}

	if (picker_visible) {
		picker_input(seq);
		return;
	}

	if (menu_visible) {
		menu_input(loop, seq);
		return;
	}

	/* resize mode: hjkl/HJKL to adjust pane edges */
	if (resize_mode && tilemgr) {
		uint32_t fid = tile_focused_id(tilemgr);
		int handled = 1;

		switch (seq->ch) {
		case 'h': /* grow left (move left edge left) */
			tile_resize_pane(tilemgr, fid, 0, -1, 1);
			break;
		case 'j': /* grow down (move bottom edge down) */
			tile_resize_pane(tilemgr, fid, 1, 0, 1);
			break;
		case 'k': /* grow up (move top edge up) */
			tile_resize_pane(tilemgr, fid, -1, 0, 1);
			break;
		case 'l': /* grow right (move right edge right) */
			tile_resize_pane(tilemgr, fid, 0, 1, 1);
			break;
		case 'H': /* shrink from left (move left edge right) */
			tile_resize_pane(tilemgr, fid, 0, -1, -1);
			break;
		case 'J': /* shrink from bottom (move bottom edge up) */
			tile_resize_pane(tilemgr, fid, 1, 0, -1);
			break;
		case 'K': /* shrink from top (move top edge down) */
			tile_resize_pane(tilemgr, fid, -1, 0, -1);
			break;
		case 'L': /* shrink from right (move right edge left) */
			tile_resize_pane(tilemgr, fid, 0, 1, -1);
			break;
		default:
			handled = 0;
			break;
		}

		if (seq->key == TKBD_KEY_ENTER ||
		    seq->key == TKBD_KEY_ESC) {
			resize_mode = 0;
			return;
		}

		if (handled) {
			tile_each_pane(tilemgr,
			    tiled_resize_pane_cb, NULL);
			tile_need_full = 1;
			need_render = 1;
			need_taskbar = 1;
			return;
		}

		/* unknown key in resize mode -- exit resize mode */
		resize_mode = 0;
		return;
	}

	/* prefix key state machine (byte-based).
	 * kitty/SS3 sequences deliver key+modifier without a C0 byte;
	 * synthesize the control byte that keys_feed expects. */
	uint32_t feed_ch = seq->ch;

	if (feed_ch >= 256 && (seq->mod & TKBD_MOD_CTRL)) {
		if (seq->key >= TKBD_KEY_A && seq->key <= TKBD_KEY_Z)
			feed_ch = seq->key & 0x1F;
		else if (seq->key >= TKBD_KEY_BACKSLASH &&
		    seq->key <= TKBD_KEY_UNDERSCORE)
			feed_ch = seq->key & 0x1F;
		else if (seq->key == TKBD_KEY_AT ||
		    seq->key == TKBD_KEY_SPACE)
			feed_ch = 0x00;
	} else if (feed_ch >= 256) {
		if (seq->key >= TKBD_KEY_A && seq->key <= TKBD_KEY_Z)
			feed_ch = seq->key + 0x20;
		else if (seq->key >= 0x20 && seq->key <= 0x7E)
			feed_ch = seq->key;
	}

	if (feed_ch < 256) {
		action = keys_feed(keybinds, (uint8_t)feed_ch);

		if (action == KEYS_ACTION_CONSUMED) {
			/* prefix key received -- the main loop uses a
			 * short poll timeout while keys_get_state() is
			 * PREFIX, so the menu only appears after a real
			 * pause (not on rapid typing). */
			return;
		}
		if (action != KEYS_ACTION_NONE) {
			if (scrollback_mode)
				scrollback_leave();
			dispatch_action(loop, action);
			return;
		}
	}

	/* Escape dismisses a visible selection even outside scrollback */
	if (seq->key == TKBD_KEY_ESC && sel_active() && !scrollback_mode) {
		sel_clear();
		turbo_need_full = 1;
		tile_need_full = 1;
		need_render = 1;
		return;
	}

	/*
	 * scrollback / copy mode.  a copy cursor moves with hjkl + arrows;
	 * v/V/Ctrl-V pick char/line/block granularity; Space sets the mark
	 * then copies; y/Enter yank and exit; Esc aborts a selection or
	 * exits; q exits.
	 */
	if (scrollback_mode) {
		int handled = 1;
		int wx, wy, ww, wh, half;

		if (copy_win_rect(&wx, &wy, &ww, &wh) != 0) {
			wx = 0;
			wy = 0;
			ww = content_cols;
			wh = content_rows;
		}
		half = wh / 2 > 0 ? wh / 2 : 1;

		/* Ctrl-modified vim keys: page moves and block mode */
		if (seq->mod & TKBD_MOD_CTRL) {
			int i;

			switch (seq->key) {
			case TKBD_KEY_U:
				for (i = 0; i < half; i++)
					copy_move_vert(-1);
				goto sb_done;
			case TKBD_KEY_D:
				for (i = 0; i < half; i++)
					copy_move_vert(1);
				goto sb_done;
			case TKBD_KEY_V:
				copy_start(SEL_MODE_BLOCK);
				goto sb_done;
			default:
				break;
			}
		}

		switch (seq->key) {
		case TKBD_KEY_UP:
			copy_move_vert(-1);
			break;
		case TKBD_KEY_DOWN:
			copy_move_vert(1);
			break;
		case TKBD_KEY_LEFT:
			copy_move_horiz(-1);
			break;
		case TKBD_KEY_RIGHT:
			copy_move_horiz(1);
			break;
		case TKBD_KEY_PGUP:
			{
				int i;

				for (i = 0; i < half; i++)
					copy_move_vert(-1);
			}
			break;
		case TKBD_KEY_PGDN:
			{
				int i;

				for (i = 0; i < half; i++)
					copy_move_vert(1);
			}
			break;
		case TKBD_KEY_ESC:
			if (copy_selecting) {
				sel_clear();
				copy_selecting = 0;
				turbo_need_full = 1;
				tile_need_full = 1;
				break;
			}
			scrollback_leave();
			return;
		case TKBD_KEY_ENTER:
			copy_yank();
			return;
		default:
			switch (seq->ch) {
			case 'q':
				scrollback_leave();
				return;
			case 'y':
				copy_yank();
				return;
			case ' ':
				if (copy_selecting) {
					copy_yank();
					return;
				}
				copy_start(SEL_MODE_CHAR);
				break;
			case 'v':
				copy_start(SEL_MODE_CHAR);
				break;
			case 'V':
				copy_start(SEL_MODE_LINE);
				break;
			case 'h':
				copy_move_horiz(-1);
				break;
			case 'l':
				copy_move_horiz(1);
				break;
			case 'k':
				copy_move_vert(-1);
				break;
			case 'j':
				copy_move_vert(1);
				break;
			case '0':
				copy_cur_col = wx;
				copy_extend();
				break;
			case '$':
				copy_cur_col = wx + ww - 1;
				copy_extend();
				break;
			case 'g':
				/* jump to top of history */
				if (!copy_selecting) {
					scrollback_offset = INT_MAX;
					copy_scroll(0);	/* clamp to oldest */
				}
				copy_cur_row = wy;
				copy_extend();
				break;
			case 'G':
				if (!copy_selecting)
					scrollback_offset = 0;
				copy_cur_row = wy + wh - 1;
				copy_extend();
				break;
			default:
				handled = 0;
				break;
			}
			break;
		}

sb_done:
		if (handled) {
			need_render = 1;
			need_taskbar = 1;
		}
		return;
	}

	/* unhandled input -> forward raw sequence to PTY */
	if (seq->len > 0) {
		struct mconn *mc = mconn_focused();
		struct client_window *fcw = cwin_focused();

		/* input lock: discard keystrokes for this window */
		if (fcw && fcw->input_lock)
			return;

		if (mc)
			mconn_ipc_send(mc, IPC_MSG_INPUT,
			    seq->data, (uint32_t)seq->len);

		/* speculative local echo for printable characters.
		 * skip for dead windows (no PTY to echo back). */
		if (mc &&
		    seq->ch >= 0x20 && seq->ch != 0x7F) {
			struct client_window *cw = cwin_focused();

			if (cw && cw->pred) {
				int w = rune_width(seq->ch);

				if (w > 0 &&
				    predict_key(cw->pred, cw->vt,
				    seq->ch, w))
					need_render = 1;
			}
		}
	}
}

static void
on_stdin_read(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	ssize_t n;
	int off = 0;

	(void)events;
	(void)arg;

	n = read(fd, stdin_buf + stdin_buflen,
	    sizeof(stdin_buf) - (size_t)stdin_buflen);
	if (n <= 0) {
		iox_loop_stop(loop);
		return;
	}
	stdin_buflen += (int)n;

	while (off < stdin_buflen) {
		struct tkbd_seq seq;
		int consumed;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		consumed = tkbd_parse(&seq, stdin_buf + off,
		    (size_t)(stdin_buflen - off));
		if (consumed == TKBD_INCOMPLETE)
			break;			/* wait for the rest */
		if (consumed == 0) {
			/* unrecognized byte (e.g. a stray terminal report):
			 * skip it to resync rather than wedge all input,
			 * which would swallow the prefix key permanently. */
			off++;
			continue;
		}
		dispatch_input(loop, &seq);
		off += consumed;
	}
	sync_prefix_timer(loop);

	/* save leftover bytes for next read */
	if (off > 0 && off < stdin_buflen)
		memmove(stdin_buf, stdin_buf + off,
		    (size_t)(stdin_buflen - off));
	stdin_buflen -= off;
}

/* ---- micro-server event handling ---- */

/* seconds to wait for an mserver's ATTACH handshake before giving up.
 * a healthy server replies immediately; a wedged or dead one would
 * otherwise hang attach indefinitely (its socket lingers in the session
 * directory even after the server stops servicing it). */
#define MCONN_HANDSHAKE_TIMEOUT 5

/* bound blocking I/O on a socket. secs == 0 restores indefinite blocking,
 * which we do once the handshake succeeds so the steady-state event loop
 * is never tripped by a slow but live peer. */
static void
sock_set_timeout(int fd, int secs)
{
	struct timeval tv;

	tv.tv_sec = secs;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* connect to all mservers in the session directory, creating cwin
 * entries for each new connection at the server's current VT size.
 * the caller is responsible for resizing to the desired size afterward.
 * returns number of new connections. */
static int
mconn_discover(struct iox_loop *lp)
{
	pid_t pids[CLIENT_WIN_MAX];
	int n, i, added = 0;

	sessdir_cleanup_stale(session_name);
	n = sessdir_list_servers(session_name, pids, CLIENT_WIN_MAX);
	if (n < 0)
		return 0;

	for (i = 0; i < n; i++) {
		char *dir, *title;
		char path[PATH_MAX];
		int fd;
		int srv_rows = 24, srv_cols = 80;
		uint32_t rtype, rlen;
		char rbuf[64];

		if (mconn_find_by_pid(pids[i]))
			continue;

		dir = sessdir_server_path(session_name, pids[i]);
		if (!dir)
			continue;
		if (snprintf(path, sizeof(path), "%s/socket", dir)
		    >= (int)sizeof(path)) {
			free(dir);
			continue;
		}
		free(dir);

		fd = ipc_connect(path);
		if (fd < 0)
			continue;

		/* a wedged or dead-but-stale mserver accepts the connection
		 * into its backlog but never replies; bound the handshake so
		 * one bad window cannot hang the whole attach. */
		sock_set_timeout(fd, MCONN_HANDSHAKE_TIMEOUT);

		/* send ATTACH carrying our content-area size so the server
		 * resizes and generates the replay at that size (avoids a
		 * blank/garbled pane when the server's size differs from
		 * ours).  receive ATTACH_REPLY with the resulting VT size so
		 * we create a matching client VT. */
		{
			struct ipc_size sz;
			uint8_t abuf[16];
			int an;

			sz.rows = (uint16_t)content_rows;
			sz.cols = (uint16_t)content_cols;
			an = ipc_size_encode(&sz, abuf, sizeof(abuf));
			if (an < 0 ||
			    ipc_msg_send(fd, IPC_MSG_ATTACH, abuf,
			    (uint32_t)an) < 0) {
				ipc_close(fd);
				continue;
			}
		}
		if (ipc_msg_recv(fd, &rtype, rbuf, sizeof(rbuf),
		    &rlen) != 0 || rtype != IPC_MSG_ATTACH_REPLY) {
			/* unresponsive: skip this window rather than hang.
			 * the other windows still attach normally. */
			log_warn("window %ld not responding, skipping",
			    (long)pids[i]);
			ipc_close(fd);
			continue;
		}
		{
			struct ipc_size sz;

			if (ipc_size_decode(&sz, (const uint8_t *)rbuf,
			    (int)rlen) >= 0 && sz.rows > 0 &&
			    sz.cols > 0) {
				srv_rows = sz.rows;
				srv_cols = sz.cols;
			}
		}

		/* live server: restore blocking I/O for the event loop */
		sock_set_timeout(fd, 0);

		title = sessdir_read_file(session_name, pids[i], "title");
		mconn_add(lp, pids[i], fd, title);
		cwin_add_sized((uint32_t)pids[i], srv_rows, srv_cols);
		free(title);
		added++;
	}

	/* remove connections for dead servers */
	for (i = mconn_count - 1; i >= 0; i--) {
		int j, found = 0;

		for (j = 0; j < n; j++) {
			if (mconns[i].pid == pids[j]) {
				found = 1;
				break;
			}
		}
		if (!found) {
			cwin_remove((uint32_t)mconns[i].pid);
			mconn_remove(lp, mconns[i].pid);
		}
	}

	return added;
}

/* ---- remote proxy helpers ---- */

static int
start_proxy(const char *user, const char *host, const char *rsession)
{
	int sv[2];
	pid_t pid;
	char dest[384];

	if (user)
		snprintf(dest, sizeof(dest), "%s@%s", user, host);
	else
		snprintf(dest, sizeof(dest), "%s", host);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return -1;
	}

	if (pid == 0) {
		/* child: wire sv[1] to stdin/stdout, exec ssh */
		close(sv[0]);
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		if (sv[1] > STDERR_FILENO)
			close(sv[1]);
		lu_umask_restore();
		execlp("ssh", "ssh", "-T", "-o", "BatchMode=yes",
		    dest, "lumi", "proxy", "-s", rsession, NULL);
		_exit(127);
	}

	/* parent */
	close(sv[1]);
	rproxy_pid = pid;
	rproxy_fd = sv[0];
	return sv[0];
}

/* decode a hex string of exactly `n` bytes into out.  returns 0 on
 * success, -1 if the input is malformed or the wrong length. */
static int
hex_decode(const char *hex, uint8_t *out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int hi = hex[i * 2], lo = hex[i * 2 + 1];

		if (!isxdigit(hi) || !isxdigit(lo))
			return -1;
		hi = (hi <= '9') ? hi - '0' : (tolower(hi) - 'a' + 10);
		lo = (lo <= '9') ? lo - '0' : (tolower(lo) - 'a' + 10);
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	if (hex[n * 2] != '\0' && hex[n * 2] != '\n')
		return -1;
	return 0;
}

/* Wrap fd in an encrypted netchan transport toward peer, authenticated by
 * psk, and run the handshake.  Takes ownership of fd (closed on failure).
 * Sets rnet on success.  Returns 0 on success, -1 on error. */
static int
net_connect(int fd, const struct nc_addr *peer, const uint8_t *psk)
{
	rnet = ipc_transport_netchan_new_crypto(fd, 0, peer, psk);
	if (!rnet) {
		close(fd);
		return -1;
	}
	if (ipc_transport_netchan_establish(rnet, 5000) != 0) {
		ipc_transport_free(rnet);
		rnet = NULL;
		return -1;
	}
	return 0;
}

/* Connect to a lumi-net-proxy over an encrypted netchan link.  Reads the
 * endpoint the proxy published as "net-addr" and the pre-shared key it
 * published as "net-key" in the local session directory, dials the
 * endpoint on the loopback interface, and runs the handshake.  Sets rnet
 * on success.  Returns 0 on success, -1 on error.  Cross-host discovery
 * (learning the remote endpoint and key over ssh) is a later step; today
 * this attaches to a net-proxy bridging a session on this host. */
static int
start_net_proxy(const char *session)
{
	char *addr, *key;
	uint8_t psk[32];
	struct sockaddr_in sin;
	struct nc_addr peer;
	int port = 0, fd;

	key = sessdir_read_session_file(session, "net-key");
	if (!key)
		return -1;
	if (hex_decode(key, psk, sizeof(psk)) != 0) {
		free(key);
		return -1;
	}
	free(key);

	addr = sessdir_read_session_file(session, "net-addr");
	if (!addr)
		return -1;
	if (sscanf(addr, "udp %d", &port) != 1 || port <= 0) {
		free(addr);
		return -1;
	}
	free(addr);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons((uint16_t)port);
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		close(fd);
		return -1;
	}

	return net_connect(fd, &peer, psk);
}

/* Reach a lumi-net-proxy on another host.  Runs `lumi net-proxy -d` there
 * over ssh, which starts (or would start) the proxy, then reports its UDP
 * port and pre-shared key on one stdout line before detaching.  The key
 * travels inside the ssh channel, which authenticates the user and
 * encrypts the transfer.  The endpoint is then dialed directly, so window
 * I/O flows over the encrypted netchan link rather than through ssh.
 * Returns 0 on success, -1 on error. */
static int
ssh_capture_endpoint(const char *user, const char *host, const char *rsession,
    int *port, uint8_t *psk)
{
	int pfd[2];
	pid_t pid;
	char dest[384], line[256], hex[65];
	size_t off = 0;

	if (user)
		snprintf(dest, sizeof(dest), "%s@%s", user, host);
	else
		snprintf(dest, sizeof(dest), "%s", host);

	if (pipe(pfd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		if (pfd[1] > STDERR_FILENO)
			close(pfd[1]);
		lu_umask_restore();
		execlp("ssh", "ssh", "-T", "-o", "BatchMode=yes", dest,
		    "lumi", "net-proxy", "-s", rsession, "-b", "0.0.0.0",
		    "-d", NULL);
		_exit(127);
	}

	close(pfd[1]);
	while (off < sizeof(line) - 1) {
		ssize_t r = read(pfd[0], line + off, sizeof(line) - 1 - off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		off += (size_t)r;
		if (memchr(line, '\n', off))
			break;
	}
	close(pfd[0]);
	waitpid(pid, NULL, 0);
	line[off] = '\0';

	if (sscanf(line, "udp %d %64s", port, hex) != 2 || *port <= 0)
		return -1;
	return hex_decode(hex, psk, 32);
}

static int
start_net_proxy_ssh(const char *user, const char *host, const char *rsession)
{
	uint8_t psk[32];
	struct addrinfo hints, *res = NULL;
	struct sockaddr_in sin;
	struct nc_addr peer;
	int port = 0, fd;

	if (ssh_capture_endpoint(user, host, rsession, &port, psk) != 0)
		return -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
		return -1;
	memcpy(&sin, res->ai_addr, sizeof(sin));
	freeaddrinfo(res);
	sin.sin_port = htons((uint16_t)port);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	if (nc_udp_from_sockaddr(&peer, (struct sockaddr *)&sin,
	    sizeof(sin)) != 0) {
		close(fd);
		return -1;
	}
	return net_connect(fd, &peer, psk);
}

/* dispatch one decoded proxy envelope (window_id + type + payload) to the
 * client's window handlers.  shared by the SSH-pipe reader (on_proxy_read)
 * and the netchan reader (net_pump_and_drain). */
static void
proxy_dispatch(struct iox_loop *lp, uint32_t window_id, uint32_t type,
    const char *buf, uint32_t len)
{
	if (window_id == 0) {
		/* proxy control messages */
		const uint8_t *p = (const uint8_t *)buf;

		switch (type) {
		case IPC_MSG_PROXY_WIN_ADDED:
			if (len >= 9) {
				uint32_t wid;
				uint16_t rows, cols;
				uint8_t tlen;
				char title[128];

				memcpy(&wid, p, 4);
				wid = BE32(wid);
				memcpy(&rows, p + 4, 2);
				rows = BE16(rows);
				memcpy(&cols, p + 6, 2);
				cols = BE16(cols);
				tlen = p[8];
				if (tlen > sizeof(title) - 1)
					tlen = sizeof(title) - 1;
				if (9 + tlen <= len) {
					memcpy(title, p + 9, tlen);
					title[tlen] = '\0';
					mconn_add(NULL, (pid_t)wid, -1, title);
					cwin_add_sized(wid, rows, cols);
					mconn_sync_winlist();
					if (client_mode == CLIENT_MODE_TURBO)
						turbo_sync_windows();
					need_render = 1;
					need_taskbar = 1;
				}
			}
			break;

		case IPC_MSG_PROXY_WIN_REMOVED:
			if (len >= 4) {
				uint32_t wid;

				memcpy(&wid, p, 4);
				wid = BE32(wid);
				remove_window(lp, wid);
			}
			break;

		default:
			break;
		}
		return;
	}

	/* per-window message: route to existing handlers */
	{
		struct mconn *mc = mconn_find_by_pid((pid_t)window_id);

		if (!mc)
			return;

		switch (type) {
		case IPC_MSG_OUTPUT:
			{
				struct client_window *cw;

				cw = cwin_find(window_id);
				if (!cw)
					break;
				cwin_feed_output(cw, window_id, buf, len);
				need_render = 1;
				need_taskbar = 1;
			}
			break;

		case IPC_MSG_PTY_FLAGS:
			if (len >= 1) {
				struct client_window *cw;
				uint8_t flags = (uint8_t)buf[0];

				cw = cwin_find(window_id);
				if (cw)
					predict_set_echo(cw->pred,
					    flags & IPC_PTY_ECHO);
			}
			break;

		case IPC_MSG_DETACH:
			iox_loop_stop(lp);
			break;

		default:
			break;
		}
	}
}

static void
on_proxy_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	uint32_t window_id, type, len;
	char buf[IPC_MAX_PAYLOAD];
	int rc;

	(void)events;
	(void)arg;

	rc = proxy_msg_recv(fd, &window_id, &type, buf, sizeof(buf), &len);
	if (rc != 0) {
		iox_loop_stop(lp);	/* proxy died or pipe closed */
		return;
	}
	proxy_dispatch(lp, window_id, type, buf, len);
}

/* ---- netchan remote carrier ---- */

static void on_net_timer(struct iox_loop *lp, void *arg);
static void on_net_proxy_read(struct iox_loop *lp, int fd, unsigned events,
    void *arg);

/* CLOCK_MONOTONIC in ms, for rate-limiting roam attempts. */
static uint32_t
net_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* If the netchan client failed to send because its local address went away
 * (a network change), swap in a fresh socket and prod the proxy so its
 * netchan migrates to our new address.  Rate-limited so a genuinely-down
 * network does not spin creating sockets. */
static void
net_maybe_roam(struct iox_loop *lp)
{
	uint32_t now;
	int newfd;

	if (!rnet || !ipc_transport_netchan_roam_pending(rnet))
		return;
	now = net_now_ms();
	if (net_last_roam_ms && now - net_last_roam_ms < NET_ROAM_COOLDOWN_MS)
		return;
	net_last_roam_ms = now;

	newfd = ipc_transport_netchan_roam(rnet);
	if (newfd < 0)
		return;
	if (newfd != net_registered_fd) {
		if (net_registered_fd >= 0)
			iox_fd_remove(lp, net_registered_fd);
		iox_fd_add(lp, newfd, IOX_READ, on_net_proxy_read, NULL);
		net_registered_fd = newfd;
	}
	/* a window_id 0 control frame is dropped by the proxy, but it is real
	 * channel DATA from our new source, which is what makes the proxy's
	 * netchan validate and accept the migration (a keepalive ping alone
	 * would not). */
	proxy_msg_xsend(rnet, 0, IPC_MSG_NOP, NULL, 0);
}

/* service netchan, then dispatch every complete envelope now buffered.
 * a dead link stops the loop. */
static void
net_pump_and_drain(struct iox_loop *lp)
{
	char buf[IPC_MAX_PAYLOAD];
	uint32_t type, len, wid;

	if (!rnet)
		return;
	if (ipc_transport_netchan_pump(rnet) != 0) {
		iox_loop_stop(lp);
		return;
	}
	for (;;) {
		int r = ipc_transport_netchan_try_recv(rnet, &type, buf,
		    sizeof(buf), &len);

		if (r == 1)
			break;
		if (r < 0) {
			iox_loop_stop(lp);
			return;
		}
		if (proxy_msg_xdecode(&wid, buf, &len) != 0)
			continue;
		proxy_dispatch(lp, wid, type, buf, len);
	}
}

static void
net_rearm_timer(struct iox_loop *lp)
{
	int ms;

	if (!rnet)
		return;
	ms = ipc_transport_netchan_timeout(rnet);
	if (ms < 0 || ms > NET_SERVICE_MS)
		ms = NET_SERVICE_MS;
	if (net_service_timer >= 0)
		iox_timer_remove(lp, net_service_timer);
	net_service_timer = iox_timer_add(lp, ms, on_net_timer, NULL);
}

static void
on_net_timer(struct iox_loop *lp, void *arg)
{
	(void)arg;
	net_service_timer = -1;
	net_pump_and_drain(lp);
	if (!iox_loop_stopped(lp)) {
		net_maybe_roam(lp);
		net_rearm_timer(lp);
	}
}

static void
on_net_proxy_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	(void)fd;
	(void)events;
	(void)arg;
	net_pump_and_drain(lp);
}

/* parse a PROXY_READY window-list payload (count + per-window records)
 * into client windows.  shared by the SSH and netchan discovery paths. */
static void
proxy_populate_windows(const uint8_t *buf, uint32_t len)
{
	const uint8_t *p = buf;
	uint16_t count;
	int i;

	if (len < 2)
		return;
	memcpy(&count, p, 2);
	count = BE16(count);
	p += 2;
	len -= 2;

	for (i = 0; i < count; i++) {
		uint32_t wid;
		uint16_t rows, cols;
		uint8_t tlen;
		char title[128];
		size_t need;

		if (len < 9)
			break;
		memcpy(&wid, p, 4);
		wid = BE32(wid);
		memcpy(&rows, p + 4, 2);
		rows = BE16(rows);
		memcpy(&cols, p + 6, 2);
		cols = BE16(cols);
		tlen = p[8];
		need = 9 + tlen;
		if (need > len)
			break;
		if (tlen > sizeof(title) - 1)
			tlen = sizeof(title) - 1;
		memcpy(title, p + 9, tlen);
		title[tlen] = '\0';

		mconn_add(NULL, (pid_t)wid, -1, title);
		cwin_add_sized(wid, rows, cols);

		p += need;
		len -= (uint32_t)need;
	}
}

static int
mconn_discover_remote(struct iox_loop *lp)
{
	uint32_t window_id, type, len;
	uint8_t buf[4096];
	int rc;

	rc = proxy_msg_recv(rproxy_fd, &window_id, &type,
	    buf, sizeof(buf), &len);
	if (rc != 0 || type != IPC_MSG_PROXY_READY || len < 2)
		return -1;

	proxy_populate_windows(buf, len);

	/* register proxy fd for incoming messages */
	iox_fd_add(lp, rproxy_fd, IOX_READ, on_proxy_read, NULL);

	return mconn_count;
}

static int
mconn_discover_remote_net(struct iox_loop *lp)
{
	uint32_t type, len, wid;
	uint8_t buf[4096];
	int rc;

	/* the proxy sends PROXY_READY first; a blocking recv is fine here,
	 * before the event loop starts. */
	rc = ipc_transport_recv(rnet, &type, (char *)buf, sizeof(buf), &len);
	if (rc != 0)
		return -1;
	if (proxy_msg_xdecode(&wid, buf, &len) != 0 || wid != 0 ||
	    type != IPC_MSG_PROXY_READY || len < 2)
		return -1;

	proxy_populate_windows(buf, len);

	net_registered_fd = ipc_transport_get_fd(rnet);
	iox_fd_add(lp, net_registered_fd, IOX_READ, on_net_proxy_read, NULL);
	net_rearm_timer(lp);

	return mconn_count;
}

/* remove a window completely: free VT state, drop mconn, remove from
 * WM or tile manager, and update focus.  used both when mserver
 * disconnects (keep-open disabled) and when killing a dead window. */
static void
remove_window(struct iox_loop *lp, uint32_t pid)
{
	if (scrollback_mode && scrollback_win_id == pid)
		scrollback_leave();

	cwin_remove(pid);
	mconn_remove(lp, (pid_t)pid);
	mru_remove(pid);
	mconn_sync_winlist();

	if (mconn_count == 0) {
		if (client_mode == CLIENT_MODE_TURBO)
			wm_remove(wmgr, pid);
		else if (tilemgr)
			tile_set_window(tilemgr, pid, 0, NULL);
		vt = NULL;
		need_render = 0;
		need_taskbar = 0;
		iox_loop_stop(lp);
		return;
	}

	if (client_mode == CLIENT_MODE_TURBO) {
		wm_remove(wmgr, pid);
		/* tilemgr is the inactive layout in turbo mode, but
		 * cwin_remove already freed this window's vt.  drop the
		 * orphan pane now so a later toggle back to screen does
		 * not render a stale half-width leaf over a freed vt. */
		if (tilemgr) {
			if (tile_close(tilemgr, pid) < 0)
				tile_set_window(tilemgr, pid, 0, NULL);
		}
		if (watching && watched_id == pid) {
			struct client_window *cw;
			uint32_t nid = mru_recent_live();

			watched_id = nid ? nid : (uint32_t)mconns[0].pid;
			watching = 1;
			cw = cwin_find(watched_id);
			if (cw)
				vt = cw->vt;
			wm_focus(wmgr, watched_id);
		}
		need_render = 1;
		need_taskbar = 1;
		return;
	}

	/* screen mode: always through tilemgr.
	 * cwin_remove already freed the vt, so the pane's
	 * vt pointer is dangling -- must fix before render. */
	if (tile_close(tilemgr, pid) < 0) {
		/* can't close the only pane -- reassign it
		 * to another window, or clear it */
		struct client_window *cw = NULL;

		if (mconn_count > 0) {
			uint32_t nid = mru_recent_live();

			if (!nid)
				nid = (uint32_t)mconns[0].pid;
			cw = cwin_find(nid);
			if (cw) {
				tile_set_window(tilemgr,
				    pid, nid, cw->vt);
				tile_focus(tilemgr, nid);
			}
		}
		if (!cw)
			tile_set_window(tilemgr, pid, 0, NULL);
	}
	tile_each_pane(tilemgr, tiled_resize_pane_cb, NULL);
	tile_sync_focus();
	tile_need_full = 1;
	need_render = 1;
	need_taskbar = 1;
}

static void
on_mserver_read(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	struct mconn *mc;
	uint32_t type, len;
	char buf[IPC_MAX_PAYLOAD];
	int rc, batch;

	(void)events;
	(void)arg;

	mc = mconn_find_by_fd(fd);
	if (!mc)
		return;

	/* drain all immediately available messages so a burst of
	 * small IPC writes (e.g. screen replay) is processed in
	 * one pass instead of one message per event-loop iteration */
	for (batch = 0; batch < 4096; batch++) {
		rc = ipc_transport_recv(mc->t, &type, buf, sizeof(buf), &len);
		if (rc != 0) {
			uint32_t pid = (uint32_t)mc->pid;
			struct client_window *cw = cwin_find(pid);

			if (cw && cwin_should_keep_open(cw)) {
				cw->dead = 1;
				iox_fd_remove(lp, fd);
				ipc_transport_free(mc->t);	/* closes fd */
				mc->t = NULL;
				mc->fd = -1;
				if (client_mode == CLIENT_MODE_TURBO) {
					struct wm_window *win;

					win = wm_find(wmgr, pid);
					if (win)
						wm_set_flags(wmgr, pid,
						    win->flags |
						    WM_WIN_DEAD);
				}
				mconn_sync_winlist();
				need_render = 1;
				need_taskbar = 1;
				return;
			}

			remove_window(lp, pid);
			return;
		}

		switch (type) {
		case IPC_MSG_OUTPUT:
			{
				struct client_window *cw;
				uint32_t win_id = (uint32_t)mc->pid;

				cw = cwin_find(win_id);
				if (!cw)
					break;

				cwin_feed_output(cw, win_id, buf, len);
				need_render = 1;
				need_taskbar = 1;
			}
			break;

		case IPC_MSG_PTY_FLAGS:
			if (len >= 1) {
				struct client_window *cw;
				uint32_t win_id = (uint32_t)mc->pid;
				uint8_t flags = (uint8_t)buf[0];

				cw = cwin_find(win_id);
				if (cw)
					predict_set_echo(cw->pred,
					    flags & IPC_PTY_ECHO);
			}
			break;

		case IPC_MSG_DETACH:
			iox_loop_stop(lp);
			return;

		default:
			break;
		}

		{
			int avail = 0;

			if (ioctl(fd, FIONREAD, &avail) < 0 ||
			    avail < IPC_HDR_SIZE)
				break;
		}
	}
}

/* ---- session switching ---- */

static void on_sessdir_watch(struct iox_loop *lp, int fd,
    unsigned events, void *arg);

void
micro_switch_session(struct iox_loop *loop, const char *name)
{
	if (session_name && strcmp(session_name, name) == 0)
		return;

	/* save current layout */
	if (client_mode == CLIENT_MODE_TURBO)
		turbo_save_layout(session_name);
	else if (tilemgr)
		screen_save_layout(session_name);

	/* tear down current connections */
	mconn_disconnect_all(loop);
	cwin_free_all();

	/* stop watching old session directory */
	if (sessdir_watch_fd >= 0) {
		iox_fd_remove(loop, sessdir_watch_fd);
		sessdir_watch_stop(sessdir_watch_fd);
		sessdir_watch_fd = -1;
	}

	/* free layout state (both may be alive after mode toggles) */
	wm_free(wmgr);
	wmgr = NULL;
	tile_free(tilemgr);
	tilemgr = NULL;

	/* switch session name */
	free(session_name);
	session_name = strdup(name);

	/* connect to new session's servers */
	mconn_discover(loop);
	mconn_sync_winlist();

	/* if empty session, spawn a window and re-discover */
	if (mconn_count == 0) {
		micro_spawn_window(loop, session_name);
		/* brief pause for mserver to register */
		usleep(100000);
		mconn_discover(loop);
		mconn_sync_winlist();
	}

	if (mconn_count == 0)
		return;	/* still empty -- nothing we can do */

	/* set initial focus */
	watched_id = (uint32_t)mconns[0].pid;
	watching = 1;
	{
		struct client_window *cw;

		cw = cwin_find(watched_id);
		if (cw)
			vt = cw->vt;
	}

	/* reinitialize layout for active mode */
	if (client_mode == CLIENT_MODE_TURBO) {
		wmgr = wm_new(content_rows, content_cols);
		turbo_sync_windows();
		turbo_apply_layout(session_name);
		wm_focus(wmgr, watched_id);
		overlay_repaint_fn = turbo_repaint;
		turbo_need_full = 1;
	} else {
		if (screen_apply_layout(session_name) < 0) {
			tilemgr = tile_new(content_rows, content_cols);
			tile_set_window(tilemgr, 0, watched_id, vt);
			tile_focus(tilemgr, watched_id);
		}
		overlay_repaint_fn = tiled_repaint;
		tile_each_pane(tilemgr, tiled_resize_pane_cb, NULL);
		screen_fit_all_windows();
		tile_need_full = 1;
	}

	/* watch new session directory */
	sessdir_watch_fd = sessdir_watch_start(session_name);
	if (sessdir_watch_fd >= 0) {
		iox_fd_add(loop, sessdir_watch_fd, IOX_READ,
		    on_sessdir_watch, NULL);
		sessdir_watch_degraded = 0;
	} else {
		/* Without the watch, windows we spawn ourselves still appear
		 * (micro_spawn_window schedules an active rescan), but windows
		 * created or closed by OTHER attach clients will not update
		 * live. If that becomes a problem, add a periodic timer-driven
		 * mconn_refresh() fallback here (the "#2 polling" option). */
		sessdir_watch_degraded = 1;
		log_warn("sessdir watch unavailable (inotify?); windows from "
		    "other clients will not appear live");
	}

	need_render = 1;
	need_taskbar = 1;
	sync_keybinds_title();
}

static int
mconn_refresh(struct iox_loop *lp)
{
	int added;

	added = mconn_discover(lp);
	mconn_sync_winlist();

	/* resize newly discovered windows to screen content size
	 * (screen mode only -- turbo_sync_windows handles its own).
	 * for 1-pane tilemgr this is the full screen; for multi-pane
	 * the window will be resized when assigned to a pane. */
	if (added > 0 && client_mode != CLIENT_MODE_TURBO) {
		int i;

		for (i = 0; i < mconn_count; i++) {
			struct client_window *cw;

			cw = cwin_find((uint32_t)mconns[i].pid);
			if (!cw)
				continue;
			if (vt_buf_rows(cw->vt->buf) == content_rows &&
			    vt_buf_cols(cw->vt->buf) == content_cols)
				continue;
			vt_state_resize(cw->vt,
			    content_rows, content_cols);
			mconn_ipc_send_size(&mconns[i],
			    IPC_MSG_WIN_RESIZE,
			    content_rows, content_cols);
		}
	}

	/* keep watched_id / vt in sync for all modes */
	if (watching && !mconn_find_by_pid((pid_t)watched_id))
		watching = 0;

	if (client_mode == CLIENT_MODE_TURBO) {
		/* turbo focus: no tile panes involved */
		if (added > 0 && mconn_count > 0) {
			watched_id = (uint32_t)mconns[mconn_count - 1].pid;
			watching = 1;
			{
				struct client_window *cw;

				cw = cwin_find(watched_id);
				if (cw)
					vt = cw->vt;
			}
		} else if (!watching && mconn_count > 0) {
			watched_id = (uint32_t)mconns[0].pid;
			watching = 1;
			{
				struct client_window *cw;

				cw = cwin_find(watched_id);
				if (cw)
					vt = cw->vt;
			}
		}
		turbo_sync_windows();
		if (watching)
			wm_focus(wmgr, watched_id);
		need_render = 1;
		need_taskbar = 1;
		return added;
	}

	/* screen mode: assign new windows to empty tile panes first,
	 * then update focus. only try newly added windows so we don't
	 * assign an already-displayed window to an empty pane. */
	{
		int pane_assigned = 0;

		if (added > 0) {
			int i;

			for (i = mconn_count - added; i < mconn_count; i++) {
				uint32_t pid = (uint32_t)mconns[i].pid;
				struct client_window *cw;

				cw = cwin_find(pid);
				if (!cw)
					continue;
				if (tile_set_window(tilemgr, 0,
				    pid, cw->vt) == 0) {
					tile_each_pane(tilemgr,
					    tiled_resize_pane_cb, NULL);
					tile_focus(tilemgr, pid);
					tile_sync_focus();
					pane_assigned = 1;
					break;
				}
			}
		}

		if (added > 0 && !pane_assigned && mconn_count > 0) {
			/* no empty pane -- swap focused pane to show
			 * newest window (C-a c new-window behavior) */
			uint32_t old_id = tile_focused_id(tilemgr);
			uint32_t new_id;
			struct client_window *cw;

			new_id = (uint32_t)mconns[mconn_count - 1].pid;
			cw = cwin_find(new_id);
			if (cw) {
				tile_set_window(tilemgr, old_id,
				    new_id, cw->vt);
				tile_focus(tilemgr, new_id);
				tile_sync_focus();
			}
		} else if (!watching && mconn_count > 0) {
			/* current focus died -- swap to first window */
			uint32_t old_id = tile_focused_id(tilemgr);
			uint32_t new_id;
			struct client_window *cw;

			new_id = (uint32_t)mconns[0].pid;
			cw = cwin_find(new_id);
			if (cw) {
				tile_set_window(tilemgr, old_id,
				    new_id, cw->vt);
				tile_focus(tilemgr, new_id);
				tile_sync_focus();
			}
		}
	}

	tile_need_full = 1;
	need_render = 1;
	need_taskbar = 1;
	return added;
}

static void
on_sessdir_watch(struct iox_loop *lp, int fd, unsigned events, void *arg)
{
	int flags;

	(void)events;
	(void)arg;

	flags = sessdir_watch_read(fd);
	if (flags & SESSDIR_WATCH_CHANGED)
		mconn_refresh(lp);
}

static void
on_sigchld(struct iox_loop *lp, int signo, void *arg)
{
	int status;
	pid_t pid;

	(void)signo;
	(void)arg;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (WIFSIGNALED(status))
			log_warn("child %d killed by signal %d",
			    (int)pid, WTERMSIG(status));
		if (mconn_find_by_pid(pid))
			remove_window(lp, (uint32_t)pid);
	}
}

static void
on_shutdown_signal(struct iox_loop *lp, int signo, void *arg)
{
	(void)signo;
	(void)arg;

	mconn_disconnect_all(lp);
	iox_loop_stop(lp);
}

static void
on_sigwinch(struct iox_loop *loop, int signo, void *arg)
{
	struct winsize ws;

	(void)loop;
	(void)signo;
	(void)arg;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
		update_content_size(ws.ws_row, ws.ws_col);
		render_resize(renderer, content_rows, content_cols);

		if (client_mode == CLIENT_MODE_TURBO) {
			wm_resize(wmgr, content_rows, content_cols);
			/* re-maximize windows to fill new size */
			for (int i = 0; i < wm_count(wmgr); i++) {
				struct wm_window *w;
				w = wm_window_at(wmgr, i);
				if (w && w->maximized) {
					w->x = 1;
					w->y = 1;
					w->w = content_cols - 2;
					w->h = content_rows - 2;
					turbo_sync_size(w->id);
				}
			}
			turbo_need_full = 1;
		} else {
			/* screen mode: always through tilemgr */
			tile_resize(tilemgr, content_rows, content_cols);
			tile_each_pane(tilemgr,
			    tiled_resize_pane_cb, NULL);
			tile_need_full = 1;
		}
		need_render = 1;
		need_taskbar = 1;
	}
}

/* ---- main ---- */

static void
get_terminal_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi-attach [-f window] [-m mode] [-n] [-s name]\n");
	fprintf(stderr,
	    "  -f window   focus this window (by PID) after startup\n"
	    "  -m mode     UI mode: screen (default), turbo, minimal\n"
	    "  -n          connect via the session's net-proxy (netchan)\n"
	    "  -s name     session name (default: 0)\n");
}

int
cmd_attach_main(int argc, char **argv)
{
	struct iox_loop *loop;
	const char *name = "0";
	const char *mode_str = NULL;
	uint32_t focus_window = 0;
	int rows, cols;
	int opt;
	int use_netchan = 0;

	if (getenv("LUMI_SESSION")) {
		fprintf(stderr,
		    "lumi-attach: already inside session '%s'\n",
		    getenv("LUMI_SESSION"));
		return 1;
	}

	while ((opt = getopt(argc, argv, "f:m:ns:")) != -1) {
		switch (opt) {
		case 'f':
			focus_window = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case 'm':
			mode_str = optarg;
			break;
		case 'n':
			use_netchan = 1;
			break;
		case 's':
			name = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}
	if (optind < argc)
		name = argv[optind];

	/* parse mode */
	if (mode_str) {
		if (strcmp(mode_str, "turbo") == 0)
			client_mode = CLIENT_MODE_TURBO;
		else if (strcmp(mode_str, "screen") == 0)
			client_mode = CLIENT_MODE_SCREEN;
		else if (strcmp(mode_str, "minimal") == 0)
			client_mode = CLIENT_MODE_MINIMAL;
		else {
			fprintf(stderr,
			    "lumi-attach: unknown mode '%s'\n",
			    mode_str);
			usage();
			return 1;
		}
	}

	dbg_init();
	rune_width_init();
	get_terminal_size(&rows, &cols);
	update_content_size(rows, cols);

	{
		const char *ruser, *rhost, *rsess;

		if (use_netchan && parse_session_name(name, &ruser, &rhost,
		    &rsess)) {
			/* remote host: bootstrap the proxy over ssh, then
			 * dial its endpoint directly over encrypted netchan. */
			is_remote = 1;
			remote_netchan = 1;
			session_name = strdup(rsess);
			if (start_net_proxy_ssh(ruser, rhost, rsess) < 0) {
				fprintf(stderr,
				    "lumi-attach: failed to reach net-proxy "
				    "on %s\n", name);
				free(session_name);
				return 1;
			}
		} else if (use_netchan) {
			/* local host: attach to a proxy already serving the
			 * session on this machine over loopback. */
			is_remote = 1;
			remote_netchan = 1;
			session_name = strdup(name);
			if (start_net_proxy(name) < 0) {
				fprintf(stderr,
				    "lumi-attach: failed to connect to "
				    "net-proxy for session '%s'\n", name);
				free(session_name);
				return 1;
			}
		} else if (parse_session_name(name, &ruser, &rhost, &rsess)) {
			is_remote = 1;
			session_name = strdup(rsess);
			if (start_proxy(ruser, rhost, rsess) < 0) {
				fprintf(stderr,
				    "lumi-attach: failed to start proxy "
				    "to %s\n", name);
				free(session_name);
				return 1;
			}
		} else {
			session_name = strdup(name);
		}
	}

	{
		struct cfg *cfg;
		const char *xdg, *home;
		char cfgpath[4096];

		cfg = cfg_new();
		xdg = getenv("XDG_CONFIG_HOME");
		home = getenv("HOME");
		if (xdg && xdg[0])
			snprintf(cfgpath, sizeof(cfgpath),
			    "%s/lumi/lumi.conf", xdg);
		else if (home && home[0])
			snprintf(cfgpath, sizeof(cfgpath),
			    "%s/.config/lumi/lumi.conf", home);
		else
			cfgpath[0] = '\0';

		if (cfgpath[0])
			cfg_load(cfg, cfgpath);

		keybinds = keys_new(0x01);
		if (!keybinds) {
			log_err("failed to create key bindings");
			cfg_free(cfg);
			return 1;
		}
		keys_default(keybinds);
		keys_load_cfg(keybinds, cfg);
		keys_set_timeout_cb(keybinds, on_prefix_timeout, NULL);

		taskbar = taskbar_new();
		taskbar_load_cfg(taskbar, cfg);

		theme_load_cfg(cfg);

		/* attach.mode from config (command-line overrides) */
		if (!mode_str) {
			const char *val = cfg_get(cfg, "attach.mode");

			if (val && strcmp(val, "turbo") == 0)
				client_mode = CLIENT_MODE_TURBO;
			else if (val && strcmp(val, "minimal") == 0)
				client_mode = CLIENT_MODE_MINIMAL;
		}

		{
			const char *val;

			val = cfg_get(cfg, "attach.keep-open");
			if (val && (strcmp(val, "true") == 0 ||
			    strcmp(val, "yes") == 0 ||
			    strcmp(val, "1") == 0))
				keep_open_default = 1;

			val = cfg_get(cfg, "attach.altscreen-scrollback");
			if (val && (strcmp(val, "true") == 0 ||
			    strcmp(val, "yes") == 0 ||
			    strcmp(val, "1") == 0))
				altscreen_scrollback = 1;
		}

		cfg_free(cfg);
	}

	/* minimal mode: no taskbar, use full terminal height */
	if (client_mode == CLIENT_MODE_MINIMAL) {
		taskbar_visible = 0;
		update_content_size(rows, cols);
	}

	txl = txl_new(NULL);
	if (!txl) {
		log_err("failed to create terminal translation");
		return 1;
	}

	if (!theme)
		theme = tui_theme_default();
	tui_stack_init(&overlay);
	tb = tui_term_new(txl, STDOUT_FILENO);
	if (!tb) {
		log_err("failed to create terminal backend");
		return 1;
	}

	renderer = render_new(content_rows, content_cols, txl);
	if (!renderer) {
		log_err("failed to create renderer");
		return 1;
	}

	if (client_mode == CLIENT_MODE_TURBO) {
		wmgr = wm_new(content_rows, content_cols);
		if (!wmgr) {
			log_err("failed to create window manager");
			return 1;
		}
		turbo_need_full = 1;
		overlay_repaint_fn = turbo_repaint;
	}

	if (tio_raw(STDIN_FILENO) < 0) {
		log_err("failed to set raw mode");
		return 1;
	}

	set_cursor_vis(0);

	if (client_mode != CLIENT_MODE_MINIMAL)
		enable_mouse();
	emit_mode(2004, 1);
	tio_flush(STDOUT_FILENO);

	loop = iox_loop_new();
	if (!loop) {
		emit_mode(2004, 0);
		tio_flush(STDOUT_FILENO);
		disable_mouse();
		tio_restore(STDIN_FILENO);
		log_err("failed to create event loop");
		return 1;
	}

	iox_fd_add(loop, STDIN_FILENO, IOX_READ, on_stdin_read, NULL);

	{
		/* discover mservers: remote via proxy, local via sessdir */
		if (is_remote && remote_netchan)
			mconn_discover_remote_net(loop);
		else if (is_remote)
			mconn_discover_remote(loop);
		else
			mconn_discover(loop);
		if (mconn_count == 0) {
			log_err("no servers in session '%s'", name);
			iox_loop_free(loop);
			tio_restore(STDIN_FILENO);
			return 1;
		}

		/* set initial focus to the session's recorded foreground
		 * window; remote sessions have no local state, so fall back
		 * to the first discovered connection. */
		watched_id = is_remote ? (uint32_t)mconns[0].pid
		    : mconn_initial_focus();
		watching = 1;
		{
			struct client_window *cw;

			cw = cwin_find(watched_id);
			if (cw)
				vt = cw->vt;
		}

		if (client_mode == CLIENT_MODE_TURBO) {
			/* turbo_sync_windows adds wm windows and
			 * resizes each to its turbo tile size */
			turbo_sync_windows();
			/* override cascade with saved layout if available */
			turbo_apply_layout(name);
			wm_focus(wmgr, watched_id);
			turbo_need_full = 1;
		} else {
			/* try loading saved screen layout */
			if (screen_apply_layout(name) < 0) {
				/* no layout -- single pane with first window */
				tilemgr = tile_new(content_rows, content_cols);
				tile_set_window(tilemgr, 0, watched_id, vt);
				tile_focus(tilemgr, watched_id);
			}
			overlay_repaint_fn = tiled_repaint;

			/* resize panes to content size and tell mservers */
			tile_each_pane(tilemgr, tiled_resize_pane_cb, NULL);
			screen_fit_all_windows();
			tile_need_full = 1;
		}

		/* a restored layout may have moved focus off the initial
		 * window; sync the winlist now so the highlighted tab
		 * matches the window actually shown in the foreground. */
		{
			struct client_window *cw = cwin_find(watched_id);

			if (cw)
				vt = cw->vt;
		}
		mconn_sync_winlist();

		/* panes are now at their final size; ask each server to
		 * resync its screen so alt-screen apps are not left blank */
		mconn_request_refresh_all();

		need_render = 1;
		need_taskbar = 1;

		/* watch sessdir for new/removed mservers (local only;
		 * remote proxy handles its own watch) */
		if (!is_remote) {
			sessdir_watch_fd = sessdir_watch_start(name);
			if (sessdir_watch_fd >= 0) {
				iox_fd_add(loop, sessdir_watch_fd, IOX_READ,
				    on_sessdir_watch, NULL);
			} else {
				sessdir_watch_degraded = 1;
				log_warn("sessdir watch unavailable (inotify?); "
				    "windows from other clients will not "
				    "appear live");
			}
		}
	}

	/* focus a specific window if requested */
	if (focus_window)
		micro_select_window(focus_window);

	/* an mserver dying must not kill us with SIGPIPE mid-write; let the
	 * write return EPIPE so we detach from that window cleanly. */
	signal(SIGPIPE, SIG_IGN);

	iox_signal_add(loop, SIGCHLD, on_sigchld, NULL);
	iox_signal_add(loop, SIGWINCH, on_sigwinch, NULL);
	iox_signal_add(loop, SIGTERM, on_shutdown_signal, NULL);
	iox_signal_add(loop, SIGINT, on_shutdown_signal, NULL);
	iox_signal_add(loop, SIGHUP, on_shutdown_signal, NULL);

	/* cursor policy: restore app-requested visibility before blocking
	 * in poll; events/rendering may move the cursor arbitrarily.
	 * prefix timeout is driven by iox_timer via sync_prefix_timer(). */
	iox_loop_start(loop);
	while (!iox_loop_stopped(loop)) {
		flush_render();
		set_cursor_vis(app_cursor_vis());
		tio_flush(STDOUT_FILENO);
		if (iox_loop_poll(loop) < 0)
			continue; /* EINTR */
	}

	/* save layout before teardown */
	if (client_mode == CLIENT_MODE_TURBO)
		turbo_save_layout(name);
	else if (tilemgr)
		screen_save_layout(name);

	mconn_disconnect_all(NULL);

	/* clean up remote proxy */
	if (remote_netchan) {
		if (net_service_timer >= 0) {
			iox_timer_remove(loop, net_service_timer);
			net_service_timer = -1;
		}
		if (rnet) {
			if (net_registered_fd >= 0)
				iox_fd_remove(loop, net_registered_fd);
			net_registered_fd = -1;
			ipc_transport_free(rnet);	/* closes the UDP fd */
			rnet = NULL;
		}
	} else if (is_remote && rproxy_fd >= 0) {
		close(rproxy_fd);
		rproxy_fd = -1;
		if (rproxy_pid > 0)
			waitpid(rproxy_pid, NULL, 0);
	}

	iox_loop_free(loop);
	reset_terminal_modes();
	emit_mode(2004, 0);
	tio_flush(STDOUT_FILENO);
	set_cursor_vis(1);
	disable_mouse();
	reset_host_title();

	{
		const char *clr = txl_str(txl, TXL_CLEAR);
		char cup[24];
		int cuplen;

		cuplen = txl_cup(txl, cup, sizeof(cup), 0, 0);
		if (cuplen > 0)
			tio_write(STDOUT_FILENO, cup, (size_t)cuplen);
		if (clr)
			tio_write(STDOUT_FILENO, clr, strlen(clr));
		tio_flush(STDOUT_FILENO);
	}

	tio_restore(STDIN_FILENO);
	if (sessdir_watch_fd >= 0)
		sessdir_watch_stop(sessdir_watch_fd);
	render_free(renderer);
	tile_free(tilemgr);
	wm_free(wmgr);
	cwin_free_all();
	tui_term_free(tb);
	txl_free(txl);
	keys_free(keybinds);
	taskbar_free(taskbar);
	taskbar_free(scrollback_sb);
	free(session_name);

	return 0;
}
