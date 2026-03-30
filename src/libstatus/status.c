/* status.c : status line template expansion */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "status.h"
#include "status_int.h"
#include "cfg.h"
#include "xmalloc.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FORMAT " ${window}:${title} ${window-list}"
#define ARENA_INIT 256
#define ARENA_MIN 256
#define FILL_SENTINEL '\x01'
#define MAX_RECURSE 8

/* ---- variable storage ---- */

struct status_var {
	char	*name;
	char	*value;
};

struct status {
	char		*format;
	int		 top;
	struct status_var *vars;
	int		 var_count;
	int		 var_alloc;
	char		**shell_allow;
	int		 shell_count;
	int		 shell_alloc;
	/* arena bump allocator */
	char		*arena;
	int		 arena_size;
	int		 arena_off;
	int		 arena_hint;
};

/* ---- arena ---- */

static void
arena_reset(struct status *s)
{
	s->arena_off = 0;
}

static void
arena_ensure(struct status *s, int need)
{
	if (s->arena_size >= need)
		return;
	while (s->arena_size < need)
		s->arena_size = s->arena_size ? s->arena_size * 2 : ARENA_INIT;
	s->arena = xrealloc(s->arena, (size_t)s->arena_size);
}

char *
status_arena_alloc(struct status *s, int size)
{
	char *p;

	size = (size + 3) & ~3;
	arena_ensure(s, s->arena_off + size);
	p = s->arena + s->arena_off;
	s->arena_off += size;
	return p;
}

char *
status_arena_strdup(struct status *s, const char *str)
{
	int len;
	char *p;

	if (!str)
		return status_arena_alloc(s, 1);
	len = (int)strlen(str);
	p = status_arena_alloc(s, len + 1);
	memcpy(p, str, (size_t)len + 1);
	return p;
}

char *
status_arena_strndup(struct status *s, const char *str, int n)
{
	char *p;

	p = status_arena_alloc(s, n + 1);
	memcpy(p, str, (size_t)n);
	p[n] = '\0';
	return p;
}

/* ---- public API ---- */

struct status *
status_new(void)
{
	struct status *s;

	s = xcalloc(1, sizeof(*s));
	s->format = xstrdup(DEFAULT_FORMAT);
	s->arena_hint = ARENA_INIT;
	return s;
}

void
status_free(struct status *s)
{
	int i;

	if (!s)
		return;
	free(s->format);
	for (i = 0; i < s->var_count; i++) {
		free(s->vars[i].name);
		free(s->vars[i].value);
	}
	free(s->vars);
	for (i = 0; i < s->shell_count; i++)
		free(s->shell_allow[i]);
	free(s->shell_allow);
	free(s->arena);
	free(s);
}

void
status_set_format(struct status *s, const char *fmt)
{
	free(s->format);
	s->format = xstrdup(fmt);
}

void
status_set_position(struct status *s, int top)
{
	s->top = top ? 1 : 0;
}

int
status_get_position(const struct status *s)
{
	return s->top;
}

void
status_set(struct status *s, const char *name, const char *value)
{
	int i;

	for (i = 0; i < s->var_count; i++) {
		if (strcmp(s->vars[i].name, name) == 0) {
			free(s->vars[i].value);
			s->vars[i].value = value ? xstrdup(value) : NULL;
			return;
		}
	}
	if (s->var_count >= s->var_alloc) {
		s->var_alloc = s->var_alloc ? s->var_alloc * 2 : 8;
		s->vars = xrealloc(s->vars,
		    (size_t)s->var_alloc * sizeof(s->vars[0]));
	}
	s->vars[s->var_count].name = xstrdup(name);
	s->vars[s->var_count].value = value ? xstrdup(value) : NULL;
	s->var_count++;
}

static const char *
var_get(const struct status *s, const char *name)
{
	int i;

	for (i = 0; i < s->var_count; i++) {
		if (strcmp(s->vars[i].name, name) == 0)
			return s->vars[i].value;
	}
	return NULL;
}

void
status_add_shell_allow(struct status *s, const char *pattern)
{
	if (s->shell_count >= s->shell_alloc) {
		s->shell_alloc = s->shell_alloc ? s->shell_alloc * 2 : 4;
		s->shell_allow = xrealloc(s->shell_allow,
		    (size_t)s->shell_alloc * sizeof(s->shell_allow[0]));
	}
	s->shell_allow[s->shell_count++] = xstrdup(pattern);
}

