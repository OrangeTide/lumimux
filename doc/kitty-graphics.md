# Kitty Graphics Protocol Support

Design notes for carrying the kitty graphics protocol through lumi. This
covers what is feasible given lumi's architecture, a per-window placement
table, the record and replay paths, image-number resolution from host
replies, and the re-transmit versus host-persistence tradeoff.

Status: partially implemented. Anchoring, client and server cursor
accounting, cell-pixel plumbing, and screen-mode window-switch replay have
shipped. See "As implemented" for what the code actually does and where it
diverges from the fuller design below. The rest (the general placement
table, image-number resolution, animation, turbo-mode clipping) remains
design only.

## As implemented

The shipped code follows the design's build order through step 6, but the
replay is simpler than Strategy A/B describe, because testing turned up a
fact the design did not account for.

**ED deletes kitty images.** `render_cells_full` (`src/librender/render.c`)
emits `\e[H\e[2J` (ED) before redrawing. Kitty ties an image's lifetime to
its anchor cell, so ED erases every image on screen. `render_cells_diff`
does not clear, so it leaves images alone. This single fact drives the whole
replay strategy: any full render wipes the images, and a full render can
fire more than once on a switch (a mid-switch full redraw was observed
wiping an image that had only been queued once). So the store is re-emitted
after *every* full render, not just on the switch itself.

What exists in `src/cmd/attach/attach.c`:

- **Per-window store.** `struct client_window` carries
  `struct kgfx_img gfx[KGFX_MAX]` (`KGFX_MAX` = 32, oldest evicted), each
  entry the raw APC bytes plus the `(row, col)` client cursor cell captured
  when the command was parsed. This is the step-1 record store from the
  build order, kept minimal; the general `kgfx_state` placement/xmit tables
  in the design below were not built.
- **Anchoring.** `dcs_passthru` records the command via `win_gfx_add` and
  sets `need_render` instead of writing the image out at parse time.
  `pending_gfx_flush` emits queued images after the cells are drawn, at the
  recorded cell, then restores the cursor.
- **Cursor accounting.** `vt_kgfx_account` (`src/libvt/vt_state.c`) advances
  the cursor by the image footprint on both the client vt_state and, through
  `window_dcs_account` (`src/libsession/window.c`), the server vt_state.
  Kitty leaves the cursor on the image's *last* row, so the advance is
  `rows - 1` index calls plus the column move, not `rows`.
- **Cell-pixel plumbing.** The outer cell pixel size is read with
  `TIOCGWINSZ` on the client tty and carried to the server in both
  `ipc_attach` and `ipc_size` (tags 8/9, backward compatible), then set on
  the program's pty via `pty_resize` and stored on the window for
  server-side accounting.
- **Replay.** On a full render, `tiled_render` re-emits the whole visible
  window's store (because ED just wiped it). On a diff render with a window
  change, it deletes all placements and re-queues the incoming window's
  store. `gfx_shown_win` tracks which window's images are currently up. This
  is effectively Strategy A (always re-transmit), which is mandatory anyway
  for icat-style images that carry no id.

Divergences from the design below, and their reasons:

- No image-id resolution, no `kgfx_state` tables, no Strategy B. The store
  keys off raw bytes and cursor cell, which is enough for re-transmit replay
  and needs none of the pending-FIFO / reply-routing machinery.
- Replay re-emits on every full render, an ED consequence the design's
  switch-hook sketch (replay once, at the tail of the switch) missed.

The known limits below still apply, in particular coordinate drift: the
stored cell is where the image was drawn, so it is wrong if that window
scrolled after placement.

## Why images are hard here

lumi is a screen-buffer multiplexer, not a byte forwarder. Every pane's
PTY output is parsed into a cell grid (`struct vt_cell` in
`src/libvt/vt_cell.h`: one codepoint, fg/bg color, attribute bits, and a
width of 1 or 2). Panes are composited cell by cell in
`src/libtile/tile_composite.c`, the result is diffed against a shadow
buffer, and only changed cells are re-emitted to the host terminal as
regenerated escape sequences (`src/librender/render.c`,
`render_cells_diff`). The host never sees the client's raw bytes.

That model has no place to store pixels. A cell holds a codepoint, not an
image reference, so anything drawn by an image protocol is not tracked and
is destroyed on the next frame diff. Supporting images therefore cannot
work by parsing them into cells. It has to work by treating the image as a
separate overlay that the host terminal draws, with lumi doing the
bookkeeping: remember what was placed, hide it on switch out, and replay it
on switch in.

