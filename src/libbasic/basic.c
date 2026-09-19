/* basic.c : a small BASIC-style expression and program interpreter */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "basic.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VAR_NAME_MAX 32

struct var {
	char			name[VAR_NAME_MAX];
	struct basic_value	val;
};

struct progline {
	char	*text;		/* statement source, owned */
};

struct userfn {
	char	name[VAR_NAME_MAX];
	char	param[VAR_NAME_MAX];
	char	*body;		/* expression source, owned */
};

struct basic {
	struct var	*vars;
	size_t		nvars;
	size_t		cap;

	struct progline	*prog;		/* program lines, in file order */
	size_t		nprog;
	size_t		progcap;

	struct userfn	*fns;		/* user-defined functions */
	size_t		nfns;
	size_t		fncap;

	int		depth;		/* function-call recursion guard */
};

/****************************************************************
 * Values
 ****************************************************************/

void
basic_value_free(struct basic_value *v)
{
	if (v->type == BV_VEC || v->type == BV_MAT)
		free(v->data);
	v->data = NULL;
	v->type = BV_NUM;
	v->num = 0;
	v->rows = v->cols = 0;
}

static struct basic_value
bv_num(double x)
{
	struct basic_value v;

	memset(&v, 0, sizeof(v));
	v.type = BV_NUM;
	v.num = x;
	return v;
}

static struct basic_value
bv_err(const char *msg)
{
	struct basic_value v;

	memset(&v, 0, sizeof(v));
	v.type = BV_ERR;
	snprintf(v.err, sizeof(v.err), "%s", msg);
	return v;
}

/* Allocate a vector of n elements (or a rows x cols matrix), zero-filled. */
static struct basic_value
bv_alloc(enum basic_type type, int rows, int cols)
{
	struct basic_value v;
	size_t n = (size_t)rows * (size_t)cols;

	memset(&v, 0, sizeof(v));
	v.data = calloc(n ? n : 1, sizeof(double));
	if (!v.data)
		return bv_err("out of memory");
	v.type = type;
	v.rows = rows;
	v.cols = cols;
	return v;
}

/* A deep copy; numbers and errors own nothing and copy by value. */
static struct basic_value
bv_copy(const struct basic_value *s)
{
	struct basic_value v = *s;

	if (s->type == BV_VEC || s->type == BV_MAT) {
		size_t n = (size_t)s->rows * (size_t)s->cols;

		v.data = malloc((n ? n : 1) * sizeof(double));
		if (!v.data)
			return bv_err("out of memory");
		memcpy(v.data, s->data, n * sizeof(double));
	}
	return v;
}

/****************************************************************
 * Interpreter state
 ****************************************************************/

struct basic *
basic_new(void)
{
	return calloc(1, sizeof(struct basic));
}

void
basic_free(struct basic *b)
{
	size_t i;

	if (!b)
		return;
	for (i = 0; i < b->nvars; i++)
		basic_value_free(&b->vars[i].val);
	free(b->vars);
	for (i = 0; i < b->nprog; i++)
		free(b->prog[i].text);
	free(b->prog);
	for (i = 0; i < b->nfns; i++)
		free(b->fns[i].body);
	free(b->fns);
	free(b);
}

/* Fold a name to upper case in place, bounded by VAR_NAME_MAX. */
static void
name_norm(char *dst, const char *src)
{
	size_t i;

	for (i = 0; src[i] && i < VAR_NAME_MAX - 1; i++)
		dst[i] = (char)toupper((unsigned char)src[i]);
	dst[i] = '\0';
}

/* Store val under name, taking ownership of val and freeing any prior. */
static void
set_var_val(struct basic *b, const char *name, struct basic_value val)
{
	char key[VAR_NAME_MAX];
	size_t i;

	name_norm(key, name);
	for (i = 0; i < b->nvars; i++) {
		if (strcmp(b->vars[i].name, key) == 0) {
			basic_value_free(&b->vars[i].val);
			b->vars[i].val = val;
			return;
		}
	}
	if (b->nvars >= b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 16;
		struct var *p = realloc(b->vars, cap * sizeof(*p));

		if (!p) {
			basic_value_free(&val);
			return;
		}
		b->vars = p;
		b->cap = cap;
	}
	snprintf(b->vars[b->nvars].name, VAR_NAME_MAX, "%s", key);
	b->vars[b->nvars].val = val;
	b->nvars++;
}

/* Return a deep copy of a variable's value, or 0 if it is unset. */
static int
get_var_copy(struct basic *b, const char *name, struct basic_value *out)
{
	char key[VAR_NAME_MAX];
	size_t i;

	name_norm(key, name);
	for (i = 0; i < b->nvars; i++) {
		if (strcmp(b->vars[i].name, key) == 0) {
			*out = bv_copy(&b->vars[i].val);
			return 1;
		}
	}
	return 0;
}

static struct userfn *
userfn_find(struct basic *b, const char *name)
{
	char key[VAR_NAME_MAX];
	size_t i;

	name_norm(key, name);
	for (i = 0; i < b->nfns; i++) {
		if (strcmp(b->fns[i].name, key) == 0)
			return &b->fns[i];
	}
	return NULL;
}

/* Define or replace a user function. Takes ownership of body. */
static int
userfn_set(struct basic *b, const char *name, const char *param, char *body)
{
	struct userfn *f = userfn_find(b, name);

	if (f) {
		free(f->body);
		name_norm(f->param, param);
		f->body = body;
		return 0;
	}
	if (b->nfns >= b->fncap) {
		size_t cap = b->fncap ? b->fncap * 2 : 8;
		struct userfn *p = realloc(b->fns, cap * sizeof(*p));

		if (!p) {
			free(body);
			return -1;
		}
		b->fns = p;
		b->fncap = cap;
	}
	f = &b->fns[b->nfns++];
	name_norm(f->name, name);
	name_norm(f->param, param);
	f->body = body;
	return 0;
}

void
basic_set_var(struct basic *b, const char *name, double v)
{
	set_var_val(b, name, bv_num(v));
}

int
basic_get_var(struct basic *b, const char *name, double *out)
{
	char key[VAR_NAME_MAX];
	size_t i;

	name_norm(key, name);
	for (i = 0; i < b->nvars; i++) {
		if (strcmp(b->vars[i].name, key) == 0) {
			if (b->vars[i].val.type != BV_NUM)
				return 0;	/* not a scalar */
			if (out)
				*out = b->vars[i].val.num;
			return 1;
		}
	}
	return 0;
}

/****************************************************************
 * Builtin functions
 ****************************************************************/

static double
fn_sgn(double x)
{
	return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0);
}

static double
fn_int(double x)
{
	return floor(x);
}

static const struct builtin {
	const char	*name;
	int		arity;
	double		(*fn)(double);
} builtins[] = {
	{ "SIN",  1, sin },
	{ "COS",  1, cos },
	{ "TAN",  1, tan },
	{ "ATN",  1, atan },
	{ "SQR",  1, sqrt },
	{ "EXP",  1, exp },
	{ "LOG",  1, log },
	{ "ABS",  1, fabs },
	{ "INT",  1, fn_int },
	{ "SGN",  1, fn_sgn },
};

#define BUILTIN_COUNT ((int)(sizeof(builtins) / sizeof(builtins[0])))

/****************************************************************
 * Tokenizer
 ****************************************************************/

