# lumimux -- terminal multiplexer

## Introduction

lumimux is a rewrite of GNU Screen with a git-style sub-command architecture.
`lumi <cmd>` searches for `lumi-<cmd>` relative to its own binary
(`../lib/lumi-core/`), then `LUMI_LIBEXEC_PATH`, then
`/usr/lib/lumi-core:~/.local/lib/lumi-core`.

Default keybindings are identical to GNU Screen (Ctrl-A prefix).
Terminal control code handling is data-driven rather than hard-coded.

## Building

Requires GNU Make and a C99 compiler.

```sh
make                    # build all executables
make run-tests          # build and run all test suites
make RELEASE=1          # optimized build (LTO, -O2)
make clean-all          # remove all build artifacts
```

Output goes to `_out/<triplet>/bin/` (binaries) and `_build/<triplet>/`
(objects), where `<triplet>` comes from `$(CC) -dumpmachine`.

The build system is a modular GNU Make setup based on [OrangeTide/makefile][1].

## Quick Start

```sh
# build
make

# set up the search path for development (points to build output)
export LUMI_LIBEXEC_PATH=_out/x86_64-linux-gnu/bin
alias lumi=_out/x86_64-linux-gnu/bin/lumi

# create a session named "work" and attach
lumi new -s work

# inside the session:
#   Ctrl-A c       create a new window
#   Ctrl-A n       next window
#   Ctrl-A p       previous window
#   Ctrl-A 0-9     select window by number
#   Ctrl-A k       kill current window
#   Ctrl-A d       detach (session keeps running)
#   Ctrl-A w       window picker
#   Ctrl-A s       toggle taskbar
#   Ctrl-A [       scrollback mode (mouse wheel, Page Up/Down)
#   Ctrl-A t       toggle between turbo and screen modes
#   Ctrl-A U       session picker
#   Ctrl-A Ctrl-A  send literal Ctrl-A
#
#   Pressing Ctrl-A shows a guided menu of all available actions.

# after detaching, reattach
lumi attach work

# or create-or-reattach in one command
lumi new -A -s work

# attach to a remote session over SSH
lumi attach user@host:work

# list active sessions
lumi list

# kill the session from outside
lumi kill -s work
```

## Features

- **Three UI modes:** screen (GNU Screen-like), turbo (overlapping windows
  with mouse-driven move/resize/minimize/maximize), and minimal (bare
  passthrough).
- **Tiled splits:** horizontal and vertical pane splits in screen mode.
- **Remote sessions:** attach to sessions on remote hosts over SSH
  (`lumi attach user@host:session`).
- **Multi-session switching:** jump between sessions with the session
  picker (Ctrl-A U).
- **Scrollback:** browse history with keyboard or mouse wheel; mouse drag
  to select and copy text.
- **Configurable key bindings:** remap keys, define state-dependent binding
  layers that activate by window title regex or toggle state.
- **DCS pass-through:** SIXEL graphics forwarded to the outer terminal.
- **Speculative local echo:** predicted characters rendered immediately,
  confirmed or rolled back on server response.
- **Kitty keyboard protocol:** prefix key recognized in both traditional
  and kitty CSI u encodings.
- **Layout persistence:** window positions and sizes saved on detach and
  restored on reattach.
- **Per-window customization:** color picker, scroll lock, input lock.
- **Themes:** 9 built-in themes; user-defined themes via config file.
- **Config-driven:** gitconfig-style `lumi.conf` for key bindings, taskbar
  format, menu colors, and UI theme.
- **Single static binary:** 330 KB stripped musl build with no runtime
  dependencies.

## Commands

| Command | Description |
|---------|-------------|
| `lumi new [-Ad] [-f window] [-m mode] [-s name] [shell]` | Create a session and attach (`-d` detached, `-A` reattach) |
| `lumi attach [-f window] [-m mode] [-s name] [name]` | Attach to a local or remote session |
| `lumi detach [-s name]` | Detach a client from its session |
| `lumi list` | List active sessions |
| `lumi kill [-s name]` | Terminate a session |
| `lumi new-window [-s name] [shell]` | Create a window in a running session |
| `lumi attr [-s name] get\|set\|delete\|list [key] [value]` | Manage per-session attributes |
| `lumi version` | Print version information |

The default session name is `0` when not specified.
Remote sessions use scp-style syntax: `[user@]host:session`.

## Installation

Extract the release tarball under `/opt` or `/usr/local` and add the
`bin/` directory to your PATH:

```sh
tar xzf lumi-*-linux-x86-64.tar.gz -C /opt
export PATH=/opt/lumi-*/bin:$PATH
```

The dispatcher finds sub-commands relative to its own binary, so no
additional environment variables are needed.

## Terminal Configuration

lumi runs inside a host ("outer") terminal and depends on it to deliver
modified keys, forward advanced escape sequences, and report a usable
`TERM`. The GNU Screen-compatible defaults (Ctrl-A prefix, arrows, Page
Up/Down) work in every terminal without changes. The settings below unlock
the rest: Meta/Alt bindings, unambiguous modified keys via the kitty
keyboard protocol, and SIXEL graphics forwarded through DCS pass-through.

Three things matter:

- **Meta/Alt keys.** On macOS the Option key usually inserts accented
  characters instead of sending a Meta prefix. Configure it to send
  `Esc+` (Meta) so Alt-key bindings and Alt-driven apps inside lumi work.