## What exists today

`dcs_passthru()` in `src/cmd/attach/attach.c` re-emits raw DCS and APC
sequences to the host verbatim, but only when a single pane is visible
(`tile_pane_count(tilemgr) == 1`). Kitty graphics arrive as an APC
(`ESC _ G ... ST`), which the parser lumps into the same
`ST_DCS_PASSTHRU` bucket as SIXEL, so a kitty image already reaches the
host in the single-pane fullscreen case. It is fire and forget: nothing is
stored, so the image is clobbered on the next redraw and is not tracked,
scrolled, or repositioned.

The kitty *keyboard* protocol is unrelated and already supported
(`src/libvt/vt_state.h`). Do not confuse the two.

## The core problem: the image is not anchored

Testing showed that the existing pass-through does not even place the image
correctly, and this is the problem to solve first. The cause is an ordering
mismatch between parsing and rendering.

The client parses a window's output with `vt_parse_feed()`
(`src/cmd/attach/attach.c`), and `dcs_passthru()` fires *synchronously
inside that parse*, writing the image bytes straight to the outer terminal
at that instant. But rendering is deferred: parsing only sets
`need_render = 1`, and `flush_render()` runs later in the event loop, which
is where the outer cursor actually gets positioned (see the `if
(need_render)` block in `flush_render`, and `render_cells_diff` in
`src/librender/render.c`).

So the image is written to the outer terminal *before* the renderer
positions the cursor. It lands wherever the previous render left the outer
cursor, not where the content will be. Then the deferred render draws the
surrounding cells at the model position. The image and the text end up on
different rows. In a test that cleared the screen, drew an image at the top,
and printed a marker after it, the marker rendered at the top while the
image dropped lower, onto the previous prompt line.

Cursor accounting alone cannot fix this, because the image position itself
is unanchored. The fix is to stop writing the image at parse time:

1. In `dcs_passthru()`, do not write to the outer terminal. Record the raw
   graphics command together with the client `vt_state` cursor position at
   that moment (the cell where the image belongs).
2. Advance the client `vt_state` cursor by the image footprint so the
   following cells flow correctly (this is the cursor accounting below, now
   a second step rather than the whole fix).
3. During render, after `render_cells_diff` has drawn the cells and
   positioned the outer cursor, move the outer cursor to each recorded
   image's cell and emit its bytes there. The image is then anchored to the
   content and drawn in the correct order.

This is the same record-then-emit mechanism the placement table below uses
for window switches. Testing showed it is required for the initial draw as
well, not only on switch.

There is a second, independent cursor that also stays wrong until addressed
separately: the *server's* `vt_state` (`src/libsession/window.c`) answers
DSR cursor-position queries from the program (`vt_state_set_reply_fd` to the
pty master, `vt_ops.c` `case 'n'`). The client fix above corrects only what
is drawn. A program that queries its cursor after an image, and detached
snapshots or reattach, need the server side advanced too, which is harder
because the server does not know the outer terminal's cell pixel size. See
"Server-side accounting" below.

## Feasibility by client mode

| Mode | Verdict | Reason |
|---------|--------------------|-----------------------------------------|
| Minimal / single fullscreen pane | Works today | The existing DCS pass-through covers it. |
| Screen | Feasible | One full-screen window at a time. Window rect equals host rect, so no clipping is needed. This is the target of this design. |
| Turbo | Dead end (general case) | Overlapping windows. The protocol has no clip or occlusion primitive, so a window drawn on top cannot punch a hole in an image below. Only a non-overlapping tiled layout could be made to work, by cropping each image to its window rect. |

The rest of this document targets screen mode. Turbo is revisited only at
the end.

## The tractable path

Screen mode shows one window at full size, so pane count is 1 and the
existing pass-through gate already fires. The window rect always equals the
host rect, so there is no clipping problem. The kitty protocol separates
image *transmission* (send bytes once, stored by id) from *placement* (put
image N at the cursor), and placements are cheap to reissue. So the plan
is:

1. Record every graphics command per window as it flows through.
2. On switch out, tell the host to drop the outgoing window's placements
   but keep its stored image data.
3. On switch in, after the full cell redraw, replay the incoming window's
   placements.

No change to `struct vt_cell` and no change to the compositor are required.

## Cursor accounting (second step, after anchoring)

Once the image is anchored (recorded at parse, emitted at render), the
client's `vt_state` cursor still has to advance past it so the following
cells flow correctly. This was originally thought to be the whole fix; it is
not (see "The core problem" above), but it is still needed.

