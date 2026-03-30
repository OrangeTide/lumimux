# TODO

## Near-term

- [x] Unit tests for libutf8 (encode/decode round-trips, width lookups).
      48-test suite covers decode, encode, runelen, and width lookups.
- [ ] Smoke test -- build all binaries, run `lumi version`, check exit codes.
      Use as a merge request gate and release blocker.

## Completed (Phase 5-6)

- [x] Multiple windows + session management (libsession).
      Per-window output subscriptions (WIN_WATCH/UNWATCH), per-window
      VT state on client, per-window resize protocol (WIN_RESIZE).
- [x] Config-driven key bindings (libkeys).
      Load defaults from code, override via [keys]/[bind] in lumi.conf.
- [x] Configuration + terminal translation (libcfg, libtxl, libstatus).
- [x] text UI for dialogs, menus, config check boxes.
      libtui widget library: tui_pad cell buffer with blend modes
      (transparent/opaque/color_bg/color_fg_bg), tui_stack 4-layer
      compositing, tui_box themed frame, tui_menu vertical key+label list,
      tui_list scrollable callback-based picker, tui_sep separator.
      9 built-in themes (ascii, thin, double, rounded, turbo, crimson, acid,
      shade, borderless). libtui_term terminal backend with SGR state tracking.
      25-test suite with mock backend.
- [x] Guided prefix-key menu (inspired by DESQview).
      Popup on prefix key (Ctrl-A) shows available actions with bound keys.
      Generated from live binding table. Arrow keys navigate, Enter selects,
      ESC cancels. Bound keys fire immediately. Window picker submenu with
      Alt-digit accumulator for windows 10+. Blue/grey color scheme with
      bright green hotkeys, configurable via [menu] config section. Unicode
      box-drawing with ASCII fallback.
- [x] Additional menu submenus: Copy/Scrollback with repositioning to
      avoid occluding cursor. Each submenu level is an additional keystroke
      layer (prefix -> menu -> submenu -> action).
- [x] Quick Apps menu. Calculator, emoji picker, dictionary, calendar.
- [x] Attach UI modes (`lumi attach -m <mode>`, `lumi new -m <mode>`,
      or `[attach] mode = turbo` in lumi.conf).
      Screen (default), turbo (overlapping windows with mouse-driven
      move/resize/close), minimal (not yet implemented).

## Phase 7: Clean-up and Architecture

- [x] Re-architect input and keybinding processing code.
      Moved prefix-pending timeout into libkeys: timeout_pending flag,
      keys_get_timeout() / keys_timeout_expired() API with registered
      callback. Event loop in attach.c simplified to query libkeys for
      poll timeout and notify on expiry. 6-test suite for timeout API.
      20 libkeys tests total.
- [x] General-purpose timer facility in libiox.
      One-shot timers backed by a min-heap priority queue (vendored pq.h).
      iox_timer_add() / iox_timer_remove() with monotonically increasing
      IDs and CLOCK_MONOTONIC deadlines. timeout_ms parameter removed from
      iox_loop_poll() -- all timeouts go through the timer API. Prefix key
      timeout in attach.c driven by iox_timer_add/remove via sync_prefix_timer().
      3 new timer tests (fires, remove, ordering). 14 libiox tests total.
- [x] Micro-server architecture.
      One lumi-mserver process per PTY, registered in a PID-named
      subdirectory under the session directory (libsessdir). Client
      connects to N mserver instances simultaneously via mconn table,
      routes input to the focused mserver, discovers new/dead servers
      via inotify. Monolithic lumi-server and WIN_WATCH/UNWATCH/LIST/
      OUTPUT/NEW/CLOSE/SELECT IPC message types removed. IPC_MSG_ATTACH_REPLY
      added so mserver reports VT size on connect. All sub-commands
      (new, new-window, kill, detach, list, attach) ported to sessdir.
- [x] emoji app no longer seems to paste into the focused window.
      Broken after micro-server migration; focus routing via mconn may
      not be wired into the apps menu send path.
      Fixed: tkbd parser mapped CR (0x0D) to Ctrl+M instead of
      TKBD_KEY_ENTER. Apps checked for TKBD_KEY_ENTER (0x0A/LF)
      but terminals send CR for the Enter key.

## Phase 8: Release Readiness

- [x] Version number v0.1.0.
- [x] Terminal compatibility: DECSCUSR cursor shape (CSI Ps SP q) in VT
      parser, forwarded to outer terminal. Alt screen, bracketed paste,
      SGR mouse, OSC 0/2 titles already supported.
- [x] Config-driven themes (theme_cfg.c): up to 8 user themes via
      [theme.<name>] config sections with base theme inheritance.
