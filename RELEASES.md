# lumiMUX Release Notes

## v26.06.0 -- 2026-06-02

### mserver robustness under load

A large paste could deadlock a window. The mserver wrote PTY output
to the client with a blocking `write_full()` inside its single event
loop while the attach client was itself blocked writing the paste
back, so each side filled the other's socket buffer and stopped
reading. The wedged loop never returned to `accept()`, so new attach
attempts hung in the listen backlog too. All server-to-client traffic
is now funneled through a growable byte queue drained with
non-blocking `send(MSG_DONTWAIT)`. When the backlog crosses a
high-water mark the mserver stops reading the PTY master so
backpressure lands on the application instead of the event loop, and
resumes once it drains below the low-water mark. A slow, stalled, or
dead client can no longer block the loop or starve `accept()`.

The attach handshake is now bounded. A wedged or dead-but-stale
mserver still accepts a connection into its listen backlog but never
sends `ATTACH_REPLY`, so attach blocked forever in `ipc_msg_recv()`
during discovery. A 5 second `SO_RCVTIMEO`/`SO_SNDTIMEO` now wraps the
handshake; on timeout the window is logged and skipped so the
remaining windows attach normally. Blocking I/O is restored once the
handshake succeeds.

### xterm title stack

Vim uses CSI 22t/23t to push and pop the window title on enter and
exit. Lumi previously discarded these sequences, so the pre-vim title
was never saved or restored and subsequent title sync cycles prepended
`lumi - N:` repeatedly. A title stack now implements push/pop, and the
stored title is emitted as OSC 2 during `vt_state_dump` so reconnecting
clients pick up the correct title from the server.

### macOS and BSD portability

- kqueue (`EVFILT_VNODE`) backend for `sessdir_watch` on systems
  without `sys/inotify.h`; the fd-based API is unchanged.
- Executable path lookup and `-lutil` ported to macOS.
- Build system fixes for GNU Make 3.81: inline directory creation,
  false circular `_LIBS` detection, the `-L` warning, and
  `compile_commands.json` generation.
- Drop the `ar -D` flag where it is unsupported.

### Status bar fixes

- Status bar no longer goes blank on toggle or initial attach, and no
  longer disappears after a full-screen repaint.
- `need_status` is now cleared even when the status bar is hidden, so
  `flush_render()` stops running its full body every poll cycle.
- The toggle handler flips `status_visible` only after the `ioctl`
  succeeds, avoiding inconsistent state on failure.
- Window tabs update immediately on a title change.
- CSI 21t (report title) is suppressed to prevent a recursive title
  loop.

### Other changes

- The forked process for a built-in sub-command now rewrites its argv
  to `lumi-<cmd>` (and sets the comm field via `prctl(PR_SET_NAME)` on
  Linux) so `ps(1)` distinguishes mserver sessions from the main lumi
  process.
- `SIGPIPE` is ignored in mserver and attach so a peer closing mid
  write no longer kills the process.
- Attach reaps mserver children to prevent zombies.
- Fixed an arena use-after-free in `${var:-default}` expansion, where
  nested expansions could grow the arena and invalidate the format
  string pointer.
- Added a NULL guard to `wm_resize` (matching `tile_resize`) to prevent
  a crash during session switching when the window manager is
  temporarily NULL.
- Picker tab brackets use rounded box-drawing arcs.

---

## v26.05.5 -- 2026-05-18

### Mouse event filtering

The VT emulator now tracks DECSET 1000/1002/1003 mouse modes in
`vt_state`. Click-through forwarding is gated on the child having
enabled mouse tracking, so applications that never request it (such
as GNU Screen running inside lumi) no longer receive raw SGR mouse
sequences as escape code garbage.

### Attach client buffer overread

The server sends replay data in up to 32 KB IPC messages, but
`on_mserver_read()` and `on_proxy_read()` received into 4 KB stack
buffers. `ipc_msg_recv()` truncated the payload but reported the
original wire length, so `vt_parse_feed()` read past the buffer into
stale stack memory and produced garbled output on reattach. Both
receive buffers are now sized to `IPC_MAX_PAYLOAD`, and
`ipc_msg_recv()` / `proxy_msg_recv()` cap the reported length on
truncation as defense in depth.