enum tok {
	T_EOF,
	T_NUM,
	T_IDENT,
	T_PLUS,
	T_MINUS,
	T_STAR,
	T_SLASH,
	T_CARET,
	T_LPAREN,
	T_RPAREN,
	T_LBRACK,
	T_RBRACK,
	T_COMMA,
	T_EQ,
	T_NE,
	T_LT,
	T_LE,
	T_GT,
	T_GE,
	T_BAD,
};

struct parser {
	const char	*src;
	size_t		pos;
	enum tok	tok;		/* current token */
	double		num;		/* T_NUM value */
	char		ident[VAR_NAME_MAX];
	struct basic	*b;
	int		error;
	char		errmsg[120];
};

static void
perror_set(struct parser *p, const char *msg)
{
	if (!p->error) {
		p->error = 1;
		snprintf(p->errmsg, sizeof(p->errmsg), "%s", msg);
	}
}

static void
next(struct parser *p)
{
	const char *s = p->src;

	while (isspace((unsigned char)s[p->pos]))
		p->pos++;

	char c = s[p->pos];

	if (c == '\0') {
		p->tok = T_EOF;
		return;
	}
	if (isdigit((unsigned char)c) || c == '.') {
		char *end;

		p->num = strtod(s + p->pos, &end);
		p->pos = (size_t)(end - s);
		p->tok = T_NUM;
		return;
	}
	if (isalpha((unsigned char)c)) {
		size_t i = 0;

		while (isalnum((unsigned char)s[p->pos]) || s[p->pos] == '_') {
			if (i < VAR_NAME_MAX - 1)
				p->ident[i++] = (char)toupper(
				    (unsigned char)s[p->pos]);
			p->pos++;
		}
		p->ident[i] = '\0';
		p->tok = T_IDENT;
		return;
	}

	p->pos++;
	switch (c) {
	case '+': p->tok = T_PLUS; return;
	case '-': p->tok = T_MINUS; return;
	case '*': p->tok = T_STAR; return;
	case '/': p->tok = T_SLASH; return;
	case '^': p->tok = T_CARET; return;
	case '(': p->tok = T_LPAREN; return;
	case ')': p->tok = T_RPAREN; return;
	case '[': p->tok = T_LBRACK; return;
	case ']': p->tok = T_RBRACK; return;
	case ',': p->tok = T_COMMA; return;
	case '=': p->tok = T_EQ; return;
	case '<':
		if (s[p->pos] == '=') { p->pos++; p->tok = T_LE; }
		else if (s[p->pos] == '>') { p->pos++; p->tok = T_NE; }
		else p->tok = T_LT;
		return;
	case '>':
		if (s[p->pos] == '=') { p->pos++; p->tok = T_GE; }
		else p->tok = T_GT;
		return;
	default:
		p->tok = T_BAD;
		perror_set(p, "unexpected character");
		return;
	}
}

/****************************************************************
 * Pratt expression parser and evaluator
 *
 * Binding powers: comparison 10, +/- 20, * / 30, unary minus 35,
 * ^ 40 (right associative). Unary minus sits below ^ so -3^2 is -(3^2).
 ****************************************************************/

#define BP_UNARY 35
#define BP_POW   40

static struct basic_value parse_expr(struct parser *p, int min_bp);

/* Report an error, free two operands, and return a dummy number. */
static struct basic_value
op_err(struct parser *p, const char *msg, struct basic_value *a,
    struct basic_value *b)
{
	perror_set(p, msg);
	if (a)
		basic_value_free(a);
	if (b)
		basic_value_free(b);
	return bv_num(0);
}

/* Left binding power of an infix operator, or 0 if the token is not one. */
static int
infix_bp(enum tok t)
{
	switch (t) {
	case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
		return 10;
	case T_PLUS: case T_MINUS:
		return 20;
	case T_STAR: case T_SLASH:
		return 30;
	case T_CARET:
		return BP_POW;
	default:
		return 0;
	}
}

static int
is_compare(enum tok t)
{
	return t == T_EQ || t == T_NE || t == T_LT || t == T_LE ||
	    t == T_GT || t == T_GE;
}

/* One elementwise arithmetic operation, flagging division by zero. */
static double
elem_arith(struct parser *p, enum tok op, double x, double y)
{
	switch (op) {
	case T_PLUS:	return x + y;
	case T_MINUS:	return x - y;
	case T_STAR:	return x * y;
	case T_SLASH:
		if (y == 0.0) {
			perror_set(p, "division by zero");
			return 0.0;
		}
		return x / y;
	default:
		return 0.0;
	}
}

/* Apply an infix operator, consuming a and b. Comparisons and ^ require
 * numbers; + - * / are elementwise with scalar broadcast. */
static struct basic_value
bv_binop(struct parser *p, enum tok op, struct basic_value a,
    struct basic_value b)
{
	struct basic_value r;
	size_t i, n;

	if (op == T_CARET || is_compare(op)) {
		double x, y, res = 0;

		if (a.type != BV_NUM || b.type != BV_NUM)
			return op_err(p, "operator needs numbers", &a, &b);
		x = a.num;
		y = b.num;
		switch (op) {
		case T_CARET:	res = pow(x, y); break;
		case T_EQ:	res = x == y; break;
		case T_NE:	res = x != y; break;
		case T_LT:	res = x < y; break;
		case T_LE:	res = x <= y; break;
		case T_GT:	res = x > y; break;
		case T_GE:	res = x >= y; break;
		default:	break;
		}
		return bv_num(res);
	}

	if (a.type == BV_NUM && b.type == BV_NUM) {
		double res = elem_arith(p, op, a.num, b.num);

		return p->error ? bv_num(0) : bv_num(res);
	}

	/* scalar broadcast over a vector or matrix */
	if (a.type == BV_NUM || b.type == BV_NUM) {
		struct basic_value *arr = a.type == BV_NUM ? &b : &a;
		double scalar = a.type == BV_NUM ? a.num : b.num;
		int left = a.type == BV_NUM;

		r = bv_alloc(arr->type, arr->rows, arr->cols);
		if (r.type == BV_ERR)
			return op_err(p, "out of memory", &a, &b);
		n = (size_t)arr->rows * (size_t)arr->cols;
		for (i = 0; i < n && !p->error; i++) {
			double e = arr->data[i];

			r.data[i] = left ? elem_arith(p, op, scalar, e)
			    : elem_arith(p, op, e, scalar);
		}
		basic_value_free(&a);
		basic_value_free(&b);
		if (p->error) {
			basic_value_free(&r);
			return bv_num(0);
		}
		return r;
	}

	/* two arrays: shapes must match exactly */
	if (a.type != b.type || a.rows != b.rows || a.cols != b.cols)
		return op_err(p, "shape mismatch", &a, &b);

	r = bv_alloc(a.type, a.rows, a.cols);
	if (r.type == BV_ERR)
		return op_err(p, "out of memory", &a, &b);
	n = (size_t)a.rows * (size_t)a.cols;
	for (i = 0; i < n && !p->error; i++)
		r.data[i] = elem_arith(p, op, a.data[i], b.data[i]);
	basic_value_free(&a);
	basic_value_free(&b);
	if (p->error) {
		basic_value_free(&r);
		return bv_num(0);
	}
	return r;
}

/* Negate a value in place (elementwise for vectors and matrices). */
static struct basic_value
bv_neg(struct basic_value v)
{
	size_t i, n;

	if (v.type == BV_NUM)
		v.num = -v.num;
	else if (v.type == BV_VEC || v.type == BV_MAT) {
		n = (size_t)v.rows * (size_t)v.cols;
		for (i = 0; i < n; i++)
			v.data[i] = -v.data[i];
	}
	return v;
}

