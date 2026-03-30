% LUMIMUX(1) | General Commands Manual

# NAME

lumimux -- terminal multiplexer

# SYNOPSIS

**lumi** *command* [*args*]

# DESCRIPTION

lumimux is a terminal multiplexer in the style of GNU Screen.
It lets you run multiple shell sessions inside a single terminal,
detach from them, and reattach later -- even from a different terminal.

lumimux uses a git-style sub-command architecture.
Each command is a separate executable found via a configurable search path.
Default keybindings are identical to GNU Screen (Ctrl-A prefix).

# COMMANDS

**lumi new** [**-Ad**] [**-n** *name*] [*shell*]
:   Create a new session and attach to it.
    **-d** creates the session without attaching.
    **-A** reattaches if the session already exists.

**lumi attach** [**-s** *name*] [*name*]
:   Attach to an existing session.

**lumi detach** [**-s** *name*]
:   Detach the currently connected client.

**lumi list**
:   List active sessions and clean up stale sockets.

**lumi kill** [**-s** *name*]
:   Terminate a session.

**lumi new-window** [**-s** *name*]
:   Create a new window in a running session.

**lumi version**
:   Print version information.

The default session name is **0** when not specified.

# KEY BINDINGS

All bindings use the prefix key **Ctrl-A** (same as GNU Screen).

| Key | Action |
|-----|--------|
| **c** | Create a new window |
| **n**, **Space** | Next window |
| **p** | Previous window |
| **0**--**9** | Select window by number |
| **k** | Kill current window |
| **d** | Detach from session |
| **s** | Toggle status line |
| **Ctrl-A** | Send literal Ctrl-A |

# QUICK START

Build from source and run:

    make
    export LUMI_LIBEXEC_PATH=_out/x86_64-linux-gnu/bin
    alias lumi=_out/x86_64-linux-gnu/bin/lumi

    lumi new -n work

Or install from a release tarball:

    tar xzf lumi-*-linux-x86-64.tar.gz -C /opt
    export PATH=/opt/lumi-*/bin:$PATH

    lumi new -n work

# INSTALLATION

Extract the release tarball under **/opt** or **/usr/local**.
The dispatcher finds sub-commands relative to its own binary
(`../lib/lumi-core/`), so no environment variables are needed.

Alternatively, set **LUMI_LIBEXEC_PATH** to a colon-separated
list of directories containing the `lumi-*` sub-commands.

The default search path is:

    /usr/lib/lumi-core:~/.local/lib/lumi-core

# BUILDING

Requires GNU Make and a C99 compiler.

    make                    # build all executables
    make run-tests          # build and run all test suites
    make RELEASE=1          # optimized build (LTO, -O2)
    make clean-all          # remove all build artifacts

# ARCHITECTURE

lumimux uses a micro-server architecture.  Each window runs as an
independent `lumi-mserver` process owning a single PTY and VT
emulation state.  The client (`lumi-attach`) discovers servers via
the session directory, connects to each over its own Unix domain
socket, and routes input/output by file descriptor.

    lumi-attach (client)           lumi-mserver (per window)
     +------------------+          +--------+ +--------+ +--------+
     | stdin -> tkbd    |--INPUT-->| PTY+VT | | PTY+VT | | PTY+VT |
     | vt_parse+render  |<-OUTPUT--+--------+ +--------+ +--------+
     | status line      |          mserver 0  mserver 1  mserver 2
     +------------------+                 |        |        |
            ^           sessdir discovery |        |        |
            +-----------------------------+--------+--------+

# LICENSE

MIT-0 OR Public Domain.

# SEE ALSO

**screen**(1), **tmux**(1)
