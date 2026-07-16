/* test_taskbar.c : tests for libtaskbar template expansion */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "taskbar.h"

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

/* ---- tests ---- */

static void
test_new_free(void)
{
	struct taskbar *s;

	TEST("new/free");

	s = taskbar_new();
	ASSERT(s != NULL, "taskbar_new returned NULL");
	ASSERT(taskbar_get_position(s) == 0, "default position is bottom");
	taskbar_free(s);
	PASS();
}

static void
test_simple_var(void)
{
	struct taskbar *s;
	char buf[80];
	int n;

	TEST("simple ${var} expansion");

	s = taskbar_new();
	taskbar_set_format(s, "${window}:${title}");
	taskbar_set(s, "window", "0");
	taskbar_set(s, "title", "bash");

	n = taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(n == 6, "content length without padding");
	ASSERT(memcmp(buf, "0:bash", 6) == 0, "content matches");
	ASSERT(buf[6] == '\0', "null terminated after content");

	taskbar_free(s);
	PASS();
}

static void
test_bare_var(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$var bare form");

	s = taskbar_new();
	taskbar_set_format(s, "$window:$title");
	taskbar_set(s, "window", "1");
	taskbar_set(s, "title", "vim");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "1:vim", 5) == 0, "bare var expands");

	taskbar_free(s);
	PASS();
}

static void
test_dollar_literal(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$$ literal dollar");

	s = taskbar_new();
	taskbar_set_format(s, "cost: $$5");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "cost: $5", 8) == 0, "literal $");

	taskbar_free(s);
	PASS();
}

static void
test_default_value(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("${var:-default}");

	s = taskbar_new();

	/* unset var uses default */
	taskbar_set_format(s, "${title:-no title}");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "no title", 8) == 0, "default used");

	/* set var ignores default */
	taskbar_set(s, "title", "bash");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "bash", 4) == 0, "value used");

	/* empty var uses default */
	taskbar_set(s, "title", "");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "no title", 8) == 0, "default for empty");

	taskbar_free(s);
	PASS();
}

static void
test_prefix_trim(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("${var#pattern} and ${var##pattern}");

	s = taskbar_new();
	taskbar_set(s, "path", "/home/jon/DEVEL/jmux");

	/* shortest prefix: remove /home */
	taskbar_set_format(s, "${path#/*/}");
	taskbar_expand(s, buf, sizeof(buf), 40);
	ASSERT(memcmp(buf, "jon/DEVEL/jmux", 14) == 0,
	    "shortest prefix trim");

	/* longest prefix: remove everything up to last / */
	taskbar_set_format(s, "${path##*/}");
	taskbar_expand(s, buf, sizeof(buf), 40);
	ASSERT(memcmp(buf, "jmux", 4) == 0, "longest prefix trim");

	taskbar_free(s);
	PASS();
}

static void
test_suffix_trim(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("${var%%pattern} and ${var%%pattern}");

	s = taskbar_new();
	taskbar_set(s, "file", "archive.tar.gz");

	/* shortest suffix: remove .gz */
	taskbar_set_format(s, "${file%.*}");
	taskbar_expand(s, buf, sizeof(buf), 40);
	ASSERT(memcmp(buf, "archive.tar", 11) == 0,
	    "shortest suffix trim");

	/* longest suffix: remove .tar.gz */
	taskbar_set_format(s, "${file%%.*}");
	taskbar_expand(s, buf, sizeof(buf), 40);
	ASSERT(memcmp(buf, "archive", 7) == 0, "longest suffix trim");

	taskbar_free(s);
	PASS();
}

static void
test_substring(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("${var:offset} and ${var:offset:length}");

	s = taskbar_new();
	taskbar_set(s, "text", "hello world");

	taskbar_set_format(s, "${text:6}");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "world", 5) == 0, "offset only");

	taskbar_set_format(s, "${text:0:5}");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "hello", 5) == 0, "offset + length");

	taskbar_free(s);
	PASS();
}