### Stale pointer after mconn swap-remove

When a window was removed, the swap-remove compaction invalidated
the iox callback argument pointer for the moved element.
`on_mserver_read` now looks up the mconn by fd instead of trusting
the stale pointer, preventing output from being routed to the wrong
window.

### Other changes

- CSI 21t (XTWINOPS report title) is handled in the VT parser,
  responding with the window title via OSC l and preventing the query
  from passing through to the outer terminal.
- Selection highlight no longer flashes every other frame in turbo
  mode; the compositor is forced to recomposite while a selection is
  active so the reverse-video XOR always applies to a clean buffer.
- Mouse segfault in screen mode fixed: selection bounds used
  `wm_cols()` on a NULL window manager instead of `content_cols`.
- Negative-row guard added to `sel_finish()` to match the existing
  guard in `sel_highlight()`.
- `parse_mouse_seq()` now populates `seq->data`/`len` so mouse
  events can be forwarded to child PTYs via click-through.

---

## v26.05.4 -- 2026-05-12

### TERM environment variable

Child shells now inherit `TERM=xterm-256color` instead of
`screen-256color`. This avoids missing terminfo entries on hosts
that ship xterm profiles but not screen profiles.

### Stale session state cleanup

`sessdir_cleanup_stale` now removes dead server entries from the
session state file before destroying their socket directories,
preventing phantom windows from appearing in the window list after
a server crash.

---

## v26.05.3 -- 2026-05-11

### Double/triple-click text selection

Double-click selects the word under the cursor; triple-click selects the
full line. Word and line selection modes extend correctly during drag,
and work in both turbo and tiled/screen modes.

### Grid arrange (Ctrl-A G)

New `arrange-grid` action tiles all non-minimized turbo windows into an
evenly spaced grid that fills the screen. Layout is saved automatically
after arranging.

### Smarter WM compositing

The window manager now tracks a `composite_needed` flag and skips
recomposition when nothing has changed. Frame drawing handles
wide (CJK) characters correctly in title bars, and `screen_put` sets
proper `width` fields for double-width codepoints.

### IPC message batching

The mserver read handler now drains all queued IPC messages per
event-loop wake-up (up to 4096 per batch), reducing per-message
overhead during heavy output.

### Other changes

- Scrollback mode clears any active text selection on entry and exit.
- Focusing a different turbo window while in scrollback leaves scrollback
  mode automatically.
- Escape dismisses a visible selection outside scrollback mode.
- Mouse wheel scrolling clears an active selection before entering
  scrollback.
- Removed redundant `turbo_need_full = 1` assignments throughout the
  action dispatch and mouse handlers; the compositor's dirty tracking
  now handles this.
- Status bar tab separators changed from block elements to diagonal
  stroke characters.

---

## v26.05.2 -- 2026-05-09

### Turbo/screen mode toggle (Ctrl-A t)

Switch between turbo (overlapping windows) and screen (tiled panes)
modes at runtime. Per-window layout snapshots preserve positions across
mode switches, so toggling back restores previous geometry. Session
switching cleans up both layout managers correctly.

### Bracketed paste forwarding

The outer terminal's bracketed paste boundaries (CSI 200~ / 201~) are
now intercepted by the attach client. Paste content bypasses the prefix
key state machine and is forwarded directly to the child PTY. Bracketed
paste delimiters are only sent to the child when it has enabled DECSET
2004, preventing raw escape sequences from reaching applications that
do not expect them. The attach client itself enables bracketed paste on
the outer terminal for the duration of the session.

### Mouse wheel scrollback

Mouse wheel events enter and exit scrollback mode automatically in both
turbo and tiled modes. Scrolling skips windows on the alternate screen
(full-screen applications like vim).

### Umask hardening

`umask(077)` is applied at startup so the runtime directory and Unix
sockets are owner-only. The original umask is restored before exec'ing
child shells and clipboard tools.

### VT title propagation

