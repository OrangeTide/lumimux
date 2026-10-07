# lumiMUX Release Notes

## v26.10.1 -- 2026-10-06

### Terminal color queries answered in place

Programs that ask the terminal for its foreground and background (OSC 10
and 11), such as `gh` and prompt frameworks, used to see the reply arrive
late and land on the shell's command line as `^[]11;rgb:...`. The attach
client now asks the outer terminal itself at startup, on a session switch,
and after roaming, reports the answers to every micro-server, and the
micro-server's VT answers the program directly. The client also strips any
terminal reply from its own input before the key parser can turn it into
keystrokes.

### Kitty graphics, finished

The kitty graphics pass-through now covers the whole plan. The host's
reply to a command is routed back to the window that sent it, and image
numbers are resolved to the ids the host assigned. Images show in turbo
mode too, placed at the focused window's origin and cropped to its edges.
An image's chunks, animation frames, and control commands are kept
together in a byte-capped per-window store, so animations survive a
window switch and the program's deletes are honored. A PNG sent without a
size is measured from its header, and when the tty reports no cell pixel
size the client asks the terminal (CSI 16 t / 14 t). A redraw re-places an
image the host already holds instead of re-sending its data, falling back
to a full re-send when the host reports it gone; `attach.graphics-replay =
full` turns that off. A window switch replays images once rather than
twice.

### Sharing

A client that does not hold the keyboard shows the session title with a
`[viewing]` suffix. The presence indicator counts a client waiting for
approval under an `ask` rule separately, as `+N?`, through the new `%p`
and `%P` format tokens. OSC 52 clipboard writes from a program reach only
the client that holds the keyboard.

### Other changes

- A program's bell now rings the outer terminal when its window is on
  screen, and shows a "bell in window N" notice when it is hidden.
- Saved layouts are keyed by stable window number (format version 2), so
  a layout survives windows closing or being renumbered between save and
  restore. Older layout files are ignored.
- `lumi edit` vi search accepts offsets (`/pat/e`, `/pat/s-1`, and the
  line forms). All further editor work is deferred.
- The man page and the other documents were brought back in line with the
  code, and the missing release notes between v26.06.1 and v26.09.1 were
  filled in.

---

## v26.10.0 -- 2026-10-05

This release turns `lumi edit` from a splash-era placeholder into a usable
editor, and adds two libraries that stand behind it: a data-driven syntax
highlighter and a drawing-surface abstraction that the full-screen tools now
render through. The VT engine also picked up a round of hardening.

### lumi edit grows up

`lumi edit` now frames the file in DOS EDIT-style chrome: a menu bar with
working drop-down menus, draggable scrollbars, a status area, and modal
dialogs. The menus and the cursor are both mouse-driven, menu and dialog
items carry underlined mnemonic keys, and a Yes/No/Cancel dialog guards
unsaved changes on quit. An About dialog and a help screen replace the old
status-line hints, and the help text and menus adapt to the active editing
mode.

### A vi personality

Edit now has a toggleable vi keybinding mode that covers far more than
cursor movement. Visual mode selects charwise and linewise, text objects
(`iw`/`aw`, brackets, quotes) compose with operators, and the motion set
includes `f`/`F`/`t`/`T` with `;`/`,`, paragraph and sentence motions, `%`
match-pair, `H`/`M`/`L`, `|`, go-to-column, and forward and backward search
with `n`, `N`, `*`, and `#`. Operators cover `d`, `c`, `>`, `<`, `D`, `C`,
`s`, `S`, `~`, `J`, `r`, and Replace mode. Marks, named registers `"a`-`"z`,
and `.` repeat of the last change are all present, along with `ZZ`, `ZQ`,
and `:qa`/`:wqa`/`:cq`.

### ex command line

The ex command line understands line ranges and `:d`, `:y`, `:>`, `:<`, a
`:s` substitute over a range, and the `:g`/`:v` global commands.

### Multiple buffers

Edit holds more than one file at a time. `:e`, `:enew`, `:ls`, `:bn`/`:bp`,
`:b N`, and `:bd` manage the buffer list, a buffer-navigation menu and
hotkeys drive it, and `:r` reads a file in below the cursor.

### Hex view and editor

A hex dump view sits alongside the text view, sharing the same request
framework rather than running a second input path. It started read-only and
grew overwrite editing, byte insert and delete, a configurable dump width,
byte and text search, a data inspector, and byte copy and paste.

