# Developer Guide

Information for developers working on lumimux.

## Project Layout

| Directory            | Description                                               |
|----------------------|-----------------------------------------------------------|
| src/                 | Main source and module.mk entry point                     |
| src/lumi.c           | Sub-command dispatcher (`lumi <cmd>` -> `lumi-<cmd>`)     |
| src/libcfg/          | Gitconfig-style config file parser with key-value lookup  |
| src/libattr/         | Transactional key-value attribute store with IPC and CLI  |
| src/libcore/         | Logging, safe allocation (xmalloc), string helpers, PATH search |
| src/libiox/          | Poll-based I/O multiplexer with fd watchers, signals, idle callbacks |
| src/libipc/          | Unix domain socket IPC with TLV message framing           |
| src/libkeys/         | Key binding table and prefix-key state machine            |
| src/libpty/          | Pseudo-terminal allocation, shell spawning, resize        |
| src/librender/       | Differential screen renderer (shadow buffer diffing)      |
| src/libsessdir/      | Filesystem session directory for micro-server discovery (inotify) |
| src/libsession/      | Window lifecycle management (PTY + VT state per window)   |
| src/libsplash/       | ANSI art splash screen scenes with viewport cropping      |
| src/libstatus/       | Status line with shell-like template expansion            |
| src/libtermlib/      | Vendored terminfo parser (aux01/termlib, MIT)             |
| src/libtile/         | Binary split-pane compositor for screen mode splits       |
| src/libtio/          | Terminal raw mode, 8KB buffered writes, restore on exit   |
| src/libtxl/          | Terminal translation engine (operations -> escape sequences) |
| src/libutf8/         | UTF-8 encode/decode, Unicode-version-aware rune_width()   |
| src/libvt/           | VT500 terminal emulator: parser, ops, cell grid, scrollback |
| src/libwm/           | Overlapping window manager compositor (z-order, hit test) |
| src/cmd/attach/      | lumi-attach -- connect to server, relay I/O, menu overlay |
| src/cmd/attr/        | lumi-attr -- get/set/delete per-session attributes        |
| src/cmd/detach/      | lumi-detach -- tell server to detach a client             |
| src/cmd/kill/        | lumi-kill -- terminate a session                          |
| src/cmd/list/        | lumi-list -- list active sessions                         |
| src/cmd/mserver/     | lumi-mserver -- single-PTY micro-server (one per window)  |
| src/cmd/new/         | lumi-new -- create session and attach                     |
| src/cmd/new-window/  | lumi-new-window -- create window in existing session      |
| src/cmd/proxy/       | lumi-proxy -- multiplexing proxy for remote session tunneling |
| src/cmd/reload/      | lumi-reload -- tell server to reload config               |
| src/cmd/send-input/  | lumi-send-input -- inject raw input into a pane           |
| src/cmd/send-keys/   | lumi-send-keys -- send keystrokes to a session            |
| src/cmd/splash/      | lumi-splash -- display ANSI art splash screens            |
| src/cmd/version/     | lumi-version -- print version info                        |

## Architecture

lumimux uses a micro-server architecture. Each window runs as an independent
`lumi-mserver` process owning a single PTY and VT emulation state. The
client (`lumi-attach`) discovers servers via the session directory
(`libsessdir`), connects to each one over its own Unix domain socket, and
routes input/output by file descriptor. There is no centralized server
process.

```
                            sessdir: /run/user/<uid>/lumi/<session>/
                            +------------------------------------+
                            | <pid1>/socket  <pid1>/title        |
                            | <pid2>/socket  <pid2>/title        |
                            | <pid3>/socket  <pid3>/title        |
                            +------------------------------------+
                                  |          |          |
lumi-attach (client)         mserver 1   mserver 2   mserver 3
 +-----------------------+   +--------+  +--------+  +--------+
 | stdin -> tkbd_parse   |   | PTY+VT |  | PTY+VT |  | PTY+VT |
 |   prefix keys/menu    |   +--------+  +--------+  +--------+
 |   overlay (tui_stack) |     ^    |      ^    |      ^    |
 |                       |     |    v      |    v      |    v
 | mconn table:          |   INPUT OUTPUT INPUT OUTPUT INPUT OUTPUT
 |   mconns[0]: fd,pid   |---/    \-------/    \-------/    \----+
 |   mconns[1]: fd,pid   |                                       |
 |   mconns[2]: fd,pid   |             Unix domain sockets       |
 |                       |---------------------------------------+
 | per-window VT state:  |
 |   cwins[0]: vt+parser |
 |   cwins[1]: vt+parser |
 |   cwins[2]: vt+parser |
 |                       |
 | renderer (shadow diff) |
 | status line           |
 +-----------------------+
```

