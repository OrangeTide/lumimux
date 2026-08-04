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

/* PTY reads pause once a client's backlog reaches the high water mark and
 * resume once it falls back to the low one, which is what keeps a client
 * that is merely slow from ever reaching the hard limit above.
 *
 * A test that lowers the hard limit has to lower these with it. The order
 * is what carries the meaning: low < high < hard. Leave the high mark
 * above the hard limit and the throttle can never engage first, so a
 * client that is keeping up is dropped the moment the machine is too busy
 * to let it drain, which is a property of the load rather than of the code
 * under test.
 */
extern size_t mserver_outq_high_water;
extern size_t mserver_outq_low_water;

int cmd_mserver_main(int argc, char **argv);

#endif /* MSERVER_H */
