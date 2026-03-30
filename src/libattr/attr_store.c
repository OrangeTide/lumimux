/* attr_store.c : transactional key=value attribute store */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "attr_store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERR (-1)
#define OK (0)

/* ---- internal helpers ---- */

static struct attr_entry *
find_entry(struct attr_store *st, const char *key)
{
	int i;

	for (i = 0; i < st->count; i++) {
		if (strcmp(st->entries[i].key, key) == 0)
			return &st->entries[i];
	}
	return NULL;
}

static struct attr_txn *
find_txn(struct attr_store *st, uint32_t txn_id)
{
	int i;

	for (i = 0; i < ATTR_TXN_MAX; i++) {
		if (st->txns[i].in_use && st->txns[i].txn_id == txn_id)
			return &st->txns[i];
	}
	return NULL;
}

/* record key's current generation in the read set (first touch only) */
static int
txn_touch_read(struct attr_txn *txn, const char *key, uint64_t gen)
{
	int i;

	for (i = 0; i < txn->read_count; i++) {
		if (strcmp(txn->read_set[i].key, key) == 0)
			return OK;
	}
	if (txn->read_count >= ATTR_TXN_KEYS_MAX)
		return ERR;
	snprintf(txn->read_set[txn->read_count].key,
	    ATTR_MAX_KEY_LEN, "%s", key);
	txn->read_set[txn->read_count].gen = gen;
	txn->read_count++;
	return OK;
}

/* find a key in the write set, or NULL */
static struct attr_txn_write *
txn_find_write(struct attr_txn *txn, const char *key)
{
	int i;

	for (i = 0; i < txn->write_count; i++) {
		if (strcmp(txn->write_set[i].key, key) == 0)
			return &txn->write_set[i];
	}
	return NULL;
}

static void
txn_clear(struct attr_txn *txn)
{
	txn->in_use = 0;
	txn->client_fd = -1;
	txn->txn_id = 0;
	txn->read_count = 0;
	txn->write_count = 0;
}

/* compare function for qsort of attr_entry by key */
static int
entry_cmp(const void *a, const void *b)
{
	const struct attr_entry *ea = a;
	const struct attr_entry *eb = b;

	return strcmp(ea->key, eb->key);
}

/* ---- public API ---- */

void
attr_store_init(struct attr_store *st)
{
	int i;

	memset(st, 0, sizeof(*st));
	st->next_gen = 1;
	st->next_txn_id = 1;
	for (i = 0; i < ATTR_TXN_MAX; i++)
		st->txns[i].client_fd = -1;
}

int
attr_store_txn_begin(struct attr_store *st, int client_fd,
    uint32_t *out_txn_id)
{
	int i;

	for (i = 0; i < ATTR_TXN_MAX; i++) {
		if (!st->txns[i].in_use) {
			txn_clear(&st->txns[i]);
			st->txns[i].in_use = 1;
			st->txns[i].client_fd = client_fd;
			st->txns[i].txn_id = st->next_txn_id++;
			*out_txn_id = st->txns[i].txn_id;
			return OK;
		}
	}
	return ERR;
}

int
attr_store_txn_commit(struct attr_store *st, uint32_t txn_id)
{
	struct attr_txn *txn;
	int i;

	txn = find_txn(st, txn_id);
	if (!txn)
		return ERR;

	/* conflict detection: check all read-set generations */
	for (i = 0; i < txn->read_count; i++) {
		struct attr_entry *e;

		e = find_entry(st, txn->read_set[i].key);
		if (e) {
			if (e->gen != txn->read_set[i].gen) {
				txn_clear(txn);
				return ERR;
			}
		} else {
			/* key was deleted since we read it */
			if (txn->read_set[i].gen != 0) {
				txn_clear(txn);
				return ERR;
			}
		}
	}

	/* apply write set */
	for (i = 0; i < txn->write_count; i++) {
		struct attr_txn_write *w = &txn->write_set[i];
		struct attr_entry *e;

		e = find_entry(st, w->key);
		if (w->is_delete) {
			if (e) {
				/* remove by swapping with last */
				*e = st->entries[st->count - 1];
				st->count--;
				/* gen is lost, but next_gen keeps
				 * incrementing so a re-create of the
				 * same key gets a new gen */
			}
		} else {
			if (e) {
				snprintf(e->value, ATTR_MAX_VALUE_LEN,
				    "%s", w->value);
				e->gen = st->next_gen++;
			} else {
				if (st->count >= ATTR_MAX_KEYS) {
					txn_clear(txn);
					return ERR;
				}
				e = &st->entries[st->count++];
				snprintf(e->key, ATTR_MAX_KEY_LEN,
				    "%s", w->key);
				snprintf(e->value, ATTR_MAX_VALUE_LEN,
				    "%s", w->value);
				e->gen = st->next_gen++;
			}
		}
	}

	txn_clear(txn);
	return OK;
}

void
attr_store_txn_rollback(struct attr_store *st, uint32_t txn_id)
{
	struct attr_txn *txn;

	txn = find_txn(st, txn_id);
	if (txn)
		txn_clear(txn);
}

void
attr_store_txn_rollback_client(struct attr_store *st, int client_fd)
{
	int i;

	for (i = 0; i < ATTR_TXN_MAX; i++) {
		if (st->txns[i].in_use &&
		    st->txns[i].client_fd == client_fd)
			txn_clear(&st->txns[i]);
	}
}