static void
test_func_firstword(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(firstword ...)");

	s = taskbar_new();
	taskbar_set_format(s, "$(firstword alpha beta gamma)");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "alpha", 5) == 0, "firstword");

	taskbar_free(s);
	PASS();
}

static void
test_func_lastword(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(lastword ...)");

	s = taskbar_new();
	taskbar_set_format(s, "$(lastword alpha beta gamma)");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "gamma", 5) == 0, "lastword");

	taskbar_free(s);
	PASS();
}

static void
test_func_word(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(word N,...)");

	s = taskbar_new();
	taskbar_set_format(s, "$(word 2,alpha beta gamma)");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "beta", 4) == 0, "word 2");

	taskbar_free(s);
	PASS();
}

static void
test_func_words(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(words ...)");

	s = taskbar_new();
	taskbar_set_format(s, "$(words alpha beta gamma)");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(buf[0] == '3', "words count");

	taskbar_free(s);
	PASS();
}

static void
test_func_strip(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(strip ...)");

	s = taskbar_new();
	taskbar_set_format(s, "[$(strip   a  b  c  )]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[a b c]", 7) == 0, "strip whitespace");

	taskbar_free(s);
	PASS();
}

static void
test_func_truncate(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(truncate N,...)");

	s = taskbar_new();
	taskbar_set_format(s, "$(truncate 5,hello world)");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "hello", 5) == 0, "truncated");
	ASSERT(buf[5] == '\0', "no extra chars");

	taskbar_free(s);
	PASS();
}

static void
test_func_left_right_center(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(left), $(right), $(center)");

	s = taskbar_new();

	taskbar_set_format(s, "[$(left 5,ab)]");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[ab   ]", 7) == 0, "left pad");

	taskbar_set_format(s, "[$(right 5,ab)]");
	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[   ab]", 7) == 0, "right pad");

	taskbar_set_format(s, "[$(center 5,ab)]");
	taskbar_expand(s, buf, sizeof(buf), 20);
	/* "ab" centered in 5: " ab  " (1 left, 2 right) */
	ASSERT(buf[0] == '[', "center lbracket");
	ASSERT(buf[1] == ' ', "center lpad");
	ASSERT(buf[2] == 'a', "center a");
	ASSERT(buf[3] == 'b', "center b");
	ASSERT(buf[4] == ' ', "center rpad1");
	ASSERT(buf[5] == ' ', "center rpad2");
	ASSERT(buf[6] == ']', "center rbracket");

	taskbar_free(s);
	PASS();
}

static void
test_fill(void)
{
	struct taskbar *s;
	char buf[80];
	int n;

	TEST("$(fill) elastic spacing");

	s = taskbar_new();
	taskbar_set_format(s, "L$(fill)R");

	n = taskbar_expand(s, buf, sizeof(buf), 10);
	ASSERT(n == 10, "padded to 10");
	ASSERT(buf[0] == 'L', "left part");
	ASSERT(buf[9] == 'R', "right part");
	ASSERT(buf[1] == ' ', "fill is spaces");

	taskbar_free(s);
	PASS();
}

static void
test_shell_blocked(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(shell) blocked without allow-list");

	s = taskbar_new();
	taskbar_set_format(s, "[$(shell echo hello)]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[]", 2) == 0, "shell blocked");

	taskbar_free(s);
	PASS();
}

static void
test_shell_allowed(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(shell) allowed with matching pattern");

	s = taskbar_new();
	taskbar_add_shell_allow(s, "echo *");
	taskbar_set_format(s, "[$(shell echo hello)]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[hello]", 7) == 0, "shell allowed");

	taskbar_free(s);
	PASS();
}

static void
test_shell_partial_match(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("$(shell) fnmatch pattern specificity");

	s = taskbar_new();
	taskbar_add_shell_allow(s, "echo hello");
	taskbar_set_format(s, "[$(shell echo world)]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	/* "echo world" does not match "echo hello" */
	ASSERT(memcmp(buf, "[]", 2) == 0, "no match blocks");

	taskbar_free(s);
	PASS();
}

static void
test_nested_expansion(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("nested $(func ${var})");

	s = taskbar_new();
	taskbar_set(s, "window-list", "0:bash 1:vim 2:top");
	taskbar_set_format(s, "$(firstword ${window-list})");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "0:bash", 6) == 0, "nested expand");

	taskbar_free(s);
	PASS();
}

