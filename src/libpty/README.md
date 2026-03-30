# libpty

Pseudo-terminal allocation and management. `pty_open` creates a PTY pair,
forks a child process running the given shell, and returns the master fd.
`pty_resize` sends a window-size change to the slave, and `pty_close` tears
down the master side.