/****************************************************************
 * Vector and matrix literals, indexing, and builtins
 ****************************************************************/

/* Parse a [ ... ] literal: a vector of scalars, or a matrix when the
 * elements are themselves vectors of equal length. Consumes the '['. */
static struct basic_value
parse_bracket(struct parser *p)
{
	struct basic_value first;

	next(p);			/* eat '[' */
	if (p->tok == T_RBRACK) {
		next(p);
		return op_err(p, "empty literal", NULL, NULL);
	}
	first = parse_expr(p, 0);
	if (p->error)
		return op_err(p, p->errmsg, &first, NULL);

	if (first.type == BV_VEC) {			/* matrix of rows */
		int cols = first.cols, rows = 1;
		double *data = malloc((size_t)cols * sizeof(double));
		struct basic_value m;

		if (!data)
			return op_err(p, "out of memory", &first, NULL);
		memcpy(data, first.data, (size_t)cols * sizeof(double));
		basic_value_free(&first);
		while (p->tok == T_COMMA) {
			struct basic_value row;
			double *nd;

			next(p);
			row = parse_expr(p, 0);
			if (p->error || row.type != BV_VEC ||
			    row.cols != cols) {
				if (!p->error)
					perror_set(p, "ragged matrix");
				basic_value_free(&row);
				free(data);
				return bv_num(0);
			}
			nd = realloc(data,
			    (size_t)(rows + 1) * cols * sizeof(double));
			if (!nd) {
				basic_value_free(&row);
				free(data);
				return op_err(p, "out of memory", NULL, NULL);
			}
			data = nd;
			memcpy(data + (size_t)rows * cols, row.data,
			    (size_t)cols * sizeof(double));
			basic_value_free(&row);
			rows++;
		}
		if (p->tok != T_RBRACK) {
			free(data);
			return op_err(p, "missing ']'", NULL, NULL);
		}
		next(p);
		memset(&m, 0, sizeof(m));
		m.type = BV_MAT;
		m.rows = rows;
		m.cols = cols;
		m.data = data;
		return m;
	}
	if (first.type == BV_NUM) {			/* vector of scalars */
		int n = 1, cap = 8;
		double *data = malloc((size_t)cap * sizeof(double));
		struct basic_value v;

		if (!data)
			return op_err(p, "out of memory", NULL, NULL);
		data[0] = first.num;
		while (p->tok == T_COMMA) {
			struct basic_value e;

			next(p);
			e = parse_expr(p, 0);
			if (p->error || e.type != BV_NUM) {
				if (!p->error)
					perror_set(p, "vector needs numbers");
				basic_value_free(&e);
				free(data);
				return bv_num(0);
			}
			if (n >= cap) {
				double *nd = realloc(data,
				    (size_t)(cap *= 2) * sizeof(double));

				if (!nd) {
					free(data);
					return op_err(p, "out of memory",
					    NULL, NULL);
				}
				data = nd;
			}
			data[n++] = e.num;
		}
		if (p->tok != T_RBRACK) {
			free(data);
			return op_err(p, "missing ']'", NULL, NULL);
		}
		next(p);
		memset(&v, 0, sizeof(v));
		v.type = BV_VEC;
		v.rows = 1;
		v.cols = n;
		v.data = data;
		return v;
	}
	return op_err(p, "bad literal element", &first, NULL);
}

/* Index a vector v(i) or matrix m(i, j), 1-based. Consumes arr and the
 * argument list; current token is '('. */
static struct basic_value
index_value(struct parser *p, struct basic_value arr)
{
	struct basic_value i1, i2;
	int r, c;

	next(p);			/* eat '(' */
	i1 = parse_expr(p, 0);
	if (p->error || i1.type != BV_NUM) {
		if (!p->error)
			perror_set(p, "index must be a number");
		basic_value_free(&i1);
		return op_err(p, p->errmsg, &arr, NULL);
	}

	if (arr.type == BV_VEC) {
		c = (int)i1.num;
		if (p->tok != T_RPAREN)
			return op_err(p, "vector takes one index", &arr, NULL);
		next(p);
		if (c < 1 || c > arr.cols)
			return op_err(p, "index out of range", &arr, NULL);
		{
			double val = arr.data[c - 1];

			basic_value_free(&arr);
			return bv_num(val);
		}
	}

	/* matrix: needs a second index */
	if (p->tok != T_COMMA)
		return op_err(p, "matrix takes two indices", &arr, NULL);
	next(p);
	i2 = parse_expr(p, 0);
	if (p->error || i2.type != BV_NUM) {
		if (!p->error)
			perror_set(p, "index must be a number");
		basic_value_free(&i2);
		return op_err(p, p->errmsg, &arr, NULL);
	}
	r = (int)i1.num;
	c = (int)i2.num;
	if (p->tok != T_RPAREN)
		return op_err(p, "missing ')'", &arr, NULL);
	next(p);
	if (r < 1 || r > arr.rows || c < 1 || c > arr.cols)
		return op_err(p, "index out of range", &arr, NULL);
	{
		double val = arr.data[(size_t)(r - 1) * arr.cols + (c - 1)];

		basic_value_free(&arr);
		return bv_num(val);
	}
}

static const char *const vec_builtins[] = {
	"ZEROS", "RANGE", "DOT", "MATMUL", "LEN",
};