Window title changes from child applications (OSC 0/2) are now
propagated to mconn and wm_window structures immediately on receipt,
without waiting for the sessdir filesystem poll. The host terminal's
title bar is updated instantly.

### Scrollback input ordering

The scrollback key handler was moved after the prefix key state machine
so that prefix commands (Ctrl-A n, etc.) work while viewing scrollback
history, rather than being swallowed. Entering scrollback while already
in scrollback is now a no-op instead of resetting scroll position.

### Other changes

- `attach.mode` config key sets the default UI mode (screen, turbo, or
  minimal).
- Man page expanded with scrollback mode, mouse interaction, terminal
  compatibility, and security sections.
- Build system updated to modular-make v1.2.0 with `compile_commands.json`
  generation and automatic `RELEASE_MARCH` detection.
- `tkbd` parser recognizes CSI 200~/201~ as `TKBD_KEY_PASTE_BEGIN` /
  `TKBD_KEY_PASTE_END`.
- Status bar rendering uses `setab` background color instead of reverse
  video; buffer widened to 1024 to accommodate escape sequences.

---

## v26.05.1 -- 2026-05-09

### Kitty keyboard protocol support

The prefix key state machine now recognizes Ctrl-A encoded as
`CSI 97;5u` (kitty keyboard protocol) in addition to the traditional
C0 byte. Unmodified key codepoints from kitty/SS3 sequences are
mapped back to ASCII so all prefix bindings work regardless of the
outer terminal's keyboard mode.

### Keyboard enhancement forwarding

Application keypad mode (DECKPAM/DECKPNM), kitty progressive
enhancement flags (`CSI > flags u`), and xterm `modifyOtherKeys`
(`CSI > 4 ; Pm m`) are tracked in the VT state and forwarded to the
host terminal. All are reset on detach and on VT full reset (RIS).

### System clipboard fallback

Text selection now copies to the system clipboard via external tools
(`wl-copy`, `xclip`, `xsel`, `pbcopy`) in addition to OSC 52. A
double-forked child process pipes the selection to the first available
tool. New bindings: `Ctrl-A ]` pastes, `Ctrl-A y` re-syncs the
internal buffer to the system clipboard.

### Selection bounds clamping

Mouse drag selection in turbo mode is clamped to the focused window's
content area, preventing selection from extending into frame borders or
adjacent windows. Multi-line selections wrap at window boundaries
instead of screen edges.

### SS3 key table expansion

The `tkbd` parser now handles all SS3 sequences (A-S), covering arrow
keys, Home, End, and keypad Enter in application mode, in addition to
the previously supported F1-F4. Modifier parameters on SS3 sequences
are parsed.

### Other changes

- CSI mouse coordinate parsing uses `int` instead of `uint8_t`,
  fixing coordinate truncation on terminals wider than 223 columns.
- Three new input dispatch tests cover kitty-encoded prefix key
  combinations.

---

## v26.05.0 -- 2026-05-06

### Rendering improvements

- Cursor positioning and visibility are now set after all rendering
  (content, status, overlays) is complete, fixing cases where the
  cursor appeared at stale positions or blinked during overlay
  transitions.
- The renderer tracks `has_bce` (background color erase) capability
  for correct erase-to-end-of-line behavior on terminals that support
  it.
- `turbo_repaint` and `tiled_repaint` flush the cursor position and
  visibility explicitly after recomposite, fixing glitches after
  overlay dismissal.

### Host terminal title sync

The outer terminal's title bar now shows `lumi - session:window` via
OSC 2, updating whenever the focused window changes or receives a
title update. The title is reset to the terminal's default on detach.

### Status bar redesign

The status bar window list uses a tabbed visual style with colored
tab separators, highlighted active window markers, and distinct
active/inactive color schemes. Buffer size increased to accommodate
the longer formatted output.

### Window selection by index

Number keys (0-9) after the prefix now select windows by mconn array
index rather than by a bare numeric ID, matching the displayed window
list order. Out-of-range indexes are silently ignored.

### Daemonize hardening