- [x] Minimal attach mode (`lumi attach -m minimal`): no status bar,
      no mouse tracking, no popup menus. Prefix key bindings still work.

## Phase 9: Screen Mode Splits

- [x] Vertical splits (screen mode horizontal/vertical split like GNU Screen).
      libtile binary split-tree compositor with horizontal/vertical splits.
      Always-on tilemgr in screen mode -- single fullscreen window is 1 tile,
      no dual-state conditional guards. tile_sync_focus() keeps globals in
      sync with tilemgr's focused pane. Key actions: split-h, split-v,
      next-pane, prev-pane, close-pane, resize-pane (hjkl). Each pane gets
      independent resize via WIN_RESIZE. Deferred damage-flag rendering
      (need_render/need_status checked once before poll). Status line
      dirty-check and CSI K replace space padding. Per-row dirty tracking
      and blank-cell skip in renderer. 19-test libtile suite.
      Output reduced from 345KB to 29KB for new-session shell prompt.

## Phase 10: Distribution

- [x] Busybox/toybox multi-call binary with symlinks for built-in libexec
      commands. Single `lumi` binary (336KB) with dispatch table replaces 12
      individual binaries (~663KB total). Symlink dispatch via basename(argv[0])
      and subcommand dispatch via argv[1]. `make symlinks` creates lumi-* links.
- [x] Portable static binary via musl libc.
      `make CC=musl-gcc LDFLAGS=-static TARGET_TRIPLET=x86_64-linux-musl lumi`
      produces a fully static binary (330KB stripped release). Fixed
      sys/unistd.h portability issue in vendored termlib. All tests pass
      under musl. Separate build/output dirs prevent clobbering native build.
- [x] Installer script for easy online installation of binary and libexec paths.
      scripts/install.sh: downloads release binary or builds from source
      (--from-source). Detects OS/arch, creates symlinks, checks PATH.
      Supports PREFIX override (default ~/.local).
- [x] Packages for debian, fedora, arch (AUR).
      packaging/debian/build-deb.sh produces .deb from static binary.
      packaging/rpm/lumimux.spec for rpmbuild. packaging/arch/PKGBUILD
      for makepkg. All use musl static build, no runtime dependencies.
- [x] Host a debian apt repo on github: signing key in github actions,
      publish to github pages. apt-repo.yml workflow downloads .deb
      assets from releases, generates Packages/Release indices, signs
      with GPG, publishes to GitHub Pages under /apt/.

## Phase 11: Advanced Features

- [ ] State-dependent bindings: match on window title (e.g. `^bash.*`),
      toggle states (on/off with only the relevant action available).
- [ ] SIXEL pass-through demo.
- [ ] Networked client connections over QUIC reliable stream.
      Swap Unix socket for QUIC transport in libipc -- existing TLV message
      protocol works unchanged. Gets encrypted roaming (survives IP/network
      changes), TLS 1.3, 0-RTT reconnect. Server listens on both Unix socket
      (local) and QUIC (remote). Auth via pre-shared key or SSH key challenge.
- [ ] Speculative local echo prediction.
      Feed input into client's local vt_state speculatively, mark unconfirmed
      cells in renderer (dim/underline), confirm or roll back when server
      responds. Leverages the existing thick-client VT + render pipeline.
- [ ] Multi-session. one client can access multiple local and remote sessions.
      Default colors for window frames can be per-session (hinted from the
      session, but also can be set by the client any time, which updates the
      session hint if write access permitted)

## Research / Exploratory

- [ ] Investigate coroutine-style state machines for libvt parser
      (see `/home/jon/jondev/code/ot.h` fake threading macros).
- [ ] Investigate built-in container support. Daemon-less / root-less
      like podman.

## Done

- [x] Plan file with architecture and phased implementation.
- [x] Phase 0 scaffolding: all directories, module.mk, stub .c/.h,
      sub-command dispatcher, `lumi version` works end-to-end.
- [x] libutf8: UTF-8 encode/decode, version-aware rune_width(),
      gen-width-tables.sh for Unicode table generation.
- [x] Build system: modular GNUmakefile with recursive module.mk,
      exported flags, transitive library deps, `make clean-all`.
- [x] CI/CD: release workflow with `git archive` source packaging.
- [x] Phase 1: single-window PTY pass-through.
      libiox (poll loop, fd watchers, self-pipe signals, one-shot timers, 14-test suite),
      libpty (forkpty/resize/close), libtio (raw mode, 8KB buffered write).
- [x] Phase 2: VT parser + terminal buffer.
      State-table parser (VT500), vt_ops callback vtable, vt_buf row-pointer
      grid with 2000-line scrollback ring, alt screen on demand, full SGR +
      CSI + ESC support, UTF-8 via lu_utf8. 25-test suite.
