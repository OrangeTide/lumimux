/* syn_gas.c : GNU assembler (AT&T) syntax table */

#include "syntax.h"

/*
 * A table for GNU assembler (AT&T) x86/x86-64 source. Comments come in three
 * forms: # to end of line, // to end of line, and block comments. Handles
 * "double" and 'single' quoted strings with escapes. The AT&T sigils give most
 * of the color: .directives are painted as preproc, %registers as types, and
 * $immediates as constants. A keyword table paints common mnemonics; the
 * size-suffixed forms (movl, movq, and so on) for the frequently used
 * instructions are listed, but rarer suffixed mnemonics are left as plain
 * text, since enumerating every suffix combination is not worthwhile here.
 */

enum {
	GA_IDLE,
	GA_IDENT,
	GA_NUM,
	GA_DQ,
	GA_DQ_ESC,
	GA_SQ,
	GA_SQ_ESC,
	GA_DIR,			/* .directive */
	GA_REG,			/* %register */
	GA_IMM,			/* $immediate */
	GA_HASH,		/* # comment to end of line */
	GA_SLASH,
	GA_LINE,		/* // comment */
	GA_BLOCK,
	GA_BLOCK_STAR,
	GA_LABEL,		/* trailing ':' of a name: label */
	GA_OP,
};

static const struct syn_kw ga_keywords[] = {
	{ "mov", SYN_KEYWORD }, { "movl", SYN_KEYWORD },
	{ "movq", SYN_KEYWORD }, { "movb", SYN_KEYWORD },
	{ "movw", SYN_KEYWORD }, { "movzbl", SYN_KEYWORD },
	{ "movslq", SYN_KEYWORD }, { "lea", SYN_KEYWORD },
	{ "leal", SYN_KEYWORD }, { "leaq", SYN_KEYWORD },
	{ "push", SYN_KEYWORD }, { "pushq", SYN_KEYWORD },
	{ "pushl", SYN_KEYWORD }, { "pop", SYN_KEYWORD },
	{ "popq", SYN_KEYWORD }, { "popl", SYN_KEYWORD },
	{ "add", SYN_KEYWORD }, { "addl", SYN_KEYWORD },
	{ "addq", SYN_KEYWORD }, { "sub", SYN_KEYWORD },
	{ "subl", SYN_KEYWORD }, { "subq", SYN_KEYWORD },
	{ "imul", SYN_KEYWORD }, { "imull", SYN_KEYWORD },
	{ "imulq", SYN_KEYWORD }, { "inc", SYN_KEYWORD },
	{ "incl", SYN_KEYWORD }, { "incq", SYN_KEYWORD },
	{ "dec", SYN_KEYWORD }, { "decl", SYN_KEYWORD },
	{ "decq", SYN_KEYWORD }, { "and", SYN_KEYWORD },
	{ "andl", SYN_KEYWORD }, { "andq", SYN_KEYWORD },
	{ "or", SYN_KEYWORD }, { "orl", SYN_KEYWORD },
	{ "orq", SYN_KEYWORD }, { "xor", SYN_KEYWORD },
	{ "xorl", SYN_KEYWORD }, { "xorq", SYN_KEYWORD },
	{ "not", SYN_KEYWORD }, { "neg", SYN_KEYWORD },
	{ "shl", SYN_KEYWORD }, { "shr", SYN_KEYWORD },
	{ "sar", SYN_KEYWORD }, { "cmp", SYN_KEYWORD },
	{ "cmpl", SYN_KEYWORD }, { "cmpq", SYN_KEYWORD },
	{ "test", SYN_KEYWORD }, { "testl", SYN_KEYWORD },
	{ "testq", SYN_KEYWORD }, { "jmp", SYN_KEYWORD },
	{ "je", SYN_KEYWORD }, { "jne", SYN_KEYWORD },
	{ "jz", SYN_KEYWORD }, { "jnz", SYN_KEYWORD },
	{ "jg", SYN_KEYWORD }, { "jge", SYN_KEYWORD },
	{ "jl", SYN_KEYWORD }, { "jle", SYN_KEYWORD },
	{ "ja", SYN_KEYWORD }, { "jb", SYN_KEYWORD },
	{ "js", SYN_KEYWORD }, { "jns", SYN_KEYWORD },
	{ "call", SYN_KEYWORD }, { "callq", SYN_KEYWORD },
	{ "ret", SYN_KEYWORD }, { "retq", SYN_KEYWORD },
	{ "leave", SYN_KEYWORD }, { "nop", SYN_KEYWORD },
	{ "int", SYN_KEYWORD }, { "syscall", SYN_KEYWORD },
	{ "cltq", SYN_KEYWORD }, { "cqto", SYN_KEYWORD },
	{ "xchg", SYN_KEYWORD }, { "hlt", SYN_KEYWORD },
};

static const struct syn_rule ga_idle_rules[] = {
	{ "a-zA-Z_", GA_IDENT, 0, 1, 0 },
	{ "0-9", GA_NUM, 1, 0, 0 },
	{ "\"", GA_DQ, 1, 0, 0 },
	{ "'", GA_SQ, 1, 0, 0 },
	{ ".", GA_DIR, 1, 1, 0 },
	{ "%", GA_REG, 1, 0, 0 },
	{ "$", GA_IMM, 1, 0, 0 },
	{ "#", GA_HASH, 1, 0, 0 },
	{ "/", GA_SLASH, 0, 0, 0 },
	{ "+*=<>&|^~,:()@-", GA_OP, 1, 0, 0 },
};

