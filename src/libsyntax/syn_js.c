/* syn_js.c : JavaScript syntax table */

#include "syntax.h"

/*
 * A C-like table for JavaScript. Handles double- and single-quoted strings,
 * backtick template literals (which span lines), line and block comments,
 * numbers, the usual keywords, and /regex/ literals.
 *
 * A slash is a regex only where an operand is expected and a division where an
 * operand just ended, so the idle state is split in two: JS_RE (regex allowed:
 * the start of input and just after an operator, an opening ( [ {, or a , ; )
 * and JS_DIV (division expected: just after an identifier, number, string, or
 * a closing ) ] }). A slash resolves against whichever context is current.
 *
 * Inside a template literal, a ${ ... } interpolation is highlighted as an
 * expression (identifiers and keywords, numbers, quoted strings, operators)
 * and ends at the matching }. A } inside a string within the interpolation is
 * consumed by that string, so ${ "}" } closes correctly. What is not tracked
 * is brace depth: an interpolation that itself contains a { } object literal,
 * or a nested template literal, ends at the first }, because the engine has no
 * counter or stack for nesting.
 *
 * Low-effort limits: a regex is not recognized after a keyword that expects
 * one (return /re/g reads as division), since the context is chosen by the
 * previous token's shape, not its meaning; a postfix ++ or -- before a
 * division is read as a regex; a / inside a regex [character class] is treated
 * as the closing delimiter; and a / inside an interpolation is always an
 * operator, never a regex.
 */

enum {
	JS_RE,			/* idle, a regex may start here */
	JS_DIV,			/* idle, a slash here is division */
	JS_IDENT,
	JS_NUM,
	JS_DQ,
	JS_DQ_ESC,
	JS_SQ,
	JS_SQ_ESC,
	JS_TMPL,		/* `...` template literal, spans lines */
	JS_TMPL_ESC,
	JS_TMPL_DOLLAR,		/* saw '$' inside a template literal */
	JS_SLASH_RE,		/* saw '/' in regex-allowed context */
	JS_SLASH_DIV,		/* saw '/' in division context */
	JS_LINE,
	JS_BLOCK,
	JS_BLOCK_STAR,
	JS_REGEX,		/* /.../ literal body */
	JS_REGEX_ESC,
	JS_REGEX_FLAGS,		/* trailing g, i, m, ... flags */
	JS_OP,			/* operator/opener: an operand follows */
	JS_CLOSE,		/* ) ] }: an operand just ended */
	JS_INTERP,		/* ${ ... } interpolation body */
	JS_INTERP_IDENT,
	JS_INTERP_NUM,
	JS_INTERP_DQ,
	JS_INTERP_DQ_ESC,
	JS_INTERP_SQ,
	JS_INTERP_SQ_ESC,
	JS_INTERP_OP,
};

