# Drawing Abstraction (libdraw) Design

Status: roadmap steps 1 through 3 have landed. libdraw (the surface, event
queue, and a mock driver) and libdraw_term (the terminal driver) exist with a
mock-driven test suite, and all three standalone apps (`edit`, `files`, and
`splash`) are ported onto the surface. The remaining steps (the pad-stack
migration and a graphical backend) are design only, and this document is the
plan of record for that work.

## Motivation

Today lumi paints screens by emitting raw terminal escape sequences from several
independent, hand-rolled paths. Nothing but a terminal can be a rendering target,
and the same SGR-building logic is duplicated three times. The paths are:

- `librender` (`src/librender/render.c`): the differential compositor renderer
  for the attach main pane. It builds escapes and writes them through `libtio`.
- `libtui_term` (`src/libtui_term/tui_term.c`): the one concrete backend behind
  `struct tui_backend`. It turns cells into escapes (cell plus flush).
- `libtui/tui_out` (`src/libtui/tui_out.c`): a frame buffer with escape helpers
  used by the standalone full-screen subcommands (`edit`, `files`, `splash`).
  Callers still pass raw escape strings through `tui_out_puts`.

Apps also inline escapes directly. `edit` has a `syn_sgr[]` palette and reverse
video literals; `picker.c` builds its taskbar from literal SGR strings; alt
screen, cursor shape, and OSC control strings are scattered across `attach`,
`edit`, and `files`.

Three near-identical SGR emitters exist because `libtxl` deliberately keeps SGR
out of the capability layer (each renderer must track incremental SGR state).
See the note in `src/libtxl/txl.h`.

Input already funnels through a single event type, `struct tkbd_seq`
(`src/libtermlib/tkbd.h`), which unifies keys, mouse, and characters. Everything
downstream consumes `tkbd_seq`, and `src/cmd/attach/gpm_mouse.c` already
synthesizes `tkbd_seq` values from a non-escape source.

The goal is a common drawing layer that apps target instead of escapes, with a
driver seam so a graphical backend can one day replace the terminal without
touching app code.

## Relationship to the existing tui_backend

`struct tui_backend` in `src/libtui/tui_pad.h` is the right idea already:

```
struct tui_backend {
    void (*cell)(void *ctx, int row, int col, uint32_t cp,
        const struct vt_color *fg, const struct vt_color *bg,
        uint16_t attrs);
    void (*flush)(void *ctx);
};
```

It isolates the pad-stack widget code from any display target, and
`src/libtui/test_tui.c` already swaps in a `mock_backend`. It is too narrow to
serve as the general seam, though: it is driven only by the overlay pad stack,
that stack is capped at 48 by 80 cells, and it has no notion of cursor,
screen lifecycle, clipboard, title, or input. `libdraw` generalizes this seam.
A later step re-expresses `tui_backend` in terms of the new driver.

## Scope

Confirmed decisions for the current effort:

- Design document only. No `libdraw` code and no app ports in this phase.
- A new `libdraw` layer, not an in-place extension of `tui_backend`.
- Target the standalone app path (`edit`, `files`, `splash`) and define a
  symmetric input seam.
- The attach compositor keeps its `librender` fast path. It is not ported. The
  terminal driver reuses `librender` internally, so the diff engine is shared,
  not duplicated.

## Core concepts

1. Cell model, reused not reinvented. `struct vt_cell` (`src/libvt/vt_cell.h`)
   holds a codepoint, `struct vt_color` fg and bg (a DEFAULT, INDEXED, or RGB
   tagged union), an `attrs` bitmask (`enum vt_attr`: BOLD, REVERSE, UNDERLINE,
   and so on), and a width. Backends consume cells and never see escapes.
2. Surface (`struct draw`). A full-screen cell grid an app draws into, plus
   logical cursor state and a bound driver. It owns the current frame buffer.
   `draw_present` hands that buffer to the driver, which diffs and flushes. The
   surface never keeps a second (previous) frame: differencing is the driver's
   job, and the terminal driver already has one shadow in `librender`.
3. Driver vtable (`struct draw_driver`). The swap point: a terminal driver
   today, a graphical driver later. One instance per output target.