### Syntax highlighting

A new `libsyntax` library provides a data-driven, joe/JSF-style highlighting
state machine. It ships tables for C, Shell, Rust, Go, Lua, Python, JavaScript,
HTML, BASIC, Forth, NASM, GAS, and Pascal, and edit highlights a file by type.
Highlight colors are configurable and derive from the active TUI theme.

### Build integration

Edit can run SciTE-style compile, make, and run commands, stream their
output into the viewer as it arrives, and jump to diagnostics across files.
Build-command paths are made absolute so a build launched from one pane runs
correctly. On the server side, `send-input` can now inject into a pane
without first taking the keyboard.

### A drawing surface

A new `libdraw` library introduces a drawing-surface abstraction with a
terminal backend and an input event queue. `edit`, `files`, and `splash`
render and read input through it instead of each owning terminal setup.
libdraw owns the terminal signals, adds a `draw_wait` entry point, and
handles `SIGTSTP` suspend and resume, which let the tools drop their own
signal code.

### VT engine hardening

The VT engine picked up three robustness fixes. Reply writes (DSR/DA
responses) now drain the whole buffer and retry on `EINTR` instead of a
single unchecked `write`. CSI parameter accumulation is clamped so a long
digit run cannot overflow a signed int. And the cursor can no longer be left
past the right edge by a glyph wider than the whole terminal. A new
randomized torture test drives the parser and terminal state with a seeded
stream of random and crafted input, checking structural invariants after
every step, and is meant to run under the sanitizers and the coverage build.

### Other changes

- `basic` runs a BASIC program file given as an argument.
- The renderer parks the cursor at the requested position in its flat-cell
  path.
- `libtext` groups multiple edit primitives into a single undo step.

---

## v26.09.1 -- 2026-09-19

This release brings the first bundled utilities, the first cut of kitty
graphics, and an install target.

### Bundled utilities

`lumi files` browses the filesystem with a preview pane, opens a file in
`$PAGER` or `$EDITOR`, and inside a session opens it in a new window; it
can toggle hidden files and make, rename, or delete entries behind a
confirm. `lumi edit` is a modeless text editor on the new `libtext`
buffer with undo and redo, search, goto-line, selection with copy, cut,
and paste mirrored to the system clipboard, and an F1 help screen.
`lumi basic` is a BASIC-style calculator REPL on the new `libbasic`
engine, with programs by file line and label, `SAVE`, `LOAD`, `EDIT`
(in `lumi edit`), `DEF FN`, vector and matrix values, and ASCII graphing.
All three accept bracketed paste.

### Sending input to windows

`lumi send-input` and `lumi send-keys` send raw bytes or named keys to a
window, chosen by focus or by index; edit and basic use the same path to
send a selection or a program to a pane. `lumi attr` attaches before
sending its requests. `new-window` runs a command with arguments.

### Kitty graphics, first cut

Kitty graphics commands are anchored to their cell, the cursor is
advanced past the image on both the client and the server, and the
images are replayed across window switches. The design and its limits
are recorded in `doc/kitty-graphics.md`.

### Other changes

- A window's OSC 52 clipboard write is forwarded to the outer terminal,
  and the VT's OSC buffer grew so large clipboards get through.
- An `[environment]` config section sets environment defaults for new
  windows.
- Mouse reports split at an ESC boundary (macOS Terminal.app) no longer
  corrupt a selection; `tkbd_drain()` decodes fragmented input safely.
- `make install` and `make uninstall` were added, defaulting to
  `~/.local`, and the man page is generated from `doc/lumi.1.in` at
  build and install time. Release artifacts are staged with the install
  target.
- Fixed the first-window startup hang on macOS.

---

## v26.09.0 -- 2026-09-15

- Optional GPM mouse support on the Linux console (`make GPM=0` turns it
  off).
- Cells are erased with the current background color (BCE), fixing
  programs that rely on it.
- Keyboard-enhancement state is replayed on attach, so a kitty-protocol
  program keeps its flags after a reattach.
- Session state is locked through a stable file rather than the renamed
  inode, fixing a window number map race.
- The guided prefix menu scrolls instead of clipping on short terminals.
- An index outside the scroll margins no longer scrolls the region.
- The README gained a See Also section listing related multiplexers.

---

## v26.08.3 -- 2026-08-27