int
status_shell_allowed(const struct status *s, const char *cmd)
{
	int i;

	for (i = 0; i < s->shell_count; i++) {
		if (fnmatch(s->shell_allow[i], cmd, 0) == 0)
			return 1;
	}
	return 0;
}

void
status_load_cfg(struct status *s, const struct cfg *c)
{
	const char *val, *p, *end;

	if (!c)
		return;

	val = cfg_get(c, "status.format");
	if (val)
		status_set_format(s, val);

	val = cfg_get(c, "status.position");
	if (val && strcmp(val, "top") == 0)
		status_set_position(s, 1);

	val = cfg_get(c, "status.shell_allow");
	if (val) {
		/* comma-separated list of fnmatch patterns */
		p = val;
		while (*p) {
			while (*p == ' ' || *p == '\t')
				p++;
			if (!*p)
				break;
			end = strchr(p, ',');
			if (!end)
				end = p + strlen(p);
			/* trim trailing whitespace */
			{
				const char *e = end;

				while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
					e--;
				if (e > p) {
					char *pat;

					pat = xmalloc((size_t)(e - p) + 1);
					memcpy(pat, p, (size_t)(e - p));
					pat[e - p] = '\0';
					status_add_shell_allow(s, pat);
					free(pat);
				}
			}
			p = *end ? end + 1 : end;
		}
	}
}

/* ---- expander ---- */

/* forward declaration */
static char *expand(struct status *s, const char *fmt, int depth);

/* check if char is valid in a variable name */
static int
is_name_char(int c)
{
	return isalnum(c) || c == '-' || c == '_';
}

/* ---- bash parameter expansion modifiers ---- */

/* ${var#pattern} -- remove shortest prefix match */
static const char *
mod_trim_prefix_short(struct status *s, const char *val, const char *pat)
{
	int len, i;
	char *tmp;

	len = (int)strlen(val);
	for (i = 1; i <= len; i++) {
		tmp = status_arena_strndup(s, val, i);
		if (fnmatch(pat, tmp, 0) == 0)
			return val + i;
	}
	return val;
}

/* ${var##pattern} -- remove longest prefix match */
static const char *
mod_trim_prefix_long(struct status *s, const char *val, const char *pat)
{
	int len, i;
	char *tmp;

	len = (int)strlen(val);
	for (i = len; i >= 1; i--) {
		tmp = status_arena_strndup(s, val, i);
		if (fnmatch(pat, tmp, 0) == 0)
			return val + i;
	}
	return val;
}

/* ${var%pattern} -- remove shortest suffix match */
static const char *
mod_trim_suffix_short(struct status *s, const char *val, const char *pat)
{
	int len, i;

	(void)s;
	len = (int)strlen(val);
	for (i = len - 1; i >= 0; i--) {
		if (fnmatch(pat, val + i, 0) == 0)
			return status_arena_strndup(s, val, i);
	}
	return val;
}

/* ${var%%pattern} -- remove longest suffix match */
static const char *
mod_trim_suffix_long(struct status *s, const char *val, const char *pat)
{
	int len, i;

	len = (int)strlen(val);
	for (i = 0; i < len; i++) {
		if (fnmatch(pat, val + i, 0) == 0)
			return status_arena_strndup(s, val, i);
	}
	return val;
}

