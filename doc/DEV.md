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
| src/libtaskbar/      | Taskbar with shell-like template expansion                |
| src/libtermlib/      | Vendored terminfo parser (aux01/termlib, MIT)             |
| src/libtile/         | Binary split-pane compositor for screen mode splits       |
| src/libtio/          | Terminal raw mode, 8KB buffered writes, restore on exit   |
| src/libtxl/          | Terminal translation engine (operations -> escape sequences) |
| src/libutf8/         | UTF-8 encode/decode, Unicode-version-aware rune_width()   |
| src/libvt/           | VT500 terminal emulator: parser, ops, cell grid, scrollback |
| src/libwm/           | Overlapping window manager compositor (z-order, hit test) |
| src/cmd/attach/      | lumi-attach -- connect to server, relay I/O, menu overlay |
| src/cmd/attr/        | lumi-attr -- get/set/delete per-session attributes        |
| src/cmd/detach/      | lumi-detach -- detach clients from a session              |
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
`lumi-mserver` process owning a single PTY and VT emulation state. Any number
of `lumi-attach` clients discover servers via the session directory
(`libsessdir`), connect to each one over its own Unix domain socket, and
route input/output by file descriptor. One client holds the keyboard (role
`IPC_ROLE_WRITE`) at a time in single-writer mode, or several at once in
multi-writer mode; every other client watches (`IPC_ROLE_VIEW`). There is no
centralized server process.

```
                            sessdir: /run/user/<uid>/lumi/<session>/
                            +------------------------------------+
                            | <pid1>/socket  <pid1>/title        |
                            | <pid2>/socket  <pid2>/title        |
                            | <pid3>/socket  <pid3>/title        |
                            +------------------------------------+
                                  |          |          |
                              mserver 1   mserver 2   mserver 3
                              +--------+  +--------+  +--------+
                              | PTY+VT |  | PTY+VT |  | PTY+VT |
                              +--------+  +--------+  +--------+
                                  ^^         ^^          ^^
                                  ||         ||          ||
                    each mserver accepts one socket connection per
                    attached client -- role_for() decides its role
                                  ||         ||          ||
                                  vv         vv          vv
 +-----------------------+                          +-----------------------+
 | lumi-attach (WRITE)   |                          | lumi-attach (VIEW)    |
 |   stdin -> tkbd_parse |                          |   input dropped by    |
 |   mconns[*]: WRITE    |                          |   role_for() (asked   |
 |   cwins[*]: vt+parser |                          |   IPC_ATTACH_F_VIEW)  |
 |   renderer, taskbar   |                          |   mconns[*]: VIEW     |
 +-----------------------+                          |   follows WRITE's     |
                                                    |   focus/layout via    |
                                                    |   mirror_sync()       |
                                                    +-----------------------+
```

One session can also be reached by a client of a different uid, relayed
through the broker described under Cross-User Broker below; that client
looks like an ordinary connection to each mserver, just relayed rather than
direct.

### Micro-Server (lumi-mserver)

Each `lumi-mserver` owns a single window: one PTY, one VT emulation state,
one listen socket. It runs an `iox_loop` event loop with fd watchers for:

- **Listen socket** -- accepts client connections
- **Client fds** -- receives IPC messages (input, resize, kill) from each
  connected client
- **PTY master fd** -- reads child output

On PTY read, the server feeds output through `window_feed()` (VT parser) to
maintain a server-side screen image, then forwards raw bytes as OUTPUT to
every connected client. On attach, it sends an ATTACH_REPLY with the current
VT dimensions and the role granted, followed by a full screen replay via
OUTPUT messages.

Several clients may be connected to the same window at once (`struct
mclient`, a linked list walked with `mclient_first()`/`mclient_next()`);
`role_for()` decides each connection's role (see Client Roles and Coupling
below) rather than one connection displacing another. SIGCHLD reaps the dead
child process and shuts down the server, which disconnects every client
still attached to it.

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
- **minimal** -- bare passthrough with no taskbar, no mouse tracking,
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