4. Input source. The driver optionally yields `struct tkbd_seq` events, so a
   graphical backend has a symmetric input story. It synthesizes `tkbd_seq`, the
   same way `gpm_mouse.c` already does for the Linux console mouse.

## Surface API (proposed `src/libdraw/draw.h`)

Backend-neutral drawing. These fill a cell grid; they do not emit bytes.

```
struct draw *draw_new(const struct draw_driver *drv, void *ctx);
                                                        /* size queried from driver */
void draw_free(struct draw *d);
void draw_begin(struct draw *d);                        /* enter; also on resume */
void draw_end(struct draw *d);                          /* leave; also to suspend */
void draw_size(struct draw *d, int *rows, int *cols);   /* cached dims, no driver call */
void draw_resize(struct draw *d, int rows, int cols);   /* resize the grid (draw_wait calls it) */

void draw_clear(struct draw *d);                        /* reset to the default cell */
void draw_cell(struct draw *d, int r, int c, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);
int  draw_text(struct draw *d, int r, int c, const char *utf8,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);
    /* width-correct; returns the column after the last cell written */
void draw_fill(struct draw *d, int r, int c, int n, uint32_t cp,
    struct vt_color fg, struct vt_color bg, uint16_t attrs);

void draw_cursor(struct draw *d, int r, int c);
void draw_cursor_vis(struct draw *d, int on);
void draw_cursor_shape(struct draw *d, enum draw_cursor_shape shape);
    /* DRAW_CURSOR_DEFAULT, DRAW_CURSOR_BLOCK, DRAW_CURSOR_BAR, DRAW_CURSOR_UNDERLINE */

void draw_present(struct draw *d);                      /* diff, then flush */

int  draw_wait(struct draw *d, struct draw_event *ev);  /* KEY | RESIZE | RESUME | EOF */
int  draw_next_event(struct draw *d, int timeout_ms, struct tkbd_seq *out);
int  draw_peek_event(struct draw *d, struct tkbd_seq *out);
void draw_set_clipboard(struct draw *d, const char *utf8, size_t n);
void draw_set_title(struct draw *d, const char *utf8);  /* optional; no-op if unsupported */
void draw_bell(struct draw *d);                         /* optional; no-op if unsupported */
```

`draw_wait` is the loop an app pumps. It blocks until the next event and reports
its type: a key (in `ev->key`), a terminal resize, a resume from suspend, or EOF.
The whole terminal lifecycle lives behind it: on a resize the surface has already
resized its own grid, and on a resume it has already left and re-entered the
terminal around the stop, so the app just repaints on either and installs no
signal handlers of its own. This is where SIGWINCH and SIGTSTP are handled, once,
for every app instead of per command.

Underneath sits a key-only event queue on the Win32 GetMessage and PeekMessage
model: the driver drains decoded key events into the surface's queue,
`draw_next_event` removes the next (blocking up to a timeout, refilling from the
driver when empty) and `draw_peek_event` returns the next without removing it.
`draw_wait` is built on `draw_next_event` plus the driver's resize and suspend
reporting; a lower-level `draw_poll` (dispatch every drained event to a callback)
sits under the queue.

`DRAW_CURSOR_DEFAULT` is the terminal's own configured cursor (the surface starts
there), so an app that does not care about the shape leaves it alone. The
terminal driver emits DECSCUSR for it.

`draw_set_clipboard`, `draw_set_title`, and `draw_bell` forward to the driver
methods of the same name, so an app talks only to the surface and never holds the
driver directly. All three are no-ops when the driver leaves the slot NULL.

`draw_begin` and `draw_end` bracket a drawing session. They run at startup and
shutdown, and again to suspend and resume. `draw_wait` calls them itself around a
job-control stop, so an app that only pumps `draw_wait` never calls them for
SIGTSTP. An app leaving for a shell-out still calls `draw_end` and then
`draw_begin` on return. They forward to the driver, so the terminal driver puts
the fd in raw mode and enters the alt screen in `begin` and restores both in
`end`, while a graphical driver opens and closes its window. Apps therefore stop
calling `tio_raw` or emitting alt-screen escapes themselves.