static const struct syn_kw js_keywords[] = {
	{ "async", SYN_KEYWORD }, { "await", SYN_KEYWORD },
	{ "break", SYN_KEYWORD }, { "case", SYN_KEYWORD },
	{ "catch", SYN_KEYWORD }, { "class", SYN_KEYWORD },
	{ "const", SYN_KEYWORD }, { "continue", SYN_KEYWORD },
	{ "debugger", SYN_KEYWORD }, { "default", SYN_KEYWORD },
	{ "delete", SYN_KEYWORD }, { "do", SYN_KEYWORD },
	{ "else", SYN_KEYWORD }, { "export", SYN_KEYWORD },
	{ "extends", SYN_KEYWORD }, { "finally", SYN_KEYWORD },
	{ "for", SYN_KEYWORD }, { "function", SYN_KEYWORD },
	{ "get", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "import", SYN_KEYWORD }, { "in", SYN_KEYWORD },
	{ "instanceof", SYN_KEYWORD }, { "let", SYN_KEYWORD },
	{ "new", SYN_KEYWORD }, { "of", SYN_KEYWORD },
	{ "return", SYN_KEYWORD }, { "set", SYN_KEYWORD },
	{ "static", SYN_KEYWORD }, { "super", SYN_KEYWORD },
	{ "switch", SYN_KEYWORD }, { "this", SYN_KEYWORD },
	{ "throw", SYN_KEYWORD }, { "try", SYN_KEYWORD },
	{ "typeof", SYN_KEYWORD }, { "var", SYN_KEYWORD },
	{ "void", SYN_KEYWORD }, { "while", SYN_KEYWORD },
	{ "with", SYN_KEYWORD }, { "yield", SYN_KEYWORD },
	{ "Array", SYN_TYPE }, { "Boolean", SYN_TYPE },
	{ "Date", SYN_TYPE }, { "Error", SYN_TYPE },
	{ "Function", SYN_TYPE }, { "Map", SYN_TYPE },
	{ "Number", SYN_TYPE }, { "Object", SYN_TYPE },
	{ "Promise", SYN_TYPE }, { "RegExp", SYN_TYPE },
	{ "Set", SYN_TYPE }, { "String", SYN_TYPE },
	{ "Symbol", SYN_TYPE },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
	{ "null", SYN_CONSTANT }, { "undefined", SYN_CONSTANT },
	{ "NaN", SYN_CONSTANT }, { "Infinity", SYN_CONSTANT },
	{ "isFinite", SYN_FUNCTION }, { "isNaN", SYN_FUNCTION },
	{ "parseFloat", SYN_FUNCTION }, { "parseInt", SYN_FUNCTION },
	{ "require", SYN_FUNCTION },
};

/*
 * The two idle contexts share every rule but the slash: JS_RE opens a regex,
 * JS_DIV starts a division/comment. Openers and separators route to JS_OP (an
 * operand follows), closers to JS_CLOSE (an operand just ended).
 */
#define JS_IDLE_COMMON(slash)						\
	{ "a-zA-Z_$", JS_IDENT, 0, 1, 0 },				\
	{ "0-9", JS_NUM, 1, 0, 0 },					\
	{ "\"", JS_DQ, 1, 0, 0 },					\
	{ "'", JS_SQ, 1, 0, 0 },					\
	{ "`", JS_TMPL, 1, 0, 0 },					\
	{ "/", (slash), 0, 0, 0 },					\
	{ ")]}", JS_CLOSE, 1, 0, 0 },					\
	{ "+*%=<>!&|^~?:.,;([{-", JS_OP, 1, 0, 0 }

static const struct syn_rule js_re_rules[] = {
	JS_IDLE_COMMON(JS_SLASH_RE),
};

static const struct syn_rule js_div_rules[] = {
	JS_IDLE_COMMON(JS_SLASH_DIV),
};

static const struct syn_rule js_ident_rules[] = {
	{ "a-zA-Z0-9_$", JS_IDENT, 0, 0, 0 },
};

static const struct syn_rule js_num_rules[] = {
	{ "0-9a-fA-FxXoObBeE._n", JS_NUM, 0, 0, 0 },
};

static const struct syn_rule js_dq_rules[] = {
	{ "\\", JS_DQ_ESC, 0, 0, 0 },
	{ "\"", JS_DIV, 0, 0, 0 },
	{ "\n", JS_DIV, 0, 0, 0 },
};

static const struct syn_rule js_sq_rules[] = {
	{ "\\", JS_SQ_ESC, 0, 0, 0 },
	{ "'", JS_DIV, 0, 0, 0 },
	{ "\n", JS_DIV, 0, 0, 0 },
};

static const struct syn_rule js_tmpl_rules[] = {
	{ "\\", JS_TMPL_ESC, 0, 0, 0 },
	{ "$", JS_TMPL_DOLLAR, 0, 0, 0 },
	{ "`", JS_DIV, 0, 0, 0 },
};

static const struct syn_rule js_tmpl_dollar_rules[] = {
	{ "{", JS_INTERP, 0, 0, 0 },	/* ${ opens an interpolation */
};

