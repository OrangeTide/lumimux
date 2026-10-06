/* syn_c.c : C syntax table */

#include "syntax.h"

enum {
	C_IDLE,
	C_IDENT,
	C_NUM,
	C_STR,
	C_STR_ESC,
	C_CHR,
	C_CHR_ESC,
	C_SLASH,
	C_LINE,
	C_BLOCK,
	C_BLOCK_STAR,
	C_PREPROC,
	C_OP,
};

static const struct syn_kw c_keywords[] = {
	{ "NULL", SYN_CONSTANT },
	{ "auto", SYN_KEYWORD }, { "break", SYN_KEYWORD },
	{ "case", SYN_KEYWORD }, { "const", SYN_KEYWORD },
	{ "continue", SYN_KEYWORD }, { "default", SYN_KEYWORD },
	{ "do", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "enum", SYN_KEYWORD }, { "extern", SYN_KEYWORD },
	{ "for", SYN_KEYWORD }, { "goto", SYN_KEYWORD },
	{ "if", SYN_KEYWORD }, { "inline", SYN_KEYWORD },
	{ "register", SYN_KEYWORD }, { "restrict", SYN_KEYWORD },
	{ "return", SYN_KEYWORD }, { "sizeof", SYN_KEYWORD },
	{ "static", SYN_KEYWORD }, { "struct", SYN_KEYWORD },
	{ "switch", SYN_KEYWORD }, { "typedef", SYN_KEYWORD },
	{ "union", SYN_KEYWORD }, { "volatile", SYN_KEYWORD },
	{ "while", SYN_KEYWORD },
	{ "bool", SYN_TYPE }, { "char", SYN_TYPE }, { "double", SYN_TYPE },
	{ "float", SYN_TYPE }, { "int", SYN_TYPE }, { "long", SYN_TYPE },
	{ "short", SYN_TYPE }, { "signed", SYN_TYPE },
	{ "unsigned", SYN_TYPE }, { "void", SYN_TYPE },
	{ "size_t", SYN_TYPE }, { "ssize_t", SYN_TYPE },
	{ "int8_t", SYN_TYPE }, { "int16_t", SYN_TYPE },
	{ "int32_t", SYN_TYPE }, { "int64_t", SYN_TYPE },
	{ "uint8_t", SYN_TYPE }, { "uint16_t", SYN_TYPE },
	{ "uint32_t", SYN_TYPE }, { "uint64_t", SYN_TYPE },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
};

static const struct syn_rule c_idle_rules[] = {
	{ "a-zA-Z_", C_IDENT, 0, 1, 0 },
	{ "0-9", C_NUM, 1, 0, 0 },
	{ "\"", C_STR, 1, 0, 0 },
	{ "'", C_CHR, 1, 0, 0 },
	{ "/", C_SLASH, 0, 0, 0 },
	{ "#", C_PREPROC, 1, 0, 0 },
	{ "+*%=<>!&|^~?:.-", C_OP, 1, 0, 0 },
};

static const struct syn_rule c_ident_rules[] = {
	{ "a-zA-Z0-9_", C_IDENT, 0, 0, 0 },
};

static const struct syn_rule c_num_rules[] = {
	{ "0-9a-fA-FxXuUlL.", C_NUM, 0, 0, 0 },
};

static const struct syn_rule c_str_rules[] = {
	{ "\\", C_STR_ESC, 0, 0, 0 },
	{ "\"", C_IDLE, 0, 0, 0 },
	{ "\n", C_IDLE, 0, 0, 0 },
};

static const struct syn_rule c_chr_rules[] = {
	{ "\\", C_CHR_ESC, 0, 0, 0 },
	{ "'", C_IDLE, 0, 0, 0 },
	{ "\n", C_IDLE, 0, 0, 0 },
};

static const struct syn_rule c_slash_rules[] = {
	{ "/", C_LINE, 2, 0, 0 },
	{ "*", C_BLOCK, 2, 0, 0 },
};

static const struct syn_rule c_line_rules[] = {
	{ "\n", C_IDLE, 0, 0, 0 },
};

static const struct syn_rule c_block_rules[] = {
	{ "*", C_BLOCK_STAR, 0, 0, 0 },
};

static const struct syn_rule c_block_star_rules[] = {
	{ "/", C_IDLE, 0, 0, 0 },
};

static const struct syn_rule c_preproc_rules[] = {
	{ "\n", C_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state c_states[] = {
	[C_IDLE] = { SYN_TEXT, C_IDLE, 0, 0, c_idle_rules, NR(c_idle_rules),
	    NULL, 0 },
	[C_IDENT] = { SYN_TEXT, C_IDLE, 1, 0, c_ident_rules,
	    NR(c_ident_rules), c_keywords, NR(c_keywords) },
	[C_NUM] = { SYN_CONSTANT, C_IDLE, 1, 0, c_num_rules, NR(c_num_rules),
	    NULL, 0 },
	[C_STR] = { SYN_STRING, C_STR, 0, 0, c_str_rules, NR(c_str_rules),
	    NULL, 0 },
	[C_STR_ESC] = { SYN_STRING, C_STR, 0, 0, NULL, 0, NULL, 0 },
	[C_CHR] = { SYN_CONSTANT, C_CHR, 0, 0, c_chr_rules, NR(c_chr_rules),
	    NULL, 0 },
	[C_CHR_ESC] = { SYN_CONSTANT, C_CHR, 0, 0, NULL, 0, NULL, 0 },
	[C_SLASH] = { SYN_TEXT, C_IDLE, 1, 0, c_slash_rules,
	    NR(c_slash_rules), NULL, 0 },
	[C_LINE] = { SYN_COMMENT, C_LINE, 0, 0, c_line_rules,
	    NR(c_line_rules), NULL, 0 },
	[C_BLOCK] = { SYN_COMMENT, C_BLOCK, 0, 0, c_block_rules,
	    NR(c_block_rules), NULL, 0 },
	[C_BLOCK_STAR] = { SYN_COMMENT, C_BLOCK, 1, 0, c_block_star_rules,
	    NR(c_block_star_rules), NULL, 0 },
	[C_PREPROC] = { SYN_PREPROC, C_PREPROC, 0, 0, c_preproc_rules,
	    NR(c_preproc_rules), NULL, 0 },
	[C_OP] = { SYN_OPERATOR, C_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const c_exts[] = {
	"c", "h", "cc", "cpp", "cxx", "hpp", "hh", NULL,
};

const struct syntax syntax_c = {
	"c", C_IDLE, 0, c_states, NR(c_states), c_exts,
};