### Micro-Server (lumi-mserver)

Each `lumi-mserver` owns a single window: one PTY, one VT emulation state,
one listen socket. It runs an `iox_loop` event loop with fd watchers for:

- **Listen socket** -- accepts a client connection
- **Client fd** -- receives IPC messages (input, resize, kill)
- **PTY master fd** -- reads child output

On PTY read, the server feeds output through `window_feed()` (VT parser) to
maintain a server-side screen image, then forwards raw bytes as OUTPUT to the
connected client. On attach, it sends an ATTACH_REPLY with the current VT
dimensions, followed by a full screen replay via OUTPUT messages.

Only one client is connected at a time. A new attach disconnects the previous
client. SIGCHLD reaps the dead child process and shuts down the server.

The server registers itself in the session directory (`sessdir`) at startup:
it creates `<session>/<pid>/socket` (the Unix domain socket path) and
`<session>/<pid>/title` (the window title). The title file is updated when
OSC title-change sequences are detected. On exit, the server removes its
sessdir entry.

### Session Directory (libsessdir)

The session directory provides filesystem-based discovery of micro-servers.
The base path is `$XDG_RUNTIME_DIR/lumi/` (fallback `/tmp/lumi-<uid>/`).
Each session is a subdirectory containing per-server PID directories:

```
/run/user/1000/lumi/
  work/
    12345/
      socket          <- Unix domain socket
      title           <- window title text
    12350/
      socket
      title
  dev/
    12400/
      socket
      title
```

`sessdir_list_sessions()` enumerates sessions. `sessdir_list_servers()`
enumerates servers within a session. `sessdir_cleanup_stale()` removes
entries for dead processes. An inotify watch (`sessdir_watch`) notifies
the client when servers appear or disappear.

### Client (lumi-attach)

The client supports three UI modes selected by `-m`:

- **screen** (default) -- GNU Screen-like single-window view. Watches one
  window at a time, switches via prefix key or picker.
- **turbo** -- Turbo Vision / DESQview style overlapping windows. Watches
  all windows, composites them via libwm, supports mouse-driven move,
  resize, close, and focus.
- **minimal** -- bare passthrough with no status bar, no mouse tracking,
  no popup menus. Prefix key bindings still work for detach and window
  switching. Content uses the full terminal height.

The mode can be set on the command line (`lumi attach -m turbo`,
`lumi new -m minimal`) or in `lumi.conf` (see Configuration below).
Command-line flags override the config file.

The client puts the terminal in raw mode, enables SGR mouse tracking
(mode 1002 + 1006 in turbo, 1000 + 1006 in screen, none in minimal),
and runs its own `iox_loop` with:

- **stdin** -- input parsed by `tkbd_parse()` into structured key/mouse
  events, routed through overlay UI layers then the prefix-key state machine
- **Server fds** -- IPC messages from each mserver (OUTPUT, DETACH)

#### Connection Discovery

On startup, the client calls `mconn_discover()` which:

1. Runs `sessdir_cleanup_stale()` to remove entries for dead processes
2. Calls `sessdir_list_servers()` to enumerate live mserver PIDs
3. For each new PID, builds the socket path (`<server_path>/socket`),
   connects via `ipc_connect()`, sends ATTACH, receives ATTACH_REPLY
   with the server's current VT dimensions
4. Creates an `mconn` entry and a matching `client_window` (VT + parser)
   sized to the server's reported dimensions
5. Removes `mconn` and `client_window` entries for PIDs no longer present

An inotify watch (`sessdir_watch`) triggers rediscovery when mservers
appear or disappear, so the client adapts dynamically as windows are
created or closed.

#### Per-Window VT State

The client maintains a `client_window` array (`cwins[]`) with a `vt_state`
and `vt_parse` per mserver connection. Each mserver sends OUTPUT messages
containing raw PTY data; the client routes these to the matching window's
parser by looking up the `mconn` that received the message.

In **screen mode**, a `watched_id` tracks the focused window's PID. Only
OUTPUT from that mserver triggers rendering. The global `vt` pointer is
set to the focused window's `vt_state`.