- Kitty keyboard flags are mirrored with a set rather than stack push and
  pop, fixing a leak that left enhancement flags on after a program
  exited. Regression test added.
- `vt_state_dump` no longer bleeds a cell's style down the screen.
- TBC 3 clears every tab stop instead of restoring the defaults.

---

## v26.08.2 -- 2026-08-03

### Attaching takes the session over again

Session sharing arrived in v26.08.0 and took the default with it. A
second `lumi attach` joined the session as a read-only viewer, and the
only way to displace the client already there was to look up its id and
run `lumi share -k`. Attaching to a session almost always means wanting
to work in it, not wanting to watch someone else work in it, so the
default is now the other way around.

A plain `lumi attach` asks the clients already attached to detach and
takes the keyboard. `-x` asks for the old behavior and joins them
instead, and `-v` still implies it, since a client that came to watch
should not displace the client it came to watch. A session put into
multi-writer mode with `lumi share -M` is never taken over either:
displacing anyone there would defeat the setting.

This is a change in behavior for anyone who was relying on the v26.08.x
default. Add `-x` to get it back.

### lumi detach did nothing

`lumi detach` connected to each mserver and sent `IPC_MSG_DETACH`, but
that message disconnects the connection that sent it. The command was
dropping its own throwaway connection and leaving the attached client
exactly where it was. It now detaches every client of the session, or a
single one with `-c id`, which is what makes a session left shared
recoverable from another terminal without attaching to it.

The session and the programs running in it are untouched either way.
Only the clients watching go.

### Reaching a client older than the mechanism

Detaching is cooperative: the session directory carries the request and
each client acts on it when it next reads that directory. A kick
addressed to every client at once is new in this release, so a client
that has been attached since before the upgrade does not understand it,
and a long-lived session is exactly where such a process lives.
`sessdir_kick_others()` therefore follows the broadcast with one kick
addressed to each client by id, a form every client back to the first
sharing release understands, and waits for each process to go before
posting the next. The mailbox holds one message at a time, so posting
the next kick early would take it away from the client it was addressed
to.

A message keeps no record of who was present when it was posted, and
`control.msg` holds the last one indefinitely, so a client now ignores
whatever a session was saying before it joined. It always did that at
startup; it now does it when switching sessions too, where carrying the
sequence number from the session it left meant comparing two unrelated
counters and could act on a kick posted before it arrived.

### Switching sessions

The session picker joins the session it moves to rather than taking it
over. Attaching and switching are different acts: one arrives at a
session to work in it, the other looks in on one, and a key that moves
this client is not a request to end somebody else's. The keyboard comes
only if nobody holds it, and is asked for through the share menu if
somebody does.

Making the switch join uncovered two bugs that taking over had hidden,
by removing the other client before either could matter. The client kept
claiming the write token it had just released, so the servers in the
session it joined took the keyboard off whoever held it. Announcing that
demotion then reached the joining client ahead of its own
`ATTACH_REPLY`, which it reads as the first message after `ATTACH`, so
it declared the window unresponsive and ended up attached to nothing at
all, having spawned a stray window on the way. A connection is now told
nothing until its reply is queued, and a client reads past anything that
arrives ahead of the reply, which is what lets it join a session whose
servers predate the fix.

A client that switches now also appears in the roster of the session it
joined, which it did not before: it had left the old session's roster
and joined no new one.

### Release build

The v26.08.0 release build failed a test and published no artifacts. The
mserver test that proves a stalled client is dropped lowered the drop cap
to 64 KiB but left the watermarks that pause and resume PTY reads at
their production 1 MiB and 256 KiB, which inverted the order the two
mechanisms have in a real session. The throttle could never engage before
the cap, so the client that was keeping up survived only while the test
was scheduled often enough to drain it, and on a small shared runner it
was not. The watermarks are now variables the test brings down together
with the cap.

Also in this release: the vendored build system moves to modular-make
v1.8.8, which adds the `-L` for `LIBDIR` only when the project builds
shared libraries. lumi builds none, so the flag leaves the link line
entirely, and with it a warning from Apple's linker about a search path
that does not exist.

---

## v26.08.1 -- 2026-08-02

- Fixed an inotify feedback loop that pegged any attached client's CPU at
  100% as soon as a second client attached.

---

## v26.08.0 -- 2026-08-02

This release delivers shared attach (Phase 12): several clients on one
session, one keyboard, access control for other users, and a networked
broker.

### Several clients, one keyboard

