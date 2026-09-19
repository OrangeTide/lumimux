/* test_text.c : tests for libtext */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "text.h"

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

/* Compare line `line` of t against the C string want. */
static int
line_is(struct text *t, size_t line, const char *want)
{
	size_t len = 0;
	const char *s = text_line(t, line, &len);

	if (!s)
		return 0;
	return len == strlen(want) && memcmp(s, want, len) == 0;
}

static void
test_new_is_one_empty_line(void)
{
	struct text *t = text_new();

	TEST("new buffer is one empty line");
	ASSERT(t != NULL, "text_new failed");
	ASSERT(text_lines(t) == 1, "expected 1 line");
	ASSERT(text_line_len(t, 0) == 0, "expected empty line");
	ASSERT(!text_dirty(t), "new buffer should be clean");
	text_free(t);
	PASS();
}

static void
test_insert(void)
{
	struct text *t = text_new();

	TEST("insert bytes");
	text_insert(t, 0, 0, "hello", 5);
	text_insert(t, 0, 5, "!", 1);
	text_insert(t, 0, 0, ">>", 2);
	ASSERT(line_is(t, 0, ">>hello!"), "unexpected line contents");
	ASSERT(text_dirty(t), "insert should set dirty");
	text_free(t);
	PASS();
}

static void
test_insert_out_of_range(void)
{
	struct text *t = text_new();

	TEST("insert out of range fails");
	ASSERT(text_insert(t, 0, 1, "x", 1) == -1, "col past end");
	ASSERT(text_insert(t, 5, 0, "x", 1) == -1, "line past end");
	ASSERT(!text_dirty(t), "failed insert must not dirty");
	text_free(t);
	PASS();
}

static void
test_delete(void)
{
	struct text *t = text_new();

	TEST("delete bytes, clamped");
	text_insert(t, 0, 0, "hello world", 11);
	text_delete(t, 0, 5, 6);		/* remove " world" */
	ASSERT(line_is(t, 0, "hello"), "delete mid failed");
	text_delete(t, 0, 3, 100);		/* clamp to end */
	ASSERT(line_is(t, 0, "hel"), "delete clamp failed");
	text_free(t);
	PASS();
}

static void
test_split_and_join(void)
{
	struct text *t = text_new();

	TEST("split then join round-trips");
	text_insert(t, 0, 0, "abcdef", 6);
	text_split(t, 0, 3);
	ASSERT(text_lines(t) == 2, "expected 2 lines after split");
	ASSERT(line_is(t, 0, "abc"), "head wrong");
	ASSERT(line_is(t, 1, "def"), "tail wrong");
	text_join(t, 0);
	ASSERT(text_lines(t) == 1, "expected 1 line after join");
	ASSERT(line_is(t, 0, "abcdef"), "join wrong");
	text_free(t);
	PASS();
}

static void
test_join_last_fails(void)
{
	struct text *t = text_new();

	TEST("join on last line fails");
	text_insert(t, 0, 0, "only", 4);
	ASSERT(text_join(t, 0) == -1, "join past end should fail");
	text_free(t);
	PASS();
}

static void
test_split_at_ends(void)
{
	struct text *t = text_new();

	TEST("split at column 0 and at end");
	text_insert(t, 0, 0, "line", 4);
	text_split(t, 0, 0);			/* blank line before */
	ASSERT(line_is(t, 0, ""), "leading blank wrong");
	ASSERT(line_is(t, 1, "line"), "moved line wrong");
	text_split(t, 1, 4);			/* blank line after */
	ASSERT(line_is(t, 2, ""), "trailing blank wrong");
	ASSERT(text_lines(t) == 3, "expected 3 lines");
	text_free(t);
	PASS();
}

static int
write_file(const char *path, const char *data, size_t n)
{
	FILE *fp = fopen(path, "wb");

	if (!fp)
		return -1;
	if (n && fwrite(data, 1, n, fp) != n) {
		fclose(fp);
		return -1;
	}
	return fclose(fp);
}

static void
test_load_save_trailing_newline(void)
{
	struct text *t = text_new();
	char tmpl[] = "/tmp/text_testXXXXXX";
	int fd = mkstemp(tmpl);
	char out[64];
	FILE *fp;
	size_t n;

	TEST("load/save preserves trailing newline");
	ASSERT(fd >= 0, "mkstemp failed");
	close(fd);

	ASSERT(write_file(tmpl, "a\nbb\n", 5) == 0, "write failed");
	ASSERT(text_load(t, tmpl) == 0, "load failed");
	ASSERT(text_lines(t) == 2, "expected 2 lines");
	ASSERT(line_is(t, 0, "a") && line_is(t, 1, "bb"), "lines wrong");
	ASSERT(!text_dirty(t), "load should clear dirty");

	ASSERT(text_save(t, tmpl) == 0, "save failed");
	fp = fopen(tmpl, "rb");
	ASSERT(fp != NULL, "reopen failed");
	n = fread(out, 1, sizeof(out), fp);
	fclose(fp);
	ASSERT(n == 5 && memcmp(out, "a\nbb\n", 5) == 0, "roundtrip wrong");

	unlink(tmpl);
	text_free(t);
	PASS();
}

