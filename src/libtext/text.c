/* text.c : editable text buffer with a line index */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK	0
#define ERR	(-1)

struct estack;
static void estack_clear(struct estack *s);
static void estack_free(struct estack *s);

struct line {
	char	*buf;		/* NUL-terminated line bytes, no newline */
	size_t	len;		/* bytes excluding the NUL */
	size_t	cap;		/* allocated bytes including room for NUL */
};

/* A reversible primitive. Applying one mutates the buffer and yields the
 * primitive that reverses it, which is how undo and redo stay symmetric. */
enum eop {
	OP_INSERT,		/* insert bytes at (line, col) */
	OP_DELETE,		/* delete n bytes at (line, col) */
	OP_SPLIT,		/* split line at col */
	OP_JOIN,		/* join line with the one after it */
};

struct erec {
	enum eop	op;
	size_t		line;
	size_t		col;
	char		*bytes;		/* owned; the payload for OP_INSERT */
	size_t		n;		/* byte count for INSERT and DELETE */
};

struct estack {
	struct erec	*v;
	size_t		n;
	size_t		cap;
};

struct text {
	struct line	*lines;
	size_t		nlines;
	size_t		cap;
	int		final_newline;	/* source ended with a newline */
	int		dirty;

	struct estack	undo;
	struct estack	redo;
	int		can_coalesce;	/* a typing run may extend the top */
};

/****************************************************************
 * Growable storage
 ****************************************************************/

static int
line_reserve(struct line *l, size_t need)
{
	/* need is bytes excluding the NUL */
	if (need + 1 > l->cap) {
		size_t cap = l->cap ? l->cap : 16;
		char *p;

		while (cap < need + 1)
			cap *= 2;
		p = realloc(l->buf, cap);
		if (!p)
			return ERR;
		l->buf = p;
		l->cap = cap;
	}
	return OK;
}

static int
line_init(struct line *l, const char *s, size_t n)
{
	l->buf = NULL;
	l->len = 0;
	l->cap = 0;
	if (line_reserve(l, n) != OK)
		return ERR;
	if (n)
		memcpy(l->buf, s, n);
	l->buf[n] = '\0';
	l->len = n;
	return OK;
}

static int
lines_reserve(struct text *t, size_t need)
{
	if (need > t->cap) {
		size_t cap = t->cap ? t->cap : 32;
		struct line *p;

		while (cap < need)
			cap *= 2;
		p = realloc(t->lines, cap * sizeof(*p));
		if (!p)
			return ERR;
		t->lines = p;
		t->cap = cap;
	}
	return OK;
}

/* Insert a fresh line initialized from s[0..n) at index idx. */
static int
lines_insert_at(struct text *t, size_t idx, const char *s, size_t n)
{
	if (idx > t->nlines)
		return ERR;
	if (lines_reserve(t, t->nlines + 1) != OK)
		return ERR;
	if (line_init(&t->lines[t->nlines], s, n) != OK)
		return ERR;
	/* the new line was built at the end; rotate it into place */
	if (idx < t->nlines) {
		struct line tmp = t->lines[t->nlines];

		memmove(&t->lines[idx + 1], &t->lines[idx],
		    (t->nlines - idx) * sizeof(struct line));
		t->lines[idx] = tmp;
	}
	t->nlines++;
	return OK;
}

static void
lines_remove_at(struct text *t, size_t idx)
{
	if (idx >= t->nlines)
		return;
	free(t->lines[idx].buf);
	memmove(&t->lines[idx], &t->lines[idx + 1],
	    (t->nlines - idx - 1) * sizeof(struct line));
	t->nlines--;
}

static void
text_clear(struct text *t)
{
	size_t i;

	for (i = 0; i < t->nlines; i++)
		free(t->lines[i].buf);
	t->nlines = 0;
}

/****************************************************************
 * Lifecycle
 ****************************************************************/

struct text *
text_new(void)
{
	struct text *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	if (lines_insert_at(t, 0, "", 0) != OK) {
		free(t->lines);
		free(t);
		return NULL;
	}
	t->final_newline = 0;
	t->dirty = 0;
	return t;
}

void
text_free(struct text *t)
{
	if (!t)
		return;
	text_clear(t);
	estack_free(&t->undo);
	estack_free(&t->redo);
	free(t->lines);
	free(t);
}

/****************************************************************
 * Load and save
 ****************************************************************/

