/* syn_bas.c : BASIC syntax table */

#include "syntax.h"

/*
 * A generic classic/QBASIC-style dialect. Keyword lookup is case-insensitive.
 * Strings are double-quoted and do not span lines. Two comment forms are
 * handled: the ' apostrophe comment and the REM keyword, both running to end
 * of line. REM is matched as a whole word by a small prefix chain, so REMARK
 * and REM1 stay identifiers. A trailing type sigil ($ % ! # &) is folded into
 * the identifier, so a variable like count% reads as one token and the string
 * builtins are named with their sigils (LEFT$, CHR$, ...).
 */

enum {
	BAS_IDLE,
	BAS_IDENT,
	BAS_SIGIL,		/* one trailing type sigil on an identifier */
	BAS_NUM,
	BAS_STR,
	BAS_TICK,		/* ' comment to end of line */
	BAS_R,			/* saw R (REM prefix or identifier) */
	BAS_RE,			/* saw RE */
	BAS_REM,		/* saw REM */
	BAS_LINE_REM,		/* REM comment body */
	BAS_OP,
};

static const struct syn_kw bas_keywords[] = {
	{ "print", SYN_KEYWORD }, { "input", SYN_KEYWORD },
	{ "let", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "then", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "elseif", SYN_KEYWORD }, { "end", SYN_KEYWORD },
	{ "endif", SYN_KEYWORD }, { "for", SYN_KEYWORD },
	{ "to", SYN_KEYWORD }, { "step", SYN_KEYWORD },
	{ "next", SYN_KEYWORD }, { "while", SYN_KEYWORD },
	{ "wend", SYN_KEYWORD }, { "do", SYN_KEYWORD },
	{ "loop", SYN_KEYWORD }, { "until", SYN_KEYWORD },
	{ "goto", SYN_KEYWORD }, { "gosub", SYN_KEYWORD },
	{ "return", SYN_KEYWORD }, { "on", SYN_KEYWORD },
	{ "dim", SYN_KEYWORD }, { "redim", SYN_KEYWORD },
	{ "data", SYN_KEYWORD }, { "read", SYN_KEYWORD },
	{ "restore", SYN_KEYWORD }, { "def", SYN_KEYWORD },
	{ "fn", SYN_KEYWORD }, { "stop", SYN_KEYWORD },
	{ "run", SYN_KEYWORD }, { "cls", SYN_KEYWORD },
	{ "select", SYN_KEYWORD }, { "case", SYN_KEYWORD },
	{ "sub", SYN_KEYWORD }, { "function", SYN_KEYWORD },
	{ "call", SYN_KEYWORD }, { "exit", SYN_KEYWORD },
	{ "const", SYN_KEYWORD }, { "declare", SYN_KEYWORD },
	{ "shared", SYN_KEYWORD }, { "static", SYN_KEYWORD },
	{ "open", SYN_KEYWORD }, { "close", SYN_KEYWORD },
	{ "line", SYN_KEYWORD }, { "option", SYN_KEYWORD },
	{ "base", SYN_KEYWORD }, { "randomize", SYN_KEYWORD },
	{ "swap", SYN_KEYWORD }, { "erase", SYN_KEYWORD },
	{ "and", SYN_OPERATOR }, { "or", SYN_OPERATOR },
	{ "not", SYN_OPERATOR }, { "xor", SYN_OPERATOR },
	{ "mod", SYN_OPERATOR }, { "eqv", SYN_OPERATOR },
	{ "imp", SYN_OPERATOR },
	{ "integer", SYN_TYPE }, { "long", SYN_TYPE },
	{ "single", SYN_TYPE }, { "double", SYN_TYPE },
	{ "string", SYN_TYPE }, { "byte", SYN_TYPE },
	{ "boolean", SYN_TYPE }, { "variant", SYN_TYPE },
	{ "as", SYN_TYPE },
	{ "abs", SYN_FUNCTION }, { "int", SYN_FUNCTION },
	{ "rnd", SYN_FUNCTION }, { "sgn", SYN_FUNCTION },
	{ "sqr", SYN_FUNCTION }, { "sin", SYN_FUNCTION },
	{ "cos", SYN_FUNCTION }, { "tan", SYN_FUNCTION },
	{ "atn", SYN_FUNCTION }, { "exp", SYN_FUNCTION },
	{ "log", SYN_FUNCTION }, { "len", SYN_FUNCTION },
	{ "val", SYN_FUNCTION },
	{ "chr$", SYN_FUNCTION }, { "asc", SYN_FUNCTION },
	{ "left$", SYN_FUNCTION }, { "right$", SYN_FUNCTION },
	{ "mid$", SYN_FUNCTION }, { "instr", SYN_FUNCTION },
	{ "ucase$", SYN_FUNCTION }, { "lcase$", SYN_FUNCTION },
	{ "space$", SYN_FUNCTION }, { "tab", SYN_FUNCTION },
	{ "hex$", SYN_FUNCTION }, { "oct$", SYN_FUNCTION },
	{ "str$", SYN_FUNCTION },
	{ "fix", SYN_FUNCTION }, { "cint", SYN_FUNCTION },
	{ "clng", SYN_FUNCTION }, { "csng", SYN_FUNCTION },
	{ "cdbl", SYN_FUNCTION },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
};

