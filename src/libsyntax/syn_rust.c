/* syn_rust.c : Rust syntax table */

#include "syntax.h"

/*
 * Char literals vs lifetimes: a ' followed by one char and then a ' is a char
 * literal; a ' followed by a letter and then a non-' is a lifetime. That test
 * is exact, so 'x', '\n', and ' ' colorize as constants while 'a and 'static
 * stay plain. Raw strings are recognized for zero and one hash levels (r"..."
 * and r#"..."#); deeper (r##"..."##) and byte-raw (br"...") forms are not.
 * Block comments nest one level, so an inner block-comment close does not end
 * an outer one; a third nesting level closes one delimiter early.
 */
enum {
	RS_IDLE,
	RS_IDENT,
	RS_NUM,
	RS_STR,
	RS_STR_ESC,
	RS_SLASH,
	RS_LINE,
	RS_BLOCK,
	RS_BLOCK_STAR,
	RS_BLOCK_SLASH,		/* saw '/' inside a block comment */
	RS_BLOCK2,		/* one level of nested block comment */
	RS_BLOCK2_STAR,
	RS_BLOCK2_SLASH,
	RS_QUOTE,		/* saw ' */
	RS_QUOTE_ID,		/* saw ' then a letter (char lit or lifetime) */
	RS_CHAR_BODY,		/* char literal body, expecting closing ' */
	RS_CHAR_BODY_ESC,
	RS_CHAR_CLOSE,		/* colors the closing ' */
	RS_LIFETIME,		/* 'a, 'static: left uncolored */
	RS_R,			/* saw 'r' (raw-string lead-in or identifier) */
	RS_RH1,			/* saw r# */
	RS_RAW0,		/* r"..." body */
	RS_RAW1,		/* r#"..."# body */
	RS_RAW1_H,		/* saw the closing " of an r#"..." */
	RS_OP,
};

static const struct syn_kw rust_keywords[] = {
	{ "as", SYN_KEYWORD }, { "async", SYN_KEYWORD },
	{ "await", SYN_KEYWORD }, { "break", SYN_KEYWORD },
	{ "const", SYN_KEYWORD }, { "continue", SYN_KEYWORD },
	{ "crate", SYN_KEYWORD }, { "dyn", SYN_KEYWORD },
	{ "else", SYN_KEYWORD }, { "enum", SYN_KEYWORD },
	{ "extern", SYN_KEYWORD }, { "fn", SYN_KEYWORD },
	{ "for", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "impl", SYN_KEYWORD }, { "in", SYN_KEYWORD },
	{ "let", SYN_KEYWORD }, { "loop", SYN_KEYWORD },
	{ "match", SYN_KEYWORD }, { "mod", SYN_KEYWORD },
	{ "move", SYN_KEYWORD }, { "mut", SYN_KEYWORD },
	{ "pub", SYN_KEYWORD }, { "ref", SYN_KEYWORD },
	{ "return", SYN_KEYWORD }, { "self", SYN_KEYWORD },
	{ "static", SYN_KEYWORD }, { "struct", SYN_KEYWORD },
	{ "super", SYN_KEYWORD }, { "trait", SYN_KEYWORD },
	{ "type", SYN_KEYWORD }, { "unsafe", SYN_KEYWORD },
	{ "use", SYN_KEYWORD }, { "where", SYN_KEYWORD },
	{ "while", SYN_KEYWORD },
	{ "bool", SYN_TYPE }, { "char", SYN_TYPE }, { "f32", SYN_TYPE },
	{ "f64", SYN_TYPE }, { "i8", SYN_TYPE }, { "i16", SYN_TYPE },
	{ "i32", SYN_TYPE }, { "i64", SYN_TYPE }, { "i128", SYN_TYPE },
	{ "isize", SYN_TYPE }, { "u8", SYN_TYPE }, { "u16", SYN_TYPE },
	{ "u32", SYN_TYPE }, { "u64", SYN_TYPE }, { "u128", SYN_TYPE },
	{ "usize", SYN_TYPE }, { "str", SYN_TYPE }, { "String", SYN_TYPE },
	{ "Vec", SYN_TYPE }, { "Option", SYN_TYPE }, { "Result", SYN_TYPE },
	{ "Box", SYN_TYPE }, { "Self", SYN_TYPE },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
	{ "None", SYN_CONSTANT }, { "Some", SYN_CONSTANT },
	{ "Ok", SYN_CONSTANT }, { "Err", SYN_CONSTANT },
};