/*
 * The interpolation body is a small expression tokenizer that ends at the
 * closing }. A } inside a nested string is consumed by that string, not taken
 * as the close. A / here is always an operator (no regex), and a nested { is
 * not depth-tracked.
 */
static const struct syn_rule js_interp_rules[] = {
	{ "}", JS_TMPL, 1, 0, 0 },	/* close, recolor } as string */
	{ "a-zA-Z_$", JS_INTERP_IDENT, 0, 1, 0 },
	{ "0-9", JS_INTERP_NUM, 1, 0, 0 },
	{ "\"", JS_INTERP_DQ, 1, 0, 0 },
	{ "'", JS_INTERP_SQ, 1, 0, 0 },
	{ "+*%=<>!&|^~?:.,;([{)]/-", JS_INTERP_OP, 1, 0, 0 },
};

static const struct syn_rule js_interp_ident_rules[] = {
	{ "a-zA-Z0-9_$", JS_INTERP_IDENT, 0, 0, 0 },
};

static const struct syn_rule js_interp_num_rules[] = {
	{ "0-9a-fA-FxXoObBeE._n", JS_INTERP_NUM, 0, 0, 0 },
};

static const struct syn_rule js_interp_dq_rules[] = {
	{ "\\", JS_INTERP_DQ_ESC, 0, 0, 0 },
	{ "\"", JS_INTERP, 0, 0, 0 },
	{ "\n", JS_INTERP, 0, 0, 0 },
};

static const struct syn_rule js_interp_sq_rules[] = {
	{ "\\", JS_INTERP_SQ_ESC, 0, 0, 0 },
	{ "'", JS_INTERP, 0, 0, 0 },
	{ "\n", JS_INTERP, 0, 0, 0 },
};

static const struct syn_rule js_slash_re_rules[] = {
	{ "/", JS_LINE, 2, 0, 0 },
	{ "*", JS_BLOCK, 2, 0, 0 },
};

static const struct syn_rule js_slash_div_rules[] = {
	{ "/", JS_LINE, 2, 0, 0 },
	{ "*", JS_BLOCK, 2, 0, 0 },
};

static const struct syn_rule js_line_rules[] = {
	{ "\n", JS_RE, 0, 0, 0 },
};

static const struct syn_rule js_block_rules[] = {
	{ "*", JS_BLOCK_STAR, 0, 0, 0 },
};

static const struct syn_rule js_block_star_rules[] = {
	{ "/", JS_RE, 0, 0, 0 },
};

static const struct syn_rule js_regex_rules[] = {
	{ "\\", JS_REGEX_ESC, 0, 0, 0 },
	{ "/", JS_REGEX_FLAGS, 0, 0, 0 },
	{ "\n", JS_RE, 0, 0, 0 },
};

