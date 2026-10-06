/* syn_go.c : Go syntax table */

#include "syntax.h"

enum {
	GO_IDLE,
	GO_IDENT,
	GO_NUM,
	GO_STR,			/* "..." interpreted string */
	GO_STR_ESC,
	GO_RAW,			/* `...` raw string, spans lines */
	GO_CHR,			/* '...' rune literal */
	GO_CHR_ESC,
	GO_SLASH,
	GO_LINE,
	GO_BLOCK,
	GO_BLOCK_STAR,
	GO_OP,
};

static const struct syn_kw go_keywords[] = {
	{ "break", SYN_KEYWORD }, { "case", SYN_KEYWORD },
	{ "chan", SYN_KEYWORD }, { "const", SYN_KEYWORD },
	{ "continue", SYN_KEYWORD }, { "default", SYN_KEYWORD },
	{ "defer", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "fallthrough", SYN_KEYWORD }, { "for", SYN_KEYWORD },
	{ "func", SYN_KEYWORD }, { "go", SYN_KEYWORD },
	{ "goto", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "import", SYN_KEYWORD }, { "interface", SYN_KEYWORD },
	{ "map", SYN_KEYWORD }, { "package", SYN_KEYWORD },
	{ "range", SYN_KEYWORD }, { "return", SYN_KEYWORD },
	{ "select", SYN_KEYWORD }, { "struct", SYN_KEYWORD },
	{ "switch", SYN_KEYWORD }, { "type", SYN_KEYWORD },
	{ "var", SYN_KEYWORD },
	{ "bool", SYN_TYPE }, { "byte", SYN_TYPE },
	{ "complex64", SYN_TYPE }, { "complex128", SYN_TYPE },
	{ "error", SYN_TYPE }, { "float32", SYN_TYPE },
	{ "float64", SYN_TYPE }, { "int", SYN_TYPE }, { "int8", SYN_TYPE },
	{ "int16", SYN_TYPE }, { "int32", SYN_TYPE }, { "int64", SYN_TYPE },
	{ "rune", SYN_TYPE }, { "string", SYN_TYPE }, { "uint", SYN_TYPE },
	{ "uint8", SYN_TYPE }, { "uint16", SYN_TYPE },
	{ "uint32", SYN_TYPE }, { "uint64", SYN_TYPE },
	{ "uintptr", SYN_TYPE }, { "any", SYN_TYPE },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
	{ "nil", SYN_CONSTANT }, { "iota", SYN_CONSTANT },
	{ "append", SYN_FUNCTION }, { "cap", SYN_FUNCTION },
	{ "close", SYN_FUNCTION }, { "complex", SYN_FUNCTION },
	{ "copy", SYN_FUNCTION }, { "delete", SYN_FUNCTION },
	{ "imag", SYN_FUNCTION }, { "len", SYN_FUNCTION },
	{ "make", SYN_FUNCTION }, { "new", SYN_FUNCTION },
	{ "panic", SYN_FUNCTION }, { "print", SYN_FUNCTION },
	{ "println", SYN_FUNCTION }, { "real", SYN_FUNCTION },
	{ "recover", SYN_FUNCTION },
};

static const struct syn_rule go_idle_rules[] = {
	{ "a-zA-Z_", GO_IDENT, 0, 1, 0 },
	{ "0-9", GO_NUM, 1, 0, 0 },
	{ "\"", GO_STR, 1, 0, 0 },
	{ "`", GO_RAW, 1, 0, 0 },
	{ "'", GO_CHR, 1, 0, 0 },
	{ "/", GO_SLASH, 0, 0, 0 },
	{ "+*%=<>!&|^:.-", GO_OP, 1, 0, 0 },
};

static const struct syn_rule go_ident_rules[] = {
	{ "a-zA-Z0-9_", GO_IDENT, 0, 0, 0 },
};

static const struct syn_rule go_num_rules[] = {
	{ "0-9a-fA-FxXoObBeE._i", GO_NUM, 0, 0, 0 },
};

static const struct syn_rule go_str_rules[] = {
	{ "\\", GO_STR_ESC, 0, 0, 0 },
	{ "\"", GO_IDLE, 0, 0, 0 },
	{ "\n", GO_IDLE, 0, 0, 0 },
};

static const struct syn_rule go_raw_rules[] = {
	{ "`", GO_IDLE, 0, 0, 0 },
};

static const struct syn_rule go_chr_rules[] = {
	{ "\\", GO_CHR_ESC, 0, 0, 0 },
	{ "'", GO_IDLE, 0, 0, 0 },
	{ "\n", GO_IDLE, 0, 0, 0 },
};

static const struct syn_rule go_slash_rules[] = {
	{ "/", GO_LINE, 2, 0, 0 },
	{ "*", GO_BLOCK, 2, 0, 0 },
};

static const struct syn_rule go_line_rules[] = {
	{ "\n", GO_IDLE, 0, 0, 0 },
};

static const struct syn_rule go_block_rules[] = {
	{ "*", GO_BLOCK_STAR, 0, 0, 0 },
};

static const struct syn_rule go_block_star_rules[] = {
	{ "/", GO_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state go_states[] = {
	[GO_IDLE] = { SYN_TEXT, GO_IDLE, 0, 0, go_idle_rules,
	    NR(go_idle_rules), NULL, 0 },
	[GO_IDENT] = { SYN_TEXT, GO_IDLE, 1, 0, go_ident_rules,
	    NR(go_ident_rules), go_keywords, NR(go_keywords) },
	[GO_NUM] = { SYN_CONSTANT, GO_IDLE, 1, 0, go_num_rules,
	    NR(go_num_rules), NULL, 0 },
	[GO_STR] = { SYN_STRING, GO_STR, 0, 0, go_str_rules,
	    NR(go_str_rules), NULL, 0 },
	[GO_STR_ESC] = { SYN_STRING, GO_STR, 0, 0, NULL, 0, NULL, 0 },
	[GO_RAW] = { SYN_STRING, GO_RAW, 0, 0, go_raw_rules,
	    NR(go_raw_rules), NULL, 0 },
	[GO_CHR] = { SYN_CONSTANT, GO_CHR, 0, 0, go_chr_rules,
	    NR(go_chr_rules), NULL, 0 },
	[GO_CHR_ESC] = { SYN_CONSTANT, GO_CHR, 0, 0, NULL, 0, NULL, 0 },
	[GO_SLASH] = { SYN_TEXT, GO_IDLE, 1, 0, go_slash_rules,
	    NR(go_slash_rules), NULL, 0 },
	[GO_LINE] = { SYN_COMMENT, GO_LINE, 0, 0, go_line_rules,
	    NR(go_line_rules), NULL, 0 },
	[GO_BLOCK] = { SYN_COMMENT, GO_BLOCK, 0, 0, go_block_rules,
	    NR(go_block_rules), NULL, 0 },
	[GO_BLOCK_STAR] = { SYN_COMMENT, GO_BLOCK, 1, 0, go_block_star_rules,
	    NR(go_block_star_rules), NULL, 0 },
	[GO_OP] = { SYN_OPERATOR, GO_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const go_exts[] = {
	"go", NULL,
};

const struct syntax syntax_go = {
	"go", GO_IDLE, 0, go_states, NR(go_states), go_exts,
};