- [x] Phase 3: differential renderer.
      Shadow buffer diffing, minimal CUP/SGR output, VT_ROW_DIRTY skip,
      render_full for initial/resize, render_diff for incremental updates.
      8-test suite.
- [x] Phase 4: client-server IPC + detach/attach.
      libipc with Unix domain sockets and TLV message framing (byte_order.h
      for portable byte swapping). Server daemonizes, holds vt_state for
      screen replay via vt_state_dump(). Client runs local VT + renderer.
      Commands: lumi-new, lumi-attach, lumi-list, lumi-kill, lumi-detach.
      7-test suite. 51 total tests pass.
- [x] Config file format: INI-style sections like .gitconfig / wayfire.
      Path: `$XDG_CONFIG_HOME/lumi/lumi.conf` (fallback `~/.config/lumi/`).
      Implement in libcfg.
      I have some toy ini parsers in /home/jon/jondev/code/ that might be insightful.
- [x] Structured IPC message encoding (microser tagged wire format).
      Vendored Tiny IDL compiler (gen.sh) and microser.h runtime. IDL
      defines IpcSize, IpcWinId, IpcWinEntry; generated encode/decode
      replaces hand-rolled memcpy+BE16/BE32. Tag-based fields provide
      forward compatibility across server/client version skew.
      9-test suite (including forward-compat skip of unknown fields).
- [x] New windows use $SHELL instead of repeating the lumi-new command.
      The initial command from `lumi new [cmd]` is one-time only.
- [x] Mouse support: tkbd_parse structured key/mouse events replace
      byte-at-a-time input. SGR mouse mode (1000+1006 screen, 1002+1006
      turbo). Menu/picker/apps click handling. Manual escape state
      machines removed from all input handlers.
- [x] Per-window output subscription protocol.
      WIN_WATCH/WIN_UNWATCH/WIN_OUTPUT. Server streams output only for
      watched windows, with VT state replay on subscribe. Client drives
      subscriptions -- server is policy-free about UI modes.
- [x] Client-side per-window VT state.
      client_window array with vt_state + vt_parse per server window.
      WIN_OUTPUT routed by 4-byte window ID prefix. cwin_sync reconciles
      with server's WIN_LIST on every update.
- [x] Window manager compositor (libwm).
      Z-ordered overlapping windows, painter's algorithm compositing,
      themed frame borders with title bar and close button, shadow
      rendering. wm_hit_test for mouse targeting. 18-test suite.
- [x] Mouse-driven window management.
      Drag state machine (idle/moving/resizing) with anchor-point
      tracking. Title bar drag-move, border drag-resize (clamped to
      minimum 4x2), close button click. Caller resizes PTY via
      per-window WIN_RESIZE.
- [x] Turbo attach mode (`lumi attach -m turbo`).
      Overlapping framed windows composited via libwm. Watched-set
      tracking avoids redundant replays. render_cells_diff for
      compositor output. Cascading window layout. Per-window resize
      keeps client VT and server PTY in sync with WM dimensions.
- [x] Per-window resize protocol (IPC_MSG_WIN_RESIZE 0x0207).
      IpcWinResize payload (window ID + rows + cols) replaces the
      removed global RESIZE. IDL definition in lumi.idl.
      10-test IPC suite.
- [x] Version 0.1.0.
- [x] Cursor shape (DECSCUSR) support.
      CSI Ps SP q parsed in vt_ops, stored in vt_state.cursor_shape,
      forwarded to outer terminal via sync_cursor_shape(), included
      in vt_state_dump() for screen replay. 26-test VT suite.
- [x] Minimal attach mode (`-m minimal`, `[attach] mode = minimal`).
      No status bar, no mouse tracking, no popup menus. Prefix key
      bindings still work for detach/window-switch. Content uses full
      terminal height.
- [x] Config-driven themes (theme_cfg.c, already shipped in Phase 5-6).
      Up to 8 user themes via [theme.<name>] config sections with
      base theme inheritance from 9 built-in themes.
- [x] Screen mode splits (Phase 9).
      libtile binary split-tree compositor (19-test suite). Always-on
      tilemgr -- single fullscreen window is 1 tile, eliminating
      dual-state conditional guards. Deferred damage-flag rendering
      with status line dirty-check. Output reduced 91.5% (345KB to
      29KB) for new-session shell prompt.
- [x] Distribution (Phase 10).
      Multi-call binary (336KB vs 663KB for 12 individual binaries).
      Portable static musl binary (330KB stripped). Installer script
      (scripts/install.sh). Packaging for debian/rpm/arch. GitHub
      Actions release workflow produces static binary + .deb + tarball.
      APT repo via GitHub Pages with GPG signing.