/* parse ${...} with modifiers, return expanded string (arena-allocated) */
static char *
expand_braced_var(struct status *s, const char **pp, int depth)
{
	const char *p, *name_start, *val;
	char *name;
	int name_len;

	p = *pp; /* points after ${ */

	/* scan variable name */
	name_start = p;
	while (*p && is_name_char(*p))
		p++;
	name_len = (int)(p - name_start);

	if (name_len == 0) {
		/* empty name -- skip to } */
		while (*p && *p != '}')
			p++;
		if (*p == '}')
			p++;
		*pp = p;
		return status_arena_strdup(s, "");
	}

	name = status_arena_strndup(s, name_start, name_len);
	val = var_get(s, name);

	if (*p == '}') {
		/* simple ${var} */
		*pp = p + 1;
		return status_arena_strdup(s, val ? val : "");
	}

	if (*p == ':' && p[1] == '-') {
		/* ${var:-default} */
		const char *def_start;
		int brace_depth = 1;
		const char *scan;
		char *def_text;

		p += 2;
		def_start = p;
		scan = p;
		while (*scan && brace_depth > 0) {
			if (*scan == '{')
				brace_depth++;
			else if (*scan == '}')
				brace_depth--;
			if (brace_depth > 0)
				scan++;
		}
		def_text = status_arena_strndup(s, def_start, (int)(scan - def_start));
		if (*scan == '}')
			scan++;
		*pp = scan;
		if (!val || !val[0]) {
			/* expand the default */
			return expand(s, def_text, depth + 1);
		}
		return status_arena_strdup(s, val);
	}

	if (*p == ':' && isdigit((unsigned char)p[1])) {
		/* ${var:offset} or ${var:offset:length} */
		int offset, length, vlen;

		p++;
		offset = (int)strtol(p, (char **)&p, 10);
		vlen = val ? (int)strlen(val) : 0;
		if (offset < 0)
			offset = 0;
		if (offset > vlen)
			offset = vlen;

		if (*p == ':' && isdigit((unsigned char)p[1])) {
			p++;
			length = (int)strtol(p, (char **)&p, 10);
			if (length < 0)
				length = 0;
			if (offset + length > vlen)
				length = vlen - offset;
		} else {
			length = vlen - offset;
		}
		while (*p && *p != '}')
			p++;
		if (*p == '}')
			p++;
		*pp = p;
		if (!val)
			return status_arena_strdup(s, "");
		return status_arena_strndup(s, val + offset, length);
	}

	if (*p == '#') {
		/* ${var#pattern} or ${var##pattern} */
		int longest = 0;
		const char *pat_start, *scan, *result;
		char *pat;

		p++;
		if (*p == '#') {
			longest = 1;
			p++;
		}
		pat_start = p;
		scan = p;
		while (*scan && *scan != '}')
			scan++;
		pat = status_arena_strndup(s, pat_start, (int)(scan - pat_start));
		if (*scan == '}')
			scan++;
		*pp = scan;
		if (!val || !val[0])
			return status_arena_strdup(s, "");
		if (longest)
			result = mod_trim_prefix_long(s, val, pat);
		else
			result = mod_trim_prefix_short(s, val, pat);
		return status_arena_strdup(s, result);
	}

	if (*p == '%') {
		/* ${var%pattern} or ${var%%pattern} */
		int longest = 0;
		const char *pat_start, *scan, *result;
		char *pat;

		p++;
		if (*p == '%') {
			longest = 1;
			p++;
		}
		pat_start = p;
		scan = p;
		while (*scan && *scan != '}')
			scan++;
		pat = status_arena_strndup(s, pat_start, (int)(scan - pat_start));
		if (*scan == '}')
			scan++;
		*pp = scan;
		if (!val || !val[0])
			return status_arena_strdup(s, "");
		if (longest)
			result = mod_trim_suffix_long(s, val, pat);
		else
			result = mod_trim_suffix_short(s, val, pat);
		return status_arena_strdup(s, result);
	}

	/* unknown modifier -- skip to } */
	while (*p && *p != '}')
		p++;
	if (*p == '}')
		p++;
	*pp = p;
	return status_arena_strdup(s, val ? val : "");
}

/* scan to matching ) with nesting awareness */
static const char *
scan_to_close_paren(const char *p)
{
	int depth = 1;

	while (*p && depth > 0) {
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		if (depth > 0)
			p++;
	}
	return p;
}

/* expand $(func args) -- p points after $( */
static char *
expand_func(struct status *s, const char **pp, int depth)
{
	const char *p, *name_start, *args_start, *close;
	char *name, *args_raw, *args_expanded;
	int name_len, i;

	p = *pp;

	/* scan function name */
	name_start = p;
	while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != ',')
		p++;
	name_len = (int)(p - name_start);
	name = status_arena_strndup(s, name_start, name_len);

	/* skip whitespace after name */
	while (*p == ' ' || *p == '\t')
		p++;

	/* collect arguments to matching ) */
	args_start = p;
	close = scan_to_close_paren(p);
	args_raw = status_arena_strndup(s, args_start, (int)(close - args_start));
	if (*close == ')')
		close++;
	*pp = close;

	/* expand arguments recursively */
	args_expanded = expand(s, args_raw, depth + 1);

	/* dispatch */
	for (i = 0; i < status_func_count; i++) {
		if (strcmp(name, status_func_table[i].name) == 0)
			return status_func_table[i].func(s, args_expanded);
	}

	/* unknown function -- empty */
	return status_arena_strdup(s, "");
}

