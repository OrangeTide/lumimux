/* syn_pas.c : Pascal syntax table */

#include "syntax.h"

/*
 * A case-insensitive table for Pascal (Turbo/Delphi/Free Pascal). Three
 * comment forms are handled, all of which may span lines for the block kinds:
 * { ... } braces, (* ... *) parentheses-star, and // to end of line. A brace
 * that opens with $ is a {$...} compiler directive, painted as preproc rather
 * than as a comment. Strings are 'single quoted'; a doubled '' inside is left
 * to read as two adjacent strings, which keeps the whole run colored. Numbers
 * cover decimal, $hex, and %binary forms. A keyword table paints reserved
 * words, the built-in types, and the common standard procedures.
 */

enum {
	PAS_IDLE,
	PAS_IDENT,
	PAS_NUM,
	PAS_HEX,		/* $FF */
	PAS_BIN,		/* %1010 */
	PAS_SQ,			/* '...' string */
	PAS_LBRACE,		/* saw '{' */
	PAS_BRACE,		/* { ... } comment */
	PAS_DIR,		/* {$ ... } compiler directive */
	PAS_LPAREN,		/* saw '(' */
	PAS_PSTAR,		/* (* ... *) comment */
	PAS_PSTAR_STAR,		/* saw '*' inside a (* comment */
	PAS_SLASH,		/* saw '/' */
	PAS_LINE,		/* // comment */
	PAS_OP,
};

static const struct syn_kw pas_keywords[] = {
	{ "and", SYN_KEYWORD }, { "array", SYN_KEYWORD },
	{ "asm", SYN_KEYWORD }, { "begin", SYN_KEYWORD },
	{ "case", SYN_KEYWORD }, { "const", SYN_KEYWORD },
	{ "constructor", SYN_KEYWORD }, { "destructor", SYN_KEYWORD },
	{ "div", SYN_KEYWORD }, { "do", SYN_KEYWORD },
	{ "downto", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "end", SYN_KEYWORD }, { "except", SYN_KEYWORD },
	{ "file", SYN_KEYWORD }, { "finalization", SYN_KEYWORD },
	{ "finally", SYN_KEYWORD }, { "for", SYN_KEYWORD },
	{ "function", SYN_KEYWORD }, { "goto", SYN_KEYWORD },
	{ "if", SYN_KEYWORD }, { "implementation", SYN_KEYWORD },
	{ "in", SYN_KEYWORD }, { "inherited", SYN_KEYWORD },
	{ "initialization", SYN_KEYWORD }, { "inline", SYN_KEYWORD },
	{ "interface", SYN_KEYWORD }, { "label", SYN_KEYWORD },
	{ "mod", SYN_KEYWORD }, { "not", SYN_KEYWORD },
	{ "object", SYN_KEYWORD }, { "of", SYN_KEYWORD },
	{ "or", SYN_KEYWORD }, { "packed", SYN_KEYWORD },
	{ "procedure", SYN_KEYWORD }, { "program", SYN_KEYWORD },
	{ "property", SYN_KEYWORD }, { "raise", SYN_KEYWORD },
	{ "record", SYN_KEYWORD }, { "repeat", SYN_KEYWORD },
	{ "set", SYN_KEYWORD }, { "shl", SYN_KEYWORD },
	{ "shr", SYN_KEYWORD }, { "then", SYN_KEYWORD },
	{ "to", SYN_KEYWORD }, { "try", SYN_KEYWORD },
	{ "type", SYN_KEYWORD }, { "unit", SYN_KEYWORD },
	{ "until", SYN_KEYWORD }, { "uses", SYN_KEYWORD },
	{ "var", SYN_KEYWORD }, { "while", SYN_KEYWORD },
	{ "with", SYN_KEYWORD }, { "xor", SYN_KEYWORD },
	{ "class", SYN_KEYWORD }, { "private", SYN_KEYWORD },
	{ "public", SYN_KEYWORD }, { "protected", SYN_KEYWORD },
	{ "published", SYN_KEYWORD },
	{ "integer", SYN_TYPE }, { "real", SYN_TYPE },
	{ "boolean", SYN_TYPE }, { "char", SYN_TYPE },
	{ "string", SYN_TYPE }, { "byte", SYN_TYPE },
	{ "word", SYN_TYPE }, { "longint", SYN_TYPE },
	{ "longword", SYN_TYPE }, { "cardinal", SYN_TYPE },
	{ "shortint", SYN_TYPE }, { "smallint", SYN_TYPE },
	{ "int64", SYN_TYPE }, { "qword", SYN_TYPE },
	{ "single", SYN_TYPE }, { "double", SYN_TYPE },
	{ "extended", SYN_TYPE }, { "comp", SYN_TYPE },
	{ "currency", SYN_TYPE }, { "pointer", SYN_TYPE },
	{ "pchar", SYN_TYPE }, { "ansistring", SYN_TYPE },
	{ "widestring", SYN_TYPE }, { "variant", SYN_TYPE },
	{ "text", SYN_TYPE },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
	{ "nil", SYN_CONSTANT },
	{ "write", SYN_FUNCTION }, { "writeln", SYN_FUNCTION },
	{ "read", SYN_FUNCTION }, { "readln", SYN_FUNCTION },
	{ "new", SYN_FUNCTION }, { "dispose", SYN_FUNCTION },
	{ "length", SYN_FUNCTION }, { "setlength", SYN_FUNCTION },
	{ "copy", SYN_FUNCTION }, { "pos", SYN_FUNCTION },
	{ "inc", SYN_FUNCTION }, { "dec", SYN_FUNCTION },
	{ "ord", SYN_FUNCTION }, { "chr", SYN_FUNCTION },
	{ "abs", SYN_FUNCTION }, { "sqr", SYN_FUNCTION },
	{ "sqrt", SYN_FUNCTION }, { "exit", SYN_FUNCTION },
	{ "halt", SYN_FUNCTION },
};

