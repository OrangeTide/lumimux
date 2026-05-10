% LUMIMUX(1) | General Commands Manual

# NAME

lumimux -- terminal multiplexer

# SYNOPSIS

**lumi** *command* [*options*] [*args*]

# DESCRIPTION

lumimux is a terminal multiplexer in the style of GNU Screen.
It lets you run multiple shell sessions inside a single terminal,
detach from them, and reattach later -- even from a different terminal
or a different machine via SSH tunneling.

lumimux uses a git-style sub-command architecture.
Each command can also be invoked directly as **lumi-***command*
(e.g. **lumi-new**).
Default keybindings are identical to GNU Screen (Ctrl-A prefix).

# COMMON OPTIONS

**-s** *name*
:   Session name. Defaults to **0** if not specified.

**-m** *mode*
:   UI mode: **screen** (default), **turbo**, **minimal**.
    Accepted by **new** and **attach**.

# COMMANDS

**lumi new** [**-Ad**] [**-f** *window*] [**-m** *mode*] [**-s** *name*] [*shell*]
:   Create a new session and attach to it.
    **-A** reattaches if the session already exists.
    **-d** creates the session without attaching.
    **-f** *window* focuses the given window (by server PID) after startup.

**lumi attach** [**-f** *window*] [**-m** *mode*] [**-s** *name*] [*name*]
:   Attach to an existing session. Remote sessions may be specified
    using scp-style syntax: [*user***@**]*host***:***session*.
    When a remote session is given, **lumi** launches **lumi proxy** on
    the remote host via **ssh**(1) and tunnels all window I/O over the
    SSH connection.

**lumi detach** [**-s** *name*]
:   Detach the currently connected client.

**lumi list**
:   List active sessions and clean up stale sockets.

**lumi kill** [**-s** *name*]
:   Terminate a session.

**lumi new-window** [**-s** *name*] [*shell*]
:   Create a new window in a running session.

**lumi attr** [**-s** *name*] **get**|**set**|**delete**|**list** [*key*] [*value*]
:   Get, set, delete, or list per-session attributes.

**lumi version**
:   Print version information.

The default session name is **0** when not specified.

# KEY BINDINGS

All bindings use the prefix key **Ctrl-A** (same as GNU Screen).
Pressing the prefix key shows a guided menu of all available actions.

| Key | Action |
|-----|--------|
| **c**, **C** | Create a new window |
| **k**, **K** | Kill current window |
| **n**, **Space**, **Ctrl-Space**, **Ctrl-N** | Next window |
| **p**, **Ctrl-P** | Previous window |
| **0**--**9** | Select window by number |
| **d**, **D** | Detach from session |
| **w**, **"** | Window list |
| **s** | Toggle status bar |
| **q** | Applications menu |
| **[**, **Escape** | Enter scrollback mode |
| **h** | Split pane horizontally |
| **v** | Split pane vertically |
| **Tab** | Next pane |
| **X** | Close pane |
| **r** | Resize pane |
| **t** | Toggle between turbo and screen modes |
| **m** | Minimize window (turbo mode) |
| **f** | Maximize window (turbo mode) |
| **P** | Window color picker (turbo mode) |
| **S** | Toggle scroll lock |
| **I** | Toggle input lock |
| **U** | Session picker |
| **]** | Paste from internal clipboard |
| **y** | Sync internal clipboard to system |
| **Ctrl-A** | Send literal Ctrl-A |

# SCROLLBACK MODE

Enter scrollback mode with **Ctrl-A [** or **Ctrl-A Escape**.

| Key | Action |
|-----|--------|
| **Up**, **Down**, **Page Up**, **Page Down** | Scroll through history |
| Mouse wheel | Scroll up and down |
| Mouse drag | Select text (copied to clipboard on release) |
| **q**, **Escape** | Exit scrollback mode |

A scrollbar is shown on the right edge in turbo mode.

# CONFIGURATION

The configuration file is loaded from
`$XDG_CONFIG_HOME/lumi/lumi.conf` (fallback `~/.config/lumi/lumi.conf`).
The file uses a git-config-style INI format; see **lumi**(1) for the
full specification.

Key configuration sections: **[attach]** (mode), **[keys]** (prefix),
**[bind]** (key bindings and layers), **[menu]** (colors),
**[status]** (format and position), **[ui]** (theme).

# QUICK START

Build from source and run:

    make
    export LUMI_LIBEXEC_PATH=_out/x86_64-linux-gnu/bin
    alias lumi=_out/x86_64-linux-gnu/bin/lumi

    lumi new -s work

Or install from a release tarball:

    tar xzf lumi-*-linux-x86-64.tar.gz -C /opt
    export PATH=/opt/lumi-*/bin:$PATH

    lumi new -s work

# INSTALLATION

Extract the release tarball under **/opt** or **/usr/local**.
The dispatcher finds sub-commands relative to its own binary
(`../lib/lumi-core/`), so no environment variables are needed.

Alternatively, set **LUMI_LIBEXEC_PATH** to a colon-separated
list of directories containing the `lumi-*` sub-commands.

Packages are available for Debian (APT repo via GitHub Pages),
RPM, and Arch (AUR).

# BUILDING

Requires GNU Make and a C99 compiler.

    make                    # build all executables
    make run-tests          # build and run all test suites
    make RELEASE=1          # optimized build (LTO, -O2)
    make clean-all          # remove all build artifacts

Static build with musl:

    make CC=musl-gcc LDFLAGS=-static TARGET_TRIPLET=x86_64-linux-musl lumi

# ENVIRONMENT

**LUMI_SESSION**
:   Set inside an attached session to the current session name.
    Prevents recursive attach. Used by **lumi attr** as the default
    session name when **-s** is not given.

**LUMI_LIBEXEC_PATH**
:   Override the search path for **lumi-***command* executables.

**LUMI_DEBUG**
:   Set to a file path to enable debug tracing. Diagnostic output
    is appended to the specified file.

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

**screen**(1), **tmux**(1), **ssh**(1)
