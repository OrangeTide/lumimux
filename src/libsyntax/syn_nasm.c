/* syn_nasm.c : NASM x86 assembly syntax table */

#include "syntax.h"

/*
 * A table for NASM-syntax x86/x86-64 assembly, case-insensitive. Handles ;
 * line comments, 'single' and "double" quoted strings (raw, as NASM treats
 * them) and `backtick` strings (with C escapes), numbers in NASM's decimal,
 * hex, binary, and octal forms, and % preprocessor words (%define, %macro,
 * %if, %1, and so on). A keyword table paints common mnemonics and directives
 * as keywords and the general-purpose and SSE registers as types. Labels are
 * left as plain text, since a name is a label only by the trailing colon and
 * this table does not look ahead.
 */

enum {
	NA_IDLE,
	NA_IDENT,
	NA_NUM,
	NA_DQ,			/* "..." raw string */
	NA_SQ,			/* '...' raw string */
	NA_BQ,			/* `...` string with escapes */
	NA_BQ_ESC,
	NA_PP,			/* % preprocessor word */
	NA_COMMENT,		/* ; to end of line */
	NA_LABEL,		/* trailing ':' of a name: label */
	NA_OP,
};

static const struct syn_kw na_keywords[] = {
	/* directives and pseudo-instructions */
	{ "section", SYN_KEYWORD }, { "segment", SYN_KEYWORD },
	{ "global", SYN_KEYWORD }, { "extern", SYN_KEYWORD },
	{ "common", SYN_KEYWORD }, { "bits", SYN_KEYWORD },
	{ "default", SYN_KEYWORD }, { "org", SYN_KEYWORD },
	{ "align", SYN_KEYWORD }, { "equ", SYN_KEYWORD },
	{ "times", SYN_KEYWORD }, { "incbin", SYN_KEYWORD },
	{ "struc", SYN_KEYWORD }, { "endstruc", SYN_KEYWORD },
	{ "db", SYN_KEYWORD }, { "dw", SYN_KEYWORD },
	{ "dd", SYN_KEYWORD }, { "dq", SYN_KEYWORD },
	{ "dt", SYN_KEYWORD }, { "resb", SYN_KEYWORD },
	{ "resw", SYN_KEYWORD }, { "resd", SYN_KEYWORD },
	{ "resq", SYN_KEYWORD },
	/* mnemonics */
	{ "mov", SYN_KEYWORD }, { "movzx", SYN_KEYWORD },
	{ "movsx", SYN_KEYWORD }, { "lea", SYN_KEYWORD },
	{ "push", SYN_KEYWORD }, { "pop", SYN_KEYWORD },
	{ "add", SYN_KEYWORD }, { "sub", SYN_KEYWORD },
	{ "adc", SYN_KEYWORD }, { "sbb", SYN_KEYWORD },
	{ "inc", SYN_KEYWORD }, { "dec", SYN_KEYWORD },
	{ "imul", SYN_KEYWORD }, { "mul", SYN_KEYWORD },
	{ "idiv", SYN_KEYWORD }, { "div", SYN_KEYWORD },
	{ "neg", SYN_KEYWORD }, { "and", SYN_KEYWORD },
	{ "or", SYN_KEYWORD }, { "xor", SYN_KEYWORD },
	{ "not", SYN_KEYWORD }, { "shl", SYN_KEYWORD },
	{ "shr", SYN_KEYWORD }, { "sal", SYN_KEYWORD },
	{ "sar", SYN_KEYWORD }, { "rol", SYN_KEYWORD },
	{ "ror", SYN_KEYWORD }, { "cmp", SYN_KEYWORD },
	{ "test", SYN_KEYWORD }, { "jmp", SYN_KEYWORD },
	{ "je", SYN_KEYWORD }, { "jne", SYN_KEYWORD },
	{ "jz", SYN_KEYWORD }, { "jnz", SYN_KEYWORD },
	{ "jg", SYN_KEYWORD }, { "jge", SYN_KEYWORD },
	{ "jl", SYN_KEYWORD }, { "jle", SYN_KEYWORD },
	{ "ja", SYN_KEYWORD }, { "jae", SYN_KEYWORD },
	{ "jb", SYN_KEYWORD }, { "jbe", SYN_KEYWORD },
	{ "js", SYN_KEYWORD }, { "jns", SYN_KEYWORD },
	{ "jc", SYN_KEYWORD }, { "jnc", SYN_KEYWORD },
	{ "call", SYN_KEYWORD }, { "ret", SYN_KEYWORD },
	{ "leave", SYN_KEYWORD }, { "enter", SYN_KEYWORD },
	{ "nop", SYN_KEYWORD }, { "int", SYN_KEYWORD },
	{ "syscall", SYN_KEYWORD }, { "loop", SYN_KEYWORD },
	{ "xchg", SYN_KEYWORD }, { "cdq", SYN_KEYWORD },
	{ "cqo", SYN_KEYWORD }, { "hlt", SYN_KEYWORD },
	/* registers */
	{ "rax", SYN_TYPE }, { "rbx", SYN_TYPE }, { "rcx", SYN_TYPE },
	{ "rdx", SYN_TYPE }, { "rsi", SYN_TYPE }, { "rdi", SYN_TYPE },
	{ "rbp", SYN_TYPE }, { "rsp", SYN_TYPE }, { "rip", SYN_TYPE },
	{ "r8", SYN_TYPE }, { "r9", SYN_TYPE }, { "r10", SYN_TYPE },
	{ "r11", SYN_TYPE }, { "r12", SYN_TYPE }, { "r13", SYN_TYPE },
	{ "r14", SYN_TYPE }, { "r15", SYN_TYPE },
	{ "eax", SYN_TYPE }, { "ebx", SYN_TYPE }, { "ecx", SYN_TYPE },
	{ "edx", SYN_TYPE }, { "esi", SYN_TYPE }, { "edi", SYN_TYPE },
	{ "ebp", SYN_TYPE }, { "esp", SYN_TYPE },
	{ "ax", SYN_TYPE }, { "bx", SYN_TYPE }, { "cx", SYN_TYPE },
	{ "dx", SYN_TYPE }, { "si", SYN_TYPE }, { "di", SYN_TYPE },
	{ "al", SYN_TYPE }, { "bl", SYN_TYPE }, { "cl", SYN_TYPE },
	{ "dl", SYN_TYPE }, { "ah", SYN_TYPE }, { "bh", SYN_TYPE },
	{ "ch", SYN_TYPE }, { "dh", SYN_TYPE },
	{ "xmm0", SYN_TYPE }, { "xmm1", SYN_TYPE }, { "xmm2", SYN_TYPE },
	{ "xmm3", SYN_TYPE }, { "xmm4", SYN_TYPE }, { "xmm5", SYN_TYPE },
	{ "xmm6", SYN_TYPE }, { "xmm7", SYN_TYPE },
};

