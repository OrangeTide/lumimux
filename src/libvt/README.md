# libvt

Virtual terminal emulation. Provides a grid buffer with scrollback
(`vt_buf`), a cell structure carrying codepoint, foreground/background color,
and attributes such as bold, underline, italic, blink, reverse, and undercurl
(`vt_cell`), and a state-machine parser for VT escape sequences (`vt_parse`).