static void
test_load_save_no_trailing_newline(void)
{
	struct text *t = text_new();
	char tmpl[] = "/tmp/text_testXXXXXX";
	int fd = mkstemp(tmpl);
	char out[64];
	FILE *fp;
	size_t n;

	TEST("load/save preserves missing trailing newline");
	ASSERT(fd >= 0, "mkstemp failed");
	close(fd);

	ASSERT(write_file(tmpl, "x\ny", 3) == 0, "write failed");
	ASSERT(text_load(t, tmpl) == 0, "load failed");
	ASSERT(text_lines(t) == 2, "expected 2 lines");
	ASSERT(text_save(t, tmpl) == 0, "save failed");

	fp = fopen(tmpl, "rb");
	ASSERT(fp != NULL, "reopen failed");
	n = fread(out, 1, sizeof(out), fp);
	fclose(fp);
	ASSERT(n == 3 && memcmp(out, "x\ny", 3) == 0, "roundtrip wrong");

	unlink(tmpl);
	text_free(t);
	PASS();
}

static void
test_load_empty(void)
{
	struct text *t = text_new();
	char tmpl[] = "/tmp/text_testXXXXXX";
	int fd = mkstemp(tmpl);

	TEST("load empty file yields one empty line");
	ASSERT(fd >= 0, "mkstemp failed");
	close(fd);
	ASSERT(write_file(tmpl, "", 0) == 0, "write failed");
	ASSERT(text_load(t, tmpl) == 0, "load failed");
	ASSERT(text_lines(t) == 1, "expected 1 line");
	ASSERT(text_line_len(t, 0) == 0, "expected empty line");
	unlink(tmpl);
	text_free(t);
	PASS();
}

static void
test_undo_redo_insert(void)
{
	struct text *t = text_new();
	size_t line = 9, col = 9;

	TEST("undo and redo an insertion");
	text_insert(t, 0, 0, "hi", 2);
	ASSERT(text_can_undo(t), "should have undo");
	ASSERT(text_undo(t, &line, &col) == 0, "undo failed");
	ASSERT(line_is(t, 0, ""), "undo did not remove text");
	ASSERT(line == 0 && col == 0, "undo cursor wrong");
	ASSERT(text_can_redo(t), "should have redo");
	ASSERT(text_redo(t, &line, &col) == 0, "redo failed");
	ASSERT(line_is(t, 0, "hi"), "redo did not restore text");
	ASSERT(col == 2, "redo cursor wrong");
	text_free(t);
	PASS();
}

static void
test_undo_coalesces_typing(void)
{
	struct text *t = text_new();

	TEST("consecutive inserts coalesce into one undo");
	text_insert(t, 0, 0, "a", 1);
	text_insert(t, 0, 1, "b", 1);
	text_insert(t, 0, 2, "c", 1);
	ASSERT(line_is(t, 0, "abc"), "insert wrong");
	ASSERT(text_undo(t, NULL, NULL) == 0, "undo failed");
	ASSERT(line_is(t, 0, ""), "one undo should clear the run");
	ASSERT(!text_can_undo(t), "run should be a single step");
	text_free(t);
	PASS();
}

static void
test_undo_boundary_splits_runs(void)
{
	struct text *t = text_new();

	TEST("a boundary breaks the coalescing run");
	text_insert(t, 0, 0, "ab", 2);
	text_undo_boundary(t);
	text_insert(t, 0, 2, "cd", 2);
	ASSERT(text_undo(t, NULL, NULL) == 0, "first undo failed");
	ASSERT(line_is(t, 0, "ab"), "should undo only the second run");
	ASSERT(text_undo(t, NULL, NULL) == 0, "second undo failed");
	ASSERT(line_is(t, 0, ""), "should undo the first run");
	text_free(t);
	PASS();
}

static void
test_undo_split_join(void)
{
	struct text *t = text_new();

	TEST("undo restores split and join");
	text_insert(t, 0, 0, "abcdef", 6);
	text_undo_boundary(t);
	text_split(t, 0, 3);
	ASSERT(text_lines(t) == 2, "split failed");
	text_undo(t, NULL, NULL);
	ASSERT(text_lines(t) == 1 && line_is(t, 0, "abcdef"),
	    "undo split failed");
	text_free(t);
	PASS();
}

static void
test_redo_cleared_by_edit(void)
{
	struct text *t = text_new();

	TEST("a new edit clears the redo stack");
	text_insert(t, 0, 0, "x", 1);
	text_undo(t, NULL, NULL);
	ASSERT(text_can_redo(t), "redo expected");
	text_insert(t, 0, 0, "y", 1);
	ASSERT(!text_can_redo(t), "new edit should clear redo");
	text_free(t);
	PASS();
}

int
main(void)
{
	test_new_is_one_empty_line();
	test_insert();
	test_insert_out_of_range();
	test_delete();
	test_split_and_join();
	test_join_last_fails();
	test_split_at_ends();
	test_load_save_trailing_newline();
	test_load_save_no_trailing_newline();
	test_load_empty();
	test_undo_redo_insert();
	test_undo_coalesces_typing();
	test_undo_boundary_splits_runs();
	test_undo_split_join();
	test_redo_cleared_by_edit();

	printf("test_text: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
