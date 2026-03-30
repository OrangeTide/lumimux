/* tio_write.c : buffered output to controlling terminal */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tio_write.h"

#include <errno.h>
#include <unistd.h>

#define TIO_BUFSZ 8192

static char tio_buf[TIO_BUFSZ];
static size_t tio_buf_used;

static int
write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

int
tio_write(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		size_t avail = TIO_BUFSZ - tio_buf_used;
		size_t chunk = len < avail ? len : avail;

		__builtin_memcpy(tio_buf + tio_buf_used, buf, chunk);
		tio_buf_used += chunk;
		buf += chunk;
		len -= chunk;

		if (tio_buf_used == TIO_BUFSZ) {
			if (tio_flush(fd) < 0)
				return -1;
		}
	}
	return 0;
}

int
tio_flush(int fd)
{
	int rc = 0;

	if (tio_buf_used > 0) {
		rc = write_all(fd, tio_buf, tio_buf_used);
		tio_buf_used = 0;
	}
	return rc;
}
