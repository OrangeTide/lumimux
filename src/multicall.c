/* multicall.c : busybox-style multi-call dispatcher */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"
#include "path.h"

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct {
	const char	*name;
	int		(*fn)(int, char **);
} commands[] = {
	{ "attach",	cmd_attach_main },
	{ "attr",	cmd_attr_main },
	{ "detach",	cmd_detach_main },
	{ "kill",	cmd_kill_main },
	{ "list",	cmd_list_main },
	{ "mserver",	cmd_mserver_main },
	{ "new",	cmd_new_main },
	{ "new-window",	cmd_new_window_main },
	{ "proxy",	cmd_proxy_main },
	{ "reload",	cmd_reload_main },
	{ "send-input",	cmd_send_input_main },
	{ "send-keys",	cmd_send_keys_main },
	{ "splash",	cmd_splash_main },
	{ "version",	cmd_version_main },
};

#define NCMDS	(sizeof(commands) / sizeof(commands[0]))

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi <command> [<args>]\n"
	    "\n"
	    "Commands:\n"
	    "  new [-Ad] [-s name]   Create a session and attach\n"
	    "  attach                Attach to an existing session\n"
	    "  attr                  Get/set mserver attributes\n"
	    "  detach                Detach a client from its session\n"
	    "  list                  List active sessions\n"
	    "  kill                  Terminate a session\n"
	    "  new-window            Create a window in a session\n"
	    "  reload                Reload server configuration\n"
	    "  send-keys             Send keystrokes to a session\n"
	    "  send-input            Send raw input to a pane\n"
	    "  splash                Display splash screen\n"
	    "  version               Print version information\n");
}

static int (*
lookup(const char *name))(int, char **)
{
	size_t i;

	for (i = 0; i < NCMDS; i++) {
		if (strcmp(name, commands[i].name) == 0)
			return commands[i].fn;
	}
	return NULL;
}

/* try to find an external lumi-<cmd> binary by searching:
 * 1. same directory as /proc/self/exe
 * 2. LUMI_LIBEXEC_PATH
 * 3. PATH */
static char *
find_external(const char *name)
{
	char self[PATH_MAX], *dir, *copy, *result;
	ssize_t n;
	int len;

	n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		self[n] = '\0';
		copy = strdup(self);
		if (copy) {
			dir = dirname(copy);
			result = malloc(PATH_MAX);
			if (result) {
				len = snprintf(result, PATH_MAX,
				    "%s/%s", dir, name);
				if (len > 0 && len < PATH_MAX &&
				    access(result, X_OK) == 0) {
					free(copy);
					return result;
				}
				free(result);
			}
			free(copy);
		}
	}

	{
		char *libexec = getenv("LUMI_LIBEXEC_PATH");

		if (libexec) {
			result = path_search(libexec, name);
			if (result)
				return result;
		}
	}

	return path_search(getenv("PATH"), name);
}

int
multicall_exec_cmd(const char *cmd, int argc, char **argv)
{
	int (*fn)(int, char **);
	char binname[64];

	fn = lookup(cmd);
	if (!fn)
		return -1;

	/* try external binary first */
	if (snprintf(binname, sizeof(binname), "lumi-%s", cmd)
	    < (int)sizeof(binname)) {
		char *path = find_external(binname);

		if (path) {
			argv[0] = binname;
			execv(path, argv);
			/* exec failed -- fall through to built-in */
			free(path);
		}
	}

	optind = 1;
	return fn(argc, argv);
}

int
main(int argc, char **argv)
{
	char *base, *copy;
	const char *cmd;
	int (*fn)(int, char **);

	/* symlink dispatch: check if argv[0] is "lumi-<cmd>" */
	copy = strdup(argv[0] ? argv[0] : "lumi");
	base = basename(copy);
	if (strncmp(base, "lumi-", 5) == 0) {
		fn = lookup(base + 5);
		if (fn) {
			free(copy);
			return fn(argc, argv);
		}
	}
	free(copy);

	/* subcommand dispatch: lumi <cmd> [args...] */
	if (argc < 2) {
		usage();
		return 1;
	}

	cmd = argv[1];

	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage();
		return 0;
	}

	fn = lookup(cmd);
	if (fn)
		return fn(argc - 1, argv + 1);

	fprintf(stderr, "lumi: '%s' is not a lumi command\n", cmd);
	return 1;
}