`draw_clear` resets every cell to the default (a space with default colors). An
app that wants a themed background fills the region with `draw_fill`; there is no
`draw_clear(bg)` variant.

`draw_text` reuses the display-width logic already in `tui_out_field`
(`src/libtui/tui_out.c`): decode UTF-8, honor rune widths, render controls as a
space, and drop a wide rune that would straddle the right edge. That logic moves
into `libdraw` so it writes cells rather than escape bytes. Both `draw_text` and
`tui_out_field` call the `libutf8` rune-width primitive, so the shared width
tables are not duplicated while the two coexist during migration.

A width-2 codepoint written by `draw_cell` or `draw_text` sets the following
cell to a continuation marker (`DRAW_CELL_CONT`, which mirrors libtui's
`TUI_CODEPOINT_CONT` but is defined locally so the surface does not depend on a
higher layer). The renderer keys off the cell width, not the marker value, so a
zero-width continuation cell is skipped and a wide glyph occupies two columns
without a stray second cell being drawn.

The drawing calls also mark their target rows dirty (a one-byte-per-row bitmap
kept in the surface). This is set on write, not by comparing frames, and it is
what `draw_present` forwards to the driver so clean rows are skipped.

## Driver vtable (proposed `src/libdraw/draw_driver.h`)

The one seam a new backend implements. `present` takes a bundled frame so the
signature stays narrow and the surface can hand over its own buffer by pointer:

```
struct draw_frame {
    const struct vt_cell *cells;    /* rows*cols, row-major */
    int rows, cols;
    const uint8_t *row_dirty;       /* one byte per row; NULL means all rows */
    int cur_row, cur_col;
    int cur_vis, cur_shape;
};

struct draw_driver {
    /* Required. begin/end may be called more than once (suspend and resume). */
    void (*begin)(void *ctx);          /* terminal: raw mode + alt screen; GUI: open window */
    void (*end)(void *ctx);            /* undo begin: restore mode/screen, or close window */
    int  (*size)(void *ctx, int *rows, int *cols);
    void (*present)(void *ctx, const struct draw_frame *f);
    int  (*poll)(void *ctx, int timeout_ms,
        void (*on_event)(void *u, const struct tkbd_seq *seq), void *u);
                                       /* returns events dispatched, or -1 on error/EOF */
    void (*free)(void *ctx);

    /* Optional; a NULL slot is a no-op the surface skips. */
    void (*set_clipboard)(void *ctx, const char *utf8, size_t n);
    void (*set_title)(void *ctx, const char *utf8);
    void (*bell)(void *ctx);
    int  (*signals)(void *ctx);        /* pending DRAW_SIG_* bits, cleared on read */
    void (*suspend)(void *ctx);        /* stop now; returns once resumed */
};
```

Six required methods plus `set_clipboard`, which the app path uses. `set_title`
and `bell` are left NULL until a ported app needs them, so a minimal driver
stays minimal. `signals` and `suspend` are how a driver that owns terminal
signals feeds `draw_wait`: `signals` returns and clears pending `DRAW_SIG_RESIZE`
and `DRAW_SIG_SUSPEND` bits, and `suspend` performs the actual job-control stop
(the terminal driver restores the default SIGTSTP disposition, `raise`s it, and
restores its handler on return). A driver that leaves both NULL delivers only
KEY and EOF events.

There is no capabilities struct in v1. Color down-conversion (RGB to 256 to 16)
lives in the driver, so an app just passes `vt_color` values and the driver maps
them to what it supports. If a later app must branch on the target (for example
ASCII versus box-drawing glyphs), a small typed query is added then, not a broad
`is_graphical` flag that would leak the backend's identity into app code.

A non-portable escape hatch (`draw_raw`) for terminal-only niches (graphics
passthrough, the kitty or modifyOtherKeys keyboard protocols) is deferred: those
niches live in `attach`, which is out of scope, so no ported app needs one yet.
When added it is documented as non-portable and a graphical backend ignores it.

## The terminal driver (proposed `src/libdraw_term/`)

The first concrete driver, built entirely from existing libraries:

