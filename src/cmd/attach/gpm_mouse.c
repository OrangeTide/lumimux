/* gpm_mouse.c : optional GPM (Linux text console) mouse input */

#include "gpm_mouse.h"

#ifdef HAVE_GPM

#include "attach_ui.h"
#include "iox_fd.h"
#include "tkbd.h"
#include "dbg.h"

#include <gpm.h>

#include <stdlib.h>
#include <string.h>

#define OK  (0)
#define ERR (-1)

static gpm_mouse_dispatch_fn gpm_dispatch;
static int gpm_connected;

/* map a GPM button bitmask to the matching tkbd mouse key. */
static int
gpm_button_key(unsigned char buttons)
{
	if (buttons & GPM_B_LEFT)
		return TKBD_MOUSE_LEFT;
	if (buttons & GPM_B_MIDDLE)
		return TKBD_MOUSE_MIDDLE;
	if (buttons & GPM_B_RIGHT)
		return TKBD_MOUSE_RIGHT;
	return -1;
}

/* translate one GPM event into a tkbd sequence and dispatch it.
 * mirrors xterm 1002 (button-motion) reporting: motion is only reported
 * while a button is held, so a bare hover produces nothing. */
static void
gpm_translate(struct iox_loop *loop, const Gpm_Event *ev)
{
	struct tkbd_seq seq;
	int key;

	memset(&seq, 0, sizeof(seq));
	seq.type = TKBD_MOUSE;
	seq.ch = TKBD_CH_NONE;

	/* GPM coordinates are 1-based; the rest of the client expects 0-based */
	seq.x = ev->x > 0 ? ev->x - 1 : 0;
	seq.y = ev->y > 0 ? ev->y - 1 : 0;

	if (ev->wdy != 0) {
		seq.key = ev->wdy > 0 ? TKBD_MOUSE_WHEEL_UP
		    : TKBD_MOUSE_WHEEL_DOWN;
	} else if (ev->type & GPM_UP) {
		seq.key = TKBD_MOUSE_RELEASE;
	} else if (ev->type & GPM_DRAG) {
		key = gpm_button_key(ev->buttons);
		if (key < 0)
			return;
		seq.key = (uint16_t)key;
		seq.mod = TKBD_MOD_MOTION;
	} else if (ev->type & GPM_DOWN) {
		key = gpm_button_key(ev->buttons);
		if (key < 0)
			return;
		seq.key = (uint16_t)key;
	} else {
		return;			/* bare move, no button: ignore */
	}

	gpm_dispatch(loop, &seq);
}

static void
on_gpm_read(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	Gpm_Event ev;
	int rc;

	(void)fd;
	(void)events;
	(void)arg;

	rc = Gpm_GetEvent(&ev);
	if (rc <= 0) {
		/* daemon went away; stop watching so we do not spin */
		gpm_mouse_shutdown(loop);
		return;
	}
	gpm_translate(loop, &ev);
}

int
gpm_mouse_init(struct iox_loop *loop, gpm_mouse_dispatch_fn dispatch)
{
	Gpm_Connect conn;
	const char *term = getenv("TERM");

	if (gpm_connected)
		return OK;

	/* only the raw Linux console lacks terminal-side mouse reporting.
	 * inside a terminal emulator the xterm mouse path already works. */
	if (!term || strcmp(term, "linux") != 0)
		return ERR;

	memset(&conn, 0, sizeof(conn));
	conn.eventMask = GPM_MOVE | GPM_DRAG | GPM_DOWN | GPM_UP;
	conn.defaultMask = GPM_MOVE | GPM_HARD;	/* let gpm draw the pointer */
	conn.minMod = 0;
	conn.maxMod = (unsigned short)~0;

	/* Gpm_Open sets the global gpm_fd; vc 0 means the current console */
	if (Gpm_Open(&conn, 0) < 0 || gpm_fd < 0) {
		dbg_trace("gpm: Gpm_Open failed\n");
		return ERR;
	}

	if (iox_fd_add(loop, gpm_fd, IOX_READ, on_gpm_read, NULL) != 0) {
		Gpm_Close();
		return ERR;
	}

	gpm_dispatch = dispatch;
	gpm_connected = 1;
	dbg_trace("gpm: connected on fd %d\n", gpm_fd);
	return OK;
}

void
gpm_mouse_shutdown(struct iox_loop *loop)
{
	if (!gpm_connected)
		return;
	if (gpm_fd >= 0)
		iox_fd_remove(loop, gpm_fd);
	Gpm_Close();
	gpm_connected = 0;
	gpm_dispatch = NULL;
}

#else /* !HAVE_GPM */

/* Built without GPM support: provide no-op stubs so the caller need not
 * guard every call site. */

int
gpm_mouse_init(struct iox_loop *loop, gpm_mouse_dispatch_fn dispatch)
{
	(void)loop;
	(void)dispatch;
	return -1;
}

void
gpm_mouse_shutdown(struct iox_loop *loop)
{
	(void)loop;
}

#endif /* HAVE_GPM */
