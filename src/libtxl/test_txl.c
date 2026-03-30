/* test_txl.c : unit tests for terminal translation engine */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "txl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count;
static int fail_count;

#define CHECK(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", \
		    __FILE__, __LINE__, (msg)); \
		fail_count++; \
	} \
} while (0)

/* ---- tests ---- */

static void
test_new_free(void)
{
	struct txl *t;

	/* NULL term means use $TERM; should not crash even if
	 * terminfo is missing -- falls back to ANSI. */
	t = txl_new(NULL);
	CHECK(t != NULL, "txl_new(NULL) should succeed");
	txl_free(t);

	/* explicit terminal name */
	t = txl_new("xterm-256color");
	CHECK(t != NULL, "txl_new(xterm-256color) should succeed");
	txl_free(t);

	/* bogus terminal name should still succeed (ANSI fallback) */
	t = txl_new("no-such-terminal-type");
	CHECK(t != NULL, "txl_new(bogus) should succeed with fallbacks");
	txl_free(t);
}

static void
test_simple_caps(void)
{
	struct txl *t;
	const char *s;

	t = txl_new(NULL);
	CHECK(t != NULL, "txl_new");
	if (!t)
		return;

	/* all simple caps should return non-NULL (fallbacks at minimum) */
	s = txl_str(t, TXL_CIVIS);
	CHECK(s != NULL, "civis should be available");

	s = txl_str(t, TXL_CNORM);
	CHECK(s != NULL, "cnorm should be available");

	s = txl_str(t, TXL_CLEAR);
	CHECK(s != NULL, "clear should be available");

	s = txl_str(t, TXL_SGR0);
	CHECK(s != NULL, "sgr0 should be available");

	s = txl_str(t, TXL_SMCUP);
	CHECK(s != NULL, "smcup should be available");

	s = txl_str(t, TXL_RMCUP);
	CHECK(s != NULL, "rmcup should be available");

	s = txl_str(t, TXL_SC);
	CHECK(s != NULL, "sc should be available");

	s = txl_str(t, TXL_RC);
	CHECK(s != NULL, "rc should be available");

	s = txl_str(t, TXL_BOLD);
	CHECK(s != NULL, "bold should be available");

	s = txl_str(t, TXL_REV);
	CHECK(s != NULL, "rev should be available");

	/* out of range */
	s = txl_str(t, -1);
	CHECK(s == NULL, "negative cap index returns NULL");
	s = txl_str(t, TXL_NCAP);
	CHECK(s == NULL, "out of range cap index returns NULL");

	txl_free(t);
}

static void
test_cup(void)
{
	struct txl *t;
	char buf[64];
	int len;

	t = txl_new(NULL);
	CHECK(t != NULL, "txl_new");
	if (!t)
		return;

	len = txl_cup(t, buf, sizeof(buf), 0, 0);
	CHECK(len > 0, "cup(0,0) should produce output");
	/* the sequence should position to row 1, col 1 (1-based) */
	CHECK(strstr(buf, "1") != NULL, "cup should contain row/col");

	len = txl_cup(t, buf, sizeof(buf), 23, 79);
	CHECK(len > 0, "cup(23,79) should produce output");

	/* tiny buffer should return 0 */
	len = txl_cup(t, buf, 2, 0, 0);
	CHECK(len == 0, "cup with tiny buffer returns 0");

	txl_free(t);
}

static void
test_setaf_setab(void)
{
	struct txl *t;
	char buf[64];
	int len;

	t = txl_new(NULL);
	CHECK(t != NULL, "txl_new");
	if (!t)
		return;

	/* basic color */
	len = txl_setaf(t, buf, sizeof(buf), 1);
	CHECK(len > 0, "setaf(1) should produce output");

	len = txl_setab(t, buf, sizeof(buf), 4);
	CHECK(len > 0, "setab(4) should produce output");

	/* 256-color */
	len = txl_setaf(t, buf, sizeof(buf), 200);
	CHECK(len > 0, "setaf(200) should produce output");

	len = txl_setab(t, buf, sizeof(buf), 200);
	CHECK(len > 0, "setab(200) should produce output");

	txl_free(t);
}

static void
test_decset_decrst(void)
{
	char buf[32];
	int len;

	/* DECCKM (mode 1) */
	len = txl_decset(buf, sizeof(buf), 1);
	CHECK(len > 0, "decset(1) should produce output");
	CHECK(strcmp(buf, "\033[?1h") == 0, "decset(1) = ESC[?1h");

	len = txl_decrst(buf, sizeof(buf), 1);
	CHECK(len > 0, "decrst(1) should produce output");
	CHECK(strcmp(buf, "\033[?1l") == 0, "decrst(1) = ESC[?1l");

	/* bracketed paste (mode 2004) */
	len = txl_decset(buf, sizeof(buf), 2004);
	CHECK(strcmp(buf, "\033[?2004h") == 0, "decset(2004) = ESC[?2004h");

	len = txl_decrst(buf, sizeof(buf), 2004);
	CHECK(strcmp(buf, "\033[?2004l") == 0, "decrst(2004) = ESC[?2004l");
}

static void
test_colors(void)
{
	struct txl *t;

	t = txl_new(NULL);
	CHECK(t != NULL, "txl_new");
	if (!t)
		return;

	CHECK(txl_colors(t) > 0, "colors should be > 0");

	txl_free(t);
}

static void
test_fallback_caps(void)
{
	struct txl *t;
	const char *s;

	/* with a bogus terminal, everything should be ANSI fallback */
	t = txl_new("no-such-terminal-xyz");
	CHECK(t != NULL, "txl_new(bogus)");
	if (!t)
		return;

	s = txl_str(t, TXL_CIVIS);
	CHECK(s != NULL && strcmp(s, "\033[?25l") == 0,
	    "fallback civis should be ANSI");

	s = txl_str(t, TXL_SGR0);
	CHECK(s != NULL && strcmp(s, "\033[0m") == 0,
	    "fallback sgr0 should be ANSI");

	s = txl_str(t, TXL_SMCUP);
	CHECK(s != NULL && strcmp(s, "\033[?1049h") == 0,
	    "fallback smcup should be ANSI");

	txl_free(t);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	test_new_free();
	test_simple_caps();
	test_cup();
	test_setaf_setab();
	test_decset_decrst();
	test_colors();
	test_fallback_caps();

	printf("test_txl: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count ? 1 : 0;
}