An earlier attempt implemented only this step and reset it after testing.
Two lessons from that: it does nothing without the anchoring fix, and it
must be validated against what is *drawn*, not against a DSR reply. A DSR
query is answered by the server, not the client, so it cannot confirm a
client-side cursor change (see "Server-side accounting").

When a graphics command displays an image (`a=T`, or `a=p`
without `C=1`), lumi must advance the window's `vt_state` cursor by the
image's cell footprint, matching kitty's cursor-movement rule (the cursor
ends just past the image). Advancing through `vt_state` rather than poking
the fields directly keeps scrolling and margins correct, the same as if
the client had emitted that many line feeds.

A capture of `kitten icat <image>` (no scaling) shows the shape of the
common case:

```
a=T,q=2,f=100,t=f,s=240,v=240,X=3
```

That is transmit-and-display (`a=T`), quiet (`q=2`), a PNG (`f=100`)
transferred by file path (`t=f`), with source pixel dimensions
`s=240,v=240` and a 3px sub-cell offset (`X=3`). Note what is absent:
there is no `c=` / `r=`, and no `i=` / `I=`. So the image is displayed at
natural size and carries no client image id.

Getting the footprint in cells therefore breaks down as:

- **Natural size, the common case (icat).** No `c=` / `r=`, so the
  footprint is `ceil(img_px / cell_px)` in each axis. The image pixel size
  is usually available directly from the command's `s=` (width) and `v=`
  (height), so the PNG header does not have to be parsed. The only missing
  input is the cell pixel size: read `ws_xpixel` / `ws_ypixel` from the
  attach client's outer TTY via `TIOCGWINSZ` and divide by `content_cols` /
  `content_rows`. This needs no host round trip. If the winsize pixel
  fields are zero, as they are on some terminals, query CSI 14 t / 16 t
  instead. For raw pixel formats (`f=24` / `f=32`) `s`/`v` are mandatory,
  so they are always present; the only case needing a header parse is a
  compressed format sent without `s`/`v`.
- **Explicit size.** If the command does carry `c=` and `r=` (a client
  that scales into a cell box), the footprint is known directly and
  overrides the computed value. Nothing extra is needed.

So the natural-size path is the one that matters for the common client, and
it requires cell pixel size. lumi does not track cell pixel size or handle
CSI 14 t / 16 t today, so that is a real dependency, not a follow-on. The
`TIOCGWINSZ` route keeps it cheap.

The exact cell where kitty leaves the cursor should be verified
empirically against the capture, then replicated, rather than assumed from
the spec wording.

## Data model

A graphics table hangs off each window. `struct client_window` is defined
in `src/cmd/attach/attach.c`.

```c
struct kgfx_cmd {
	char	*raw;		/* full "\e_G...\e\\" transmit bytes */
	size_t	 len;
	uint32_t img;		/* i= assigned id (0 until resolved) */
	uint32_t num;		/* I= image number the client used */
};

struct kgfx_place {
	uint32_t img;		/* i= image id */
	uint32_t num;		/* I= image number */
	uint32_t plc;		/* p= placement id */
	int	 row, col;	/* cursor cell at placement time */
};

struct kgfx_state {
	struct kgfx_cmd	  *xmit;			/* image data, replay verbatim */
	size_t		   xmit_n, xmit_cap;
	struct kgfx_place *place;		/* live placements to redisplay */
	size_t		   place_n, place_cap;
	uint32_t	   open_img, open_num;	/* transmit in progress */
};

struct client_window {
	uint32_t	id;
	struct vt_state	*vt;
	...
	struct kgfx_state gfx;			/* NEW */
};
```

The split between `xmit` and `place` matters. Transmission stores the
image bytes in the host by id. Placement puts a stored image on screen.
Replaying placements is cheap; re-transmitting bytes is the fallback.

Image id (`i=`) and image number (`I=`) are separate. A client may place
by number and let the host assign the id, which the host reports in a
reply. Both fields are carried so a reply can be matched back
(see "Image-number resolution").

## Record path

`dcs_passthru()` already sees every APC. Two changes: give it the window
id as its callback context (copy the `osc_passthru` wiring), and parse
kitty control data before the single-pane gate so background windows still
build their tables.

Parsing is split into `kgfx_parse()` (fill a `struct kgfx_ctl` from the
control keys) and `kgfx_apply()` (update the table), so the reply path can
reuse the parser.

