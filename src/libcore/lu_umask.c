/* lu_umask.c : restrictive umask for runtime directory files */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "lu_umask.h"

#include <sys/stat.h>

static mode_t orig_umask;

void
lu_umask_save(void)
{
	orig_umask = umask(077);
}

void
lu_umask_restore(void)
{
	umask(orig_umask);
}