In **turbo mode**, OUTPUT from any mserver triggers a recomposite:
`wm_composite()` paints all windows into a flat cell buffer, then
`render_cells_diff()` diffs it against the shadow buffer. Each
`wm_window` holds a reference to the corresponding `client_window`'s
`vt_state`. The cursor follows the focused window's VT cursor, offset by
the window's screen position. New windows are positioned using a cascading
layout (staggered offsets).

The overlay system (menus, picker) references `vt->buf` for erase-and-
restore operations. In turbo mode, `vt` points at the focused window's
VT to keep overlays functional.

`mconn_sync_winlist()` rebuilds the picker's window list from the current
`mconn` table, reading titles from sessdir. It is called after discovery,
window selection, mserver disconnect, and sessdir watch events.

### Overlay System

Menu popups (prefix-key menu, window picker, apps menu) use a `tui_stack`
overlay system. Each overlay layer is a `tui_pad` with its own cell buffer
drawn by menu/picker/app code. `tui_stack_render()` paints visible layers
to the terminal. `tui_stack_erase()` restores the overlay region from the
VT buffer underneath. Overlays are managed as a stack -- push adds a layer,
pop removes and restores.

### Window Manager Compositor (libwm)

The `libwm` library implements a Turbo Vision / DESQview style overlapping
window compositor. It maintains a screen buffer and composites visible
windows using a painter's algorithm (back-to-front by z-order):

1. Clear screen buffer to background
2. For each non-minimized window (lowest z to highest z):
   a. Draw shadow (half-block or shade style per theme)
   b. Draw frame border with themed glyphs, title bar, close button
   c. Copy content cells from the window's `vt_buf`

`wm_hit_test()` classifies screen coordinates into content, title bar,
border, close button, or background. Mouse interaction follows a drag
state machine (`WM_DRAG_IDLE` / `WM_DRAG_MOVING` / `WM_DRAG_RESIZING`):

- `wm_mouse_press` -- hit-tests, focuses the clicked window, starts
  a move (title bar) or resize (border) drag using anchor-point tracking
- `wm_mouse_drag` -- applies delta from anchor to move/resize the window
- `wm_mouse_release` -- ends the drag, reports whether a resize occurred
  (caller must resize the PTY via IPC)

Close button clicks are reported to the caller, which sends
`IPC_MSG_KILL` to the corresponding mserver. Resize is clamped to
`WM_MIN_WIDTH` x `WM_MIN_HEIGHT` (4x2).

### Session Model

A session is a named directory in the sessdir base path. Each window is
an independent `lumi-mserver` process registered under the session
directory with its PID as the subdirectory name. The window ID is the
mserver's PID (`uint32_t`).

Within each mserver, `libsession` manages the single window's PTY and VT
state. The session name is passed via `-s` and set as `LUMI_SESSION` in
the child shell's environment. `lumi-attach` checks this variable at
startup to prevent recursive attach (which would deadlock).

## IPC Protocol

Communication uses TLV (type-length-value) messages over Unix domain sockets.
Structured payloads are defined in `src/libipc/lumi.idl` and generated with
[Microser][2] (`make gen-ipc-msg`).

### Wire Format

```
+--------+--------+-----------+
| type   | len    | payload   |
| 4B BE  | 4B BE  | len bytes |
+--------+--------+-----------+
```

All multi-byte integers are big-endian (network byte order). Maximum payload
is 64 KB (`IPC_MAX_PAYLOAD`).

### Message Types

Types are organized by category (high byte):

**0x00xx -- Session / Connection Control**

| Type         | Code     | Direction | Payload                          |
|--------------|----------|-----------|----------------------------------|
| ATTACH       | `0x0001` | C -> S    | (empty or microser `IpcSize`)    |
| ATTACH_REPLY | `0x0006` | S -> C    | microser `IpcSize` (rows, cols)  |
| DETACH       | `0x0002` | either    | (empty)                          |
| KILL         | `0x0003` | C -> S    | (empty)                          |
| OK           | `0x0004` | S -> C    | (empty)                          |
| ERROR        | `0x0005` | S -> C    | error message bytes              |

**0x01xx -- Data Transfer**

| Type   | Code     | Direction | Payload            |
|--------|----------|-----------|--------------------|
| INPUT  | `0x0100` | C -> S    | raw keyboard bytes |
| OUTPUT | `0x0101` | S -> C    | raw PTY output     |

**0x02xx -- Window / PTY Management**

| Type       | Code     | Direction | Payload                  |
|------------|----------|-----------|--------------------------|
| WIN_RESIZE | `0x0207` | C -> S    | microser `IpcWinResize`  |