- **Kitty keyboard protocol.** Terminals that support it report modified
  keys (Shift/Alt/Ctrl combined with Enter, Tab, etc.) unambiguously.
  lumi recognizes the prefix in both the traditional byte encoding and
  the kitty CSI u encoding, and forwards enhancement flags to child apps.
- **SIXEL / DCS pass-through.** lumi forwards SIXEL and other DCS
  sequences to the outer terminal when a single pane is focused, so the
  outer terminal must itself support SIXEL for graphics to appear.

### Per-terminal setup

- **[iTerm2][10]** (macOS): Settings -> Profiles -> Keys -> General, set
  *Left Option key* (and *Right Option key*) to **Esc+**. SIXEL and the
  kitty keyboard protocol are supported out of the box. Consider a
  separate profile if you still want Option to compose characters
  elsewhere.
- **[kitty][11]**: kitty keyboard protocol and SIXEL are native, no setup
  needed. On macOS set `macos_option_as_alt yes` in `kitty.conf` to make
  Option send Meta.
- **[WezTerm][12]**: kitty protocol and SIXEL supported. On macOS add
  `send_composed_key_when_left_alt_is_pressed = false` to `wezterm.lua`
  so Left-Alt sends Meta.
- **[Alacritty][13]**: recent versions speak the kitty keyboard protocol.
  On macOS set `window.option_as_alt: Both` in `alacritty.toml`. Alacritty
  does not implement SIXEL, so graphics pass-through will not render.
- **[GNOME Terminal][14]** / other VTE terminals: Alt sends Meta by
  default. SIXEL renders only when VTE was built with SIXEL support
  (most current distributions enable it). The kitty keyboard protocol is
  not supported, so lumi falls back to the traditional prefix encoding.
- **[xterm][15]**: add `XTerm*metaSendsEscape: true` to `~/.Xresources`
  (then `xrdb -merge ~/.Xresources`) so Alt sends Meta. For SIXEL, start
  xterm with `-ti vt340` or set `XTerm*decTerminalID: vt340`.
- **[Windows Terminal][16]**: Alt/Meta and, in current releases, SIXEL
  work without configuration.

To confirm your terminal reaches lumi, run a session and press a bound
Alt key or a modified Enter; if nothing happens, the outer terminal is
swallowing the modifier and needs the setting above.

## Architecture

lumimux uses a micro-server architecture. Each window runs as an independent
`lumi-mserver` process owning a single PTY and VT emulation state. The
client (`lumi-attach`) discovers servers via the session directory, connects
to each over its own Unix domain socket, and routes input/output by file
descriptor. Both client and server maintain independent VT state; the
server's copy is used for screen replay on attach.

```
lumi-attach (client)           lumi-mserver (per window)
 +------------------+          +--------+  +--------+  +--------+
 | stdin -> tkbd    |--INPUT-->| PTY+VT |  | PTY+VT |  | PTY+VT |
 | vt_parse+render  |<-OUTPUT--+--------+  +--------+  +--------+
 | taskbar          |           mserver 0   mserver 1   mserver 2
 +------------------+                  |         |         |
        ^            sessdir discovery |         |         |
        +------------------------------+---------+---------+
```

Internal libraries are static (.a), linked only by the sub-commands that
need them. See [doc/DEV.md][2] for the full developer guide, IPC protocol
format, and library dependency graph.

## Known Issues

- Single client per session -- a new attach disconnects the previous one.
- QUIC networked connections are not yet implemented (Unix sockets and SSH
  tunneling only).

## Credits and Inspiration

lumimux draws ideas and inspiration from several projects:

- [GNU Screen][3] -- the original terminal multiplexer. lumimux uses
  Screen-compatible default keybindings (Ctrl-A prefix) and nomenclature.
- [tmux][4] -- modern multiplexer whose client-server model and layout
  persistence informed lumimux's architecture.
- [dtach][5] -- minimal detach/attach tool. Its single-purpose design
  influenced the micro-server approach (one process per PTY).
- [mosh][6] -- mobile shell with speculative local echo and roaming.
  Inspired lumimux's predictive echo and planned QUIC transport.
- [DESQview][7] -- Quarterdeck's DOS multitasker. Its guided keystroke
  menus inspired the prefix-key popup that shows available actions.
- [Turbo Vision][8] -- Borland's text-mode UI framework. Its overlapping
  window manager inspired lumimux's turbo attach mode with mouse-driven
  move, resize, minimize, and maximize.
- [tvterm][9] -- Terminal emulator built around Turbo Vision (by magiblot)
## License

MIT-0 OR Public Domain.

[1]: https://github.com/OrangeTide/makefile
[2]: doc/DEV.md
[3]: https://www.gnu.org/software/screen/
[4]: https://github.com/tmux/tmux
[5]: https://github.com/crigler/dtach
[6]: https://mosh.org/
[7]: https://en.wikipedia.org/wiki/DESQview
[8]: https://en.wikipedia.org/wiki/Turbo_Vision
[9]: https://github.com/magiblot/tvterm
[10]: https://iterm2.com/
[11]: https://sw.kovidgoyal.net/kitty/
[12]: https://wezterm.org/
[13]: https://alacritty.org/
[14]: https://wiki.gnome.org/Apps/Terminal
[15]: https://invisible-island.net/xterm/
[16]: https://github.com/microsoft/terminal
