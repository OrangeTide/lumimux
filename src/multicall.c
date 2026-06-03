/* multicall.c : busybox-style multi-call dispatcher */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"
#include "lu_umask.h"
#include "path.h"

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

static char *proc_argv_base;
static size_t proc_argv_size;

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

/* Rewrite the original argv buffer so ps(1) reflects the active command. */
static void
update_proctitle(int argc, char **argv)
{
	size_t off;
	int i;

	if (!proc_argv_base || proc_argv_size == 0)
		return;

	memset(proc_argv_base, 0, proc_argv_size);
	off = 0;
	for (i = 0; i < argc && argv[i]; i++) {
		size_t len = strlen(argv[i]);

		if (off + len >= proc_argv_size)
			break;
		memcpy(proc_argv_base + off, argv[i], len);
		off += len + 1;
	}

#ifdef __linux__
	if (argc > 0 && argv[0])
		prctl(PR_SET_NAME, (unsigned long)argv[0], 0, 0, 0);
#endif
}

static int
get_exe_path(char *buf, size_t bufsz)
{
#ifdef __APPLE__
	uint32_t sz = (uint32_t)bufsz;
	char raw[PATH_MAX];

	if (_NSGetExecutablePath(raw, &sz) != 0)
		return -1;
	if (!realpath(raw, buf))
		return -1;
	return 0;
#else
	ssize_t n;

	n = readlink("/proc/self/exe", buf, bufsz - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return 0;
#endif
}

/* try to find an external lumi-<cmd> binary by searching:
 * 1. same directory as our executable
 * 2. LUMI_LIBEXEC_PATH
 * 3. PATH */
static char *
find_external(const char *name)
{
	char self[PATH_MAX], *dir, *copy, *result;
	int len;

	if (get_exe_path(self, sizeof(self)) == 0) {
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

	if (snprintf(binname, sizeof(binname), "lumi-%s", cmd)
	    >= (int)sizeof(binname))
		binname[0] = '\0';

	/* try external binary first */
	if (binname[0]) {
		char *path = find_external(binname);

		if (path) {
			argv[0] = binname;
			execv(path, argv);
			/* exec failed -- fall through to built-in */
			free(path);
		}
	}

	if (binname[0])
		argv[0] = binname;
	update_proctitle(argc, argv);

	optind = 1;
	return fn(argc, argv);
}

int
main(int argc, char **argv)
{
	char *base, *copy;
	const char *cmd;
	int (*fn)(int, char **);

	lu_umask_save();

	/* save argv buffer extent for update_proctitle() */
	if (argc > 0 && argv[0]) {
		proc_argv_base = argv[0];
		proc_argv_size = (size_t)(argv[argc - 1] +
		    strlen(argv[argc - 1]) + 1 - argv[0]);
	}

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
