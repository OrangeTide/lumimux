/* test_predict.c : tests for speculative local echo */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "predict.h"
#include "vt_state.h"
#include "vt_buf.h"
#include "vt_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- basic prediction tests ---- */

static void
test_predict_new_free(void)
{
	struct predict *pr;

	TEST("predict new/free");
	pr = predict_new();
	ASSERT(pr != NULL, "predict_new returned NULL");
	ASSERT(predict_pending(pr) == 0, "new predictor should be empty");
	predict_free(pr);
	PASS();
}

static void
test_predict_key_basic(void)
{
	struct predict *pr;
	struct vt_state *vt;
	struct vt_cell *c;
	int rc;

	TEST("predict_key basic");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	rc = predict_key(pr, vt, 'A', 1);
	ASSERT(rc == 1, "predict_key should succeed");
	ASSERT(predict_pending(pr) == 1, "should have 1 pending");

	c = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(c != NULL, "cell should exist");
	ASSERT(c->codepoint == 'A', "wrong codepoint");
	ASSERT(c->attrs & VT_ATTR_PREDICTED, "should have PREDICTED flag");
	ASSERT(vt->cursor_col == 0, "cursor should not advance");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_key_control(void)
{
	struct predict *pr;
	struct vt_state *vt;
	int rc;

	TEST("predict_key rejects control chars");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	rc = predict_key(pr, vt, '\n', 1);
	ASSERT(rc == 0, "should reject newline");
	rc = predict_key(pr, vt, 0x7F, 1);
	ASSERT(rc == 0, "should reject DEL");
	rc = predict_key(pr, vt, 0x01, 1);
	ASSERT(rc == 0, "should reject Ctrl-A");
	ASSERT(predict_pending(pr) == 0, "nothing should be pending");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_key_altscreen(void)
{
	struct predict *pr;
	struct vt_state *vt;
	int rc;

	TEST("predict_key skips alt screen");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	vt->modes |= VT_MODE_ALTSCREEN;
	rc = predict_key(pr, vt, 'A', 1);
	ASSERT(rc == 0, "should skip in alt screen");
	ASSERT(predict_pending(pr) == 0, "nothing pending");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_key_cursor_hidden(void)
{
	struct predict *pr;
	struct vt_state *vt;
	int rc;

	TEST("predict_key skips hidden cursor");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	vt->modes &= ~VT_MODE_CURSOR_VIS;
	rc = predict_key(pr, vt, 'A', 1);
	ASSERT(rc == 0, "should skip when cursor hidden");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

/* ---- confirm tests ---- */

static void
test_predict_confirm_match(void)
{
	struct predict *pr;
	struct vt_state *vt;
	struct vt_cell *c;

	TEST("predict_confirm matching");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	predict_key(pr, vt, 'H', 1);
	predict_key(pr, vt, 'i', 1);
	ASSERT(predict_pending(pr) == 2, "should have 2 pending");

	/* server echoes back "Hi" */
	predict_confirm(pr, vt, "Hi", 2);
	ASSERT(predict_pending(pr) == 0, "all confirmed");

	/* confirmed cells should have PREDICTED cleared */
	c = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(c != NULL, "cell exists");
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "PREDICTED should be cleared");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_confirm_mismatch(void)
{
	struct predict *pr;
	struct vt_state *vt;
	struct vt_cell *c;

	TEST("predict_confirm mismatch rollback");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	predict_key(pr, vt, 'A', 1);
	predict_key(pr, vt, 'B', 1);
	ASSERT(predict_pending(pr) == 2, "should have 2 pending");

	/* server sends "X" -- doesn't match "A" */
	predict_confirm(pr, vt, "X", 1);
	ASSERT(predict_pending(pr) == 0, "all rolled back");

	/* rolled-back cells keep content (VT parser overwrites) but
	 * PREDICTED flag is removed */
	c = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(c != NULL, "cell exists");
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "PREDICTED flag cleared");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_confirm_escape(void)
{
	struct predict *pr;
	struct vt_state *vt;

	TEST("predict_confirm rollback on escape");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	predict_key(pr, vt, 'A', 1);
	ASSERT(predict_pending(pr) == 1, "1 pending");

	/* server sends an escape sequence -- rollback */
	predict_confirm(pr, vt, "\033[H", 3);
	ASSERT(predict_pending(pr) == 0, "rolled back on ESC");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_confirm_control_skip(void)
{
	struct predict *pr;
	struct vt_state *vt;
	struct vt_cell *c;

	TEST("predict_confirm skips control chars");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	predict_key(pr, vt, 'A', 1);
	predict_key(pr, vt, 'B', 1);
	ASSERT(predict_pending(pr) == 2, "should have 2 pending");

	/* BS (0x08) should be skipped, then 'A' confirms first prediction */
	predict_confirm(pr, vt, "\bA", 2);
	ASSERT(predict_pending(pr) == 1, "1 remaining after BS + confirm");

	c = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(c != NULL, "cell exists");
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "PREDICTED cleared on A");

	/* 'B' confirms second prediction */
	predict_confirm(pr, vt, "B", 1);
	ASSERT(predict_pending(pr) == 0, "all confirmed");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

/* ---- reset test ---- */

static void
test_predict_reset(void)
{
	struct predict *pr;
	struct vt_state *vt;
	struct vt_cell *c;

	TEST("predict_reset clears all");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	predict_key(pr, vt, 'X', 1);
	predict_key(pr, vt, 'Y', 1);
	predict_key(pr, vt, 'Z', 1);
	ASSERT(predict_pending(pr) == 3, "3 pending");

	predict_reset(pr, vt);
	ASSERT(predict_pending(pr) == 0, "all cleared");

	/* cells keep content but PREDICTED flag is removed */
	c = vt_buf_cell(vt->buf, 0, 0);
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "cell 0 not predicted");
	c = vt_buf_cell(vt->buf, 0, 1);
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "cell 1 not predicted");
	c = vt_buf_cell(vt->buf, 0, 2);
	ASSERT(!(c->attrs & VT_ATTR_PREDICTED), "cell 2 not predicted");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

/* ---- wrap avoidance test ---- */

static void
test_predict_no_wrap(void)
{
	struct predict *pr;
	struct vt_state *vt;
	int rc;

	TEST("predict_key skips at line end");
	vt = vt_state_new(24, 5, 0);
	pr = predict_new();

	/* fill to column 4 (0-based, 5-col terminal) */
	predict_key(pr, vt, 'A', 1);
	predict_key(pr, vt, 'B', 1);
	predict_key(pr, vt, 'C', 1);
	predict_key(pr, vt, 'D', 1);
	predict_key(pr, vt, 'E', 1);
	ASSERT(vt->cursor_col == 0, "cursor should not advance");

	/* next char would wrap -- should be skipped */
	rc = predict_key(pr, vt, 'F', 1);
	ASSERT(rc == 0, "should skip wrap");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

static void
test_predict_echo_off(void)
{
	struct predict *pr;
	struct vt_state *vt;
	int rc;

	TEST("predict_key suppressed when echo off");
	vt = vt_state_new(24, 80, 0);
	pr = predict_new();

	/* echo off -- should suppress */
	predict_set_echo(pr, 0);
	rc = predict_key(pr, vt, 'p', 1);
	ASSERT(rc == 0, "should skip with echo off");
	ASSERT(predict_pending(pr) == 0, "nothing pending");

	/* echo back on -- should predict again */
	predict_set_echo(pr, 1);
	rc = predict_key(pr, vt, 'p', 1);
	ASSERT(rc == 1, "should predict with echo on");
	ASSERT(predict_pending(pr) == 1, "one pending");

	predict_free(pr);
	vt_state_free(vt);
	PASS();
}

int
main(void)
{
	printf("test_predict:\n");

	test_predict_new_free();
	test_predict_key_basic();
	test_predict_key_control();
	test_predict_key_altscreen();
	test_predict_key_cursor_hidden();
	test_predict_confirm_match();
	test_predict_confirm_mismatch();
	test_predict_confirm_escape();
	test_predict_confirm_control_skip();
	test_predict_reset();
	test_predict_no_wrap();
	test_predict_echo_off();

	printf("%d tests, %d failures\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