```c
struct kgfx_ctl {
	int	 action, del_tgt, more, quiet, has_payload;
	uint32_t img, num, plc;
};
```

The control block is `G` then comma-separated `key=value` pairs, then an
optional `;` and base64 payload. The one subtlety is that some values are
letters (`a=t`, `d=a`) while others are integers (`i=`, `p=`, `m=`), so the
parser captures both the first value character and the integer value for
each key.

```c
static long
kgfx_int(const char **pp, const char *end)
{
	const char *p = *pp;
	long	 val = 0;
	int	 sign = 1;

	if (p < end && *p == '-') {
		sign = -1;
		p++;
	}
	while (p < end && *p >= '0' && *p <= '9')
		val = val * 10 + (*p++ - '0');
	*pp = p;
	return val * sign;
}

static void
kgfx_parse(const char *data, size_t len, struct kgfx_ctl *c)
{
	const char *p, *ctl_end;

	memset(c, 0, sizeof *c);
	if (len < 1 || data[0] != 'G')
		return;

	p = data + 1;
	ctl_end = p;
	while (ctl_end < data + len && *ctl_end != ';')
		ctl_end++;
	c->has_payload = (ctl_end < data + len);

	while (p < ctl_end) {
		int	 key = *p++;
		int	 vch;
		long	 val;

		if (p < ctl_end && *p == '=')
			p++;
		vch = (p < ctl_end && *p != ',') ? (unsigned char)*p : 0;
		val = kgfx_int(&p, ctl_end);

		switch (key) {
		case 'a': c->action  = vch;             break;	/* t T p d q f a c */
		case 'd': c->del_tgt = vch;             break;	/* a A i I p P ... */
		case 'i': c->img = (uint32_t)val;       break;
		case 'I': c->num = (uint32_t)val;       break;
		case 'p': c->plc = (uint32_t)val;       break;
		case 'm': c->more = (int)val;           break;
		case 'q': c->quiet = (int)val;          break;
		}

		while (p < ctl_end && *p != ',')	/* skip rest of value */
			p++;
		if (p < ctl_end && *p == ',')
			p++;
	}
}
```

`kgfx_apply()` records the effect. A payload-bearing block feeds the
retransmit stream; a placement or a delete updates the tables. A large
image arrives as several continuation blocks where only the first carries
`a=` and the id, so a chunk group is tracked across blocks by `open_img`
and `open_num`.

```c
static void
kgfx_apply(struct kgfx_state *g, const struct vt_state *vt,
    const char *data, size_t len, const struct kgfx_ctl *c)
{
	if (c->has_payload) {
		if (c->action == 't' || c->action == 'T') {
			g->open_img = c->img;
			g->open_num = c->num;
		}
		kgfx_add_xmit(g, g->open_img, g->open_num, data, len);
		if (!c->more)
			g->open_img = g->open_num = 0;
	}

	switch (c->action) {
	case 'T':	/* transmit + place at the cursor */
	case 'p':	/* place a previously transmitted image */
		kgfx_add_place(g, c->img, c->num, c->plc,
		    vt->cursor_row, vt->cursor_col);
		break;
	case 'd':	/* delete placements / images */
		kgfx_delete(g, c->del_tgt, c->img, c->num, c->plc);
		break;
	}
}
```

The cursor is read directly from `vt->cursor_row` / `vt->cursor_col`, the
same fields the cursor helpers use elsewhere in `attach.c`.

Continuation blocks assume a client never interleaves two images'
transmissions. The kitty spec forbids interleaving, so this holds for
conforming clients. A malformed stream at worst mis-tags a chunk, which a
later replay repairs by re-transmitting. It cannot corrupt the session.

## Table mutators

```c
static int
grow(void *arr_p, size_t *cap, size_t elt)
{
	void **arr = arr_p;
	size_t ncap = *cap ? *cap * 2 : 8;
	void *n = realloc(*arr, ncap * elt);

	if (!n)
		return -1;		/* drop the image, keep the session */
	*arr = n;
	*cap = ncap;
	return 0;
}
```

