# TODO

## Bugs

# DONE

- [x] in GNOME terminal: the numbered lists display in Claude CLI while inside
      lumi show up as blank, instead of:
      ```
      1. Something
      2. Something
      3. Something
      ```
      it showed:
      ```
                    Something
                    Something
                    Something
      ```
      Claude CLI queries the terminal's background/foreground color via
      OSC 10/11 to pick a contrasting color for things like list markers.
      lumi's renderer is cell-based, not a raw passthrough, so the query
      had nowhere to go: `osc_passthru()` forwarded only one-way
      notification OSCs (9/99/777), and a query with no answer left the
      asking program guessing a background that didn't match, making the
      marker digits invisible. `osc_passthru()` now also forwards OSC
      10/11 *queries* (not color-set requests) to the outer terminal, and
      a new stdin-side OSC observer (`stdin_osc_parser`/
      `stdin_osc_reply()`) matches the terminal's reply against the
      pending query and routes it back to the asking window's PTY as
      input, via `mconn_ipc_send(..., IPC_MSG_INPUT, ...)`. Verified both
      directions against a live session: a real query printed by a child
      process is forwarded out lumi's stdout, and a simulated terminal
      reply written to lumi's stdin is routed back and delivered into
      that same child's PTY (confirmed by its own tty echoing the
      delivered bytes back out as window output).

- [x] Detach (ctrl-A d) of lumi while in GNOME terminal consistently segfaults.
      Also reproduced in iTerm2 and kitty -- not terminal-specific.
      `cmd_attach_main()`'s exit path freed `tilemgr` before calling
      `cwin_free_all()`, which still walks every client window and calls
      `tile_forget_vt()` on the now-freed tile manager: a heap-use-after-free
      on every clean exit with at least one window open (i.e. always).
      Already fixed on this branch in a34cc27, found earlier reviewing
      Phase 12 under ASan; the crash the user hit was from a separate,
      out-of-date checkout that predates that fix.

- [x] BUG: reattaching came up with a blank screen that no redraw would fix.
      Ctrl-A l did nothing and only switching windows brought the content
      back. The saved screen layout stores each pane as an index into the
      session's window order, and a pane whose window was not in that order
      was written out as index -1, then restored as a pane with no VT, which
      composites as blank. The state fed itself: the restored empty pane was
      saved as -1 again on the next detach. Unresolvable panes are now
      rejected on import (a split collapses onto its surviving side) and
      refused on export, and the focus that `tile_focus()` actually applied
      is read back so the taskbar cannot highlight a window no pane shows.

- [x] BUG: the tab bar came back with bare window numbers after a reattach,
      showing names again only once something set a title. `sync_vt_title()`
      copied an empty VT title over the name read from the session
      directory on the first output after attaching. Each window now keeps
      the name it was discovered under as a fallback.

- [x] Networked client connections over a reliable channel (netchan-v2
      reliable UDP, not QUIC). See doc/FUTURE.md#11c for the design.
      All five steps landed. Step 1: the ipc_transport seam, attach client
      and proxy ported onto it. Step 2: src/libnet/ extracted (netchan +
      nc_udp + nc_crypto + Monocypher) as the lu_net library. Step 3:
      loss/reorder stress test, which found and fixed a netchan
      reliable-retransmit bug (since taken upstream on re-vendor). Step 4:
      the netchan ipc_transport impl, its non-blocking drain, the
      lumi-net-proxy bridge, and the attach -n client. Step 5: encryption
      (per-session PSK, X25519 + XChaCha20-Poly1305), cross-host bootstrap
      over ssh (attach -n host:session), and roaming (address migration,
      plus hands-free auto-roam on a network change).

- [x] creating new windows highlights the wrong window tab (the first one). closing a window does not return the order correctly.

- [x] keep windows sorted in most recently used order so that when closing a window we bounce back to the last used window. but next/prev will still go in numeric order. bound to Ctrl-A Ctrl-A (GNU Screen's "other" key) rather than TAB, which stays Next pane.

- [x] fix colon (:) line editing. it redraws and shows a repeated characters.

- [x] change the :title command to override the client title. and use :title with no arguments to clear the override.

- [x] elide titles in the taskbar when there is not enough space for tabs. cut the max width down in half until all tabs fit, but stop if a tab's max width would be less than 8.

- [x] fix strobing of pop-up menu (backdrop repaint flushed under the overlay).

- [x] add a renumber feature, a colon(:) like GNU screen. this can leverage the ,. commands to bump a window number left/right.

- [x] clear mouse selection after pasting. erase selection when window scrolls.

- [x] support keyboard selection, cut, pasted while in scrollback mode. similar to GNU screen with hjkl movement, block / line selection modes, etc.

- [x] pass through notification pop-ups. programs like Claude can display notifications to some terminals like Kitty.

- [x] when spawning internal commands, update argv[0] so that ps can see the
      difference between lumi and an mserver session.

- [x] BUG: selecting text and changing windows leave the selection area behind. it should be cleared when: changing focus, entering/leaving scroll back mode, moving windows.

- [x] add input dialog to run simple directives like the ctrl-A : in screen
  - setenv VAR="value to set"
    - add or clear environment used when creating new windows.
  - title New Title Name
    - change current window's title

- [x] fix Mac build error:
  ```
  GNUmakefile:1033: *** circular _LIBS dependency detected:   .  Stop.
  ```

