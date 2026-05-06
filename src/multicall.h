/* multicall.h : entry points for busybox-style multi-call binary */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef MULTICALL_H
#define MULTICALL_H

int cmd_attach_main(int argc, char **argv);
int cmd_attr_main(int argc, char **argv);
int cmd_detach_main(int argc, char **argv);
int cmd_kill_main(int argc, char **argv);
int cmd_list_main(int argc, char **argv);
int cmd_mserver_main(int argc, char **argv);
int cmd_new_main(int argc, char **argv);
int cmd_new_window_main(int argc, char **argv);
int cmd_proxy_main(int argc, char **argv);
int cmd_reload_main(int argc, char **argv);
int cmd_send_input_main(int argc, char **argv);
int cmd_send_keys_main(int argc, char **argv);
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