- `begin` puts the fd in raw mode (`tio_raw`), enters the alt screen (`libtxl`
  SMCUP), and enables bracketed paste (mode 2004). Mouse reporting is opt-in
  through `draw_term_mouse`, off by default, so an app that ignores the mouse
  (like `edit`) leaves the terminal's own selection working. `end` reverses all
  of it (`tio_restore`, RMCUP, paste and mouse off). These are the sequences
  `edit` and `files` inline today, and both run again across a SIGTSTP suspend
  and resume, so an app never touches raw mode or these escapes directly.
- `size` uses one shared `TIOCGWINSZ` helper. The ioctl is currently inlined in
  about a dozen places; the driver is a good home for a single copy.
- `present` forwards `f->cells` straight to `render_cells_diff`
  (`src/librender/render.h`) with no copy, because the surface stores its grid
  as the same flat `struct vt_cell` array `render` consumes, and passes
  `f->row_dirty` as `render`'s existing dirty hint. The diff engine and its one
  shadow buffer are reused, not rewritten; `present` is close to a one-line
  forward.
- `set_clipboard` emits OSC 52, as `edit`'s `clip_osc52` and `selection.c` do.
- `poll` reads the fd and runs `tkbd_drain` (`src/libtermlib/tkbd.h`),
  dispatching each decoded `tkbd_seq`.
- `set_title` (OSC 2) and `bell` (BEL) are trivial to add when the first app
  needs them; the terminal driver leaves those slots NULL until then.
- Color down-conversion (RGB to 256 to 16) is a driver concern, but the reused
  renderer currently emits truecolor SGR directly (as it does for attach), so
  the terminal driver forwards `vt_color` values unchanged for now. Down-
  converting for non-truecolor terminals is a shared enhancement to `librender`
  (`txl_has_rgb`, `txl_colors`), deferred so it lands once for both this driver
  and attach.

A trivial mock or capture driver, mirroring the `mock_backend` in
`src/libtui/test_tui.c`, records `present` cells for headless tests. It proves
the seam is swappable and is what the eventual `test_draw` suite drives.

## Out of scope

- No implementation. This phase produces this document only.
- The attach compositor keeps its `librender` fast path. It is not ported. The
  terminal driver reuses `librender`, so no rendering logic is duplicated.
- The overlay pad stack and `struct tui_backend` stay as they are. A later step
  re-expresses them in terms of `draw_driver`.
- No graphical backend is written. Only the seam is defined so one can be added.
- Terminal-only protocol (graphics passthrough, keyboard protocol) has no ported
  consumer, so the `draw_raw` hatch and a capabilities query are deferred until
  one appears.

## Adoption roadmap

Steps 1 and 2 have landed; the rest is documented for later.

1. (Done) Land `libdraw` (surface, width logic, mock driver) and `libdraw_term`
   (the terminal driver wrapping `librender`, `libtxl`, and `libtio`) with a
   `test_draw` suite driven by the mock driver.
2. (Done) Port `edit`: `syn_sgr[]` became `vt_color` and attrs on `draw_cell`;
   reverse video became `VT_ATTR_REVERSE`; DECSCUSR became `draw_cursor_shape`;
   raw mode, alt screen, and paste became `draw_begin` and `draw_end`; OSC 52
   became `draw_set_clipboard`; input moved to `draw_wait`, which also delivers
   resize and resume events so the app installs no signal handlers.
3. (Done) Port `files`, then `splash`. `files` moved the same way as `edit`, and
   its pager and editor shell-out (`open_file`) became `draw_end` before the
   child and `draw_begin` after, which is the suspend and resume the surface
   lifecycle was built for. `splash` blits its canvas cells into the surface with
   `draw_cell` and presents; its old `splash_show` (which built its own vt_state
   and render) is removed, so libsplash no longer depends on librender.
3a. (Done) Consolidate the signal handling. SIGWINCH and SIGTSTP moved into
   `libdraw_term`, which raises `DRAW_SIG_RESIZE` and `DRAW_SIG_SUSPEND` flags
   through the driver's `signals` method. `draw_wait` acts on them: on resize it
   re-queries the driver and calls `draw_resize`, then returns a RESIZE event; on
   suspend it runs `draw_end`, delegates the stop to the driver's `suspend`, then
   `draw_begin` and returns a RESUME event. `edit`, `files`, and `splash` dropped
   all of their per-app signal code, including `edit`'s earlier SIGTSTP one-off.
   Raw mode disables the terminal's own Ctrl-Z (it is a key), so the driver's
   SIGTSTP is one from job control or an explicit `kill -TSTP`.
