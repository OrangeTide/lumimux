# TODO

## Bugs

- [ ] in GNOME terminal: the numbered lists display in Claude CLI while inside lumi show up as blank, instead of:
    ```
    1. Something
    2. Something
    3. Something
    ```
    I see:
    ```
                  Something
                  Something
                  Something
    ```

- [ ] Detach (ctrl-A d) of lumi while in GNOME terminal consistently segfaults. possibly in other terminals.

# DONE

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

