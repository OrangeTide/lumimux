/* test_attr_store.c : tests for transactional attribute store */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "attr_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* ---- init ---- */

static void
test_init(void)
{
	struct attr_store st;

	TEST("init zeroes store");
	attr_store_init(&st);
	ASSERT(st.count == 0, "count should be 0");
	ASSERT(st.next_gen == 1, "next_gen should be 1");
	ASSERT(st.next_txn_id == 1, "next_txn_id should be 1");
	PASS();
}

/* ---- basic set/get ---- */

static void
test_set_get(void)
{
	struct attr_store st;
	uint32_t txn;
	char val[256];

	TEST("set then get within same txn");
	attr_store_init(&st);
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	ASSERT(attr_store_set(&st, txn, "color", "red") == 0, "set failed");
	ASSERT(attr_store_get(&st, txn, "color", val, (int)sizeof(val)) == 0,
	    "get failed");
	ASSERT(strcmp(val, "red") == 0, "value mismatch");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	/* read back after commit */
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	ASSERT(attr_store_get(&st, txn, "color", val, (int)sizeof(val)) == 0,
	    "get after commit failed");
	ASSERT(strcmp(val, "red") == 0, "value after commit mismatch");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- delete ---- */

static void
test_delete(void)
{
	struct attr_store st;
	uint32_t txn;
	char val[256];

	TEST("delete removes key");
	attr_store_init(&st);

	/* create key */
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "fruit", "apple");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	/* delete it */
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	ASSERT(attr_store_delete(&st, txn, "fruit") == 0, "delete failed");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit2 failed");

	/* verify gone */
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin3 failed");
	ASSERT(attr_store_get(&st, txn, "fruit", val, (int)sizeof(val)) < 0,
	    "get should fail after delete");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- list ---- */

static void
test_list(void)
{
	struct attr_store st;
	uint32_t txn;
	char buf[4096];

	TEST("list returns all keys");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "a", "1");
	attr_store_set(&st, txn, "b", "2");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	ASSERT(attr_store_list(&st, txn, buf, (int)sizeof(buf)) == 0,
	    "list failed");
	ASSERT(strstr(buf, "a=1\n") != NULL, "missing a=1");
	ASSERT(strstr(buf, "b=2\n") != NULL, "missing b=2");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- list shows uncommitted writes ---- */

static void
test_list_uncommitted(void)
{
	struct attr_store st;
	uint32_t txn;
	char buf[4096];

	TEST("list includes uncommitted writes");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "x", "99");
	ASSERT(attr_store_list(&st, txn, buf, (int)sizeof(buf)) == 0,
	    "list failed");
	ASSERT(strstr(buf, "x=99\n") != NULL, "missing uncommitted x=99");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- rollback discards changes ---- */

static void
test_rollback(void)
{
	struct attr_store st;
	uint32_t txn;
	char val[256];

	TEST("rollback discards writes");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "key", "val");
	attr_store_txn_rollback(&st, txn);

	/* should not exist */
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	ASSERT(attr_store_get(&st, txn, "key", val, (int)sizeof(val)) < 0,
	    "key should not exist after rollback");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- OCC conflict detection ---- */

static void
test_conflict(void)
{
	struct attr_store st;
	uint32_t txn1, txn2;

	TEST("OCC conflict on concurrent write");
	attr_store_init(&st);

	/* create a key */
	ASSERT(attr_store_txn_begin(&st, 10, &txn1) == 0, "begin failed");
	attr_store_set(&st, txn1, "counter", "0");
	ASSERT(attr_store_txn_commit(&st, txn1) == 0, "commit failed");

	/* txn1 reads counter */
	ASSERT(attr_store_txn_begin(&st, 10, &txn1) == 0, "begin1 failed");
	{
		char val[256];

		attr_store_get(&st, txn1, "counter", val, (int)sizeof(val));
	}
	attr_store_set(&st, txn1, "counter", "1");

	/* txn2 also reads and writes counter, commits first */
	ASSERT(attr_store_txn_begin(&st, 11, &txn2) == 0, "begin2 failed");
	{
		char val[256];

		attr_store_get(&st, txn2, "counter", val, (int)sizeof(val));
	}
	attr_store_set(&st, txn2, "counter", "2");
	ASSERT(attr_store_txn_commit(&st, txn2) == 0, "txn2 commit failed");

	/* txn1 should fail -- generation changed */
	ASSERT(attr_store_txn_commit(&st, txn1) < 0,
	    "txn1 commit should fail due to conflict");
	PASS();
}

/* ---- no conflict on disjoint keys ---- */

static void
test_no_conflict_disjoint(void)
{
	struct attr_store st;
	uint32_t txn1, txn2;

	TEST("no conflict on disjoint keys");
	attr_store_init(&st);

	/* create two keys */
	ASSERT(attr_store_txn_begin(&st, 10, &txn1) == 0, "begin failed");
	attr_store_set(&st, txn1, "a", "1");
	attr_store_set(&st, txn1, "b", "2");
	ASSERT(attr_store_txn_commit(&st, txn1) == 0, "commit failed");

	/* txn1 writes a, txn2 writes b */
	ASSERT(attr_store_txn_begin(&st, 10, &txn1) == 0, "begin1 failed");
	attr_store_set(&st, txn1, "a", "10");

	ASSERT(attr_store_txn_begin(&st, 11, &txn2) == 0, "begin2 failed");
	attr_store_set(&st, txn2, "b", "20");
	ASSERT(attr_store_txn_commit(&st, txn2) == 0, "txn2 commit failed");

	ASSERT(attr_store_txn_commit(&st, txn1) == 0,
	    "txn1 should succeed on disjoint key");
	PASS();
}

/* ---- client disconnect rolls back all txns ---- */

static void
test_rollback_client(void)
{
	struct attr_store st;
	uint32_t txn1, txn2;
	char val[256];

	TEST("rollback_client cleans up all txns for fd");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 42, &txn1) == 0, "begin1 failed");
	ASSERT(attr_store_txn_begin(&st, 42, &txn2) == 0, "begin2 failed");
	attr_store_set(&st, txn1, "k1", "v1");
	attr_store_set(&st, txn2, "k2", "v2");

	attr_store_txn_rollback_client(&st, 42);

	/* both txns should be gone -- operations on them should fail */
	ASSERT(attr_store_get(&st, txn1, "k1", val, (int)sizeof(val)) < 0,
	    "txn1 should be invalid");
	ASSERT(attr_store_get(&st, txn2, "k2", val, (int)sizeof(val)) < 0,
	    "txn2 should be invalid");
	PASS();
}

