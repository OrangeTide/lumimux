/* cfg.c : gitconfig-style configuration file parser */
/* Copyright (c) 2012, 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "cfg.h"
#include "xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- storage ---- */

struct cfg_entry {
	char	*key;
	char	*value;
};

struct cfg {
	struct cfg_entry	*entries;
	int			count;
	int			alloc;
};

struct cfg *
cfg_new(void)
{
	struct cfg *c;

	c = xcalloc(1, sizeof(*c));
	return c;
}

void
cfg_free(struct cfg *c)
{
	int i;

	if (!c)
		return;
	for (i = 0; i < c->count; i++) {
		free(c->entries[i].key);
		free(c->entries[i].value);
	}
	free(c->entries);
	free(c);
}

/* Store or overwrite a key-value pair. Key is already normalized. */
static void
cfg_set(struct cfg *c, const char *key, const char *value)
{
	int i;

	/* overwrite existing? */
	for (i = 0; i < c->count; i++) {
		if (strcmp(c->entries[i].key, key) == 0) {
			free(c->entries[i].value);
			c->entries[i].value = xstrdup(value);
			return;
		}
	}

	/* append */
	if (c->count >= c->alloc) {
		c->alloc = c->alloc ? c->alloc * 2 : 16;
		c->entries = xrealloc(c->entries,
		    (size_t)c->alloc * sizeof(c->entries[0]));
	}
	c->entries[c->count].key = xstrdup(key);
	c->entries[c->count].value = xstrdup(value);
	c->count++;
}

const char *
cfg_get(const struct cfg *c, const char *key)
{
	int i;

	if (!c)
		return NULL;
	for (i = 0; i < c->count; i++) {
		if (strcmp(c->entries[i].key, key) == 0)
			return c->entries[i].value;
	}
	return NULL;
}

int
cfg_each(const struct cfg *c,
    int (*fn)(const char *key, const char *value, void *arg), void *arg)
{
	int i, ret;

	if (!c)
		return 0;
	for (i = 0; i < c->count; i++) {
		ret = fn(c->entries[i].key, c->entries[i].value, arg);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- parsing helpers ---- */

/* Strip leading whitespace. */
static char *
skip_ws(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* Strip trailing whitespace and newlines in place. */
static void
trim_end(char *s)
{
	char *end;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	    end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
}

/*
 * Build a dotted key from section + subsection + name.
 * Examples:
 *   section="core", subsect=NULL,   name="shell"  -> "core.shell"
 *   section="bind", subsect="prefix", name="c"    -> "bind.prefix.c"
 *   section="",     subsect=NULL,   name="foo.bar" -> "foo.bar"
 */
static char *
make_key(const char *section, const char *subsect, const char *name)
{
	size_t len;
	char *key;

	if (section[0] == '\0')
		return xstrdup(name);

	len = strlen(section) + 1 + strlen(name) + 1;
	if (subsect)
		len += strlen(subsect) + 1;

	key = xmalloc(len);
	if (subsect)
		snprintf(key, len, "%s.%s.%s", section, subsect, name);
	else
		snprintf(key, len, "%s.%s", section, name);
	return key;
}

/* ---- parser ---- */

int
cfg_load(struct cfg *c, const char *path)
{
	char buf[4096];
	FILE *f;
	int line;
	char *section;
	char *subsect;

	if (!c)
		return -1;

	f = fopen(path, "r");
	if (!f)
		return -1;

	section = xstrdup("");
	subsect = NULL;
	line = 0;

	while (fgets(buf, (int)sizeof(buf), f)) {
		char *base;
		char *tmp;

		line++;

		/* strip comments */
		tmp = buf;
		while (*tmp) {
			if (*tmp == '#' || *tmp == ';') {
				*tmp = '\0';
				break;
			}
			/* skip quoted regions */
			if (*tmp == '"') {
				tmp++;
				while (*tmp && *tmp != '"')
					tmp++;
				if (*tmp)
					tmp++;
				continue;
			}
			tmp++;
		}

		base = skip_ws(buf);
		if (*base == '\0' || *base == '\n' || *base == '\r')
			continue;

		if (*base == '[') {
			/* section header: [section] or [section "subsect"] */
			char *end, *q;

			base++;
			end = strchr(base, ']');
			if (!end) {
				fprintf(stderr, "%s:%d: missing ']'\n",
				    path, line);
				goto err;
			}
			*end = '\0';

			/* check for subsection: [section "subsect"] */
			q = strchr(base, '"');
			if (q) {
				char *q2;

				/* section name is before the quote */
				*q = '\0';
				base = skip_ws(base);
				trim_end(base);

				/* subsection is between quotes */
				q++;
				q2 = strchr(q, '"');
				if (!q2) {
					fprintf(stderr,
					    "%s:%d: unterminated quote\n",
					    path, line);
					goto err;
				}
				*q2 = '\0';

				free(section);
				section = xstrdup(base);
				free(subsect);
				subsect = xstrdup(q);
			} else {
				base = skip_ws(base);
				trim_end(base);
				free(section);
				section = xstrdup(base);
				free(subsect);
				subsect = NULL;
			}
			continue;
		}

		/* key = value */
		tmp = strchr(base, '=');
		if (!tmp) {
			fprintf(stderr, "%s:%d: expected '='\n",
			    path, line);
			goto err;
		}

		{
			char *name, *value, *key, *dot;

			*tmp = '\0';
			name = base;
			value = skip_ws(tmp + 1);

			trim_end(name);
			trim_end(value);

			/*
			 * Dotted shorthand: foo.bar = value
			 * Only when no section is active (section is "").
			 * With a section active, dots are part of the name.
			 */
			if (section[0] == '\0' &&
			    (dot = strchr(name, '.')) != NULL) {
				char *sect;

				*dot = '\0';
				sect = name;
				name = dot + 1;
				trim_end(sect);
				name = skip_ws(name);

				key = make_key(sect, NULL, name);
			} else {
				key = make_key(section, subsect, name);
			}

			cfg_set(c, key, value);
			free(key);
		}
	}

	fclose(f);
	free(section);
	free(subsect);
	return 0;

err:
	fclose(f);
	free(section);
	free(subsect);
	return -1;
}
