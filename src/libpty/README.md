# libpty

Pseudo-terminal allocation and management. `pty_open` creates a PTY pair
sized to the given rows and cols, forks a child process running the given
shell, and returns the master fd. `pty_resize` sends a later window-size
change to the slave, and `pty_close` tears down the master side.
