# lumimux -- terminal multiplexer

## Introduction

lumimux is a rewrite of GNU Screen with a git-style sub-command architecture.
`lumi <cmd>` searches for `lumi-<cmd>` relative to its own binary
(`../lib/lumi-core/`), then `LUMI_LIBEXEC_PATH`, then
`/usr/lib/lumi-core:~/.local/lib/lumi-core`.

Default keybindings are identical to GNU Screen (Ctrl-A prefix).

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
lumi new -n work

# inside the session:
#   Ctrl-A c       create a new window
#   Ctrl-A n       next window
#   Ctrl-A p       previous window
#   Ctrl-A 0-9     select window by number
#   Ctrl-A k       kill current window
#   Ctrl-A d       detach (session keeps running)
#   Ctrl-A w       window picker
#   Ctrl-A s       toggle status line
#   Ctrl-A Ctrl-A  send literal Ctrl-A
#
#   Pressing Ctrl-A shows a guided menu of all available actions.

# after detaching, reattach
lumi attach work

# or create-or-reattach in one command
lumi new -A -n work

# list active sessions
lumi list

# kill the session from outside
lumi kill -s work
```

## Commands

| Command | Description |
|---------|-------------|
| `lumi new [-Ad] [-n name] [shell]` | Create a session and attach (`-d` detached, `-A` reattach) |
| `lumi attach [-s name]` | Attach to an existing session |
| `lumi detach [-s name]` | Detach a client from its session |
| `lumi list` | List active sessions |
| `lumi kill [-s name]` | Terminate a session |
| `lumi new-window [-s name]` | Create a window in a running session |
| `lumi version` | Print version information |

The default session name is `0` when not specified.

## Installation

Extract the release tarball under `/opt` or `/usr/local` and add the
`bin/` directory to your PATH:

```sh
tar xzf lumi-*-linux-x86-64.tar.gz -C /opt
export PATH=/opt/lumi-*/bin:$PATH
```

The dispatcher finds sub-commands relative to its own binary, so no
additional environment variables are needed.

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
 | status line      |           mserver 0   mserver 1   mserver 2
 +------------------+                  |         |         |
        ^            sessdir discovery |         |         |
        +------------------------------+---------+---------+
```

Internal libraries are static (.a), linked only by the sub-commands that
need them. See [doc/DEV.md][2] for the full developer guide, IPC protocol
format, and library dependency graph.

## Known Issues

This is pre-release software under active development.

- Single client per session -- a new attach disconnects the previous one.
- No vertical splits or copy/paste yet.

## License

MIT-0 OR Public Domain.

[1]: https://github.com/OrangeTide/makefile
[2]: doc/DEV.md