#### Client Roster and Coupling

A session's attached clients are tracked in two places. Each `lumi-mserver`
keeps its own in-process list of connections to that one window (`struct
mclient`, walked with `mclient_first()`/`mclient_next()`); this is what
`role_for()` consults to decide a new connection's role. Separately, the
session directory keeps a session-wide roster (`sessdir_client_register()`/
`sessdir_client_list()`/`sessdir_client_unregister()`, `libsessdir`) that
`lumi share -l` reads to print one row per client rather than one row per
window connection.

Each roster entry (`struct sessdir_client`, `sessdir_control.h`) holds:

- **client_id** -- the client's own id, its pid for a local attach or a
  broker child's pid for a relayed one (a broker uses its own pid rather
  than the peer's uid so two connections from the same foreign uid do not
  collide in the roster)
- **name** -- display name, `user@host`
- **who** -- `uid:<n>` or `key:<name>` for a broker- or netchan-relayed
  client; empty for a same-uid local client, since that case is always the
  session owner's own uid
- **role** -- `"write"` or `"view"` (a pending `ask` admission is shown as
  `"ask"` until approved)
- **mode** -- the client's UI mode (`screen`/`turbo`/`minimal`), or `"-"`
  when a broker registered the entry on the client's behalf and has no way
  to know it
- **coupling** -- `"mirror"` or `"free"` for a local view-role client (see
  `-v`/`-F` under `lumi attach`); `"-"` for a write-role client or a
  broker/netchan registration, neither of which the concept applies to
- **pending** -- whether this connection is an `ask` admission still
  awaiting `lumi share -a`/`-d`

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

## Client Roles and Coupling

### Takeover vs. Sharing

Sharing is opt-in. A plain `lumi attach` takes the session over: the
clients already attached detach and the new one gets the keyboard.
`client_claim_role()` (`attach.c`) runs before the first `ATTACH` goes out,
tries `sessdir_token_acquire()`, and on failure calls `client_takeover()`
unless the client asked to join (`-x`, `share_join`) or asked to watch
(`-v`, `IPC_ATTACH_F_VIEW`), or the session is in multi-writer mode.

Its `may_take_over` argument is what separates attaching from switching.
`micro_switch_session()` passes 0: moving this client to another session
joins that session, since a key that moves this client is not a request to
end anybody else's, and the picker gives no warning that it might. That
also keeps the switch path off the blocking wait below, which matters
because it runs inside the event loop where startup does not.

`client_takeover()` calls `sessdir_kick_others()` (`libsessdir`) and then
retries the token acquire. The token is an `flock` held for the lifetime of
the holder, so the acquire only succeeds once that process is gone, which
is why the takeover waits rather than assuming. Blocking there is safe: it
only runs before the event loop starts. A client reached through a broker
does not read this session directory and so is not displaced; the takeover
then fails and the new client attaches read-only with a notice saying so.