static const struct syn_rule ga_ident_rules[] = {
	{ "a-zA-Z0-9_.$", GA_IDENT, 0, 0, 0, 0 },
	{ ":", GA_LABEL, 0, 0, 1, 1 },	/* name: -> label, recolor the name */
};

static const struct syn_rule ga_label_rules[] = {
	{ ":", GA_IDLE, 0, 0, 0, 0 },
};

static const struct syn_rule ga_num_rules[] = {
	{ "0-9a-fA-FxXbB._", GA_NUM, 0, 0, 0 },
};

static const struct syn_rule ga_dq_rules[] = {
	{ "\\", GA_DQ_ESC, 0, 0, 0 },
	{ "\"", GA_IDLE, 0, 0, 0 },
	{ "\n", GA_IDLE, 0, 0, 0 },
};

static const struct syn_rule ga_sq_rules[] = {
	{ "\\", GA_SQ_ESC, 0, 0, 0 },
	{ "'", GA_IDLE, 0, 0, 0 },
	{ "\n", GA_IDLE, 0, 0, 0 },
};

static const struct syn_rule ga_dir_rules[] = {
	{ "a-zA-Z0-9_.", GA_DIR, 0, 0, 0, 0 },
	{ ":", GA_LABEL, 0, 0, 1, 1 },	/* .L1: local label, recolor the name */
};

static const struct syn_rule ga_reg_rules[] = {
	{ "a-zA-Z0-9", GA_REG, 0, 0, 0 },
};

static const struct syn_rule ga_imm_rules[] = {
	{ "0-9a-fA-FxX._", GA_IMM, 0, 0, 0 },
};

static const struct syn_rule ga_hash_rules[] = {
	{ "\n", GA_IDLE, 0, 0, 0 },
};

static const struct syn_rule ga_slash_rules[] = {
	{ "/", GA_LINE, 2, 0, 0 },
	{ "*", GA_BLOCK, 2, 0, 0 },
};

static const struct syn_rule ga_line_rules[] = {
	{ "\n", GA_IDLE, 0, 0, 0 },
};

static const struct syn_rule ga_block_rules[] = {
	{ "*", GA_BLOCK_STAR, 0, 0, 0 },
};

static const struct syn_rule ga_block_star_rules[] = {
	{ "/", GA_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state ga_states[] = {
	[GA_IDLE] = { SYN_TEXT, GA_IDLE, 0, 0, ga_idle_rules,
	    NR(ga_idle_rules), NULL, 0 },
	[GA_IDENT] = { SYN_TEXT, GA_IDLE, 1, 0, ga_ident_rules,
	    NR(ga_ident_rules), ga_keywords, NR(ga_keywords) },
	[GA_NUM] = { SYN_CONSTANT, GA_IDLE, 1, 0, ga_num_rules,
	    NR(ga_num_rules), NULL, 0 },
	[GA_DQ] = { SYN_STRING, GA_DQ, 0, 0, ga_dq_rules,
	    NR(ga_dq_rules), NULL, 0 },
	[GA_DQ_ESC] = { SYN_STRING, GA_DQ, 0, 0, NULL, 0, NULL, 0 },
	[GA_SQ] = { SYN_STRING, GA_SQ, 0, 0, ga_sq_rules,
	    NR(ga_sq_rules), NULL, 0 },
	[GA_SQ_ESC] = { SYN_STRING, GA_SQ, 0, 0, NULL, 0, NULL, 0 },
	[GA_DIR] = { SYN_PREPROC, GA_IDLE, 1, 0, ga_dir_rules,
	    NR(ga_dir_rules), NULL, 0 },
	[GA_REG] = { SYN_TYPE, GA_IDLE, 1, 0, ga_reg_rules,
	    NR(ga_reg_rules), NULL, 0 },
	[GA_IMM] = { SYN_CONSTANT, GA_IDLE, 1, 0, ga_imm_rules,
	    NR(ga_imm_rules), NULL, 0 },
	[GA_HASH] = { SYN_COMMENT, GA_HASH, 0, 0, ga_hash_rules,
	    NR(ga_hash_rules), NULL, 0 },
	[GA_SLASH] = { SYN_TEXT, GA_IDLE, 1, 0, ga_slash_rules,
	    NR(ga_slash_rules), NULL, 0 },
	[GA_LINE] = { SYN_COMMENT, GA_LINE, 0, 0, ga_line_rules,
	    NR(ga_line_rules), NULL, 0 },
	[GA_BLOCK] = { SYN_COMMENT, GA_BLOCK, 0, 0, ga_block_rules,
	    NR(ga_block_rules), NULL, 0 },
	[GA_BLOCK_STAR] = { SYN_COMMENT, GA_BLOCK, 1, 0, ga_block_star_rules,
	    NR(ga_block_star_rules), NULL, 0 },
	[GA_LABEL] = { SYN_FUNCTION, GA_IDLE, 1, 0, ga_label_rules,
	    NR(ga_label_rules), NULL, 0 },
	[GA_OP] = { SYN_OPERATOR, GA_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const ga_exts[] = {
	"s", "S", "gas", NULL,
};

const struct syntax syntax_gas = {
	"gas", GA_IDLE, 1, ga_states, NR(ga_states), ga_exts,
};
