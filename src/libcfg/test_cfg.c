/* test_cfg.c : unit tests for config file parser */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Write a temporary config file, return path (caller must free). */
static char *
write_tmp(const char *content)
{
	char path[] = "/tmp/test_cfg_XXXXXX";
	int fd;
	FILE *f;

	fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return NULL;
	}
	fputs(content, f);
	fclose(f);
	return strdup(path);
}

/* ---- tests ---- */

static void
test_basic_section(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "[core]\n"
	    "shell = /bin/bash\n"
	    "prefix = C-a\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/bash") == 0,
	    "core.shell = /bin/bash");
	CHECK(strcmp(cfg_get(c, "core.prefix"), "C-a") == 0,
	    "core.prefix = C-a");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_dotted_shorthand(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "core.shell = /bin/zsh\n"
	    "status.position = bottom\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/zsh") == 0,
	    "dotted: core.shell = /bin/zsh");
	CHECK(strcmp(cfg_get(c, "status.position"), "bottom") == 0,
	    "dotted: status.position = bottom");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_subsection(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "[bind \"prefix\"]\n"
	    "c = new-window\n"
	    "n = next-window\n"
	    "[bind \"copy\"]\n"
	    "v = paste\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "bind.prefix.c"), "new-window") == 0,
	    "bind.prefix.c = new-window");
	CHECK(strcmp(cfg_get(c, "bind.prefix.n"), "next-window") == 0,
	    "bind.prefix.n = next-window");
	CHECK(strcmp(cfg_get(c, "bind.copy.v"), "paste") == 0,
	    "bind.copy.v = paste");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_comments(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "# this is a comment\n"
	    "[core]\n"
	    "; another comment\n"
	    "shell = /bin/sh  # inline comment\n"
	    "name = hello\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/sh") == 0,
	    "inline comment stripped");
	CHECK(strcmp(cfg_get(c, "core.name"), "hello") == 0,
	    "value after comment line");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_overwrite(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "[core]\n"
	    "shell = /bin/sh\n"
	    "shell = /bin/zsh\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/zsh") == 0,
	    "later value overwrites");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_missing_key(void)
{
	struct cfg *c;

	c = cfg_new();
	CHECK(cfg_get(c, "nonexistent") == NULL, "missing key returns NULL");
	cfg_free(c);
}

static void
test_missing_file(void)
{
	struct cfg *c;

	c = cfg_new();
	CHECK(cfg_load(c, "/tmp/no_such_cfg_file_xyz") == -1,
	    "missing file returns -1");
	cfg_free(c);
}

static void
test_empty_file(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp("");
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "empty file loads ok");
	CHECK(cfg_get(c, "anything") == NULL, "no keys in empty file");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_whitespace(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "  [  core  ]  \n"
	    "  shell  =  /bin/bash  \n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/bash") == 0,
	    "whitespace trimmed from value");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_merge_files(void)
{
	struct cfg *c;
	char *p1, *p2;

	p1 = write_tmp("[core]\nshell = /bin/sh\n");
	p2 = write_tmp("[status]\nposition = top\n");
	CHECK(p1 != NULL && p2 != NULL, "tmpfiles created");
	if (!p1 || !p2) {
		free(p1);
		free(p2);
		return;
	}

	c = cfg_new();
	CHECK(cfg_load(c, p1) == 0, "first load");
	CHECK(cfg_load(c, p2) == 0, "second load merges");
	CHECK(strcmp(cfg_get(c, "core.shell"), "/bin/sh") == 0,
	    "key from first file");
	CHECK(strcmp(cfg_get(c, "status.position"), "top") == 0,
	    "key from second file");

	cfg_free(c);
	unlink(p1);
	unlink(p2);
	free(p1);
	free(p2);
}

static int
count_cb(const char *key, const char *value, void *arg)
{
	int *n = arg;

	(void)key;
	(void)value;
	(*n)++;
	return 0;
}

static void
test_each(void)
{
	struct cfg *c;
	char *path;
	int n;

	path = write_tmp(
	    "[a]\nx = 1\ny = 2\n"
	    "[b]\nz = 3\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	cfg_load(c, path);

	n = 0;
	CHECK(cfg_each(c, count_cb, &n) == 0, "each returns 0");
	CHECK(n == 3, "each visits 3 entries");

	cfg_free(c);
	unlink(path);
	free(path);
}

static void
test_quoted_value_with_hash(void)
{
	struct cfg *c;
	char *path;

	path = write_tmp(
	    "[ui]\n"
	    "title = \"hello # world\"\n"
	);
	CHECK(path != NULL, "tmpfile created");
	if (!path)
		return;

	c = cfg_new();
	CHECK(cfg_load(c, path) == 0, "load succeeds");
	/* the quotes and content inside are preserved */
	CHECK(strcmp(cfg_get(c, "ui.title"), "\"hello # world\"") == 0,
	    "hash inside quotes is not a comment");

	cfg_free(c);
	unlink(path);
	free(path);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	test_basic_section();
	test_dotted_shorthand();
	test_subsection();
	test_comments();
	test_overwrite();
	test_missing_key();
	test_missing_file();
	test_empty_file();
	test_whitespace();
	test_merge_files();
	test_each();
	test_quoted_value_with_hash();

	printf("test_cfg: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count ? 1 : 0;
}