`sessdir_kick_others()` posts a `SESSDIR_CTL_KICK` addressed to every
client at once (`target = 0`, honored by `share_ctl_poll()` as "everyone
but the actor"), then walks the roster posting one addressed to each
client in turn, waiting for its pid to go before posting the next. Both
passes are needed. The broadcast is the fast path, but target 0 is newer
than the sessions it has to work on: a client that has been attached since
before the last upgrade only understands a kick addressed to it by id, and
a long-lived session is exactly where such a process lives. The per-client
posts are serialized because the mailbox holds one message, so posting the
next before the current one has been read would take it away from the
client it was addressed to. `lumi detach` is the same call with actor 0,
and `lumi share -k` / `lumi detach -c` post a single addressed kick.

### Single-Writer vs. Multi-Writer

The session's write mode is a persistent setting, not a per-connection
choice: `sessdir_share_mode_get()`/`_set()` (`libsessdir`) read and write
`MODE=` in `<session>/control`, defaulting to `SESSDIR_SHARE_SINGLE_WRITER`
when the file does not exist. `lumi share -M` sets it. Each `lumi-mserver`
re-reads it directly (no watch, no caching) inside `role_for()` every time a
connection asks for a role, so a mode change takes effect for the next
attach without restarting any server; a client already attached keeps
whatever role it has until it reattaches or is handed the keyboard with
`lumi share -g`.

`role_for()` (`mserver.c`) decides the role for a new (or role-requesting)
connection in this order:

1. `IPC_ATTACH_F_VIEW` set -- grant `IPC_ROLE_VIEW` outright.
2. `IPC_ATTACH_F_TOKEN` set -- demote every other `IPC_ROLE_WRITE`
   connection on this window to `IPC_ROLE_VIEW` (each gets an
   `IPC_MSG_ROLE_CHANGE`), then grant this one `IPC_ROLE_WRITE`. The token
   is trusted because only the session owner's own uid can present it; it
   settles which of several simultaneously-attaching cooperating clients
   ends up holding the keyboard, on every window, rather than one process
   winning window 1 and another winning window 2.
3. Session mode is multi-writer -- grant `IPC_ROLE_WRITE` unconditionally;
   nobody already typing is displaced.
4. Otherwise (single-writer, no token) -- grant `IPC_ROLE_WRITE` only if no
   other connection on this window already holds it, else `IPC_ROLE_VIEW`.

In multi-writer mode the token still works as an attach-time tiebreaker
(step 3 above never applies once step 2 has already granted the role) but
no longer demotes anyone as a side effect of *not* being presented -- it
survives as an optional input lock, not a precondition for typing.

### Independent vs. Shared Display

`share.display` (`sessdir_share_display_get()`/`_set()`, also stored in
`<session>/control`) only matters for write-role clients in multi-writer
mode, and only decides whether a writer's own view is coupled to the
others':

- **independent** (default) -- a writer publishes its own focus and layout
  (`mirror_publish()`) so others can follow it, but never applies another
  writer's layout or focus to itself.
- **shared** -- a writer also follows the others, the same way a
  view-role mirror-coupled client already does. `mirror_following()`
  (`attach.c`) returns true for a writer once `mirror_display_shared()`
  is true, so `mirror_sync()`'s periodic and watch-driven checks apply an
  incoming layout to it as well, *except* while this writer has its own
  unpublished edit pending (`layout_dirty`) -- `mirror_apply_layout()`
  refuses to import the disk tree over a live edit it would otherwise
  free out from under itself. `mirror_publish()` then exports that
  pending edit as-is: last-writer-wins on disk, same as single-writer
  mode, this is not a merge. Catching up on another writer's more recent
  change happens on `mirror_sync()`'s own timer, in whatever gap this
  writer is not dirty.

### View-Role Coupling

A view-role (`IPC_ROLE_VIEW`) client chooses independently whether to
follow the write-role client's focus and layout, via `IPC_ATTACH_F_MIRROR`
on attach (`-v` without `-F`, the default for a client that attaches
read-only). `mirror_sync()` (`attach.c`) is what a mirror-coupled client
runs on a session-directory watch event and a slow poll fallback: it reads
the published focus and, if it names a window this client already has a
connection to and is not already watching, calls `micro_select_window()`;
it then checks the layout file's mtime and generation and reimports it via
`screen_apply_layout()`/`turbo_apply_layout()` if either changed. `-F`
(`IPC_ATTACH_F_MIRROR` not set) leaves a client to pick its own focus and
layout while still receiving every window's output.

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

| Type          | Code     | Direction | Payload                                     |
|---------------|----------|-----------|----------------------------------------------|
| ATTACH        | `0x0001` | C -> S    | (empty), microser `IpcSize` (old client), or `ipc_attach` (size + `IPC_ATTACH_F_*` flags + identity) |
| ATTACH_REPLY  | `0x0006` | S -> C    | microser `IpcSize` (old server, decodes as role WRITE) or `ipc_attach_reply` (size + granted role) |
| ROLE_CHANGE   | `0x0007` | S -> C    | 1 byte: the role now held (sent when it changes while attached) |
| CLIENT_EVENT  | `0x0008` | S -> C    | 1 byte kind (`IPC_CLIENT_JOIN`/`LEAVE`/`ROLE`) + client id (u32 BE) + name bytes |
| ROLE_REQUEST  | `0x0009` | C -> S    | 1 byte of `IPC_ATTACH_F_*` flags: ask for a new role without reattaching |
| DETACH        | `0x0002` | either    | (empty)                                      |
| KILL          | `0x0003` | C -> S    | (empty)                                      |
| OK            | `0x0004` | S -> C    | (empty)                                      |
| ERROR         | `0x0005` | S -> C    | error message bytes                          |

See `src/libipc/ipc_msg.h` for `IPC_ATTACH_F_VIEW`, `_SIZE_OBSERVE`, `_MIRROR`,
and `_TOKEN`, and the Client Roles and Attach Handshake sections below.

**0x01xx -- Data Transfer**

| Type        | Code     | Direction | Payload                                  |
|-------------|----------|-----------|--------------------------------------------|
| INPUT       | `0x0100` | C -> S    | raw keyboard bytes                       |
| OUTPUT      | `0x0101` | S -> C    | raw PTY output                           |
| FLOW_CTRL   | `0x0102` | C -> S    | 1 byte: 1 = pause, 0 = resume             |
| REFRESH     | `0x0103` | C -> S    | (empty): resend a full screen replay at the current size |
| INPUT_BEGIN | `0x0104` | C -> S    | (empty): open a bracketed multi-message input run (13-b) |
| INPUT_END   | `0x0105` | C -> S    | (empty): close the run and flush it to the PTY as one write |

INPUT_BEGIN/INPUT_END exist so a bracketed paste (or any other multi-message
input run) cannot be torn apart by another writer's own `INPUT` landing
between two messages of the run in multi-writer mode: the server buffers a
connection's `INPUT` payloads per-connection between BEGIN and END (or until
disconnect) instead of writing each one immediately.