int
attr_store_set(struct attr_store *st, uint32_t txn_id,
    const char *key, const char *value)
{
	struct attr_txn *txn;
	struct attr_txn_write *w;
	struct attr_entry *e;
	uint64_t gen;

	txn = find_txn(st, txn_id);
	if (!txn)
		return ERR;
	if (strlen(key) == 0 || strlen(key) >= ATTR_MAX_KEY_LEN)
		return ERR;
	if (strlen(value) >= ATTR_MAX_VALUE_LEN)
		return ERR;

	/* record current gen in read set */
	e = find_entry(st, key);
	gen = e ? e->gen : 0;
	if (txn_touch_read(txn, key, gen) < 0)
		return ERR;

	/* stage in write set (update existing or append) */
	w = txn_find_write(txn, key);
	if (!w) {
		if (txn->write_count >= ATTR_TXN_KEYS_MAX)
			return ERR;
		w = &txn->write_set[txn->write_count++];
		snprintf(w->key, ATTR_MAX_KEY_LEN, "%s", key);
	}
	snprintf(w->value, ATTR_MAX_VALUE_LEN, "%s", value);
	w->is_delete = 0;
	return OK;
}

int
attr_store_delete(struct attr_store *st, uint32_t txn_id, const char *key)
{
	struct attr_txn *txn;
	struct attr_txn_write *w;
	struct attr_entry *e;
	uint64_t gen;

	txn = find_txn(st, txn_id);
	if (!txn)
		return ERR;

	e = find_entry(st, key);
	gen = e ? e->gen : 0;
	if (txn_touch_read(txn, key, gen) < 0)
		return ERR;

	w = txn_find_write(txn, key);
	if (!w) {
		if (txn->write_count >= ATTR_TXN_KEYS_MAX)
			return ERR;
		w = &txn->write_set[txn->write_count++];
		snprintf(w->key, ATTR_MAX_KEY_LEN, "%s", key);
	}
	w->value[0] = '\0';
	w->is_delete = 1;
	return OK;
}

int
attr_store_get(struct attr_store *st, uint32_t txn_id,
    const char *key, char *out_value, int value_max)
{
	struct attr_txn *txn;
	struct attr_txn_write *w;
	struct attr_entry *e;
	uint64_t gen;

	txn = find_txn(st, txn_id);
	if (!txn)
		return ERR;

	e = find_entry(st, key);
	gen = e ? e->gen : 0;
	if (txn_touch_read(txn, key, gen) < 0)
		return ERR;

	/* check write set first (read-your-writes) */
	w = txn_find_write(txn, key);
	if (w) {
		if (w->is_delete)
			return ERR;
		snprintf(out_value, (size_t)value_max, "%s", w->value);
		return OK;
	}

	if (!e)
		return ERR;
	snprintf(out_value, (size_t)value_max, "%s", e->value);
	return OK;
}

int
attr_store_list(struct attr_store *st, uint32_t txn_id,
    char *out_buf, int buf_max)
{
	struct attr_txn *txn;
	int i, pos = 0, n;

	txn = find_txn(st, txn_id);
	if (!txn)
		return ERR;

	out_buf[0] = '\0';

	/* emit committed entries, overlaying with write set */
	for (i = 0; i < st->count; i++) {
		struct attr_txn_write *w;

		w = txn_find_write(txn, st->entries[i].key);
		if (w && w->is_delete)
			continue;
		if (w) {
			n = snprintf(out_buf + pos, (size_t)(buf_max - pos),
			    "%s=%s\n", w->key, w->value);
		} else {
			n = snprintf(out_buf + pos, (size_t)(buf_max - pos),
			    "%s=%s\n",
			    st->entries[i].key, st->entries[i].value);
		}
		if (n < 0 || pos + n >= buf_max)
			break;
		pos += n;
	}

	/* emit write-set entries for new keys (not in committed store) */
	for (i = 0; i < txn->write_count; i++) {
		struct attr_txn_write *w = &txn->write_set[i];

		if (w->is_delete)
			continue;
		if (find_entry(st, w->key))
			continue;
		n = snprintf(out_buf + pos, (size_t)(buf_max - pos),
		    "%s=%s\n", w->key, w->value);
		if (n < 0 || pos + n >= buf_max)
			break;
		pos += n;
	}

	return OK;
}

/* ---- persistence ---- */

int
attr_store_save(const struct attr_store *st, const char *path)
{
	char tmp[PATH_MAX];
	FILE *f;
	struct attr_entry sorted[ATTR_MAX_KEYS];
	int i;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return ERR;

	f = fopen(tmp, "w");
	if (!f)
		return ERR;

	/* sort entries by key for deterministic output */
	memcpy(sorted, st->entries,
	    (size_t)st->count * sizeof(sorted[0]));
	qsort(sorted, (size_t)st->count, sizeof(sorted[0]), entry_cmp);

	for (i = 0; i < st->count; i++)
		fprintf(f, "%s=%s\n", sorted[i].key, sorted[i].value);

	if (fclose(f) != 0) {
		unlink(tmp);
		return ERR;
	}

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return ERR;
	}
	return OK;
}

int
attr_store_load(struct attr_store *st, const char *path)
{
	FILE *f;
	char line[ATTR_MAX_KEY_LEN + ATTR_MAX_VALUE_LEN + 4];
	char *eq;

	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT)
			return OK;
		return ERR;
	}

	while (fgets(line, (int)sizeof(line), f)) {
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] == '\n')
			line[--len] = '\0';
		if (len == 0)
			continue;

		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';

		if (st->count >= ATTR_MAX_KEYS)
			break;

		snprintf(st->entries[st->count].key,
		    ATTR_MAX_KEY_LEN, "%.*s",
		    ATTR_MAX_KEY_LEN - 1, line);
		snprintf(st->entries[st->count].value,
		    ATTR_MAX_VALUE_LEN, "%s", eq + 1);
		st->entries[st->count].gen = st->next_gen++;
		st->count++;
	}

	fclose(f);
	return OK;
}
