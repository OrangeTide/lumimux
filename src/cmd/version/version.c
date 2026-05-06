/* version.c : print version information */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"
#include "version.h"

#include <stdio.h>

int
cmd_version_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("lumi version %s\n", LUMI_VERSION);
	return 0;
}