`kgfx_add_xmit()` re-wraps the payload as a complete `ESC _ ... ESC \`
block so replay is a plain write, and stores the group id and number.
`kgfx_add_place()` replaces an existing entry for the same `(img, plc)`
rather than duplicating it.

## Delete

Placements are what is visible, so match them precisely. Freeing stored
bytes is the uppercase (`d=A`, `d=I`, `d=P`) case. Placements may
swap-remove; transmit blocks must keep arrival order for chunk groups.

```c
static void
kgfx_delete(struct kgfx_state *g, int tgt, uint32_t img, uint32_t num,
    uint32_t plc)
{
	int	 lower, sel;
	size_t	 i;

	if (tgt == 0)
		tgt = 'a';				/* d= absent defaults to all */
	lower = (tgt >= 'a' && tgt <= 'z');
	sel = lower ? tgt : (tgt - 'A' + 'a');

	i = 0;
	while (i < g->place_n) {
		struct kgfx_place *p = &g->place[i];
		int hit;

		switch (sel) {
		case 'a': hit = 1;					break;
		case 'i': hit = (p->img == img);			break;
		case 'n': hit = (p->num == num);			break;
		case 'p': hit = (p->img == img && p->plc == plc);	break;
		default:  hit = 0;			/* c/z/x/y ranges: unsupported */
		}
		if (hit)
			g->place[i] = g->place[--g->place_n];
		else
			i++;
	}

	if (lower)			/* keep image data for re-placement */
		return;

	switch (sel) {
	case 'a':
		for (i = 0; i < g->xmit_n; i++)
			free(g->xmit[i].raw);
		g->xmit_n = 0;
		break;
	case 'i':
		kgfx_free_xmit_img(g, img);	/* order-preserving compaction */
		break;
	}
}
```

## Forwarding and image-number resolution

Only the visible, watched window forwards to the host, so at most one
window has graphics in flight at a time. A client that places by image
number (`I=`) with no id expects the host to assign an id and report it in
a reply. lumi must catch that reply, resolve the number to the id in the
window's table, and route the raw reply back to the program that asked so
its acknowledgement is not lost.

This mirrors the existing OSC 10/11 color-query round trip
(`osc_passthru` and `stdin_osc_reply` in `attach.c`).

### Forward gate and pending queue

```c
static void
dcs_passthru(void *ctx, int introducer, const char *data, size_t len)
{
	uint32_t id = (uint32_t)(uintptr_t)ctx;
	struct client_window *cw = cwin_find(id);
	int visible;
	char intro[2];

	visible = tilemgr && tile_pane_count(tilemgr) == 1 &&
	    watching && watched_id == id;

	if (introducer == '_' && len >= 1 && data[0] == 'G' && cw) {
		struct kgfx_ctl c;

		kgfx_parse(data, len, &c);
		kgfx_apply(&cw->gfx, cw->vt, data, len, &c);

		if (visible && c.num && !c.img && c.quiet < 2 &&
		    (c.action == 't' || c.action == 'T'))
			kgfx_pend_push(id, c.num);	/* await host's assigned id */
	}

	if (!visible)
		return;
	intro[0] = '\033';
	intro[1] = (char)introducer;
	tio_write(STDOUT_FILENO, intro, 2);
	tio_write(STDOUT_FILENO, data, len);
	tio_write(STDOUT_FILENO, "\033\\", 2);
}
```

The pending queue is a small FIFO of `{win, num}` matched by image number.
Because only one window forwards at a time, numbers cannot collide across
windows in the queue.

```c
#define KGFX_PEND_MAX 64
struct kgfx_pending { uint32_t win, num; };
static struct kgfx_pending kgfx_pend[KGFX_PEND_MAX];
static int kgfx_pend_n;
/* kgfx_pend_push, kgfx_pend_take(num), kgfx_pend_drop_win(win) */
```

### Catching the reply

The host's reply arrives on the client's stdin, where `stdin_osc_parser`
already runs (fed at `attach.c` around line 4882). The parser NULL-guards
its ops, so adding a DCS callback is safe. Wire it next to the OSC one at
setup time:

```c
	vt_parse_set_dcs_cb(stdin_osc_parser, stdin_kgfx_reply, NULL);
```

```c
static void
kgfx_resolve(struct kgfx_state *g, uint32_t num, uint32_t img)
{
	size_t i;

	for (i = 0; i < g->xmit_n; i++)
		if (g->xmit[i].num == num && g->xmit[i].img == 0)
			g->xmit[i].img = img;
	for (i = 0; i < g->place_n; i++)
		if (g->place[i].num == num && g->place[i].img == 0)
			g->place[i].img = img;
}

