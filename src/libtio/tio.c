/* tio.c : terminal I/O -- raw mode and termios management */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "tio.h"

#include <termios.h>
#include <unistd.h>

static struct termios tio_saved;
static int tio_saved_valid;

int
tio_raw(int fd)
{
	struct termios t;

	if (!isatty(fd))
		return -1;

	if (tcgetattr(fd, &tio_saved) < 0)
		return -1;
	tio_saved_valid = 1;

	t = tio_saved;
	t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	t.c_oflag &= ~(OPOST);
	t.c_cflag &= ~(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSAFLUSH, &t) < 0)
		return -1;

	return 0;
}

int
tio_restore(int fd)
{
	if (!tio_saved_valid)
		return -1;

	if (tcsetattr(fd, TCSAFLUSH, &tio_saved) < 0)
		return -1;

	tio_saved_valid = 0;
	return 0;
}