static const struct syn_rule bas_idle_rules[] = {
	{ "Rr", BAS_R, 0, 1, 0 },	/* REM prefix chain, or identifier */
	{ "a-zA-Z_", BAS_IDENT, 0, 1, 0 },
	{ "0-9", BAS_NUM, 1, 0, 0 },
	{ "\"", BAS_STR, 1, 0, 0 },
	{ "'", BAS_TICK, 1, 0, 0 },
	{ "+*/^\\<>=-", BAS_OP, 1, 0, 0 },
};

static const struct syn_rule bas_ident_rules[] = {
	{ "a-zA-Z0-9_", BAS_IDENT, 0, 0, 0 },
	{ "$%!#&", BAS_SIGIL, 0, 0, 0 },	/* trailing type sigil */
};

static const struct syn_rule bas_r_rules[] = {
	{ "Ee", BAS_RE, 0, 0, 0 },
	{ "a-zA-Z0-9_", BAS_IDENT, 0, 0, 0 },
};

static const struct syn_rule bas_re_rules[] = {
	{ "Mm", BAS_REM, 0, 0, 0 },
	{ "a-zA-Z0-9_", BAS_IDENT, 0, 0, 0 },
};

static const struct syn_rule bas_rem_rules[] = {
	{ "a-zA-Z0-9_", BAS_IDENT, 0, 0, 0 },
};

static const struct syn_rule bas_line_rem_rules[] = {
	{ "\n", BAS_IDLE, 0, 0, 0 },
};

static const struct syn_rule bas_num_rules[] = {
	{ "0-9.eEdD", BAS_NUM, 0, 0, 0 },
};

static const struct syn_rule bas_str_rules[] = {
	{ "\"", BAS_IDLE, 0, 0, 0 },
	{ "\n", BAS_IDLE, 0, 0, 0 },
};

static const struct syn_rule bas_tick_rules[] = {
	{ "\n", BAS_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state bas_states[] = {
	[BAS_IDLE] = { SYN_TEXT, BAS_IDLE, 0, 0, bas_idle_rules,
	    NR(bas_idle_rules), NULL, 0 },
	[BAS_IDENT] = { SYN_TEXT, BAS_IDLE, 1, 0, bas_ident_rules,
	    NR(bas_ident_rules), bas_keywords, NR(bas_keywords) },
	[BAS_SIGIL] = { SYN_TEXT, BAS_IDLE, 1, 0, NULL, 0,
	    bas_keywords, NR(bas_keywords) },
	[BAS_NUM] = { SYN_CONSTANT, BAS_IDLE, 1, 0, bas_num_rules,
	    NR(bas_num_rules), NULL, 0 },
	[BAS_STR] = { SYN_STRING, BAS_STR, 0, 0, bas_str_rules,
	    NR(bas_str_rules), NULL, 0 },
	[BAS_TICK] = { SYN_COMMENT, BAS_TICK, 0, 0, bas_tick_rules,
	    NR(bas_tick_rules), NULL, 0 },
	[BAS_R] = { SYN_TEXT, BAS_IDENT, 1, 0, bas_r_rules,
	    NR(bas_r_rules), bas_keywords, NR(bas_keywords) },
	[BAS_RE] = { SYN_TEXT, BAS_IDENT, 1, 0, bas_re_rules,
	    NR(bas_re_rules), bas_keywords, NR(bas_keywords) },
	[BAS_REM] = { SYN_TEXT, BAS_LINE_REM, 0, 4, bas_rem_rules,
	    NR(bas_rem_rules), NULL, 0 },
	[BAS_LINE_REM] = { SYN_COMMENT, BAS_LINE_REM, 0, 0, bas_line_rem_rules,
	    NR(bas_line_rem_rules), NULL, 0 },
	[BAS_OP] = { SYN_OPERATOR, BAS_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const bas_exts[] = {
	"bas", "bi", NULL,
};

const struct syntax syntax_bas = {
	"basic", BAS_IDLE, 1, bas_states, NR(bas_states), bas_exts,
};