int
text_load(struct text *t, const char *path)
{
	FILE *fp;
	char *data = NULL;
	size_t len = 0, cap = 0;
	int c;
	size_t start, i;
	int saved_errno;

	fp = fopen(path, "rb");
	if (!fp)
		return ERR;

	while ((c = fgetc(fp)) != EOF) {
		if (len + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 4096;
			char *p = realloc(data, ncap);

			if (!p) {
				saved_errno = errno;
				free(data);
				fclose(fp);
				errno = saved_errno;
				return ERR;
			}
			data = p;
			cap = ncap;
		}
		data[len++] = (char)c;
	}
	if (ferror(fp)) {
		saved_errno = errno;
		free(data);
		fclose(fp);
		errno = saved_errno;
		return ERR;
	}
	fclose(fp);

	text_clear(t);
	estack_clear(&t->undo);		/* history does not span a reload */
	estack_clear(&t->redo);
	t->can_coalesce = 0;
	t->final_newline = (len > 0 && data[len - 1] == '\n');

	start = 0;
	for (i = 0; i < len; i++) {
		if (data[i] == '\n') {
			if (lines_insert_at(t, t->nlines, data + start,
			    i - start) != OK) {
				free(data);
				errno = ENOMEM;
				return ERR;
			}
			start = i + 1;
		}
	}
	/* trailing bytes with no newline form a final line */
	if (start < len) {
		if (lines_insert_at(t, t->nlines, data + start,
		    len - start) != OK) {
			free(data);
			errno = ENOMEM;
			return ERR;
		}
	}
	free(data);

	if (t->nlines == 0)
		lines_insert_at(t, 0, "", 0);	/* empty file: one line */
	t->dirty = 0;
	return OK;
}

int
text_save(struct text *t, const char *path)
{
	FILE *fp;
	size_t i;
	int saved_errno;

	fp = fopen(path, "wb");
	if (!fp)
		return ERR;

	for (i = 0; i < t->nlines; i++) {
		if (t->lines[i].len &&
		    fwrite(t->lines[i].buf, 1, t->lines[i].len, fp)
		    != t->lines[i].len)
			goto werr;
		/* newline between lines, and after the last only when the
		 * source carried a trailing newline */
		if (i + 1 < t->nlines || t->final_newline) {
			if (fputc('\n', fp) == EOF)
				goto werr;
		}
	}
	if (fclose(fp) != 0)
		return ERR;
	t->dirty = 0;
	return OK;

werr:
	saved_errno = errno;
	fclose(fp);
	errno = saved_errno;
	return ERR;
}

/****************************************************************
 * Queries
 ****************************************************************/

size_t
text_lines(const struct text *t)
{
	return t->nlines;
}

const char *
text_line(const struct text *t, size_t line, size_t *len)
{
	if (line >= t->nlines)
		return NULL;
	if (len)
		*len = t->lines[line].len;
	return t->lines[line].buf;
}

size_t
text_line_len(const struct text *t, size_t line)
{
	if (line >= t->nlines)
		return 0;
	return t->lines[line].len;
}

int
text_dirty(const struct text *t)
{
	return t->dirty;
}

/****************************************************************
 * Undo primitives
 ****************************************************************/

static void
estack_clear(struct estack *s)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		free(s->v[i].bytes);
	s->n = 0;
}

static void
estack_free(struct estack *s)
{
	estack_clear(s);
	free(s->v);
	s->v = NULL;
	s->cap = 0;
}

static int
estack_push(struct estack *s, const struct erec *rec)
{
	if (s->n >= s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 32;
		struct erec *p = realloc(s->v, cap * sizeof(*p));

		if (!p)
			return ERR;
		s->v = p;
		s->cap = cap;
	}
	s->v[s->n++] = *rec;
	return OK;
}

/* Apply one primitive to the buffer and fill inv with the primitive that
 * reverses it. inv->bytes, when set, is owned by the caller. */