The daemonize routine now double-forks (preventing accidental
acquisition of a controlling terminal), replaces `freopen` with
explicit `open`/`dup2`/`close` for stdio redirection, and drops the
intermediate `sid` variable.

### Other changes

- Window removal in tiled mode properly detaches the dead pane from
  the tile tree and suppresses rendering after the last window
  closes, fixing a crash path.
- `analyze-capture.py` script added for offline analysis of terminal
  capture files.

---

## v26.04.0 -- 2026-05-06

Versioning scheme change: moved from semver (`v0.x.y`) to CalVer
(`vYY.MM.patch`). The `bump-version.sh` script was updated to produce
the new format.

No functional changes from v0.1.0.

---

## v0.1.0 -- 2026-04-23

Initial public release of lumiMUX, a terminal multiplexer with a
git-style sub-command architecture and GNU Screen-compatible default
keybindings.

### Architecture

- Busybox-style multi-call binary: `lumi` dispatches to built-in
  sub-commands (`attach`, `new`, `detach`, `list`, `version`, `proxy`,
  `mserver`, `splash`) with external `lumi-*` fallback.
- Micro-server model: each window runs as an independent `lumi-mserver`
  process with its own PTY. The attach client connects to all servers
  in a session via Unix domain sockets.
- Session directory library (`libsessdir`) manages per-session runtime
  state under `$XDG_RUNTIME_DIR/lumi/`.

### Terminal emulation

- VT emulator (`libvt`) with primary/alternate screen buffers, scroll
  regions, DECSET/DECRST mode tracking, SGR attributes (256-color and
  RGB), OSC title parsing, and DCS/SIXEL pass-through.
- Terminal translation engine (`libtxl`) for terminfo-driven output.
- `tkbd` key input parser with SGR mouse support, vendored from
  aux01/termlib.
- Data-driven control code handling rather than hard-coded sequences.

### Display modes

- **Turbo mode**: overlapping windows with a compositing window manager
  (`libwm`). Title bars with close/minimize/maximize buttons, themable
  frame colors, drag-move and drag-resize, shadow effects, z-ordering,
  and per-window color customization via an interactive color picker.
- **Screen mode**: binary split-pane tiling compositor (`libtile`) with
  horizontal and vertical splits, pane focus cycling, and resize
  controls.
- **Minimal mode**: single full-screen window.

### Key features

- Config-driven keybindings with state-dependent binding layers
  (title regex matching, toggle predicates). Gitconfig-style INI
  parser (`libcfg`).
- Nine built-in themes (ASCII and Unicode variants) with config-file
  customization.
- DESQview-style guided prefix-key menu and window picker.
- Applications menu with built-in calculator and emoji picker.
- Mouse text selection with OSC 52 clipboard copy and right-click
  paste. Selection renders as reverse video.
- Scrollback viewer with render-target architecture, keyboard/mouse
  navigation, and scrollbar in turbo mode.
- Speculative local echo with dim rendering for predicted cells;
  suppressed when PTY ECHO is disabled (password entry).
- Per-window scroll lock (flow control via PTY backpressure) and
  input lock.
- Keep-open mode: windows remain visible after child process exit.
- Synchronized output (mode 2026) to prevent partial frame display.
- Double-buffered dirty tracking in both compositors.
- Runtime debug tracing via `LUMI_DEBUG=path`.
- Window layout persistence across attach/detach cycles.
- Multi-session switching with session picker UI.

### Remote sessions

- SSH-tunneled remote session support via `lumi proxy` with scp-style
  syntax (`user@host:session`). Multiplexing proxy aggregates mserver
  connections onto a single SSH pipe.

### Documentation

- `lumi(1)` man page covering all sub-commands, key bindings,
  configuration, and environment variables.
- Developer guide with architecture and protocol documentation.

### Build system

- Modular GNU Make framework with per-target flags, transitive
  exported include paths, and automatic dependency tracking.
- GitHub Actions CI for pull requests and main branch.
- Static musl build support.
- `compile_commands.json` generation for clangd.
- Smoke test suite gated in CI.
- Unit tests for `libutf8` and `libwm`.