/* ---- txn pool exhaustion ---- */

static void
test_txn_pool_full(void)
{
	struct attr_store st;
	uint32_t txns[ATTR_TXN_MAX];
	uint32_t extra;
	int i;

	TEST("txn pool full returns error");
	attr_store_init(&st);

	for (i = 0; i < ATTR_TXN_MAX; i++)
		ASSERT(attr_store_txn_begin(&st, 10 + i, &txns[i]) == 0,
		    "begin should succeed");

	ASSERT(attr_store_txn_begin(&st, 99, &extra) < 0,
	    "begin should fail when pool full");

	/* free one and retry */
	attr_store_txn_rollback(&st, txns[0]);
	ASSERT(attr_store_txn_begin(&st, 99, &extra) == 0,
	    "begin should succeed after rollback");
	attr_store_txn_rollback(&st, extra);

	for (i = 1; i < ATTR_TXN_MAX; i++)
		attr_store_txn_rollback(&st, txns[i]);
	PASS();
}

/* ---- persistence ---- */

static void
test_save_load(void)
{
	struct attr_store st, st2;
	uint32_t txn;
	char val[256];
	char path[] = "/tmp/test_attr_XXXXXX";
	int fd;

	TEST("save and load round-trip");
	fd = mkstemp(path);
	ASSERT(fd >= 0, "mkstemp failed");
	close(fd);

	attr_store_init(&st);
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "host", "localhost");
	attr_store_set(&st, txn, "port", "8080");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	ASSERT(attr_store_save(&st, path) == 0, "save failed");

	attr_store_init(&st2);
	ASSERT(attr_store_load(&st2, path) == 0, "load failed");
	ASSERT(st2.count == 2, "loaded count mismatch");

	ASSERT(attr_store_txn_begin(&st2, 10, &txn) == 0, "begin2 failed");
	ASSERT(attr_store_get(&st2, txn, "host", val, (int)sizeof(val)) == 0,
	    "get host failed");
	ASSERT(strcmp(val, "localhost") == 0, "host value mismatch");
	ASSERT(attr_store_get(&st2, txn, "port", val, (int)sizeof(val)) == 0,
	    "get port failed");
	ASSERT(strcmp(val, "8080") == 0, "port value mismatch");
	attr_store_txn_rollback(&st2, txn);

	unlink(path);
	PASS();
}

