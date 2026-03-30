/* ipc_attr.c : synchronous client-side attribute IPC helpers */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "ipc_attr.h"

#include "ipc_msg.h"
#include "lumi_msg.h"

#include <stdio.h>
#include <string.h>

#define ERR (-1)
#define OK (0)

/* send a txn_id-only message using IpcAttrKey with empty key */
static int
send_txn_msg(int fd, uint32_t type, uint32_t txn_id)
{
	struct ipc_attr_key msg;
	uint8_t buf[32];
	int n;

	msg.txn_id = txn_id;
	msg.key = "";
	msg.key_len = 0;
	n = ipc_attr_key_encode(&msg, buf, (int)sizeof(buf));
	if (n < 0)
		return ERR;
	return ipc_msg_send(fd, type, buf, (uint32_t)n);
}

/* wait for a response and check it matches the expected type.
 * returns 0 on expected type, -1 on error or unexpected type. */
static int
recv_expect(int fd, uint32_t expect, void *buf, size_t bufsz,
    uint32_t *out_len)
{
	uint32_t type;

	if (ipc_msg_recv(fd, &type, buf, bufsz, out_len) != 0)
		return ERR;
	if (type == IPC_MSG_ERROR)
		return ERR;
	if (type != expect)
		return ERR;
	return OK;
}

int
ipc_attr_txn_begin(int fd, uint32_t *out_txn_id)
{
	struct ipc_attr_txn_ok tok;
	uint8_t buf[32];
	uint32_t len;

	if (ipc_msg_send_empty(fd, IPC_MSG_ATTR_TXN_BEGIN) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_TXN_OK, buf, sizeof(buf),
	    &len) < 0)
		return ERR;
	if (ipc_attr_txn_ok_decode(&tok, buf, (int)len) < 0)
		return ERR;
	*out_txn_id = tok.txn_id;
	return OK;
}

int
ipc_attr_txn_commit(int fd, uint32_t txn_id)
{
	uint8_t buf[32];
	uint32_t len;

	if (send_txn_msg(fd, IPC_MSG_ATTR_TXN_COMMIT, txn_id) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_TXN_OK, buf, sizeof(buf),
	    &len) < 0)
		return ERR;
	return OK;
}

int
ipc_attr_txn_rollback(int fd, uint32_t txn_id)
{
	uint8_t buf[32];
	uint32_t len;

	if (send_txn_msg(fd, IPC_MSG_ATTR_TXN_ROLLBACK, txn_id) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_TXN_OK, buf, sizeof(buf),
	    &len) < 0)
		return ERR;
	return OK;
}

int
ipc_attr_set(int fd, uint32_t txn_id, const char *key, const char *value)
{
	struct ipc_attr_kv msg;
	uint8_t enc[4096];
	uint8_t buf[32];
	uint32_t len;
	int n;

	msg.txn_id = txn_id;
	msg.key = key;
	msg.key_len = (uint16_t)strlen(key);
	msg.value = value;
	msg.value_len = (uint16_t)strlen(value);
	n = ipc_attr_kv_encode(&msg, enc, (int)sizeof(enc));
	if (n < 0)
		return ERR;
	if (ipc_msg_send(fd, IPC_MSG_ATTR_SET, enc, (uint32_t)n) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_OK, buf, sizeof(buf), &len) < 0)
		return ERR;
	return OK;
}

int
ipc_attr_delete(int fd, uint32_t txn_id, const char *key)
{
	struct ipc_attr_key msg;
	uint8_t enc[256];
	uint8_t buf[32];
	uint32_t len;
	int n;

	msg.txn_id = txn_id;
	msg.key = key;
	msg.key_len = (uint16_t)strlen(key);
	n = ipc_attr_key_encode(&msg, enc, (int)sizeof(enc));
	if (n < 0)
		return ERR;
	if (ipc_msg_send(fd, IPC_MSG_ATTR_DELETE, enc, (uint32_t)n) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_OK, buf, sizeof(buf), &len) < 0)
		return ERR;
	return OK;
}

int
ipc_attr_get(int fd, uint32_t txn_id, const char *key,
    char *out_value, int value_max)
{
	struct ipc_attr_key msg;
	struct ipc_attr_kv resp;
	uint8_t enc[256];
	uint8_t buf[4096];
	uint32_t len;
	int n;

	msg.txn_id = txn_id;
	msg.key = key;
	msg.key_len = (uint16_t)strlen(key);
	n = ipc_attr_key_encode(&msg, enc, (int)sizeof(enc));
	if (n < 0)
		return ERR;
	if (ipc_msg_send(fd, IPC_MSG_ATTR_GET, enc, (uint32_t)n) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_VALUE, buf, sizeof(buf),
	    &len) < 0)
		return ERR;
	if (ipc_attr_kv_decode(&resp, buf, (int)len) < 0)
		return ERR;
	snprintf(out_value, (size_t)value_max, "%.*s",
	    (int)resp.value_len, resp.value);
	return OK;
}

int
ipc_attr_list(int fd, uint32_t txn_id, char *out_buf, int buf_max)
{
	struct ipc_attr_entries resp;
	uint8_t buf[IPC_MAX_PAYLOAD];
	uint32_t len;

	if (send_txn_msg(fd, IPC_MSG_ATTR_LIST, txn_id) < 0)
		return ERR;
	if (recv_expect(fd, IPC_MSG_ATTR_ENTRIES, buf, sizeof(buf),
	    &len) < 0)
		return ERR;
	if (ipc_attr_entries_decode(&resp, buf, (int)len) < 0)
		return ERR;
	snprintf(out_buf, (size_t)buf_max, "%.*s",
	    (int)resp.entries_len, resp.entries);
	return OK;
}