static void
stdin_kgfx_reply(void *ctx, int introducer, const char *data, size_t len)
{
	struct kgfx_ctl c;
	struct client_window *cw;
	struct mconn *mc;
	uint32_t win;
	char out[320];
	int n;

	(void)ctx;
	if (introducer != '_' || len < 1 || data[0] != 'G')
		return;

	kgfx_parse(data, len, &c);

	win = c.num ? kgfx_pend_take(c.num) : 0;
	if (!win)
		win = watched_id;		/* only the visible window forwards */

	if (c.num && c.img && (cw = cwin_find(win)) != NULL)
		kgfx_resolve(&cw->gfx, c.num, c.img);

	mc = mconn_find_by_pid((pid_t)win);
	if (!mc)
		return;
	n = snprintf(out, sizeof out, "\033_%.*s\033\\", (int)len, data);
	if (n > 0 && n < (int)sizeof out)
		mconn_ipc_send(mc, IPC_MSG_INPUT, out, (uint32_t)n);
}
```

A client that sets `q=1` or `q=2` gets no reply, so nothing resolves. That
is correct: those clients used ids directly or do not need acks. Background
windows never forward, so their numbers resolve lazily at replay time, when
the ids actually need to exist on the host.

A capture of `kitten icat` shows it uses `q=2` and no `i=` / `I=` at all,
so the resolution path does not apply to it. Resolution matters only for
clients that place by image number and expect the host to assign an id.
The consequence for icat is in the replay section below: an image with no
client id cannot be re-placed by id, so it must be re-transmitted.

## Switch hooks and replay

`micro_select_window()` in `attach.c` is the switch point. Its screen-mode
branch already forces a full redraw with `tile_need_full = 1`. Wrap the
image handoff around it: hide the outgoing window's placements, and defer
the incoming replay until after the redraw so text does not paint over
freshly placed images.

```c
	} else {
		kgfx_hide_all(STDOUT_FILENO);	/* \e_Ga=d,d=a\e\\ : drop placements, keep data */
		tile_show_window(id, vt);
		screen_fit_window(id);
		tile_need_full = 1;
		gfx_replay_pending = id;	/* replay after the redraw */
	}
```

Run the replay at the tail of the full-redraw branch in `tiled_render()`:

```c
	if (tile_need_full) {
		render_cells_full(renderer, STDOUT_FILENO, ...);
		tile_need_full = 0;
		if (gfx_replay_pending) {
			kgfx_replay(cwin_find(gfx_replay_pending), STDOUT_FILENO);
			gfx_replay_pending = 0;
		}
	}
