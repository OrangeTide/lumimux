/* reload.c : tell server to reload configuration */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include <stdio.h>

int
cmd_reload_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "lumi-reload: not yet implemented\n");
	return 1;
}
