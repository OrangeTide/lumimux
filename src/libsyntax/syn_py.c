/* syn_py.c : Python syntax table */

#include "syntax.h"

enum {
	PY_IDLE,
	PY_IDENT,
	PY_NUM,
	PY_COMMENT,
	/* double-quote family */
	PY_Q1D,			/* saw one " */
	PY_Q2D,			/* saw "" */
	PY_TDQ,			/* inside """ ... """ */
	PY_TDQ1,		/* saw " inside a triple */
	PY_TDQ2,		/* saw "" inside a triple */
	PY_SDQ,			/* inside a single-line "..." */
	PY_SDQ_ESC,
	/* single-quote family */
	PY_Q1S,
	PY_Q2S,
	PY_TSQ,
	PY_TSQ1,
	PY_TSQ2,
	PY_SSQ,
	PY_SSQ_ESC,
};

static const struct syn_kw py_keywords[] = {
	{ "and", SYN_KEYWORD }, { "as", SYN_KEYWORD },
	{ "assert", SYN_KEYWORD }, { "async", SYN_KEYWORD },
	{ "await", SYN_KEYWORD }, { "break", SYN_KEYWORD },
	{ "class", SYN_KEYWORD }, { "continue", SYN_KEYWORD },
	{ "def", SYN_KEYWORD }, { "del", SYN_KEYWORD },
	{ "elif", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "except", SYN_KEYWORD }, { "finally", SYN_KEYWORD },
	{ "for", SYN_KEYWORD }, { "from", SYN_KEYWORD },
	{ "global", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "import", SYN_KEYWORD }, { "in", SYN_KEYWORD },
	{ "is", SYN_KEYWORD }, { "lambda", SYN_KEYWORD },
	{ "nonlocal", SYN_KEYWORD }, { "not", SYN_KEYWORD },
	{ "or", SYN_KEYWORD }, { "pass", SYN_KEYWORD },
	{ "raise", SYN_KEYWORD }, { "return", SYN_KEYWORD },
	{ "try", SYN_KEYWORD }, { "while", SYN_KEYWORD },
	{ "with", SYN_KEYWORD }, { "yield", SYN_KEYWORD },
	{ "None", SYN_CONSTANT }, { "True", SYN_CONSTANT },
	{ "False", SYN_CONSTANT },
	{ "bool", SYN_TYPE }, { "bytearray", SYN_TYPE },
	{ "bytes", SYN_TYPE }, { "complex", SYN_TYPE },
	{ "dict", SYN_TYPE }, { "float", SYN_TYPE },
	{ "frozenset", SYN_TYPE }, { "int", SYN_TYPE },
	{ "list", SYN_TYPE }, { "object", SYN_TYPE },
	{ "set", SYN_TYPE }, { "str", SYN_TYPE },
	{ "tuple", SYN_TYPE },
	{ "abs", SYN_FUNCTION }, { "enumerate", SYN_FUNCTION },
	{ "filter", SYN_FUNCTION }, { "getattr", SYN_FUNCTION },
	{ "hasattr", SYN_FUNCTION }, { "input", SYN_FUNCTION },
	{ "isinstance", SYN_FUNCTION }, { "len", SYN_FUNCTION },
	{ "map", SYN_FUNCTION }, { "max", SYN_FUNCTION },
	{ "min", SYN_FUNCTION }, { "open", SYN_FUNCTION },
	{ "print", SYN_FUNCTION }, { "range", SYN_FUNCTION },
	{ "repr", SYN_FUNCTION }, { "setattr", SYN_FUNCTION },
	{ "sorted", SYN_FUNCTION }, { "sum", SYN_FUNCTION },
	{ "super", SYN_FUNCTION }, { "zip", SYN_FUNCTION },
};

static const struct syn_rule py_idle_rules[] = {
	{ "a-zA-Z_", PY_IDENT, 0, 1, 0 },
	{ "0-9", PY_NUM, 1, 0, 0 },
	{ "#", PY_COMMENT, 1, 0, 0 },
	{ "\"", PY_Q1D, 1, 0, 0 },
	{ "'", PY_Q1S, 1, 0, 0 },
};

static const struct syn_rule py_ident_rules[] = {
	{ "a-zA-Z0-9_", PY_IDENT, 0, 0, 0 },
};

static const struct syn_rule py_num_rules[] = {
	{ "0-9a-fA-FxXoObBeE._jJ", PY_NUM, 0, 0, 0 },
};

static const struct syn_rule py_comment_rules[] = {
	{ "\n", PY_IDLE, 0, 0, 0 },
};

