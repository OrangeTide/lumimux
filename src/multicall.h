/* multicall.h : entry points for busybox-style multi-call binary */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef MULTICALL_H
#define MULTICALL_H

#include <stddef.h>		/* size_t */
#include <sys/types.h>		/* pid_t */

int cmd_attach_main(int argc, char **argv);
int cmd_attr_main(int argc, char **argv);
int cmd_basic_main(int argc, char **argv);
int cmd_detach_main(int argc, char **argv);
int cmd_edit_main(int argc, char **argv);
int cmd_files_main(int argc, char **argv);
int cmd_kill_main(int argc, char **argv);
int cmd_list_main(int argc, char **argv);
int cmd_mserver_main(int argc, char **argv);
int cmd_new_main(int argc, char **argv);
int cmd_new_window_main(int argc, char **argv);
int cmd_net_proxy_main(int argc, char **argv);
int cmd_net_keygen_main(int argc, char **argv);
int cmd_net_passwd_main(int argc, char **argv);
int cmd_proxy_main(int argc, char **argv);
int cmd_reload_main(int argc, char **argv);
int cmd_send_input_main(int argc, char **argv);
int cmd_send_keys_main(int argc, char **argv);

/* Result of lu_send_input(). */
enum lu_send_result {
	LU_SEND_OK = 0,
	LU_SEND_NO_SESSION,	/* session directory not found */
	LU_SEND_NO_TARGET,	/* no suitable window to send to */
	LU_SEND_READONLY,	/* another client holds the keyboard */
	LU_SEND_ERROR,		/* connect, handshake, or send failure */
};

/* Inject len bytes into a session window as one atomic input run. target > 0
 * selects that server pid, 0 the focused window, and -1 a non-focused
 * ("other") window, honoring $LUMI_SEND_TARGET when it names one. Shared by
 * lumi-send-input and the in-session send bindings of lumi edit and basic. */
enum lu_send_result lu_send_input(const char *session, pid_t target,
    const char *data, size_t len);

/* Resolve a 0-based window number (as shown in the tab bar) to its server pid,
 * or 0 when the number is out of range or names an empty slot. */
pid_t lu_window_pid(const char *session, int index);

int cmd_share_main(int argc, char **argv);
int cmd_splash_main(int argc, char **argv);
int cmd_version_main(int argc, char **argv);

/** Try to exec an external lumi-<cmd> binary, falling back to the
 *  built-in entry point if no external binary is found.
 *
 *  Search order for external binary:
 *    1. same directory as the running executable (via /proc/self/exe)
 *    2. LUMI_LIBEXEC_PATH
 *    3. PATH
 *
 *  If exec succeeds this function does not return.  Otherwise it resets
 *  optind and calls the built-in cmd_*_main(), returning its exit code.
 *  Returns -1 if cmd is not a known subcommand. */
int multicall_exec_cmd(const char *cmd, int argc, char **argv);

#endif