static void
test_unknown_func(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("unknown function expands to empty");

	s = taskbar_new();
	taskbar_set_format(s, "[$(bogus hello)]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[]", 2) == 0, "unknown func empty");

	taskbar_free(s);
	PASS();
}

static void
test_unset_var(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("unset variable expands to empty");

	s = taskbar_new();
	taskbar_set_format(s, "[${nosuchvar}]");

	taskbar_expand(s, buf, sizeof(buf), 20);
	ASSERT(memcmp(buf, "[]", 2) == 0, "unset var empty");

	taskbar_free(s);
	PASS();
}

static void
test_set_overwrite(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("set overwrites previous value");

	s = taskbar_new();
	taskbar_set_format(s, "${title}");
	taskbar_set(s, "title", "old");
	taskbar_set(s, "title", "new");

	taskbar_expand(s, buf, sizeof(buf), 10);
	ASSERT(memcmp(buf, "new", 3) == 0, "overwritten");

	taskbar_free(s);
	PASS();
}

static void
test_position(void)
{
	struct taskbar *s;

	TEST("position get/set");

	s = taskbar_new();
	ASSERT(taskbar_get_position(s) == 0, "default bottom");
	taskbar_set_position(s, 1);
	ASSERT(taskbar_get_position(s) == 1, "set top");
	taskbar_set_position(s, 0);
	ASSERT(taskbar_get_position(s) == 0, "set bottom");

	taskbar_free(s);
	PASS();
}

static void
test_arena_hint(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("arena hint adjustment");

	s = taskbar_new();
	taskbar_set_format(s, "${title}");
	taskbar_set(s, "title", "x");

	/* first expand */
	taskbar_expand(s, buf, sizeof(buf), 10);
	/* second expand should not need to grow */
	taskbar_expand(s, buf, sizeof(buf), 10);
	ASSERT(memcmp(buf, "x", 1) == 0, "second expand works");

	taskbar_free(s);
	PASS();
}

static void
test_truncation(void)
{
	struct taskbar *s;
	char buf[80];
	int n;

	TEST("truncation to cols");

	s = taskbar_new();
	taskbar_set_format(s, "${text}");
	taskbar_set(s, "text", "this is a very long string");

	n = taskbar_expand(s, buf, sizeof(buf), 10);
	ASSERT(n == 10, "truncated to 10");
	ASSERT(memcmp(buf, "this is a ", 10) == 0, "truncated content");

	taskbar_free(s);
	PASS();
}

static void
test_default_format(void)
{
	struct taskbar *s;
	char buf[80];

	TEST("default format");

	s = taskbar_new();
	taskbar_set(s, "window", "0");
	taskbar_set(s, "title", "bash");
	taskbar_set(s, "window-list", "0*bash");

	taskbar_expand(s, buf, sizeof(buf), 30);
	/* default "${window-list}" -> "0*bash" */
	ASSERT(memcmp(buf, "0*bash", 6) == 0,
	    "default format expands");

	taskbar_free(s);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libtaskbar tests:\n");

	test_new_free();
	test_simple_var();
	test_bare_var();
	test_dollar_literal();
	test_default_value();
	test_prefix_trim();
	test_suffix_trim();
	test_substring();
	test_func_firstword();
	test_func_lastword();
	test_func_word();
	test_func_words();
	test_func_strip();
	test_func_truncate();
	test_func_left_right_center();
	test_fill();
	test_shell_blocked();
	test_shell_allowed();
	test_shell_partial_match();
	test_nested_expansion();
	test_unknown_func();
	test_unset_var();
	test_set_overwrite();
	test_position();
	test_arena_hint();
	test_truncation();
	test_default_format();

	printf("\ntest_taskbar: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