static const struct syn_rule js_regex_flags_rules[] = {
	{ "a-zA-Z", JS_REGEX_FLAGS, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state js_states[] = {
	[JS_RE] = { SYN_TEXT, JS_RE, 0, 0, js_re_rules,
	    NR(js_re_rules), NULL, 0 },
	[JS_DIV] = { SYN_TEXT, JS_DIV, 0, 0, js_div_rules,
	    NR(js_div_rules), NULL, 0 },
	[JS_IDENT] = { SYN_TEXT, JS_DIV, 1, 0, js_ident_rules,
	    NR(js_ident_rules), js_keywords, NR(js_keywords) },
	[JS_NUM] = { SYN_CONSTANT, JS_DIV, 1, 0, js_num_rules,
	    NR(js_num_rules), NULL, 0 },
	[JS_DQ] = { SYN_STRING, JS_DQ, 0, 0, js_dq_rules,
	    NR(js_dq_rules), NULL, 0 },
	[JS_DQ_ESC] = { SYN_STRING, JS_DQ, 0, 0, NULL, 0, NULL, 0 },
	[JS_SQ] = { SYN_STRING, JS_SQ, 0, 0, js_sq_rules,
	    NR(js_sq_rules), NULL, 0 },
	[JS_SQ_ESC] = { SYN_STRING, JS_SQ, 0, 0, NULL, 0, NULL, 0 },
	[JS_TMPL] = { SYN_STRING, JS_TMPL, 0, 0, js_tmpl_rules,
	    NR(js_tmpl_rules), NULL, 0 },
	[JS_TMPL_ESC] = { SYN_STRING, JS_TMPL, 0, 0, NULL, 0, NULL, 0 },
	[JS_TMPL_DOLLAR] = { SYN_STRING, JS_TMPL, 1, 0, js_tmpl_dollar_rules,
	    NR(js_tmpl_dollar_rules), NULL, 0 },
	[JS_SLASH_RE] = { SYN_TEXT, JS_REGEX, 1, 2, js_slash_re_rules,
	    NR(js_slash_re_rules), NULL, 0 },
	[JS_SLASH_DIV] = { SYN_TEXT, JS_RE, 1, 0, js_slash_div_rules,
	    NR(js_slash_div_rules), NULL, 0 },
	[JS_LINE] = { SYN_COMMENT, JS_LINE, 0, 0, js_line_rules,
	    NR(js_line_rules), NULL, 0 },
	[JS_BLOCK] = { SYN_COMMENT, JS_BLOCK, 0, 0, js_block_rules,
	    NR(js_block_rules), NULL, 0 },
	[JS_BLOCK_STAR] = { SYN_COMMENT, JS_BLOCK, 1, 0, js_block_star_rules,
	    NR(js_block_star_rules), NULL, 0 },
	[JS_REGEX] = { SYN_STRING, JS_REGEX, 0, 0, js_regex_rules,
	    NR(js_regex_rules), NULL, 0 },
	[JS_REGEX_ESC] = { SYN_STRING, JS_REGEX, 0, 0, NULL, 0, NULL, 0 },
	[JS_REGEX_FLAGS] = { SYN_STRING, JS_DIV, 1, 0, js_regex_flags_rules,
	    NR(js_regex_flags_rules), NULL, 0 },
	[JS_OP] = { SYN_OPERATOR, JS_RE, 1, 0, NULL, 0, NULL, 0 },
	[JS_CLOSE] = { SYN_OPERATOR, JS_DIV, 1, 0, NULL, 0, NULL, 0 },
	[JS_INTERP] = { SYN_TEXT, JS_INTERP, 0, 0, js_interp_rules,
	    NR(js_interp_rules), NULL, 0 },
	[JS_INTERP_IDENT] = { SYN_TEXT, JS_INTERP, 1, 0, js_interp_ident_rules,
	    NR(js_interp_ident_rules), js_keywords, NR(js_keywords) },
	[JS_INTERP_NUM] = { SYN_CONSTANT, JS_INTERP, 1, 0, js_interp_num_rules,
	    NR(js_interp_num_rules), NULL, 0 },
	[JS_INTERP_DQ] = { SYN_STRING, JS_INTERP_DQ, 0, 0, js_interp_dq_rules,
	    NR(js_interp_dq_rules), NULL, 0 },
	[JS_INTERP_DQ_ESC] = { SYN_STRING, JS_INTERP_DQ, 0, 0, NULL, 0,
	    NULL, 0 },
	[JS_INTERP_SQ] = { SYN_STRING, JS_INTERP_SQ, 0, 0, js_interp_sq_rules,
	    NR(js_interp_sq_rules), NULL, 0 },
	[JS_INTERP_SQ_ESC] = { SYN_STRING, JS_INTERP_SQ, 0, 0, NULL, 0,
	    NULL, 0 },
	[JS_INTERP_OP] = { SYN_OPERATOR, JS_INTERP, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const js_exts[] = {
	"js", "mjs", "cjs", "jsx", NULL,
};

const struct syntax syntax_js = {
	"javascript", JS_RE, 0, js_states, NR(js_states), js_exts,
};