/* ---- main expand ---- */

/*
 * expand() uses a separate malloc'd buffer for output instead of the arena,
 * because arena_alloc may realloc the arena and invalidate pointers returned
 * by sub-expansions. The result is copied into the arena before returning.
 */
static char *
expand(struct status *s, const char *fmt, int depth)
{
	const char *p;
	char *out;
	int outlen, outpos;
	char *result;

	if (depth > MAX_RECURSE)
		return status_arena_strdup(s, "");

	outlen = 256;
	out = xmalloc((size_t)outlen);
	outpos = 0;

#define NEED(n) \
	do { \
		if (outpos + (n) >= outlen) { \
			outlen = outlen * 2; \
			out = xrealloc(out, (size_t)outlen); \
		} \
	} while (0)

#define EMIT(c) do { NEED(1); out[outpos++] = (c); } while (0)

#define EMIT_STR(str) \
	do { \
		const char *_es = (str); \
		if (_es) { \
			int _el = (int)strlen(_es); \
			NEED(_el); \
			memcpy(out + outpos, _es, (size_t)_el); \
			outpos += _el; \
		} \
	} while (0)

	p = fmt;
	while (*p) {
		if (p[0] == '$' && p[1] == '$') {
			/* literal $ */
			EMIT('$');
			p += 2;
		} else if (p[0] == '$' && p[1] == '{') {
			/* ${var} with possible modifiers */
			p += 2;
			EMIT_STR(expand_braced_var(s, &p, depth));
		} else if (p[0] == '$' && p[1] == '(') {
			/* $(func args) */
			p += 2;
			EMIT_STR(expand_func(s, &p, depth));
		} else if (p[0] == '$' && is_name_char(p[1])) {
			/* $bare-name */
			const char *ns = p + 1;
			const char *val;
			char *name;

			p++;
			while (*p && is_name_char(*p))
				p++;
			name = status_arena_strndup(s, ns, (int)(p - ns));
			val = var_get(s, name);
			if (val)
				EMIT_STR(val);
		} else {
			EMIT(*p);
			p++;
		}
	}

	NEED(0);
	out[outpos] = '\0';

#undef NEED
#undef EMIT
#undef EMIT_STR

	/* copy result into arena and free the temp buffer */
	result = status_arena_strdup(s, out);
	free(out);
	return result;
}

/* ---- public expand ---- */

int
status_expand(struct status *s, char *buf, size_t bufsz, int cols)
{
	char *raw;
	char *fill_pos;
	int pos, i;

	if (!s || !buf || bufsz == 0 || cols <= 0) {
		if (buf && bufsz > 0)
			buf[0] = '\0';
		return 0;
	}

	/* reset arena, ensure hint size */
	arena_reset(s);
	arena_ensure(s, s->arena_hint);

	raw = expand(s, s->format, 0);

	pos = 0;
	fill_pos = strchr(raw, FILL_SENTINEL);

	if (fill_pos) {
		int left_len, right_len, pad;
		const char *right;

		left_len = (int)(fill_pos - raw);
		right = fill_pos + 1;
		right_len = (int)strlen(right);
		pad = cols - left_len - right_len;
		if (pad < 0)
			pad = 0;

		/* left part */
		for (i = 0; i < left_len && pos < cols &&
		    pos + 1 < (int)bufsz; i++)
			buf[pos++] = raw[i];
		/* fill */
		for (i = 0; i < pad && pos < cols &&
		    pos + 1 < (int)bufsz; i++)
			buf[pos++] = ' ';
		/* right part */
		for (i = 0; i < right_len && pos < cols &&
		    pos + 1 < (int)bufsz; i++)
			buf[pos++] = right[i];
	} else {
		int len = (int)strlen(raw);

		for (i = 0; i < len && pos < cols &&
		    pos + 1 < (int)bufsz; i++)
			buf[pos++] = raw[i];
	}

	buf[pos] = '\0';

	/* adjust arena hint */
	if (s->arena_off > s->arena_hint)
		s->arena_hint = s->arena_size;
	else if (s->arena_off < s->arena_hint / 4 &&
	    s->arena_hint > ARENA_MIN)
		s->arena_hint /= 2;

	return pos;
}