static const struct syn_rule pas_idle_rules[] = {
	{ "a-zA-Z_", PAS_IDENT, 0, 1, 0 },
	{ "0-9", PAS_NUM, 1, 0, 0 },
	{ "$", PAS_HEX, 1, 0, 0 },
	{ "%", PAS_BIN, 1, 0, 0 },
	{ "'", PAS_SQ, 1, 0, 0 },
	{ "{", PAS_LBRACE, 1, 0, 0 },
	{ "(", PAS_LPAREN, 0, 0, 0 },
	{ "/", PAS_SLASH, 0, 0, 0 },
	{ "+*=<>:;,.@^[])-", PAS_OP, 1, 0, 0 },
};

static const struct syn_rule pas_ident_rules[] = {
	{ "a-zA-Z0-9_", PAS_IDENT, 0, 0, 0 },
};

static const struct syn_rule pas_num_rules[] = {
	{ "0-9.eE", PAS_NUM, 0, 0, 0 },
};

static const struct syn_rule pas_hex_rules[] = {
	{ "0-9a-fA-F", PAS_HEX, 0, 0, 0 },
};

static const struct syn_rule pas_bin_rules[] = {
	{ "01", PAS_BIN, 0, 0, 0 },
};

static const struct syn_rule pas_sq_rules[] = {
	{ "'", PAS_IDLE, 0, 0, 0 },
	{ "\n", PAS_IDLE, 0, 0, 0 },
};

static const struct syn_rule pas_lbrace_rules[] = {
	{ "$", PAS_DIR, 2, 0, 0 },
};

static const struct syn_rule pas_brace_rules[] = {
	{ "}", PAS_IDLE, 0, 0, 0 },
};

static const struct syn_rule pas_dir_rules[] = {
	{ "}", PAS_IDLE, 0, 0, 0 },
};

static const struct syn_rule pas_lparen_rules[] = {
	{ "*", PAS_PSTAR, 2, 0, 0 },
};

static const struct syn_rule pas_pstar_rules[] = {
	{ "*", PAS_PSTAR_STAR, 0, 0, 0 },
};

static const struct syn_rule pas_pstar_star_rules[] = {
	{ ")", PAS_IDLE, 0, 0, 0 },
	{ "*", PAS_PSTAR_STAR, 0, 0, 0 },
};

static const struct syn_rule pas_slash_rules[] = {
	{ "/", PAS_LINE, 2, 0, 0 },
};

static const struct syn_rule pas_line_rules[] = {
	{ "\n", PAS_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state pas_states[] = {
	[PAS_IDLE] = { SYN_TEXT, PAS_IDLE, 0, 0, pas_idle_rules,
	    NR(pas_idle_rules), NULL, 0 },
	[PAS_IDENT] = { SYN_TEXT, PAS_IDLE, 1, 0, pas_ident_rules,
	    NR(pas_ident_rules), pas_keywords, NR(pas_keywords) },
	[PAS_NUM] = { SYN_CONSTANT, PAS_IDLE, 1, 0, pas_num_rules,
	    NR(pas_num_rules), NULL, 0 },
	[PAS_HEX] = { SYN_CONSTANT, PAS_IDLE, 1, 0, pas_hex_rules,
	    NR(pas_hex_rules), NULL, 0 },
	[PAS_BIN] = { SYN_CONSTANT, PAS_IDLE, 1, 0, pas_bin_rules,
	    NR(pas_bin_rules), NULL, 0 },
	[PAS_SQ] = { SYN_STRING, PAS_SQ, 0, 0, pas_sq_rules,
	    NR(pas_sq_rules), NULL, 0 },
	[PAS_LBRACE] = { SYN_COMMENT, PAS_BRACE, 1, 0, pas_lbrace_rules,
	    NR(pas_lbrace_rules), NULL, 0 },
	[PAS_BRACE] = { SYN_COMMENT, PAS_BRACE, 0, 0, pas_brace_rules,
	    NR(pas_brace_rules), NULL, 0 },
	[PAS_DIR] = { SYN_PREPROC, PAS_DIR, 0, 0, pas_dir_rules,
	    NR(pas_dir_rules), NULL, 0 },
	[PAS_LPAREN] = { SYN_TEXT, PAS_IDLE, 1, 0, pas_lparen_rules,
	    NR(pas_lparen_rules), NULL, 0 },
	[PAS_PSTAR] = { SYN_COMMENT, PAS_PSTAR, 0, 0, pas_pstar_rules,
	    NR(pas_pstar_rules), NULL, 0 },
	[PAS_PSTAR_STAR] = { SYN_COMMENT, PAS_PSTAR, 1, 0, pas_pstar_star_rules,
	    NR(pas_pstar_star_rules), NULL, 0 },
	[PAS_SLASH] = { SYN_TEXT, PAS_IDLE, 1, 0, pas_slash_rules,
	    NR(pas_slash_rules), NULL, 0 },
	[PAS_LINE] = { SYN_COMMENT, PAS_LINE, 0, 0, pas_line_rules,
	    NR(pas_line_rules), NULL, 0 },
	[PAS_OP] = { SYN_OPERATOR, PAS_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const pas_exts[] = {
	"pas", "pp", "dpr", "lpr", NULL,
};

const struct syntax syntax_pas = {
	"pascal", PAS_IDLE, 1, pas_states, NR(pas_states), pas_exts,
};