static const struct syn_rule na_idle_rules[] = {
	{ "a-zA-Z_.", NA_IDENT, 0, 1, 0 },
	{ "0-9", NA_NUM, 1, 0, 0 },
	{ "\"", NA_DQ, 1, 0, 0 },
	{ "'", NA_SQ, 1, 0, 0 },
	{ "`", NA_BQ, 1, 0, 0 },
	{ "%", NA_PP, 1, 0, 0 },
	{ ";", NA_COMMENT, 1, 0, 0 },
	{ "+*/=<>&|^~,:[]()-", NA_OP, 1, 0, 0 },
};

static const struct syn_rule na_ident_rules[] = {
	{ "a-zA-Z0-9_.$#@~?", NA_IDENT, 0, 0, 0, 0 },
	{ ":", NA_LABEL, 0, 0, 1, 1 },	/* name: -> label, recolor the name */
};

static const struct syn_rule na_label_rules[] = {
	{ ":", NA_IDLE, 0, 0, 0, 0 },
};

static const struct syn_rule na_num_rules[] = {
	{ "0-9a-fA-FxXbBhHoO._", NA_NUM, 0, 0, 0 },
};

static const struct syn_rule na_dq_rules[] = {
	{ "\"", NA_IDLE, 0, 0, 0 },
	{ "\n", NA_IDLE, 0, 0, 0 },
};

static const struct syn_rule na_sq_rules[] = {
	{ "'", NA_IDLE, 0, 0, 0 },
	{ "\n", NA_IDLE, 0, 0, 0 },
};

static const struct syn_rule na_bq_rules[] = {
	{ "\\", NA_BQ_ESC, 0, 0, 0 },
	{ "`", NA_IDLE, 0, 0, 0 },
	{ "\n", NA_IDLE, 0, 0, 0 },
};

static const struct syn_rule na_pp_rules[] = {
	{ "a-zA-Z0-9_", NA_PP, 0, 0, 0 },
};

static const struct syn_rule na_comment_rules[] = {
	{ "\n", NA_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state na_states[] = {
	[NA_IDLE] = { SYN_TEXT, NA_IDLE, 0, 0, na_idle_rules,
	    NR(na_idle_rules), NULL, 0 },
	[NA_IDENT] = { SYN_TEXT, NA_IDLE, 1, 0, na_ident_rules,
	    NR(na_ident_rules), na_keywords, NR(na_keywords) },
	[NA_NUM] = { SYN_CONSTANT, NA_IDLE, 1, 0, na_num_rules,
	    NR(na_num_rules), NULL, 0 },
	[NA_DQ] = { SYN_STRING, NA_DQ, 0, 0, na_dq_rules,
	    NR(na_dq_rules), NULL, 0 },
	[NA_SQ] = { SYN_STRING, NA_SQ, 0, 0, na_sq_rules,
	    NR(na_sq_rules), NULL, 0 },
	[NA_BQ] = { SYN_STRING, NA_BQ, 0, 0, na_bq_rules,
	    NR(na_bq_rules), NULL, 0 },
	[NA_BQ_ESC] = { SYN_STRING, NA_BQ, 0, 0, NULL, 0, NULL, 0 },
	[NA_PP] = { SYN_PREPROC, NA_IDLE, 1, 0, na_pp_rules,
	    NR(na_pp_rules), NULL, 0 },
	[NA_COMMENT] = { SYN_COMMENT, NA_COMMENT, 0, 0, na_comment_rules,
	    NR(na_comment_rules), NULL, 0 },
	[NA_LABEL] = { SYN_FUNCTION, NA_IDLE, 1, 0, na_label_rules,
	    NR(na_label_rules), NULL, 0 },
	[NA_OP] = { SYN_OPERATOR, NA_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const na_exts[] = {
	"asm", "nasm", NULL,
};

const struct syntax syntax_nasm = {
	"nasm", NA_IDLE, 1, na_states, NR(na_states), na_exts,
};