static int
name_is_builtin(const char *name)
{
	int i;

	for (i = 0; i < BUILTIN_COUNT; i++) {
		if (strcmp(builtins[i].name, name) == 0)
			return 1;
	}
	for (i = 0; i < (int)(sizeof(vec_builtins) /
	    sizeof(vec_builtins[0])); i++) {
		if (strcmp(vec_builtins[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Dispatch a builtin over already-evaluated argument values. */
static struct basic_value
dispatch_builtin(struct parser *p, const char *name,
    struct basic_value *args, int argc)
{
	int i;

	for (i = 0; i < BUILTIN_COUNT; i++) {
		if (strcmp(builtins[i].name, name) != 0)
			continue;
		if (argc != builtins[i].arity)
			return op_err(p, "wrong argument count", NULL, NULL);
		if (args[0].type != BV_NUM)
			return op_err(p, "function needs a number",
			    NULL, NULL);
		return bv_num(builtins[i].fn(args[0].num));
	}

	if (strcmp(name, "ZEROS") == 0) {
		if (argc == 1 && args[0].type == BV_NUM)
			return bv_alloc(BV_VEC, 1, (int)args[0].num);
		if (argc == 2 && args[0].type == BV_NUM &&
		    args[1].type == BV_NUM)
			return bv_alloc(BV_MAT, (int)args[0].num,
			    (int)args[1].num);
		return op_err(p, "ZEROS(n) or ZEROS(r,c)", NULL, NULL);
	}
	if (strcmp(name, "RANGE") == 0) {
		double a, bb;
		int n, k;
		struct basic_value v;

		if (argc != 2 || args[0].type != BV_NUM ||
		    args[1].type != BV_NUM)
			return op_err(p, "RANGE(a,b)", NULL, NULL);
		a = args[0].num;
		bb = args[1].num;
		n = (int)floor(bb - a) + 1;
		if (n < 1)
			return op_err(p, "empty range", NULL, NULL);
		v = bv_alloc(BV_VEC, 1, n);
		if (v.type == BV_ERR)
			return v;
		for (k = 0; k < n; k++)
			v.data[k] = a + k;
		return v;
	}
	if (strcmp(name, "DOT") == 0) {
		double sum = 0;
		int k;

		if (argc != 2 || args[0].type != BV_VEC ||
		    args[1].type != BV_VEC || args[0].cols != args[1].cols)
			return op_err(p, "DOT needs two equal vectors",
			    NULL, NULL);
		for (k = 0; k < args[0].cols; k++)
			sum += args[0].data[k] * args[1].data[k];
		return bv_num(sum);
	}
	if (strcmp(name, "MATMUL") == 0) {
		struct basic_value m;
		int ar, ac, bc, ri, ci, k;

		if (argc != 2 || args[0].type != BV_MAT ||
		    args[1].type != BV_MAT ||
		    args[0].cols != args[1].rows)
			return op_err(p, "MATMUL shape mismatch", NULL, NULL);
		ar = args[0].rows;
		ac = args[0].cols;
		bc = args[1].cols;
		m = bv_alloc(BV_MAT, ar, bc);
		if (m.type == BV_ERR)
			return m;
		for (ri = 0; ri < ar; ri++)
			for (ci = 0; ci < bc; ci++) {
				double s = 0;

				for (k = 0; k < ac; k++)
					s += args[0].data[ri * ac + k] *
					    args[1].data[k * bc + ci];
				m.data[ri * bc + ci] = s;
			}
		return m;
	}
	if (strcmp(name, "LEN") == 0) {
		if (argc != 1 || args[0].type == BV_NUM)
			return op_err(p, "LEN needs a vector or matrix",
			    NULL, NULL);
		return bv_num((double)args[0].rows * args[0].cols);
	}
	return op_err(p, "unknown function", NULL, NULL);
}

static struct basic_value
call_builtin(struct parser *p, const char *name)
{
	struct basic_value args[4];
	struct basic_value r;
	int argc = 0, i;

	next(p);			/* eat '(' */
	if (p->tok != T_RPAREN) {
		args[argc++] = parse_expr(p, 0);
		while (p->tok == T_COMMA && argc < 4) {
			next(p);
			args[argc++] = parse_expr(p, 0);
		}
	}
	if (!p->error && p->tok != T_RPAREN)
		perror_set(p, "missing ')'");
	if (p->error) {
		for (i = 0; i < argc; i++)
			basic_value_free(&args[i]);
		return bv_num(0);
	}
	next(p);

	r = dispatch_builtin(p, name, args, argc);
	for (i = 0; i < argc; i++)
		basic_value_free(&args[i]);
	return r;
}

/* Call a user-defined function: bind its parameter to the argument,
 * evaluate its stored body, then restore the parameter. */
static struct basic_value
call_user_fn(struct parser *p, struct userfn *f)
{
	struct basic_value arg, old, result;
	char param[VAR_NAME_MAX], body[256];
	int had;

	snprintf(param, sizeof(param), "%s", f->param);
	snprintf(body, sizeof(body), "%s", f->body);

	next(p);			/* eat '(' */
	arg = parse_expr(p, 0);
	if (!p->error && p->tok != T_RPAREN)
		perror_set(p, "missing ')'");
	if (p->error) {
		basic_value_free(&arg);
		return bv_num(0);
	}
	next(p);

	if (p->b->depth > 64) {
		basic_value_free(&arg);
		return op_err(p, "recursion too deep", NULL, NULL);
	}

	had = get_var_copy(p->b, param, &old);
	set_var_val(p->b, param, arg);		/* binds and owns arg */
	p->b->depth++;
	result = basic_eval(p->b, body);
	p->b->depth--;
	if (had)
		set_var_val(p->b, param, old);	/* restore prior value */
	else
		basic_set_var(p->b, param, 0);

	if (result.type == BV_ERR) {
		perror_set(p, result.err);
		basic_value_free(&result);
		return bv_num(0);
	}
	return result;
}

static struct basic_value
parse_primary(struct parser *p)
{
	struct basic_value v;

	switch (p->tok) {
	case T_NUM:
		v = bv_num(p->num);
		next(p);
		return v;
	case T_LBRACK:
		return parse_bracket(p);
	case T_MINUS:
		next(p);
		return bv_neg(parse_expr(p, BP_UNARY));
	case T_PLUS:
		next(p);
		return parse_expr(p, BP_UNARY);
	case T_LPAREN:
		next(p);
		v = parse_expr(p, 0);
		if (p->tok != T_RPAREN)
			return op_err(p, "missing ')'", &v, NULL);
		next(p);
		return v;
	case T_IDENT: {
		char name[VAR_NAME_MAX];

		snprintf(name, sizeof(name), "%s", p->ident);
		next(p);
		if (p->tok == T_LPAREN) {
			struct userfn *f;

			if (name_is_builtin(name))
				return call_builtin(p, name);
			f = userfn_find(p->b, name);
			if (f)
				return call_user_fn(p, f);
			if (get_var_copy(p->b, name, &v)) {
				if (v.type == BV_NUM)
					return op_err(p,
					    "cannot index a number", &v, NULL);
				return index_value(p, v);
			}
			return op_err(p, "unknown function", NULL, NULL);
		}
		if (get_var_copy(p->b, name, &v))
			return v;
		return bv_num(0);		/* unset variable reads as 0 */
	}
	default:
		return op_err(p, "expected a value", NULL, NULL);
	}
}

static struct basic_value
parse_expr(struct parser *p, int min_bp)
{
	struct basic_value left = parse_primary(p);

	while (!p->error) {
		int bp = infix_bp(p->tok);
		enum tok op = p->tok;
		struct basic_value right;

		if (bp == 0 || bp < min_bp)
			break;
		next(p);
		right = parse_expr(p, op == T_CARET ? bp : bp + 1);
		left = bv_binop(p, op, left, right);
	}
	return left;
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct basic_value
basic_eval(struct basic *b, const char *expr)
{
	struct parser p;
	struct basic_value v;

	memset(&p, 0, sizeof(p));
	p.src = expr;
	p.b = b;
	next(&p);

	if (p.tok == T_EOF)
		return bv_err("empty expression");

	v = parse_expr(&p, 0);

	if (!p.error && p.tok != T_EOF)
		perror_set(&p, "trailing tokens");

	if (p.error) {
		basic_value_free(&v);
		return bv_err(p.errmsg);
	}
	return v;
}

/****************************************************************
 * Immediate-mode statements
 ****************************************************************/

/* Print a number as an integer when it has no fractional part, else with a
 * general format that avoids a trailing mantissa of zeros. */
static void
print_num(FILE *out, double v)
{
	if (v == floor(v) && fabs(v) < 1e15)
		fprintf(out, "%lld", (long long)v);
	else
		fprintf(out, "%.10g", v);
}

/* Print a value: a scalar, a vector as [a, b, c], or a matrix as
 * [[a, b], [c, d]]. */
static void
print_value(FILE *out, const struct basic_value *v)
{
	int r, c;

	if (v->type == BV_NUM) {
		print_num(out, v->num);
		return;
	}
	if (v->type == BV_VEC) {
		fputc('[', out);
		for (c = 0; c < v->cols; c++) {
			if (c)
				fputs(", ", out);
			print_num(out, v->data[c]);
		}
		fputc(']', out);
		return;
	}
	if (v->type == BV_MAT) {
		fputc('[', out);
		for (r = 0; r < v->rows; r++) {
			if (r)
				fputs(", ", out);
			fputc('[', out);
			for (c = 0; c < v->cols; c++) {
				if (c)
					fputs(", ", out);
				print_num(out, v->data[(size_t)r * v->cols + c]);
			}
			fputc(']', out);
		}
		fputc(']', out);
	}
}

static struct basic_result
result_ok(void)
{
	struct basic_result r;

	r.status = BAS_OK;
	r.msg[0] = '\0';
	return r;
}

static struct basic_result
result_err(const char *msg)
{
	struct basic_result r;

	r.status = BAS_ERROR;
	snprintf(r.msg, sizeof(r.msg), "%s", msg);
	return r;
}

/* Read a leading identifier (letters, digits, underscore) into word,
 * uppercased, and return the number of source bytes consumed. */
static size_t
lead_word(const char *s, char *word, size_t wordsz)
{
	size_t i = 0;

	while (isalpha((unsigned char)s[i]) ||
	    (i > 0 && (isalnum((unsigned char)s[i]) || s[i] == '_'))) {
		if (i < wordsz - 1)
			word[i] = (char)toupper((unsigned char)s[i]);
		i++;
	}
	word[i < wordsz ? i : wordsz - 1] = '\0';
	return i;
}

/* Execute PRINT: a list of string literals and expressions separated by ','
 * (a space between items) or ';' (nothing between). */
static struct basic_result
exec_print(struct basic *b, const char *s, FILE *out)
{
	while (*s) {
		while (isspace((unsigned char)*s))
			s++;
		if (*s == '\0')
			break;

		if (*s == '"') {
			s++;
			while (*s && *s != '"')
				fputc(*s++, out);
			if (*s == '"')
				s++;
		} else {
			char expr[256];
			size_t n = 0;
			int depth = 0;
			struct basic_value v;

			while (*s && (depth > 0 ||
			    (*s != ',' && *s != ';'))) {
				if (*s == '(' || *s == '[')
					depth++;
				else if (*s == ')' || *s == ']')
					depth--;
				if (n < sizeof(expr) - 1)
					expr[n++] = *s;
				s++;
			}
			expr[n] = '\0';
			v = basic_eval(b, expr);
			if (v.type == BV_ERR) {
				struct basic_result r = result_err(v.err);

				basic_value_free(&v);
				return r;
			}
			print_value(out, &v);
			basic_value_free(&v);
		}

		while (isspace((unsigned char)*s))
			s++;
		if (*s == ',') {
			fputc(' ', out);
			s++;
		} else if (*s == ';') {
			s++;
		} else {
			break;
		}
	}
	fputc('\n', out);
	return result_ok();
}

/* Execute an assignment: name = expr. */
static struct basic_result
exec_assign(struct basic *b, const char *name, const char *expr)
{
	struct basic_value v = basic_eval(b, expr);

	if (v.type == BV_ERR) {
		struct basic_result r = result_err(v.err);

		basic_value_free(&v);
		return r;
	}
	set_var_val(b, name, v);		/* takes ownership */
	return result_ok();
}

/* Handle an assignment statement, returning nonzero when the line was one
 * (LET given, or a bare "name = expr"). */
static int
try_assign(struct basic *b, const char *word, size_t wlen, const char *s,
    struct basic_result *r)
{
	char name[VAR_NAME_MAX];
	const char *rest = s + wlen;
	size_t nlen = wlen;

	if (strcmp(word, "LET") == 0) {
		while (isspace((unsigned char)*rest))
			rest++;
		nlen = lead_word(rest, name, sizeof(name));
		if (nlen == 0) {
			*r = result_err("expected a variable name");
			return 1;
		}
		rest += nlen;
	} else {
		snprintf(name, sizeof(name), "%s", word);
	}
	while (isspace((unsigned char)*rest))
		rest++;
	if (*rest != '=' || rest[1] == '=') {
		if (strcmp(word, "LET") == 0) {
			*r = result_err("expected '='");
			return 1;
		}
		return 0;		/* not an assignment */
	}
	*r = exec_assign(b, name, rest + 1);
	return 1;
}

/****************************************************************
 * Program store
 ****************************************************************/

/* Duplicate s with leading and trailing whitespace removed, or NULL if the
 * trimmed text is empty or allocation fails. */
static char *
dup_trim(const char *s)
{
	size_t len;
	char *d;

	while (isspace((unsigned char)*s))
		s++;
	len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		len--;
	if (len == 0)
		return NULL;
	d = malloc(len + 1);
	if (!d)
		return NULL;
	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

/* Duplicate s with only a trailing newline (CR/LF) removed, preserving blank
 * lines and any indentation. NULL only on allocation failure. */
static char *
dup_line(const char *s)
{
	size_t len = strlen(s);
	char *d;

	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		len--;
	d = malloc(len + 1);
	if (!d)
		return NULL;
	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

/* Append an owned string as the last program line. Takes ownership of text,
 * and on a growth failure frees it and returns -1. */
static int
prog_push(struct basic *b, char *text)
{
	if (b->nprog >= b->progcap) {
		size_t cap = b->progcap ? b->progcap * 2 : 32;
		struct progline *p = realloc(b->prog, cap * sizeof(*p));

		if (!p) {
			free(text);
			return -1;
		}
		b->prog = p;
		b->progcap = cap;
	}
	b->prog[b->nprog++].text = text;
	return 0;
}

/* Edit the program by file line number (1-based), the REPL's numeric syntax.
 * A blank statement deletes the line; an existing line is replaced; one past
 * the end appends. A number beyond that also appends, since LIST shows the
 * real numbering the user can then correct. */
static void
program_set_line(struct basic *b, int n, const char *text)
{
	char *d = dup_trim(text);

	if (n < 1) {
		free(d);
		return;
	}
	if (!d) {				/* blank: delete line n */
		if ((size_t)n <= b->nprog) {
			free(b->prog[n - 1].text);
			memmove(&b->prog[n - 1], &b->prog[n],
			    (b->nprog - (size_t)n) * sizeof(*b->prog));
			b->nprog--;
		}
		return;
	}
	if ((size_t)n <= b->nprog) {		/* replace line n */
		free(b->prog[n - 1].text);
		b->prog[n - 1].text = d;
		return;
	}
	prog_push(b, d);			/* append (frees d on failure) */
}

static void
program_clear(struct basic *b)
{
	size_t i;

	for (i = 0; i < b->nprog; i++)
		free(b->prog[i].text);
	b->nprog = 0;
}

/* LIST for the human: a 1-based line-number gutter tells you which N to edit.
 * SAVE and EDIT use basic_program_dump() instead, which omits the gutter. */
static void
program_list(struct basic *b, FILE *out)
{
	size_t i;

	for (i = 0; i < b->nprog; i++)
		fprintf(out, "%zu  %s\n", i + 1, b->prog[i].text);
}

void
basic_program_append(struct basic *b, const char *text)
{
	char *d = dup_line(text);

	if (d)
		prog_push(b, d);
}

void
basic_program_dump(struct basic *b, FILE *out)
{
	size_t i;

	for (i = 0; i < b->nprog; i++)
		fprintf(out, "%s\n", b->prog[i].text);
}

/****************************************************************
 * Program execution
 ****************************************************************/

struct forframe {
	char	var[VAR_NAME_MAX];
	double	limit;
	double	step;
	int	body;		/* index of the line after FOR */
};

struct runctx {
	int		cur;		/* index of the current line */
	int		next;		/* index to run next */
	int		stop;
	int		gstack[64];	/* GOSUB return indices */
	int		gsp;
	struct forframe	fstack[32];
	int		fsp;
};

/* Find kw as a standalone word in s (case-insensitive), or NULL. */
static const char *
find_keyword(const char *s, const char *kw)
{
	size_t klen = strlen(kw);
	size_t i;

	for (i = 0; s[i]; i++) {
		char before = i > 0 ? s[i - 1] : ' ';
		char after;

		if (strncasecmp(s + i, kw, klen) != 0)
			continue;
		after = s[i + klen];
		if (!isalnum((unsigned char)before) && before != '_' &&
		    !isalnum((unsigned char)after) && after != '_')
			return s + i;
	}
	return NULL;
}

static struct basic_result exec_stmt(struct basic *b, const char *text,
    FILE *in, FILE *out, struct runctx *rc);

/* If text begins with an identifier immediately followed by a colon, copy the
 * label name into out and return a pointer to the statement after the colon.
 * Otherwise set out to empty and return text unchanged. */
static const char *
parse_label(const char *text, char *out, size_t outsz)
{
	const char *s = text, *start;
	size_t n;

	out[0] = '\0';
	while (isspace((unsigned char)*s))
		s++;
	start = s;
	if (!isalpha((unsigned char)*s))
		return text;
	while (isalnum((unsigned char)*s) || *s == '_')
		s++;
	if (*s != ':')
		return text;
	n = (size_t)(s - start);
	if (n >= outsz)
		n = outsz - 1;
	memcpy(out, start, n);
	out[n] = '\0';
	s++;				/* skip the colon */
	while (isspace((unsigned char)*s))
		s++;
	return s;
}

/* Index of the program line labelled name (case-insensitive), or -1. */
static int
program_label_index(struct basic *b, const char *name)
{
	char lbl[VAR_NAME_MAX];
	size_t i;

	for (i = 0; i < b->nprog; i++) {
		parse_label(b->prog[i].text, lbl, sizeof(lbl));
		if (lbl[0] && strcasecmp(lbl, name) == 0)
			return (int)i;
	}
	return -1;
}

/* Jump the run to a labelled line, or fail if the label does not exist. */
static struct basic_result
do_goto(struct basic *b, struct runctx *rc, const char *name)
{
	int idx = program_label_index(b, name);

	if (idx < 0)
		return result_err("no such label");
	rc->next = idx;
	return result_ok();
}

/* Evaluate expr, requiring a scalar result; store it in out. */
static struct basic_result
eval_scalar(struct basic *b, const char *expr, double *out)
{
	struct basic_value v = basic_eval(b, expr);
	struct basic_result r;

	if (v.type == BV_ERR) {
		r = result_err(v.err);
		basic_value_free(&v);
		return r;
	}
	if (v.type != BV_NUM) {
		basic_value_free(&v);
		return result_err("expected a number");
	}
	*out = v.num;
	return result_ok();
}

static struct basic_result
exec_for(struct basic *b, const char *s, struct runctx *rc)
{
	char var[VAR_NAME_MAX];
	const char *to, *step;
	char expr[128];
	struct basic_result r;
	double start, limit, stepv = 1.0;
	size_t nlen;

	while (isspace((unsigned char)*s))
		s++;
	nlen = lead_word(s, var, sizeof(var));
	if (nlen == 0)
		return result_err("expected a loop variable");
	s += nlen;
	while (isspace((unsigned char)*s))
		s++;
	if (*s != '=')
		return result_err("expected '=' in FOR");
	s++;

	to = find_keyword(s, "TO");
	if (!to)
		return result_err("expected TO");
	snprintf(expr, sizeof(expr), "%.*s", (int)(to - s), s);
	r = eval_scalar(b, expr, &start);
	if (r.status != BAS_OK)
		return r;

	s = to + 2;
	step = find_keyword(s, "STEP");
	if (step) {
		snprintf(expr, sizeof(expr), "%.*s", (int)(step - s), s);
		r = eval_scalar(b, expr, &limit);
		if (r.status != BAS_OK)
			return r;
		r = eval_scalar(b, step + 4, &stepv);
		if (r.status != BAS_OK)
			return r;
	} else {
		r = eval_scalar(b, s, &limit);
		if (r.status != BAS_OK)
			return r;
	}
	if (stepv == 0.0)
		return result_err("FOR step of zero");
	if (rc->fsp >= (int)(sizeof(rc->fstack) / sizeof(rc->fstack[0])))
		return result_err("FOR nested too deep");

	basic_set_var(b, var, start);
	snprintf(rc->fstack[rc->fsp].var, VAR_NAME_MAX, "%s", var);
	rc->fstack[rc->fsp].limit = limit;
	rc->fstack[rc->fsp].step = stepv;
	rc->fstack[rc->fsp].body = rc->cur + 1;
	rc->fsp++;
	return result_ok();
}

static struct basic_result
exec_next(struct basic *b, struct runctx *rc)
{
	struct forframe *f;
	double v = 0;

	if (rc->fsp == 0)
		return result_err("NEXT without FOR");
	f = &rc->fstack[rc->fsp - 1];
	(void)basic_get_var(b, f->var, &v);
	v += f->step;
	basic_set_var(b, f->var, v);
	if (f->step > 0 ? v <= f->limit : v >= f->limit)
		rc->next = f->body;	/* loop again */
	else
		rc->fsp--;		/* fall out of the loop */
	return result_ok();
}

static struct basic_result
exec_if(struct basic *b, const char *s, FILE *in, FILE *out,
    struct runctx *rc)
{
	const char *then = find_keyword(s, "THEN");
	char cond[160];
	struct basic_result r;
	double truth = 0;

	if (!then)
		return result_err("expected THEN");
	snprintf(cond, sizeof(cond), "%.*s", (int)(then - s), s);
	r = eval_scalar(b, cond, &truth);
	if (r.status != BAS_OK)
		return r;
	if (truth == 0.0)
		return result_ok();		/* condition false: skip */

	s = then + 4;
	while (isspace((unsigned char)*s))
		s++;
	/* "THEN <label>" jumps when the rest is a lone, defined label */
	{
		char tgt[VAR_NAME_MAX];
		size_t tl = lead_word(s, tgt, sizeof(tgt));
		const char *rest = s + tl;

		while (isspace((unsigned char)*rest))
			rest++;
		if (tgt[0] && *rest == '\0' &&
		    program_label_index(b, tgt) >= 0)
			return do_goto(b, rc, tgt);
	}
	return exec_stmt(b, s, in, out, rc);	/* THEN <statement> */
}

static struct basic_result
exec_input(struct basic *b, const char *s, FILE *in)
{
	char var[VAR_NAME_MAX];
	char buf[256];
	struct basic_value v;
	size_t nlen;

	while (isspace((unsigned char)*s))
		s++;
	nlen = lead_word(s, var, sizeof(var));
	if (nlen == 0)
		return result_err("expected a variable name");

	if (!in || !fgets(buf, sizeof(buf), in)) {
		basic_set_var(b, var, 0.0);
		return result_ok();
	}
	v = basic_eval(b, buf);
	basic_set_var(b, var, v.type == BV_NUM ? v.num : 0.0);
	basic_value_free(&v);
	return result_ok();
}

/* DEF name(param) = body : define a user function. */
static struct basic_result
exec_def(struct basic *b, const char *s)
{
	char name[VAR_NAME_MAX], param[VAR_NAME_MAX];
	char *body;
	size_t n;

	while (isspace((unsigned char)*s))
		s++;
	n = lead_word(s, name, sizeof(name));
	if (n == 0)
		return result_err("expected a function name");
	s += n;
	while (isspace((unsigned char)*s))
		s++;
	if (*s != '(')
		return result_err("expected '('");
	s++;
	while (isspace((unsigned char)*s))
		s++;
	n = lead_word(s, param, sizeof(param));
	if (n == 0)
		return result_err("expected a parameter");
	s += n;
	while (isspace((unsigned char)*s))
		s++;
	if (*s != ')')
		return result_err("expected ')'");
	s++;
	while (isspace((unsigned char)*s))
		s++;
	if (*s != '=')
		return result_err("expected '='");
	s++;
	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return result_err("empty function body");
	body = strdup(s);
	if (!body)
		return result_err("out of memory");
	userfn_set(b, name, param, body);
	return result_ok();
}

/****************************************************************
 * ASCII-art graphing
 ****************************************************************/

#define PLOT_W    61
#define PLOT_H    17
#define PLOT_MAXW 78

/* Render sampled y values into an ASCII grid with zero axes. */
static void
draw_plot(FILE *out, const double *y, const int *valid, int w,
    double xmin, double xmax)
{
	char grid[PLOT_H][PLOT_MAXW];
	double ymin = 0, ymax = 0;
	int any = 0, i, r, zrow = -1, zcol = -1;

	for (i = 0; i < w; i++) {
		if (!valid[i])
			continue;
		if (!any || y[i] < ymin)
			ymin = y[i];
		if (!any || y[i] > ymax)
			ymax = y[i];
		any = 1;
	}
	if (!any) {
		fputs("(no plottable points)\n", out);
		return;
	}
	if (ymax == ymin) {
		ymax += 1;
		ymin -= 1;
	}

	for (r = 0; r < PLOT_H; r++)
		memset(grid[r], ' ', (size_t)w);

	if (0 >= ymin && 0 <= ymax)
		zrow = (int)lround(ymax / (ymax - ymin) * (PLOT_H - 1));
	if (zrow >= 0 && zrow < PLOT_H)
		for (i = 0; i < w; i++)
			grid[zrow][i] = '-';
	if (xmax > xmin && 0 >= xmin && 0 <= xmax)
		zcol = (int)lround((0 - xmin) / (xmax - xmin) * (w - 1));
	if (zcol >= 0 && zcol < w)
		for (r = 0; r < PLOT_H; r++)
			grid[r][zcol] = grid[r][zcol] == '-' ? '+' : '|';

	for (i = 0; i < w; i++) {
		if (!valid[i])
			continue;
		r = (int)lround((ymax - y[i]) / (ymax - ymin) * (PLOT_H - 1));
		if (r < 0)
			r = 0;
		if (r >= PLOT_H)
			r = PLOT_H - 1;
		grid[r][i] = '*';
	}

	for (r = 0; r < PLOT_H; r++) {
		fwrite(grid[r], 1, (size_t)w, out);
		fputc('\n', out);
	}
	fprintf(out, "x: %g .. %g   y: %g .. %g\n", xmin, xmax, ymin, ymax);
}

/* Plot expr as a function of X sampled across [a, b]. */
static struct basic_result
plot_function(struct basic *b, const char *expr, double a, double bb,
    FILE *out)
{
	double y[PLOT_W];
	int valid[PLOT_W];
	struct basic_value old;
	int i, had;

	had = get_var_copy(b, "X", &old);
	for (i = 0; i < PLOT_W; i++) {
		double x = a + (bb - a) * i / (PLOT_W - 1);
		struct basic_value v;

		basic_set_var(b, "X", x);
		v = basic_eval(b, expr);
		valid[i] = v.type == BV_NUM;
		y[i] = valid[i] ? v.num : 0;
		basic_value_free(&v);
	}
	if (had)
		set_var_val(b, "X", old);
	else
		basic_set_var(b, "X", 0);
	draw_plot(out, y, valid, PLOT_W, a, bb);
	return result_ok();
}

/* Plot a vector's elements against their positions. */
static void
plot_vector(FILE *out, const struct basic_value *v)
{
	double y[PLOT_MAXW];
	int valid[PLOT_MAXW];
	int i, w = v->cols > PLOT_MAXW ? PLOT_MAXW : v->cols;

	for (i = 0; i < w; i++) {
		y[i] = v->data[i];
		valid[i] = 1;
	}
	draw_plot(out, y, valid, w, 1, v->cols);
}

static struct basic_result
exec_plot(struct basic *b, const char *s, FILE *out)
{
	const char *from = find_keyword(s, "FROM");

	if (from) {
		char expr[256], astr[128];
		const char *to;
		double a, bb;
		struct basic_result r;

		snprintf(expr, sizeof(expr), "%.*s", (int)(from - s), s);
		to = find_keyword(from + 4, "TO");
		if (!to)
			return result_err("expected TO");
		snprintf(astr, sizeof(astr), "%.*s",
		    (int)(to - (from + 4)), from + 4);
		r = eval_scalar(b, astr, &a);
		if (r.status != BAS_OK)
			return r;
		r = eval_scalar(b, to + 2, &bb);
		if (r.status != BAS_OK)
			return r;
		return plot_function(b, expr, a, bb, out);
	} else {
		struct basic_value v = basic_eval(b, s);

		if (v.type == BV_ERR) {
			struct basic_result r = result_err(v.err);

			basic_value_free(&v);
			return r;
		}
		if (v.type != BV_VEC) {
			basic_value_free(&v);
			return result_err("PLOT needs FROM a TO b, or a vector");
		}
		plot_vector(out, &v);
		basic_value_free(&v);
		return result_ok();
	}
}

static struct basic_result
exec_stmt(struct basic *b, const char *text, FILE *in, FILE *out,
    struct runctx *rc)
{
	char word[VAR_NAME_MAX];
	const char *s = text;
	struct basic_result r;
	size_t wlen;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return result_ok();

	/* an optional "label:" prefix marks a jump target; skip past it */
	{
		char lbl[VAR_NAME_MAX];
		const char *after = parse_label(s, lbl, sizeof(lbl));

		if (lbl[0]) {
			s = after;
			if (*s == '\0')
				return result_ok();	/* label-only line */
		}
	}

	wlen = lead_word(s, word, sizeof(word));

	if (strcmp(word, "REM") == 0)
		return result_ok();
	if (strcmp(word, "QUIT") == 0 || strcmp(word, "EXIT") == 0 ||
	    strcmp(word, "BYE") == 0)
		return (struct basic_result){ .status = BAS_QUIT };
	if (strcmp(word, "PRINT") == 0)
		return exec_print(b, s + wlen, out);
	if (strcmp(word, "DEF") == 0)
		return exec_def(b, s + wlen);
	if (strcmp(word, "PLOT") == 0)
		return exec_plot(b, s + wlen, out);

	/* flow control is only meaningful inside a running program */
	if (strcmp(word, "GOTO") == 0 || strcmp(word, "GOSUB") == 0 ||
	    strcmp(word, "RETURN") == 0 || strcmp(word, "FOR") == 0 ||
	    strcmp(word, "NEXT") == 0 || strcmp(word, "IF") == 0 ||
	    strcmp(word, "INPUT") == 0 || strcmp(word, "END") == 0 ||
	    strcmp(word, "STOP") == 0) {
		if (!rc)
			return result_err("only valid in a program");
		if (strcmp(word, "GOTO") == 0 || strcmp(word, "GOSUB") == 0) {
			char tgt[VAR_NAME_MAX];
			const char *a = s + wlen;

			while (isspace((unsigned char)*a))
				a++;
			lead_word(a, tgt, sizeof(tgt));
			if (!tgt[0])
				return result_err("expected a label");
			if (strcmp(word, "GOSUB") == 0) {
				if (rc->gsp >= (int)(sizeof(rc->gstack) /
				    sizeof(rc->gstack[0])))
					return result_err(
					    "GOSUB nested too deep");
				rc->gstack[rc->gsp++] = rc->cur + 1;
			}
			return do_goto(b, rc, tgt);
		}
		if (strcmp(word, "RETURN") == 0) {
			if (rc->gsp == 0)
				return result_err("RETURN without GOSUB");
			rc->next = rc->gstack[--rc->gsp];
			return result_ok();
		}
		if (strcmp(word, "FOR") == 0)
			return exec_for(b, s + wlen, rc);
		if (strcmp(word, "NEXT") == 0)
			return exec_next(b, rc);
		if (strcmp(word, "IF") == 0)
			return exec_if(b, s + wlen, in, out, rc);
		if (strcmp(word, "INPUT") == 0)
			return exec_input(b, s + wlen, in);
		rc->stop = 1;			/* END or STOP */
		return result_ok();
	}

	if (try_assign(b, word, wlen, s, &r))
		return r;

	/* a bare expression: print its value in immediate mode only */
	if (rc)
		return result_err("expected a statement");
	{
		struct basic_value v = basic_eval(b, s);

		if (v.type == BV_ERR) {
			r = result_err(v.err);
			basic_value_free(&v);
			return r;
		}
		print_value(out, &v);
		basic_value_free(&v);
		fputc('\n', out);
	}
	return result_ok();
}

/* True if word (already uppercased) names a statement valid inside a program,
 * so a lone occurrence after THEN is a statement rather than a label jump. */
static int
is_stmt_keyword(const char *word)
{
	static const char *const kw[] = {
		"REM", "QUIT", "EXIT", "BYE", "PRINT", "DEF", "PLOT",
		"GOTO", "GOSUB", "RETURN", "FOR", "NEXT", "IF", "INPUT",
		"END", "STOP", "LET", "RUN", "LIST", "NEW",
	};
	size_t i;

	for (i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
		if (strcmp(word, kw[i]) == 0)
			return 1;
	return 0;
}

/* Check that any label the statement jumps to is defined. stmt has already had
 * its own leading label stripped; lineno is 1-based for the message. Sets *err
 * and returns -1 on an undefined target, else returns 0. */
static int
check_jump(struct basic *b, const char *stmt, int lineno,
    struct basic_result *err)
{
	char word[VAR_NAME_MAX], tgt[VAR_NAME_MAX];
	const char *s = stmt;
	size_t wlen, tl;

	while (isspace((unsigned char)*s))
		s++;
	wlen = lead_word(s, word, sizeof(word));

	/* Unwrap "IF ... THEN <clause>" and inspect the clause. */
	if (strcmp(word, "IF") == 0) {
		const char *then = find_keyword(s, "THEN");

		if (!then)
			return 0;		/* exec reports the missing THEN */
		s = then + 4;
		while (isspace((unsigned char)*s))
			s++;
		wlen = lead_word(s, word, sizeof(word));
		if (strcmp(word, "GOTO") != 0 && strcmp(word, "GOSUB") != 0) {
			/* the bare "THEN <label>" jump shorthand */
			const char *rest = s + wlen;

			while (isspace((unsigned char)*rest))
				rest++;
			if (word[0] && *rest == '\0' && !is_stmt_keyword(word) &&
			    program_label_index(b, word) < 0) {
				err->status = BAS_ERROR;
				snprintf(err->msg, sizeof(err->msg),
				    "line %d: no such label '%.*s'", lineno,
				    (int)wlen, s);
				return -1;
			}
			return 0;		/* THEN <statement>: nothing to check */
		}
	} else if (strcmp(word, "GOTO") != 0 && strcmp(word, "GOSUB") != 0) {
		return 0;			/* not a jump statement */
	}

	/* word is GOTO or GOSUB (possibly after THEN); read its target label. */
	s += wlen;
	while (isspace((unsigned char)*s))
		s++;
	tl = lead_word(s, tgt, sizeof(tgt));
	if (!tgt[0])
		return 0;			/* exec reports the missing label */
	if (program_label_index(b, tgt) < 0) {
		err->status = BAS_ERROR;
		snprintf(err->msg, sizeof(err->msg),
		    "line %d: no such label '%.*s'", lineno, (int)tl, s);
		return -1;
	}
	return 0;
}

/* Validate the program before running it: report a duplicate label or a jump
 * to an undefined label, naming the file line. This turns a jump typo into a
 * clear error up front rather than a mid-run failure or a stray statement. */
static struct basic_result
program_check(struct basic *b)
{
	char lbl[VAR_NAME_MAX], other[VAR_NAME_MAX];
	struct basic_result err;
	size_t i, j;

	for (i = 0; i < b->nprog; i++) {
		const char *stmt = parse_label(b->prog[i].text, lbl,
		    sizeof(lbl));

		if (lbl[0]) {
			for (j = 0; j < i; j++) {
				parse_label(b->prog[j].text, other,
				    sizeof(other));
				if (other[0] && strcasecmp(other, lbl) == 0) {
					err.status = BAS_ERROR;
					snprintf(err.msg, sizeof(err.msg),
					    "line %d: duplicate label '%s'",
					    (int)(i + 1), lbl);
					return err;
				}
			}
		}
		if (check_jump(b, stmt, (int)(i + 1), &err) < 0)
			return err;
	}
	return result_ok();
}

static struct basic_result
basic_program_run(struct basic *b, FILE *in, FILE *out)
{
	struct runctx rc;
	int pc = 0;
	struct basic_result chk = program_check(b);

	if (chk.status != BAS_OK)
		return chk;

	memset(&rc, 0, sizeof(rc));
	while (pc >= 0 && (size_t)pc < b->nprog) {
		struct basic_result r;

		rc.cur = pc;
		rc.next = pc + 1;
		r = exec_stmt(b, b->prog[pc].text, in, out, &rc);
		if (r.status == BAS_ERROR) {
			struct basic_result e;

			e.status = BAS_ERROR;
			snprintf(e.msg, sizeof(e.msg), "line %d: %.100s",
			    pc + 1, r.msg);
			return e;
		}
		if (r.status == BAS_QUIT || rc.stop)
			return r.status == BAS_QUIT ? r : result_ok();
		pc = rc.next;
	}
	return result_ok();
}

struct basic_result
basic_exec(struct basic *b, const char *line, FILE *in, FILE *out)
{
	char word[VAR_NAME_MAX];
	const char *s = line;
	size_t wlen;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return result_ok();

	if (isdigit((unsigned char)*s)) {	/* edit a file line by number */
		char *end;
		long n = strtol(s, &end, 10);

		program_set_line(b, (int)n, end);
		return result_ok();
	}

	wlen = lead_word(s, word, sizeof(word));
	if (strcmp(word, "RUN") == 0)
		return basic_program_run(b, in, out);
	if (strcmp(word, "LIST") == 0) {
		program_list(b, out);
		return result_ok();
	}
	if (strcmp(word, "NEW") == 0) {
		program_clear(b);
		return result_ok();
	}
	return exec_stmt(b, s, in, out, NULL);
}
