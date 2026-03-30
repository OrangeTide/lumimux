/* tio.h : terminal I/O -- raw mode and termios management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TIO_H
#define TIO_H

int tio_raw(int fd);
int tio_restore(int fd);

#endif /* TIO_H */
