/* lumi_msg.h : generated from lumi.idl -- do not edit */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef LUMI_MSG_H
#define LUMI_MSG_H

#include <stdint.h>

struct ipc_size {
	uint16_t	rows;
	uint16_t	cols;
};

int ipc_size_encode(const struct ipc_size *msg, uint8_t *buf, int len);
int ipc_size_decode(struct ipc_size *msg, const uint8_t *buf, int len);

struct ipc_win_id {
	uint32_t	id;
};

int ipc_win_id_encode(const struct ipc_win_id *msg, uint8_t *buf, int len);
int ipc_win_id_decode(struct ipc_win_id *msg, const uint8_t *buf, int len);

struct ipc_win_entry {
	uint32_t	id;
	uint8_t	flags;
	const char	*title;
	uint16_t	 title_len;
};

int ipc_win_entry_encode(const struct ipc_win_entry *msg, uint8_t *buf, int len);
int ipc_win_entry_decode(struct ipc_win_entry *msg, const uint8_t *buf, int len);

struct ipc_win_resize {
	uint32_t	id;
	uint16_t	rows;
	uint16_t	cols;
};

int ipc_win_resize_encode(const struct ipc_win_resize *msg, uint8_t *buf, int len);
int ipc_win_resize_decode(struct ipc_win_resize *msg, const uint8_t *buf, int len);

struct ipc_attr_txn_ok {
	uint32_t	txn_id;
};

int ipc_attr_txn_ok_encode(const struct ipc_attr_txn_ok *msg, uint8_t *buf, int len);
int ipc_attr_txn_ok_decode(struct ipc_attr_txn_ok *msg, const uint8_t *buf, int len);

struct ipc_attr_kv {
	uint32_t	txn_id;
	const char	*key;
	uint16_t	 key_len;
	const char	*value;
	uint16_t	 value_len;
};

int ipc_attr_kv_encode(const struct ipc_attr_kv *msg, uint8_t *buf, int len);
int ipc_attr_kv_decode(struct ipc_attr_kv *msg, const uint8_t *buf, int len);

struct ipc_attr_key {
	uint32_t	txn_id;
	const char	*key;
	uint16_t	 key_len;
};

int ipc_attr_key_encode(const struct ipc_attr_key *msg, uint8_t *buf, int len);
int ipc_attr_key_decode(struct ipc_attr_key *msg, const uint8_t *buf, int len);

struct ipc_attr_entries {
	const char	*entries;
	uint16_t	 entries_len;
};

int ipc_attr_entries_encode(const struct ipc_attr_entries *msg, uint8_t *buf, int len);
int ipc_attr_entries_decode(struct ipc_attr_entries *msg, const uint8_t *buf, int len);

#endif /* LUMI_MSG_H */