/* double-quote family */
static const struct syn_rule py_q1d_rules[] = {
	{ "\"", PY_Q2D, 0, 0, 0 },
};
static const struct syn_rule py_q2d_rules[] = {
	{ "\"", PY_TDQ, 0, 0, 0 },
};
static const struct syn_rule py_tdq_rules[] = {
	{ "\"", PY_TDQ1, 0, 0, 0 },
};
static const struct syn_rule py_tdq1_rules[] = {
	{ "\"", PY_TDQ2, 0, 0, 0 },
};
static const struct syn_rule py_tdq2_rules[] = {
	{ "\"", PY_IDLE, 0, 0, 0 },
};
static const struct syn_rule py_sdq_rules[] = {
	{ "\\", PY_SDQ_ESC, 0, 0, 0 },
	{ "\"", PY_IDLE, 0, 0, 0 },
	{ "\n", PY_IDLE, 0, 0, 0 },
};

/* single-quote family */
static const struct syn_rule py_q1s_rules[] = {
	{ "'", PY_Q2S, 0, 0, 0 },
};
static const struct syn_rule py_q2s_rules[] = {
	{ "'", PY_TSQ, 0, 0, 0 },
};
static const struct syn_rule py_tsq_rules[] = {
	{ "'", PY_TSQ1, 0, 0, 0 },
};
static const struct syn_rule py_tsq1_rules[] = {
	{ "'", PY_TSQ2, 0, 0, 0 },
};
static const struct syn_rule py_tsq2_rules[] = {
	{ "'", PY_IDLE, 0, 0, 0 },
};
static const struct syn_rule py_ssq_rules[] = {
	{ "\\", PY_SSQ_ESC, 0, 0, 0 },
	{ "'", PY_IDLE, 0, 0, 0 },
	{ "\n", PY_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state py_states[] = {
	[PY_IDLE] = { SYN_TEXT, PY_IDLE, 0, 0, py_idle_rules,
	    NR(py_idle_rules), NULL, 0 },
	[PY_IDENT] = { SYN_TEXT, PY_IDLE, 1, 0, py_ident_rules,
	    NR(py_ident_rules), py_keywords, NR(py_keywords) },
	[PY_NUM] = { SYN_CONSTANT, PY_IDLE, 1, 0, py_num_rules,
	    NR(py_num_rules), NULL, 0 },
	[PY_COMMENT] = { SYN_COMMENT, PY_COMMENT, 0, 0, py_comment_rules,
	    NR(py_comment_rules), NULL, 0 },

	[PY_Q1D] = { SYN_STRING, PY_SDQ, 1, 0, py_q1d_rules,
	    NR(py_q1d_rules), NULL, 0 },
	[PY_Q2D] = { SYN_STRING, PY_IDLE, 1, 0, py_q2d_rules,
	    NR(py_q2d_rules), NULL, 0 },
	[PY_TDQ] = { SYN_STRING, PY_TDQ, 0, 0, py_tdq_rules,
	    NR(py_tdq_rules), NULL, 0 },
	[PY_TDQ1] = { SYN_STRING, PY_TDQ, 1, 0, py_tdq1_rules,
	    NR(py_tdq1_rules), NULL, 0 },
	[PY_TDQ2] = { SYN_STRING, PY_TDQ, 1, 0, py_tdq2_rules,
	    NR(py_tdq2_rules), NULL, 0 },
	[PY_SDQ] = { SYN_STRING, PY_SDQ, 0, 0, py_sdq_rules,
	    NR(py_sdq_rules), NULL, 0 },
	[PY_SDQ_ESC] = { SYN_STRING, PY_SDQ, 0, 0, NULL, 0, NULL, 0 },

	[PY_Q1S] = { SYN_STRING, PY_SSQ, 1, 0, py_q1s_rules,
	    NR(py_q1s_rules), NULL, 0 },
	[PY_Q2S] = { SYN_STRING, PY_IDLE, 1, 0, py_q2s_rules,
	    NR(py_q2s_rules), NULL, 0 },
	[PY_TSQ] = { SYN_STRING, PY_TSQ, 0, 0, py_tsq_rules,
	    NR(py_tsq_rules), NULL, 0 },
	[PY_TSQ1] = { SYN_STRING, PY_TSQ, 1, 0, py_tsq1_rules,
	    NR(py_tsq1_rules), NULL, 0 },
	[PY_TSQ2] = { SYN_STRING, PY_TSQ, 1, 0, py_tsq2_rules,
	    NR(py_tsq2_rules), NULL, 0 },
	[PY_SSQ] = { SYN_STRING, PY_SSQ, 0, 0, py_ssq_rules,
	    NR(py_ssq_rules), NULL, 0 },
	[PY_SSQ_ESC] = { SYN_STRING, PY_SSQ, 0, 0, NULL, 0, NULL, 0 },
};

static const char *const py_exts[] = {
	"py", "pyw", NULL,
};

const struct syntax syntax_py = {
	"python", PY_IDLE, 0, py_states, NR(py_states), py_exts,
};
