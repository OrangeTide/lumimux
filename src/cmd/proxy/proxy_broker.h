/* proxy_broker.h : path helpers for the local cross-user broker (12-b) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef PROXY_BROKER_H
#define PROXY_BROKER_H

#include <stddef.h>

/* All three build off /tmp/lumi-broker-<uid>, deliberately outside
 * $XDG_RUNTIME_DIR/lumi (which stays 0700): a broker endpoint must be
 * reachable by a foreign uid's directory traversal all the way down from a
 * world-traversable ancestor, and /tmp already is one. Each returns 0 with
 * buf filled, or -1 if the result would not fit in bufsz. */

int proxy_broker_dir(char *buf, size_t bufsz);
int proxy_broker_sock_path(const char *session, char *buf, size_t bufsz);
int proxy_broker_pidfile_path(const char *session, char *buf, size_t bufsz);

#endif /* PROXY_BROKER_H */