4. Re-express the pad-stack `tui_backend` on top of `draw_driver`, then migrate
   attach's overlay apps. Note the model shift: `tui_backend` is per-cell
   (`cell` called for each changed cell), while `draw_driver` is per-frame, so
   the pad stack composites into a `draw` surface's grid and then calls
   `draw_present`, rather than emitting cell by cell.
5. Future: a graphical driver that implements `draw_driver` and synthesizes
   `tkbd_seq`.

## Mapping from current escapes to libdraw

| Today | libdraw |
|-------|---------|
| `edit` `syn_sgr[]` raw SGR | `vt_color` and attrs on `draw_cell` |
| `draw_line` reverse video (`ESC [7m` / `ESC [27m`) | `VT_ATTR_REVERSE` |
| `edit` DECSCUSR (`ESC [N q`) | `draw_cursor_shape` |
| raw mode, alt screen, paste (`tio_raw`, `?1049`, `?2004`) | `draw_begin` / `draw_end` |
| cursor hide and show (`?25`) | `draw_cursor_vis` |
| OSC 52 (`clip_osc52`, `selection.c`) | `set_clipboard` |
| OSC 2 title (`sync_host_title`, attach only) | `set_title` (optional, deferred) |
| `tui_out_field` | internal to `draw_text` |
| `render_cells_diff` | terminal driver `present` |
| `tkbd_drain` | terminal driver `poll` |
| graphics passthrough, keyboard protocol (attach only) | `draw_raw` (deferred, non-portable) |

## Resolved decisions

- Resize and suspend ownership. The driver owns the terminal signals: it catches
  SIGWINCH and SIGTSTP and reports them through the optional `signals` method as
  `DRAW_SIG_RESIZE` and `DRAW_SIG_SUSPEND` bits, and stops itself in `suspend`.
  `draw_wait` acts on those bits, re-querying `size` and calling `draw_resize` on
  a resize and bracketing the stop with `draw_end` and `draw_begin` on a suspend,
  and delivers a RESIZE or RESUME event to the app. Apps install no signal
  handlers, and `draw_size` only returns cached dimensions. An earlier design had
  each app own a SIGWINCH handler and call `draw_resize`; that was rejected once
  three apps repeated the same signal boilerplate. Signals are process-global and
  terminal-specific, so they belong in the driver, and the surface orchestrates
  the response so a graphical backend can raise the same events without signals.
- Dirty tracking. The surface marks a row dirty when a drawing call writes to it
  (a one-byte-per-row bitmap), never by comparing frames. `draw_present` passes
  that bitmap to the driver, and the terminal driver hands it to
  `render_cells_diff`, which already keeps the only shadow. Frame differencing in
  the surface was rejected: it would duplicate `librender`'s shadow and diff.
- Vtable size. Six required methods plus `set_clipboard`. `set_title`, `bell`, a
  capabilities query, and the `draw_raw` hatch are deferred behind NULL slots or
  omitted until a ported app is their first consumer, so the abstraction does not
  carry capability no user exercises.
- Session lifecycle. `draw_begin` and `draw_end` bracket drawing and are callable
  more than once, so they double as the suspend, resume, and shell-out mechanism
  (SIGTSTP, or leaving for a pager). The terminal driver owns raw mode and the
  alt screen inside them, which removes the last direct `tio_raw` and alt-screen
  calls from apps. `draw_new` takes no size and queries the driver instead.

## Related source

- Cell model: `src/libvt/vt_cell.h`
- Existing backend seam: `src/libtui/tui_pad.h`, `src/libtui_term/tui_term.h`
- Diff renderer to reuse: `src/librender/render.h`
- Capability layer: `src/libtxl/txl.h`
- Byte sink: `src/libtio/tio_write.h`
- Input events: `src/libtermlib/tkbd.h`
- Width-correct field writer: `src/libtui/tui_out.h`
