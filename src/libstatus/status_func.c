/* status_func.c : built-in functions for status line expansion */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "status_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* split "N,text" into number and text pointer */
static int
split_num_arg(const char *args, const char **text)
{
	int n;
	char *end;

	n = (int)strtol(args, &end, 10);
	if (*end == ',')
		end++;
	*text = end;
	return n;
}

static char *
func_firstword(struct status *s, const char *args)
{
	const char *p, *start;

	p = args;
	while (*p && (*p == ' ' || *p == '\t'))
		p++;
	start = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	return status_arena_strndup(s, start, (int)(p - start));
}

static char *
func_lastword(struct status *s, const char *args)
{
	const char *p, *last_start, *last_end;

	last_start = NULL;
	last_end = NULL;
	p = args;
	while (*p) {
		while (*p && (*p == ' ' || *p == '\t'))
			p++;
		if (!*p)
			break;
		last_start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		last_end = p;
	}
	if (!last_start)
		return status_arena_strdup(s, "");
	return status_arena_strndup(s, last_start,
	    (int)(last_end - last_start));
}

static char *
func_word(struct status *s, const char *args)
{
	int n, cur;
	const char *text, *start, *end;

	n = split_num_arg(args, &text);
	if (n < 1)
		return status_arena_strdup(s, "");

	cur = 0;
	start = text;
	while (*start) {
		while (*start && (*start == ' ' || *start == '\t'))
			start++;
		if (!*start)
			break;
		end = start;
		while (*end && *end != ' ' && *end != '\t')
			end++;
		cur++;
		if (cur == n)
			return status_arena_strndup(s, start,
			    (int)(end - start));
		start = end;
	}
	return status_arena_strdup(s, "");
}

static char *
func_words(struct status *s, const char *args)
{
	int count;
	const char *p;
	char buf[12];

	count = 0;
	p = args;
	while (*p) {
		while (*p && (*p == ' ' || *p == '\t'))
			p++;
		if (!*p)
			break;
		count++;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	snprintf(buf, sizeof(buf), "%d", count);
	return status_arena_strdup(s, buf);
}

static char *
func_strip(struct status *s, const char *args)
{
	const char *p;
	char *out;
	int len, pos, in_space;

	len = (int)strlen(args);
	out = status_arena_alloc(s, len + 1);
	pos = 0;
	in_space = 1; /* suppress leading */
	p = args;
	while (*p) {
		if (*p == ' ' || *p == '\t') {
			if (!in_space) {
				out[pos++] = ' ';
				in_space = 1;
			}
		} else {
			out[pos++] = *p;
			in_space = 0;
		}
		p++;
	}
	/* strip trailing space */
	if (pos > 0 && out[pos - 1] == ' ')
		pos--;
	out[pos] = '\0';
	return out;
}

static char *
func_truncate(struct status *s, const char *args)
{
	int n;
	const char *text;
	int len;

	n = split_num_arg(args, &text);
	if (n < 0)
		n = 0;
	len = (int)strlen(text);
	if (len <= n)
		return status_arena_strdup(s, text);
	return status_arena_strndup(s, text, n);
}

static char *
func_left(struct status *s, const char *args)
{
	int n, len, i;
	const char *text;
	char *out;

	n = split_num_arg(args, &text);
	if (n < 0)
		n = 0;
	len = (int)strlen(text);
	out = status_arena_alloc(s, n + 1);
	for (i = 0; i < n; i++) {
		if (i < len)
			out[i] = text[i];
		else
			out[i] = ' ';
	}
	out[n] = '\0';
	return out;
}

static char *
func_right(struct status *s, const char *args)
{
	int n, len, pad, i;
	const char *text;
	char *out;

	n = split_num_arg(args, &text);
	if (n < 0)
		n = 0;
	len = (int)strlen(text);
	out = status_arena_alloc(s, n + 1);
	pad = n - len;
	if (pad < 0)
		pad = 0;
	for (i = 0; i < pad; i++)
		out[i] = ' ';
	for (i = 0; i < n - pad && i < len; i++)
		out[pad + i] = text[i];
	out[n] = '\0';
	return out;
}

static char *
func_center(struct status *s, const char *args)
{
	int n, len, lpad, rpad, i, pos;
	const char *text;
	char *out;

	n = split_num_arg(args, &text);
	if (n < 0)
		n = 0;
	len = (int)strlen(text);
	if (len >= n)
		return status_arena_strndup(s, text, n);
	lpad = (n - len) / 2;
	rpad = n - len - lpad;
	out = status_arena_alloc(s, n + 1);
	pos = 0;
	for (i = 0; i < lpad; i++)
		out[pos++] = ' ';
	memcpy(out + pos, text, (size_t)len);
	pos += len;
	for (i = 0; i < rpad; i++)
		out[pos++] = ' ';
	out[pos] = '\0';
	return out;
}

#define FILL_SENTINEL '\x01'

static char *
func_fill(struct status *s, const char *args)
{
	char *out;

	(void)args;
	out = status_arena_alloc(s, 2);
	out[0] = FILL_SENTINEL;
	out[1] = '\0';
	return out;
}

static char *
func_shell(struct status *s, const char *cmd)
{
	FILE *fp;
	char buf[256];
	char *out;
	int len;

	if (!status_shell_allowed(s, cmd))
		return status_arena_strdup(s, "");

	fp = popen(cmd, "r");
	if (!fp)
		return status_arena_strdup(s, "");

	len = 0;
	while (len < (int)sizeof(buf) - 1) {
		int ch = fgetc(fp);

		if (ch == EOF)
			break;
		buf[len++] = (char)ch;
	}
	pclose(fp);

	/* strip trailing newlines */
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		len--;
	buf[len] = '\0';
	out = status_arena_strdup(s, buf);
	return out;
}

const struct status_func_entry status_func_table[] = {
	{ "firstword",	func_firstword },
	{ "lastword",	func_lastword },
	{ "word",	func_word },
	{ "words",	func_words },
	{ "strip",	func_strip },
	{ "truncate",	func_truncate },
	{ "left",	func_left },
	{ "right",	func_right },
	{ "center",	func_center },
	{ "fill",	func_fill },
	{ "shell",	func_shell },
};

const int status_func_count =
    (int)(sizeof(status_func_table) / sizeof(status_func_table[0]));
