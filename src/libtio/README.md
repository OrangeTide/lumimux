# libtio

Terminal I/O layer. `tio_raw` puts a file descriptor into raw mode (disabling
line buffering and echo), and `tio_restore` reverts it. `tio_write` and
`tio_flush` provide buffered output to the controlling terminal.