**0x02xx -- Window / PTY Management**

| Type       | Code     | Direction | Payload                          |
|------------|----------|-----------|-------------------------------------|
| PTY_FLAGS  | `0x0201` | S -> C    | 1 byte bitmask (`IPC_PTY_ECHO`)   |
| WIN_RESIZE | `0x0207` | C -> S    | microser `IpcWinResize`          |

**0x03xx -- Attribute Store**

Used by `lumi attr` (`libattr`) to get/set/delete per-session key-value
attributes transactionally.

| Type              | Code     | Direction | Payload                  |
|-------------------|----------|-----------|--------------------------|
| ATTR_TXN_BEGIN    | `0x0300` | C -> S    | (empty): begin a transaction |
| ATTR_TXN_COMMIT   | `0x0301` | C -> S    | txn id                   |
| ATTR_TXN_ROLLBACK | `0x0302` | C -> S    | txn id                   |
| ATTR_TXN_OK       | `0x0303` | S -> C    | txn id                   |
| ATTR_SET          | `0x0310` | C -> S    | txn id + key + value     |
| ATTR_DELETE       | `0x0311` | C -> S    | txn id + key             |
| ATTR_GET          | `0x0320` | C -> S    | txn id + key             |
| ATTR_VALUE        | `0x0321` | S -> C    | key + value              |
| ATTR_LIST         | `0x0322` | C -> S    | txn id                   |
| ATTR_ENTRIES      | `0x0323` | S -> C    | key=value entries        |
| ATTR_OK           | `0x0324` | S -> C    | (empty): success         |

**0x04xx -- Proxy Control**

Sent over the multiplexed `lumi-proxy` connection (SSH-tunneled or netchan),
not directly to an mserver; `window_id` is 0 in the proxy envelope.