static const struct syn_rule rust_idle_rules[] = {
	{ "r", RS_R, 0, 1, 0 },		/* raw-string lead-in or identifier */
	{ "a-zA-Z_", RS_IDENT, 0, 1, 0 },
	{ "0-9", RS_NUM, 1, 0, 0 },
	{ "\"", RS_STR, 1, 0, 0 },
	{ "'", RS_QUOTE, 0, 1, 0 },
	{ "/", RS_SLASH, 0, 0, 0 },
	{ "+*%=<>!&|^~?:.@-", RS_OP, 1, 0, 0 },
};

static const struct syn_rule rust_ident_rules[] = {
	{ "a-zA-Z0-9_", RS_IDENT, 0, 0, 0 },
};

static const struct syn_rule rust_num_rules[] = {
	{ "0-9a-fA-FxXoObBeE._iuf", RS_NUM, 0, 0, 0 },
};

static const struct syn_rule rust_str_rules[] = {
	{ "\\", RS_STR_ESC, 0, 0, 0 },
	{ "\"", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_slash_rules[] = {
	{ "/", RS_LINE, 2, 0, 0 },
	{ "*", RS_BLOCK, 2, 0, 0 },
};

static const struct syn_rule rust_line_rules[] = {
	{ "\n", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_block_rules[] = {
	{ "*", RS_BLOCK_STAR, 0, 0, 0 },
	{ "/", RS_BLOCK_SLASH, 0, 0, 0 },
};

static const struct syn_rule rust_block_star_rules[] = {
	{ "/", RS_IDLE, 0, 0, 0 },
	{ "*", RS_BLOCK_STAR, 0, 0, 0 },
};

static const struct syn_rule rust_block_slash_rules[] = {
	{ "*", RS_BLOCK2, 0, 0, 0 },
	{ "/", RS_BLOCK_SLASH, 0, 0, 0 },
};

static const struct syn_rule rust_block2_rules[] = {
	{ "*", RS_BLOCK2_STAR, 0, 0, 0 },
	{ "/", RS_BLOCK2_SLASH, 0, 0, 0 },
};

static const struct syn_rule rust_block2_star_rules[] = {
	{ "/", RS_BLOCK, 0, 0, 0 },
	{ "*", RS_BLOCK2_STAR, 0, 0, 0 },
};

static const struct syn_rule rust_block2_slash_rules[] = {
	{ "*", RS_BLOCK2, 0, 0, 0 },
	{ "/", RS_BLOCK2_SLASH, 0, 0, 0 },
};

static const struct syn_rule rust_quote_rules[] = {
	{ "\\", RS_CHAR_BODY_ESC, 0, 0, 0 },
	{ "a-zA-Z_", RS_QUOTE_ID, 0, 0, 0 },
	{ "'", RS_IDLE, 0, 0, 0 },
	{ "\n", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_quote_id_rules[] = {
	{ "'", RS_CHAR_CLOSE, 0, 0, 1, 1 },	/* 'x' char: recolor buffer */
	{ "a-zA-Z0-9_", RS_LIFETIME, 0, 0, 0 },
};

static const struct syn_rule rust_char_body_rules[] = {
	{ "\\", RS_CHAR_BODY_ESC, 0, 0, 0 },
	{ "'", RS_CHAR_CLOSE, 0, 0, 1, 1 },
	{ "\n", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_char_close_rules[] = {
	{ "'", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_lifetime_rules[] = {
	{ "a-zA-Z0-9_", RS_LIFETIME, 0, 0, 0 },
};

static const struct syn_rule rust_r_rules[] = {
	{ "\"", RS_RAW0, 2, 0, 0 },	/* r" : paint the r" as string */
	{ "#", RS_RH1, 0, 0, 0 },
	{ "a-zA-Z0-9_", RS_IDENT, 0, 0, 0 },
};

static const struct syn_rule rust_rh1_rules[] = {
	{ "\"", RS_RAW1, 3, 0, 0 },	/* r#" : paint the r#" as string */
};

static const struct syn_rule rust_raw0_rules[] = {
	{ "\"", RS_IDLE, 0, 0, 0 },
};

static const struct syn_rule rust_raw1_rules[] = {
	{ "\"", RS_RAW1_H, 0, 0, 0 },
};

static const struct syn_rule rust_raw1_h_rules[] = {
	{ "#", RS_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state rust_states[] = {
	[RS_IDLE] = { SYN_TEXT, RS_IDLE, 0, 0, rust_idle_rules,
	    NR(rust_idle_rules), NULL, 0 },
	[RS_IDENT] = { SYN_TEXT, RS_IDLE, 1, 0, rust_ident_rules,
	    NR(rust_ident_rules), rust_keywords, NR(rust_keywords) },
	[RS_NUM] = { SYN_CONSTANT, RS_IDLE, 1, 0, rust_num_rules,
	    NR(rust_num_rules), NULL, 0 },
	[RS_STR] = { SYN_STRING, RS_STR, 0, 0, rust_str_rules,
	    NR(rust_str_rules), NULL, 0 },
	[RS_STR_ESC] = { SYN_STRING, RS_STR, 0, 0, NULL, 0, NULL, 0 },
	[RS_SLASH] = { SYN_TEXT, RS_IDLE, 1, 0, rust_slash_rules,
	    NR(rust_slash_rules), NULL, 0 },
	[RS_LINE] = { SYN_COMMENT, RS_LINE, 0, 0, rust_line_rules,
	    NR(rust_line_rules), NULL, 0 },
	[RS_BLOCK] = { SYN_COMMENT, RS_BLOCK, 0, 0, rust_block_rules,
	    NR(rust_block_rules), NULL, 0 },
	[RS_BLOCK_STAR] = { SYN_COMMENT, RS_BLOCK, 1, 0, rust_block_star_rules,
	    NR(rust_block_star_rules), NULL, 0 },
	[RS_BLOCK_SLASH] = { SYN_COMMENT, RS_BLOCK, 1, 0, rust_block_slash_rules,
	    NR(rust_block_slash_rules), NULL, 0 },
	[RS_BLOCK2] = { SYN_COMMENT, RS_BLOCK2, 0, 0, rust_block2_rules,
	    NR(rust_block2_rules), NULL, 0 },
	[RS_BLOCK2_STAR] = { SYN_COMMENT, RS_BLOCK2, 1, 0, rust_block2_star_rules,
	    NR(rust_block2_star_rules), NULL, 0 },
	[RS_BLOCK2_SLASH] = { SYN_COMMENT, RS_BLOCK2, 1, 0,
	    rust_block2_slash_rules, NR(rust_block2_slash_rules), NULL, 0 },
	[RS_QUOTE] = { SYN_TEXT, RS_CHAR_BODY, 0, 0, rust_quote_rules,
	    NR(rust_quote_rules), NULL, 0 },
	[RS_QUOTE_ID] = { SYN_TEXT, RS_LIFETIME, 1, 0, rust_quote_id_rules,
	    NR(rust_quote_id_rules), NULL, 0 },
	[RS_CHAR_BODY] = { SYN_TEXT, RS_CHAR_BODY, 0, 0, rust_char_body_rules,
	    NR(rust_char_body_rules), NULL, 0 },
	[RS_CHAR_BODY_ESC] = { SYN_TEXT, RS_CHAR_BODY, 0, 0, NULL, 0, NULL, 0 },
	[RS_CHAR_CLOSE] = { SYN_CONSTANT, RS_IDLE, 0, 0, rust_char_close_rules,
	    NR(rust_char_close_rules), NULL, 0 },
	[RS_LIFETIME] = { SYN_TEXT, RS_IDLE, 1, 0, rust_lifetime_rules,
	    NR(rust_lifetime_rules), NULL, 0 },
	[RS_R] = { SYN_TEXT, RS_IDENT, 1, 0, rust_r_rules,
	    NR(rust_r_rules), rust_keywords, NR(rust_keywords) },
	[RS_RH1] = { SYN_TEXT, RS_IDLE, 1, 0, rust_rh1_rules,
	    NR(rust_rh1_rules), NULL, 0 },
	[RS_RAW0] = { SYN_STRING, RS_RAW0, 0, 0, rust_raw0_rules,
	    NR(rust_raw0_rules), NULL, 0 },
	[RS_RAW1] = { SYN_STRING, RS_RAW1, 0, 0, rust_raw1_rules,
	    NR(rust_raw1_rules), NULL, 0 },
	[RS_RAW1_H] = { SYN_STRING, RS_RAW1, 1, 0, rust_raw1_h_rules,
	    NR(rust_raw1_h_rules), NULL, 0 },
	[RS_OP] = { SYN_OPERATOR, RS_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const rust_exts[] = {
	"rs", NULL,
};

const struct syntax syntax_rust = {
	"rust", RS_IDLE, 0, rust_states, NR(rust_states), rust_exts,
};