static int
apply_op(struct text *t, const struct erec *in, struct erec *inv)
{
	inv->bytes = NULL;
	inv->n = 0;
	inv->col = 0;

	switch (in->op) {
	case OP_INSERT: {
		struct line *l;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		if (line_reserve(l, l->len + in->n) != OK)
			return ERR;
		memmove(l->buf + in->col + in->n, l->buf + in->col,
		    l->len - in->col);
		memcpy(l->buf + in->col, in->bytes, in->n);
		l->len += in->n;
		l->buf[l->len] = '\0';
		inv->op = OP_DELETE;
		inv->line = in->line;
		inv->col = in->col;
		inv->n = in->n;
		break;
	}
	case OP_DELETE: {
		struct line *l;
		size_t nn;
		char *cap;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		nn = in->n;
		if (nn > l->len - in->col)
			nn = l->len - in->col;
		cap = malloc(nn ? nn : 1);
		if (!cap)
			return ERR;
		memcpy(cap, l->buf + in->col, nn);
		memmove(l->buf + in->col, l->buf + in->col + nn,
		    l->len - in->col - nn);
		l->len -= nn;
		l->buf[l->len] = '\0';
		inv->op = OP_INSERT;
		inv->line = in->line;
		inv->col = in->col;
		inv->n = nn;
		inv->bytes = cap;
		break;
	}
	case OP_SPLIT: {
		struct line *l;

		if (in->line >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		if (in->col > l->len)
			return ERR;
		if (lines_insert_at(t, in->line + 1, l->buf + in->col,
		    l->len - in->col) != OK)
			return ERR;
		l = &t->lines[in->line];	/* array may have moved */
		l->len = in->col;
		l->buf[in->col] = '\0';
		inv->op = OP_JOIN;
		inv->line = in->line;
		break;
	}
	case OP_JOIN: {
		struct line *l, *next;
		size_t boundary;

		if (in->line + 1 >= t->nlines)
			return ERR;
		l = &t->lines[in->line];
		next = &t->lines[in->line + 1];
		boundary = l->len;
		if (next->len) {
			if (line_reserve(l, l->len + next->len) != OK)
				return ERR;
			memcpy(l->buf + l->len, next->buf, next->len);
			l->len += next->len;
			l->buf[l->len] = '\0';
		}
		lines_remove_at(t, in->line + 1);
		inv->op = OP_SPLIT;
		inv->line = in->line;
		inv->col = boundary;
		break;
	}
	default:
		return ERR;
	}
	t->dirty = 1;
	return OK;
}

/* Record an inverse on the undo stack, extending the top record when a
 * typing run continues, and drop the now-stale redo stack. */
static void
record_undo(struct text *t, struct erec *inv)
{
	estack_clear(&t->redo);

	if (t->can_coalesce && inv->op == OP_DELETE && t->undo.n > 0) {
		struct erec *top = &t->undo.v[t->undo.n - 1];

		if (top->op == OP_DELETE && top->line == inv->line &&
		    top->col + top->n == inv->col) {
			top->n += inv->n;	/* inv carries no bytes */
			return;
		}
	}
	if (estack_push(&t->undo, inv) != OK)
		free(inv->bytes);	/* drop the record rather than leak */
}

/* Fill the cursor location a change should move to, given the primitive
 * that was applied and the inverse it produced. */
static void
cursor_after(const struct erec *applied, const struct erec *inv,
    size_t *line, size_t *col)
{
	size_t cl = applied->line, cc = applied->col;

	switch (applied->op) {
	case OP_INSERT:
		cc = applied->col + applied->n;
		break;
	case OP_DELETE:
		cc = applied->col;
		break;
	case OP_SPLIT:
		cl = applied->line + 1;
		cc = 0;
		break;
	case OP_JOIN:
		cc = inv->col;		/* the boundary the join merged at */
		break;
	}
	if (line)
		*line = cl;
	if (col)
		*col = cc;
}

/****************************************************************
 * Editing
 ****************************************************************/

int
text_insert(struct text *t, size_t line, size_t col,
    const char *s, size_t n)
{
	struct erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;
	if (n == 0)
		return OK;

	in.op = OP_INSERT;
	in.line = line;
	in.col = col;
	in.bytes = (char *)s;
	in.n = n;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	t->can_coalesce = 1;
	return OK;
}

int
text_delete(struct text *t, size_t line, size_t col, size_t n)
{
	struct erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;
	if (n == 0 || col == t->lines[line].len)
		return OK;

	in.op = OP_DELETE;
	in.line = line;
	in.col = col;
	in.bytes = NULL;
	in.n = n;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

int
text_split(struct text *t, size_t line, size_t col)
{
	struct erec in, inv;

	if (line >= t->nlines || col > t->lines[line].len)
		return ERR;

	in.op = OP_SPLIT;
	in.line = line;
	in.col = col;
	in.bytes = NULL;
	in.n = 0;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

int
text_join(struct text *t, size_t line)
{
	struct erec in, inv;

	if (line + 1 >= t->nlines)
		return ERR;

	in.op = OP_JOIN;
	in.line = line;
	in.col = 0;
	in.bytes = NULL;
	in.n = 0;
	t->can_coalesce = 0;
	if (apply_op(t, &in, &inv) != OK)
		return ERR;
	record_undo(t, &inv);
	return OK;
}

/****************************************************************
 * Undo and redo
 ****************************************************************/

int
text_can_undo(const struct text *t)
{
	return t->undo.n > 0;
}

int
text_can_redo(const struct text *t)
{
	return t->redo.n > 0;
}

void
text_undo_boundary(struct text *t)
{
	t->can_coalesce = 0;
}

/* Shared engine for undo and redo: pop one record from `from`, apply it,
 * and push the resulting inverse onto `to`. */
static int
undo_step(struct text *t, struct estack *from, struct estack *to,
    size_t *line, size_t *col)
{
	struct erec rec, inv;

	t->can_coalesce = 0;
	if (from->n == 0)
		return ERR;
	rec = from->v[--from->n];
	if (apply_op(t, &rec, &inv) != OK) {
		free(rec.bytes);
		return ERR;
	}
	if (estack_push(to, &inv) != OK)
		free(inv.bytes);
	cursor_after(&rec, &inv, line, col);
	free(rec.bytes);
	return OK;
}

int
text_undo(struct text *t, size_t *line, size_t *col)
{
	return undo_step(t, &t->undo, &t->redo, line, col);
}

int
text_redo(struct text *t, size_t *line, size_t *col)
{
	return undo_step(t, &t->redo, &t->undo, line, col);
}