| Type              | Code     | Direction    | Payload                       |
|-------------------|----------|--------------|-------------------------------|
| PROXY_READY       | `0x0400` | proxy -> C   | initial window list           |
| PROXY_WIN_ADDED   | `0x0401` | proxy -> C   | a new window appeared         |
| PROXY_WIN_REMOVED | `0x0402` | proxy -> C   | a window's server exited      |
| NOP               | `0x0403` | C -> proxy   | ignored; used as a migration nudge after roaming |

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

**`IpcAttach`** (ATTACH). Tags 1 and 2 deliberately match `IpcSize` so an old
peer on either side decodes the shared prefix and ignores the rest:

| Field | Tag | Type   | Description          |
|-------|-----|--------|----------------------|
| 1     | u16 | rows   | Terminal row count   |
| 2     | u16 | cols   | Terminal column count|
| 3     | u8  | flags  | `IPC_ATTACH_F_*` bitmask (see Client Roles below) |
| 4     | u32 | client_id | This client's id, for `CLIENT_EVENT` correlation |
| 5     | string | name | Identity string (`user@host`, or the keystore key name for a netchan client) |

**`IpcAttachReply`** (ATTACH_REPLY):

| Field | Tag | Type   | Description          |
|-------|-----|--------|----------------------|
| 1     | u16 | rows   | Terminal row count   |
| 2     | u16 | cols   | Terminal column count|
| 3     | u8  | role   | Granted role: `IPC_ROLE_WRITE` (0) or `IPC_ROLE_VIEW` (1) |
| 4     | u8  | nclients | Number of clients already attached to this window |

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

Each `lumi-attach` <-> `lumi-mserver` connection is independent, and any
number of them may exist for the same window at once (see Client Roles and
Coupling above):

```
Client                          Micro-Server
  |                               |
  |--- ATTACH (r,c,flags,id,name)>|  client connects to mserver socket
  |<----- ATTACH_REPLY (r,c,role)-|  server grants a role (role_for())
  |<----- OUTPUT (replay) -------|  server dumps current VT state
  |<----- CLIENT_EVENT (to others)|  every other connected client is told
  |                               |  this one joined
  |<----- OUTPUT (ongoing) ------|  server forwards PTY reads
  |                               |
  |--- INPUT -------------------->|  keyboard bytes to PTY (write role only)
  |--- ROLE_REQUEST (flags) ----->|  ask for a different role without
  |<----- ROLE_CHANGE (role) -----|  reattaching; may demote another
  |                               |  connection if IPC_ATTACH_F_TOKEN
  |--- WIN_RESIZE (id,r,c) ------>|  resize PTY
  |                               |
  |--- DETACH ------------------->|  client disconnects
  |                               |
  |--- KILL --------------------->|  terminate mserver
  |<----- OK --------------------|  confirmed (mserver exits)
```

An old client that sends no `flags`/`client_id`/`name` fields is decoded as
a bare `IpcSize` (tags 1-2 only) and granted `IPC_ROLE_WRITE`, matching its
old behavior; an old server that sends no `role` field back is decoded the
same way by a new client. This is why `IpcAttach`/`IpcAttachReply` keep
tags 1 and 2 identical to `IpcSize` (see `lumi.idl`) rather than being
defined from scratch.

The client maintains N such connections simultaneously (one per mserver
in the session, times however many role/mirror connections it opens --
today exactly one per window per client process). Window switching in
screen mode changes which connection receives INPUT; in turbo mode, all
connections receive OUTPUT and the focused connection receives INPUT.

One-shot commands (KILL, DETACH) can be sent on a fresh connection
without ATTACH.

## Cross-User Broker

`lumi share -u user` or `-G group` starts `lumi proxy -L` (`proxy.c`) as a
background broker for the session: a listener on a Unix socket outside the
session directory (`proxy_broker_dir()`/`proxy_broker_sock_path()`, under
`/tmp/lumi-broker-<uid>/`, since a single uid cannot be expressed in socket
permissions alone) that relays connections from other local users to every
mserver in the session.