Several clients can attach to one window. Identity and role travel in the
attach handshake, the keyboard is decided per session, and `lumi share`
lists the clients and passes the keyboard between them. `lumi attach -v`
attaches read-only; a watching client follows the keyboard holder's view;
the window is sized for the clients that can type. The taskbar shows who
else is attached and says when that changes. `share.mode` lets a
multi-writer session grant the keyboard to everyone, with speculative
echo gated off while more than one writer is attached, and `share.display`
with a layout generation counter keeps shared displays in step.

### Access control and the broker

A session-wide ACL (`session/access`) decides what another local user may
do, with `ask` rules that admit a client pending approval. `lumi proxy -L`
is the local cross-user broker, and `lumi net-proxy -L` forks a child per
client and applies the same ACL to networked clients. Broker-relayed
clients appear in the session roster, the ACL is re-evaluated on clients
already connected, and the share indicator is colored for foreign uids
with coupling and pending columns in `lumi share -l`. The runtime
directory is refused when it is not ours, and Unix socket peer credentials
are reported.

### Other changes

- OSC 10/11 color queries are forwarded so the outer terminal can answer
  (superseded in a later release by mserver-side answers).
- Input runs are bracketed so a paste cannot be torn in half.
- Window titles survive a reattach, a restored split whose window is gone
  collapses, and a screen layout never restores a pane with no window.
- Several teardown, error-path, and multi-writer bugs found in review were
  fixed. Vendored libiox 0.1.0.

---

## v26.07.1 -- 2026-07-24

- `lumi net-keygen` makes a netchan client identity key and `lumi
  net-passwd` enrolls a direct-connect password; the netchan transport
  gained an ssh-shaped userauth phase and server-identity plumbing.
- Direct-connect net-proxy deployment without ssh, covered by tests that
  found and fixed two listener bugs.
- Opt-in ssh-style host-key verification for networked attach
  (`attach -V`).
- `lumi new -d` no longer blocks captured output: the mserver detaches its
  standard descriptors.
- Vendored netchan re-synced to upstream 0.6.0; entropy comes from
  `getentropy()` so macOS builds; release build paths fixed for the
  variant directory.

---

## v26.07.0 -- 2026-07-16

This release lands networked connections (11C) on netchan-v2 instead of
the originally planned QUIC.

### Networked attach

An `ipc_transport` seam, the extracted `libnet` transport core, and a
stress-tested reliable channel underpin `lumi net-proxy`, the netchan
bridge. `lumi attach -n` reaches a proxy on this host, or on another host
over ssh (`-n host:session`), with the link encrypted under a per-session
pre-shared key. A moved client roams automatically on a network change.

### Other changes

- Alt-screen content can be captured into scrollback on exit.
- A `Ctrl-A :` command line takes directives, including `:title` as a
  client-side override and `:number` to renumber the window; the doubled
  text bug in it was fixed.
- Most-recently-used window order; redisplay action; panes resync at
  attach size; the taskbar highlights the live focus, elides overflowing
  titles, and switches focus on a tab click. The status line was renamed
  the taskbar throughout.
- Desktop-notification OSCs pass through to the outer terminal.
- The prefix menu no longer strobes over a live backdrop, and send-prefix
  from it works. xterm modifyOtherKeys (CSI 27) is decoded for the prefix
  key. Input parsing resyncs on incomplete CSI sequences.
- Vi-style keyboard copy mode in scrollback; stale selections clear on
  paste, scroll, and focus change; stuck bracketed-paste state is
  cancelled on focus change.
- The mserver re-registers its socket on SIGHUP; a use-after-free when a
  window closed in a tiled layout was fixed.
- Terminal configuration for common terminals is documented.

---

## v26.06.2 -- 2026-06-26

- Mouse selection no longer bounces to the start of the screen.

---

## v26.06.1 -- 2026-06-24

- Windows get stable numbers that survive other windows closing.
- The kitty keyboard protocol is modelled as a flag stack and
  capability queries are answered.
- Mouse wheel events are forwarded to alt-screen programs that want
  mouse tracking.
- New-window creation works without the session directory watch; windows
  are resized to the screen-mode area on entry; orphan tile panes are
  dropped when a window closes in turbo mode; stale tab bar titles on
  window switch were fixed.
- The Phase 11 plan moved to `doc/FUTURE.md`. Fixed the mserver build on
  macOS and BSD.

---

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
