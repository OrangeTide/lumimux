/* ipc_attr.h : synchronous client-side attribute IPC helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef IPC_ATTR_H
#define IPC_ATTR_H

#include <stdint.h>

/* All functions send a request and block for the response.
 * Returns 0 on success, -1 on error (including server-side errors). */

int ipc_attr_txn_begin(int fd, uint32_t *out_txn_id);
int ipc_attr_txn_commit(int fd, uint32_t txn_id);
int ipc_attr_txn_rollback(int fd, uint32_t txn_id);

int ipc_attr_set(int fd, uint32_t txn_id, const char *key,
    const char *value);
int ipc_attr_delete(int fd, uint32_t txn_id, const char *key);
int ipc_attr_get(int fd, uint32_t txn_id, const char *key,
    char *out_value, int value_max);
int ipc_attr_list(int fd, uint32_t txn_id, char *out_buf, int buf_max);

#endif /* IPC_ATTR_H */
