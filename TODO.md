# TODO

- [x] when spawning internal commands, update argv[0] so that ps can see the
      difference between lumi and an mserver session.

- [ ] Networked client connections over QUIC reliable stream.
      See doc/FUTURE.md#11c for the design.

- [ ] BUG: selecting text and changing windows leave the selection area behind. it should be cleared when: changing focus, entering/leaving scroll back mode, moving windows.

- [ ] add input dialog to run simple directives like the ctrl-A : in screen
  - setenv VAR="value to set"
    - add or clear environment used when creating new windows.
  - title New Title Name
    - change current window's title