### Microser Encoding

Structured payloads use [microser][2], a compact tag-length-value encoding.
Each message starts with a 2-byte little-endian length prefix (not counting
itself), followed by tagged fields. Each field has a 1-byte tag (field
number in bits 7-3, wire type in bits 2-0). Unknown fields are skipped via
wire type, providing forward compatibility.

**`IpcSize`** (ATTACH_REPLY):

| Field | Tag | Type   | Description          |
|-------|-----|--------|----------------------|
| 1     | u16 | rows   | Terminal row count   |
| 2     | u16 | cols   | Terminal column count|

**`IpcWinResize`** (WIN_RESIZE):

| Field | Tag | Type   | Description          |
|-------|-----|--------|----------------------|
| 1     | u32 | id     | Window ID (mserver PID) |
| 2     | u16 | rows   | New row count        |
| 3     | u16 | cols   | New column count     |

### IDL and Code Generation

Structured message definitions live in `src/libipc/lumi.idl`. The IDL file
is the source of truth for all microser-encoded payloads (`IpcSize`,
`IpcWinResize`). Raw byte payloads (INPUT, OUTPUT) and empty messages
(ATTACH, DETACH, KILL, OK) are not defined in the IDL.

To regenerate `lumi_msg.h` and `lumi_msg.c` from the IDL:

```sh
make gen-ipc-msg
```

This runs `gen.sh lumi.idl lumi_msg` inside `src/libipc/`. After
regenerating, review the diff and rebuild:

```sh
git diff src/libipc/lumi_msg.*
make
make run-tests
```

When adding a new structured message:

1. Add the `message` block to `lumi.idl`
2. Run `make gen-ipc-msg`
3. Add the corresponding `IPC_MSG_*` constant to `ipc_msg.h`
4. If needed, add a convenience send function to `ipc_msg.c` / `ipc_msg.h`

### Connection Lifecycle

Each `lumi-attach` <-> `lumi-mserver` connection is independent:

```
Client                          Micro-Server
  |                               |
  |--- ATTACH ------------------->|  client connects to mserver socket
  |<----- ATTACH_REPLY (r,c) ----|  server reports its VT dimensions
  |<----- OUTPUT (replay) -------|  server dumps current VT state
  |<----- OUTPUT (ongoing) ------|  server forwards PTY reads
  |                               |
  |--- INPUT -------------------->|  keyboard bytes to PTY
  |--- WIN_RESIZE (id,r,c) ------>|  resize PTY
  |                               |
  |--- DETACH ------------------->|  client disconnects
  |                               |
  |--- KILL --------------------->|  terminate mserver
  |<----- OK --------------------|  confirmed (mserver exits)
```

The client maintains N such connections simultaneously (one per mserver
in the session). Window switching in screen mode changes which connection
receives INPUT; in turbo mode, all connections receive OUTPUT and the
focused connection receives INPUT.

One-shot commands (KILL, DETACH) can be sent on a fresh connection
without ATTACH.

## Configuration

Config file: `$XDG_CONFIG_HOME/lumi/lumi.conf` (fallback
`~/.config/lumi/lumi.conf`). Gitconfig-style INI format parsed by libcfg.

### Attach Mode

```ini
[attach]
    mode = turbo
```

Values: `screen` (default), `turbo`, `minimal`. Selects the UI mode for
`lumi attach` and `lumi new`. The `-m` command-line flag overrides this
setting.

### Key Bindings

```ini
[keys]
    prefix = C-a

[bind]
    c = new-window
    n = next-window
    p = prev-window
    d = detach
```

Key names: `C-x` (ctrl), `space`, `quote`, `tab`, `esc`, `backspace`, or a
single character. Action names match `keys_action_to_name()` output.

### Menu Colors

```ini
[menu]
    fg = 7
    bg = 4
    sel_fg = 0
    sel_bg = 15
    key_fg = 10
    sel_key_fg = 2
    border_fg = 7
```

Values are indexed terminal colors (0-255).

### Status Line

```ini
[status]
    format = : ${window-list}
    position = bottom
```

Template variables: `${window-list}`, `${session-name}`, plus functions like
`$(left)`, `$(right)`, `$(center)`, `$(fill)`, `$(truncate N,...)`.

## Build System

The build system is a modular GNU Make framework ([OrangeTide/makefile][1]).
Full documentation is in the comment block at the top of `GNUmakefile`.

**Target types:** `EXECUTABLES` (binaries), `LIBRARIES` (static .a),
`SHARED_LIBS` (.so/.dylib/.dll).