The broker's accept loop (`proxy_listen_run()`) `fork()`s a child per
accepted connection; the child (`broker_serve_client()`) does all the work
and never
returns -- it always `_exit()`s once its one client is done:

1. Reads the peer's kernel-reported uid/gid via `ipc_peer_cred()`. No peer
   credential support on the platform is a hard refusal, not an open door.
2. Checks `sessdir_access_check()` against `<session>/access` (see
   `doc/lumi.1`'s Cross-user access section for the file format) to get a
   role ceiling.
3. If the matching rule was `ask`, admits the connection as pending
   (`sessdir_client_register()` with `pending=1`) rather than granting or
   refusing outright, but only if `approver_available()` says someone could
   plausibly answer (a write-token holder or a locally attached owner
   client); otherwise it is denied, the same fail-closed default as an
   explicit `deny` rule. `broker_wait_for_approval()` then blocks the child
   on the session's control-message watch until `lumi share -a`/`-d`
   answers it or a timeout elapses.
4. Every decision is audited via `sessdir_access_audit()` (`<session>/access.log`).
5. Once admitted, the child registers itself in the session-wide roster
   (`sessdir_client_register()`) on the client's behalf -- the connecting
   process has no local session directory to write into itself -- using
   its own pid as `client_id` (not the peer's uid: two connections from the
   same uid would otherwise collide in the roster) and `who="uid:<n>"`.
6. `dup2()`s the accepted socket onto stdin/stdout and runs the same proxy
   engine used for an SSH-tunneled remote session, with `proxy_attach_flags`
   set to `IPC_ATTACH_F_VIEW` when the granted role is view-only. From here
   the child is indistinguishable from an SSH-tunneled `lumi proxy`.

A same-uid client never goes through any of this: each mserver's listen
socket only accepts connections from its own uid (checked by the kernel via
the session directory's `0700` mode plus each socket's own permissions), so
`lumi-attach` connects directly. The broker exists purely to bridge a uid
that could not otherwise reach those sockets.

## Connection Types and Trust Tiers

| Tier | Path | Peer authenticated by | Encrypted | Authorized by |
|------|------|------------------------|-----------|----------------|
| Local | Unix socket, same uid | Kernel (socket/directory permissions) | No (local IPC) | Implicit (same user) |
| Broker | `lumi proxy -L`, same host | Kernel peer uid/gid (`ipc_peer_cred()`) | No (local IPC) | `<session>/access` ACL |
| SSH-tunneled | `lumi proxy` over `ssh(1)` | SSH's own login (key, password, etc.) | Yes (SSH channel) | Implicit (whoever can log in) |
| Netchan, key | `lumi net-proxy -L`/`-k`, direct-connect | Client's `~/.config/lumi/id_netchan` identity key against `~/.config/lumi/authorized_keys` | Yes (X25519 netchan) | `authorized_keys` |
| Netchan, password | `lumi net-proxy -L`, direct-connect | Username + password against `~/.config/lumi/passwd` | Yes (X25519 netchan) | `passwd` |

All netchan tiers additionally support server identity pinning
(`lumi attach -V`): the server's long-term host key is compared against
what the server published out of band and against the client's
`~/.config/lumi/known_hosts`, so the rotating per-connection key alone
does not have to carry authentication of the server across restarts.

Local and broker tiers assume trust in the local kernel's uid/gid
reporting and the session directory's own file permissions; the SSH and
netchan tiers assume trust in, respectively, the system's SSH
configuration or the netchan key/password stores under
`~/.config/lumi`. See `doc/lumi.1`'s Security and Cross-user access
sections for the exact file paths, modes, and ACL rule syntax.

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

### Taskbar

```ini
[taskbar]
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
| test_taskbar   | libtaskbar | 27    |
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
              libtermlib, libvt, libutf8, libkeys, libcfg, libtaskbar,
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