```

## Replay: re-transmit versus host persistence

There are two ways to replay, and they trade memory and switch latency
against a dependency on host behavior.

### Strategy A: always re-transmit

Re-upload every image, then re-place. Self-contained and assumes nothing
about the host.

```c
static void
kgfx_replay(struct client_window *cw, int fd)
{
	size_t i;
	char cmd[64];
	int n;

	if (!cw)
		return;
	for (i = 0; i < cw->gfx.xmit_n; i++)		/* re-upload every image */
		tio_write(fd, cw->gfx.xmit[i].raw, cw->gfx.xmit[i].len);
	for (i = 0; i < cw->gfx.place_n; i++) {
		struct kgfx_place *p = &cw->gfx.place[i];
		render_move_cursor(renderer, fd, p->row, p->col);
		n = snprintf(cmd, sizeof cmd, "\033_Ga=p,i=%u,p=%u,q=2\033\\",
		    p->img, p->plc);
		tio_write(fd, cmd, n);
	}
}
```

### Strategy B: trust host persistence, repair lazily

Switch out hides placements with lowercase `d=a`, which keeps the stored
image data. Switch in only re-places, sending the placement with `q=0` so
the host reports an error if it has evicted the image. Those replies must
not reach the client, so they are tracked in a separate internal pending
list and swallowed by `stdin_kgfx_reply`. On an error, re-transmit that
image's blocks and re-issue the placement. If the image bytes were
discarded to save memory (call it B2), there is nothing to retransmit and
the image is simply gone.

### Comparison

| Axis | A: always re-transmit | B1: lazy, keep bytes | B2: lazy, drop bytes |
|---|---|---|---|
| Client memory | full image bytes per window | same as A | near zero |
| Bytes per switch | every image re-uploaded | placement commands only | placement commands only |
| Switch latency | grows with image size, worse over ssh | near-instant, repair on miss | near-instant, miss = lost image |
| Host assumptions | none | persistence plus a reply on eviction | persistence, loss acceptable |
| Failure mode | none from host | correct even if evicted | image silently missing |
| Extra machinery | none | internal pending list, q=0 branch, retransmit-by-id | internal pending list, swallow |
| Portability | works wherever protocol works | depends on host keep-data and quota | most fragile |

### Recommendation

Ship Strategy A first. It is correct on every terminal that supports the
protocol, needs no reply-suppression machinery, and its only real cost,
memory, is bounded by an eviction cap on `xmit` that is worth having
regardless. It composes cleanly with resolution: an unresolved `img == 0`
placement simply uploads, and the reply resolves it.

Move to B1 only behind a measurement of switch latency on a networked
attach, and only after the internal-versus-client reply branch is in place,
since getting that wrong routes spurious acks into the user's shell. Keep
B2 off the table unless memory is the binding constraint and a
vanished-after-eviction image is acceptable, which for an image viewer or
plot it usually is not. The pieces B needs are additive and can land later
without touching the record, delete, or resolution code.

There is a hard limit on B: it re-places by image id, so it only works for
images the client transmitted with an id (`i=`). `kitten icat` transmits
with no id (see the capture above), so its images can never be re-placed,
only re-transmitted. Strategy A is therefore mandatory for icat-style
output, and B can only ever be an optimization layered on top for the
subset of clients that assign ids. Replay logic should key off whether a
recorded placement has a resolved id: id present, B may re-place; id
absent, fall back to re-transmitting the stored command.

## Lifecycle and plumbing

`cwin_add_sized()` inits window fields individually without zeroing the
struct, and `cwin_remove()` compacts the array with the swap-remove idiom
`cwins[i] = cwins[--cwin_count]`. Both matter for a table that owns heap
memory.

- `cwin_add_sized()`: pass the window id to the DCS callback
  (`vt_parse_set_dcs_cb(cw->parser, dcs_passthru, (void *)(uintptr_t)id)`)
  and zero the table (`memset(&cw->gfx, 0, sizeof cw->gfx)`).
- `cwin_remove()`: call `kgfx_free(&cwins[i].gfx)` and
  `kgfx_pend_drop_win(cwins[i].id)` before the swap.
- `cwin_free_all()`: call `kgfx_free()` in the loop.

```c
static void
kgfx_free(struct kgfx_state *g)
{
	size_t i;

	for (i = 0; i < g->xmit_n; i++)
		free(g->xmit[i].raw);
	free(g->xmit);
	free(g->place);
	memset(g, 0, sizeof *g);
}
```

## Server-side accounting

There are two cursors, and the client fix only corrects one.

- The **client** `vt_state` (`src/cmd/attach/attach.c`) drives what is drawn
  on the outer terminal. The anchoring and cursor-accounting fixes above
  operate here.
- The **server** `vt_state` (`src/libsession/window.c`) is what the program
  actually talks to. It owns the pty and answers DSR cursor-position queries
  (`vt_state_set_reply_fd` to the pty master; `vt_ops.c` `case 'n'`). It is
  the state used for detached rendering and for the screen a client rebuilds
  on reattach.

The client and server each parse the same raw byte stream independently
(`IPC_MSG_OUTPUT` carries raw pty bytes, which the client re-parses). So for
full consistency both cursors must advance by the same footprint. The client
can compute the footprint because it knows the outer terminal's cell pixel
size (`TIOCGWINSZ` on its own tty). The server cannot: it allocated the
program's pty and does not know the outer cell size.

Options for the server side, in rough order of effort:

1. Plumb the outer pixel dimensions from the client to the server on resize
   (extend the resize IPC), set them on the program's pty with `TIOCSWINSZ`
   (`ws_xpixel`/`ws_ypixel`), and compute the footprint server-side. This
   also lets programs like `kitten icat` query the correct pixel size and
   send `c=`/`r=` themselves. This is the correct long-term fix.
2. Have the client send the computed cursor advance to the server as an
   explicit cursor move, keeping the server authoritative for DSR without it
   needing the cell size.

Until one of these lands, a program that queries its cursor after drawing an
image, and reattach or detached snapshots, will be off. The purely visual
case (draw an image, print below it) is fixed by the client side alone.

## Transfer modes and remote connections

The kitty protocol sends image data one of several ways, chosen by the `t=`
key: `t=d` inlines the base64 image data in the APC payload; `t=f` names a
file by path; `t=t` names a temporary file; `t=s` uses shared memory. The
file and shared-memory modes require whoever renders the image, the outer
terminal, to open that path or segment on its own host.

That breaks across a host boundary. Over SSH, or over lumi's own networked
attach (`attach -n`), the program runs on one host and the outer terminal on
another, so a `t=f` path is meaningless to the terminal and it reports
something like "the file could not be opened." Confirmed in testing: a
hand-written `t=f` command run through kitty over SSH fails. Only `t=d`
(and `t=s` when both ends share memory, i.e. same host) survive a remote
connection.

Real `kitten icat` detects a remote or unshareable target and switches to
`t=d`, so it keeps working; only a hand-crafted `t=f` command fails. But the
inline path is heavier: the whole image is base64-encoded and streamed
through the pty, the lumi server, the IPC channel, and out to the terminal.

lumi does not rewrite transfer modes. It forwards the APC as sent, so a
`t=f` command only works when the terminal shares a filesystem with the
program, which for a networked lumi session it does not. This is a genuine
disadvantage of the kitty protocol for remote use compared to SIXEL, which
always carries its pixels inline and is therefore unaffected by host
boundaries.

Two things follow for this design:

- Cursor accounting does not depend on the transfer mode, as long as the
  command carries `r=`/`c=` or `s=`/`v=`. Inline clients such as icat do.
  A file-only command with no size keys can neither render remotely nor be
  measured, which reinforces relying on `s=`/`v=` rather than reading the
  file.
- A possible future improvement is for lumi to rewrite `t=f`/`t=t` into
  `t=d` by reading the file on the server side and inlining it, so
  file-based clients work across the lumi boundary. The cost is server file
  access, base64 encoding, and much larger IPC and terminal writes, so it is
  noted as future work, not part of this design.

## Known limits and open questions

- **Coordinate drift.** Classic placements anchor at a cursor cell and
  scroll with the region. A saved absolute `(row, col)` is correct only if
  the window has not scrolled since placement. This is fine for a static
  full-screen image and wrong for images in a scrolling log. The robust
  fix is kitty's unicode-placeholder mode (`U=1`), where the image binds to
  placeholder cells that flow with the grid. That requires storing the
  placeholder codepoint, its combining diacritics, and a foreground-encoded
  image id per cell, which the current `vt_cell` cannot hold. That cell
  model extension is the dividing line between "works for static images"
  and "works generally."
- **Memory.** Cap total `xmit` bytes and evict oldest, evicting whole
  chunk groups rather than single blocks.
- **Animation.** `a=f` frames and `a=a`/`a=c` control are stored as raw
  payload but not modeled, so a static replay shows the last composed
  frame. Full animation replay needs a frame table.
- **Turbo mode.** The same tables work, but a placement anchor must be
  offset by the window origin and the source cropped to the window rect
  (kitty placement source x/y/w/h). Occlusion by an overlapping window
  cannot be expressed in the protocol, so replay is meaningful only for the
  top window. Screen mode is where this design closes.

## Build order

Steps 1, 2, 4, and 6 have shipped; see "As implemented". Step 3 shipped in
the minimal form (the step-1 record store), not as the general table. Steps
5, 7, and 8 remain.

1. **Anchoring.** (done) Record the graphics command plus the client
   `vt_state` cursor at parse time in `dcs_passthru` instead of writing it
   out, and emit it during render (`tiled_render`, after `render_cells_diff`)
   at the recorded cell. This is what makes the image appear in the right
   place at all. Confirmed by testing to be the actual first bug.
2. **Client cursor accounting.** (done) Advance the client `vt_state` cursor
   by the footprint (`r=`/`c=`, else `ceil(v= / TIOCGWINSZ cell size)`) so
   the following output flows below the image. Kitty leaves the cursor on the
   image's last row, so advance `rows - 1`, not `rows`. Validate visually,
   not with DSR.
3. Data model, `kgfx_parse` / `kgfx_apply`, table mutators, `kgfx_free`,
   and the lifecycle plumbing (generalize the step-1 record store).
   (Shipped only as the minimal per-window `gfx[]` store, not the general
   table.)
4. **Switch hooks and Strategy A replay.** (done) Re-transmit; the only
   option for images with no client id. Note the ED consequence: re-emit the
   store after every full render, not once per switch.
5. Image-number resolution: pending FIFO, stdin DCS callback,
   `kgfx_resolve`, reply routing. Only benefits clients that place by
   number; icat does not.
6. **Server-side accounting** (done) so DSR, reattach, and detached
   snapshots are correct (plumb outer pixel size to the server, or forward
   the cursor advance). See "Server-side accounting".
7. Header parsing fallback for compressed images sent without `s=`/`v=`,
   and CSI 14 t / 16 t if `TIOCGWINSZ` pixel fields are zero.
8. Optional: Strategy B lazy replay, for id-bearing clients only, behind a
   latency measurement.
