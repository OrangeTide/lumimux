/* attr_store.h : transactional key=value attribute store */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef ATTR_STORE_H
#define ATTR_STORE_H

#include <stdint.h>

#define ATTR_MAX_KEYS		64
#define ATTR_MAX_KEY_LEN	128
#define ATTR_MAX_VALUE_LEN	1024
#define ATTR_TXN_MAX		4
#define ATTR_TXN_KEYS_MAX	16

struct attr_entry {
	char		key[ATTR_MAX_KEY_LEN];
	char		value[ATTR_MAX_VALUE_LEN];
	uint64_t	gen;
};

struct attr_txn_touch {
	char		key[ATTR_MAX_KEY_LEN];
	uint64_t	gen;
};

struct attr_txn_write {
	char		key[ATTR_MAX_KEY_LEN];
	char		value[ATTR_MAX_VALUE_LEN];
	int		is_delete;
};

struct attr_txn {
	int			in_use;
	int			client_fd;
	uint32_t		txn_id;
	struct attr_txn_touch	read_set[ATTR_TXN_KEYS_MAX];
	int			read_count;
	struct attr_txn_write	write_set[ATTR_TXN_KEYS_MAX];
	int			write_count;
};

struct attr_store {
	struct attr_entry	entries[ATTR_MAX_KEYS];
	int			count;
	uint64_t		next_gen;
	struct attr_txn		txns[ATTR_TXN_MAX];
	uint32_t		next_txn_id;
};

/* initialize an empty store */
void attr_store_init(struct attr_store *st);

/* transaction lifecycle -- returns 0 on success, -1 on error */
int  attr_store_txn_begin(struct attr_store *st, int client_fd,
	 uint32_t *out_txn_id);
int  attr_store_txn_commit(struct attr_store *st, uint32_t txn_id);
void attr_store_txn_rollback(struct attr_store *st, uint32_t txn_id);
void attr_store_txn_rollback_client(struct attr_store *st, int client_fd);

/* operations within a transaction */
int  attr_store_set(struct attr_store *st, uint32_t txn_id,
	 const char *key, const char *value);
int  attr_store_delete(struct attr_store *st, uint32_t txn_id,
	 const char *key);
int  attr_store_get(struct attr_store *st, uint32_t txn_id,
	 const char *key, char *out_value, int value_max);
int  attr_store_list(struct attr_store *st, uint32_t txn_id,
	 char *out_buf, int buf_max);

/* persistence */
int  attr_store_save(const struct attr_store *st, const char *path);
int  attr_store_load(struct attr_store *st, const char *path);

#endif /* ATTR_STORE_H */