/* ---- load nonexistent file is OK ---- */

static void
test_load_missing(void)
{
	struct attr_store st;

	TEST("load nonexistent file returns OK");
	attr_store_init(&st);
	ASSERT(attr_store_load(&st, "/tmp/no_such_attr_file_ever") == 0,
	    "load of missing file should succeed");
	ASSERT(st.count == 0, "count should remain 0");
	PASS();
}

/* ---- overwrite existing key ---- */

static void
test_overwrite(void)
{
	struct attr_store st;
	uint32_t txn;
	char val[256];

	TEST("overwrite existing key");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "k", "old");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	attr_store_set(&st, txn, "k", "new");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit2 failed");

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin3 failed");
	ASSERT(attr_store_get(&st, txn, "k", val, (int)sizeof(val)) == 0,
	    "get failed");
	ASSERT(strcmp(val, "new") == 0, "should have new value");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- validation ---- */

static void
test_empty_key_rejected(void)
{
	struct attr_store st;
	uint32_t txn;

	TEST("empty key rejected");
	attr_store_init(&st);
	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	ASSERT(attr_store_set(&st, txn, "", "val") < 0,
	    "empty key should be rejected");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

/* ---- list hides deleted keys ---- */

static void
test_list_hides_deleted(void)
{
	struct attr_store st;
	uint32_t txn;
	char buf[4096];

	TEST("list hides deleted keys");
	attr_store_init(&st);

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin failed");
	attr_store_set(&st, txn, "keep", "yes");
	attr_store_set(&st, txn, "drop", "no");
	ASSERT(attr_store_txn_commit(&st, txn) == 0, "commit failed");

	ASSERT(attr_store_txn_begin(&st, 10, &txn) == 0, "begin2 failed");
	attr_store_delete(&st, txn, "drop");
	ASSERT(attr_store_list(&st, txn, buf, (int)sizeof(buf)) == 0,
	    "list failed");
	ASSERT(strstr(buf, "keep=yes\n") != NULL, "missing keep=yes");
	ASSERT(strstr(buf, "drop") == NULL, "drop should be hidden");
	attr_store_txn_rollback(&st, txn);
	PASS();
}

int
main(void)
{
	printf("test_attr_store:\n");

	test_init();
	test_set_get();
	test_delete();
	test_list();
	test_list_uncommitted();
	test_rollback();
	test_conflict();
	test_no_conflict_disjoint();
	test_rollback_client();
	test_txn_pool_full();
	test_save_load();
	test_load_missing();
	test_overwrite();
	test_empty_key_rejected();
	test_list_hides_deleted();

	printf("test_attr_store: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count ? 1 : 0;
}
