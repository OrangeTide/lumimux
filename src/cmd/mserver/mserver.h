/* mserver.h : lumi-mserver entry point and tunables */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef MSERVER_H
#define MSERVER_H

#include <stddef.h>

/* how many clients may be attached to one window at once */
#define MSERVER_CLIENT_MAX	8

/* A client whose backlog crosses the hard limit, or stops draining for
 * this many seconds, is dropped rather than throttling the clients that
 * are keeping up.  These are variables rather than constants so a test can
 * lower them before the server starts and provoke a drop without pushing
 * megabytes through a PTY.
 */
extern size_t mserver_outq_hard_limit;
extern int mserver_stall_secs;

int cmd_mserver_main(int argc, char **argv);

#endif /* MSERVER_H */
