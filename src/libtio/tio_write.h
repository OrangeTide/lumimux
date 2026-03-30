/* tio_write.h : buffered output to controlling terminal */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TIO_WRITE_H
#define TIO_WRITE_H

#include <stddef.h>

int tio_write(int fd, const char *buf, size_t len);
int tio_flush(int fd);

#endif /* TIO_WRITE_H */