**Module discovery:** `GNUmakefile` seeds from `src/module.mk`. Each
module.mk declares targets and may set `SUBDIRS` to pull in child module.mk
files. No filesystem scanning -- the tree is driven entirely by `SUBDIRS`.

**Per-target variables:** Each target gets `_CFLAGS`, `_CPPFLAGS`, `_LDFLAGS`,
`_LDLIBS`, `_LIBS` (internal deps), and `_SRCS` (relative to `_DIR`).

**Exported flags:** Libraries declare `_EXPORTED_CPPFLAGS` (typically
`-I$(<name>_DIR)`) inherited transitively by consumers via `_LIBS`.

Output: `_build/<triplet>/` (objects), `_out/<triplet>/bin/` (binaries).

### Adding a New Module

1. Create `src/<name>/module.mk`
2. Set `<name>_DIR := $(dir $(lastword $(MAKEFILE_LIST)))`
3. List sources in `<name>_SRCS` (relative to `_DIR`)
4. Set `<name>_EXPORTED_CPPFLAGS = -I$(<name>_DIR)` for libraries
5. Append to `EXECUTABLES`, `LIBRARIES`, or `SHARED_LIBS`
6. Add the subdirectory to a parent's `SUBDIRS`

## Tests

`make run-tests` builds and runs all test suites. Test binaries are declared
via `_TESTCMD` in module.mk files.

| Suite          | Library    | Tests |
|----------------|------------|-------|
| test_iox       | libiox     | 14    |
| test_vt        | libvt      | 32    |
| test_render    | librender  | 11    |
| test_txl       | libtxl     | 38    |
| test_ipc       | libipc     | 8     |
| test_attr_store| libattr    | 15    |
| test_sessdir   | libsessdir | 55    |
| test_keys      | libkeys    | 31    |
| test_cfg       | libcfg     | 39    |
| test_status    | libstatus  | 27    |
| test_splash    | libsplash  | 131   |
| test_utf8      | libutf8    | 48    |
| test_tui       | libtui     | 27    |
| test_wm        | libwm      | 23    |
| test_tile      | libtile    | 19    |
| test_input     | attach     | 15    |
| test_predict   | attach     | 12    |
| **Total**      |            | **545** |

## Library Dependency Graph

```
lumi-mserver: libiox, libsessdir, libsession, libipc, libpty, libvt,
              libutf8, libcore
lumi-attach:  libiox, libsessdir, libipc, libtio, librender, libtxl,
              libtermlib, libvt, libutf8, libkeys, libcfg, libstatus,
              libcore, libtui, libtui_term, libwm, libtile, libattr
lumi-proxy:   libiox, libsessdir, libipc, libcore
libwm:        libvt, libtui, libutf8
libtui:       libvt, libutf8
libtui_term:  libtui, libtxl, libtio, libtermlib, libvt, libutf8, libcore
```

Internal libraries are static archives (.a), linked only by the sub-commands
that need them. Transitive dependencies are resolved by the build system via
`_LIBS`.

## VT Emulation Pipeline

```
raw bytes -> vt_parse (state table) -> vt_ops (callbacks) -> vt_state/vt_buf
```

The parser is a data-driven VT500 state machine. `vt_ops` translates parsed
sequences into operations on `vt_state` (cursor movement, SGR attributes,
screen clearing, scrolling). `vt_buf` manages a row-pointer grid with a
2000-line scrollback ring and on-demand alternate screen.

Both server and client maintain independent VT state. The server's copy is
authoritative (used for screen replay). The client's copy drives the local
renderer.

## Renderer

The differential renderer (`librender`) maintains a shadow buffer mirroring
what is currently displayed on the real terminal. On each update:

1. `render_diff` compares each cell in `vt_buf` against the shadow
2. Only changed cells emit terminal output (CUP positioning + SGR + character)
3. `VT_ROW_DIRTY` flags allow skipping entire unchanged rows
4. Shadow buffer is updated to match

`render_full` redraws everything (used on attach and after overlay dismiss).

For compositor output (turbo mode), `render_cells_full` and
`render_cells_diff` accept a flat `vt_cell *` array instead of a
`vt_state`. These compare every cell against the shadow (no dirty-row
optimization since the compositor rebuilds the entire frame). Cursor
position and visibility are passed explicitly.

[1]: https://github.com/OrangeTide/makefile
[2]: https://orangetide.github.io/the-mechanical-researcher/serialization-formats/index.html
