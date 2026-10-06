/* syn_sh.c : POSIX/bash shell syntax table */

#include "syntax.h"

enum {
	SH_IDLE,
	SH_IDENT,
	SH_COMMENT,
	SH_DQ,
	SH_DQ_ESC,
	SH_SQ,
	SH_BQ,
	SH_VAR,
	SH_VARBRACE,
};

static const struct syn_kw sh_keywords[] = {
	{ "if", SYN_KEYWORD }, { "then", SYN_KEYWORD },
	{ "elif", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "fi", SYN_KEYWORD }, { "for", SYN_KEYWORD },
	{ "while", SYN_KEYWORD }, { "until", SYN_KEYWORD },
	{ "do", SYN_KEYWORD }, { "done", SYN_KEYWORD },
	{ "case", SYN_KEYWORD }, { "esac", SYN_KEYWORD },
	{ "in", SYN_KEYWORD }, { "function", SYN_KEYWORD },
	{ "select", SYN_KEYWORD }, { "time", SYN_KEYWORD },
	{ "echo", SYN_FUNCTION }, { "cd", SYN_FUNCTION },
	{ "export", SYN_FUNCTION }, { "local", SYN_FUNCTION },
	{ "readonly", SYN_FUNCTION }, { "return", SYN_FUNCTION },
	{ "exit", SYN_FUNCTION }, { "read", SYN_FUNCTION },
	{ "set", SYN_FUNCTION }, { "unset", SYN_FUNCTION },
	{ "shift", SYN_FUNCTION }, { "source", SYN_FUNCTION },
	{ "eval", SYN_FUNCTION }, { "exec", SYN_FUNCTION },
	{ "trap", SYN_FUNCTION }, { "test", SYN_FUNCTION },
	{ "printf", SYN_FUNCTION }, { "declare", SYN_FUNCTION },
	{ "getopts", SYN_FUNCTION }, { "break", SYN_FUNCTION },
	{ "continue", SYN_FUNCTION },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
};

static const struct syn_rule sh_idle_rules[] = {
	{ "a-zA-Z_", SH_IDENT, 0, 1, 0 },
	{ "#", SH_COMMENT, 1, 0, 0 },
	{ "\"", SH_DQ, 1, 0, 0 },
	{ "'", SH_SQ, 1, 0, 0 },
	{ "`", SH_BQ, 1, 0, 0 },
	{ "$", SH_VAR, 1, 0, 0 },
};

static const struct syn_rule sh_ident_rules[] = {
	{ "a-zA-Z0-9_", SH_IDENT, 0, 0, 0 },
};

static const struct syn_rule sh_comment_rules[] = {
	{ "\n", SH_IDLE, 0, 0, 0 },
};

static const struct syn_rule sh_dq_rules[] = {
	{ "\\", SH_DQ_ESC, 0, 0, 0 },
	{ "\"", SH_IDLE, 0, 0, 0 },
};

static const struct syn_rule sh_sq_rules[] = {
	{ "'", SH_IDLE, 0, 0, 0 },
};

static const struct syn_rule sh_bq_rules[] = {
	{ "`", SH_IDLE, 0, 0, 0 },
};

static const struct syn_rule sh_var_rules[] = {
	{ "{", SH_VARBRACE, 0, 0, 0 },
	{ "a-zA-Z0-9_", SH_VAR, 0, 0, 0 },
};

static const struct syn_rule sh_varbrace_rules[] = {
	{ "}", SH_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state sh_states[] = {
	[SH_IDLE] = { SYN_TEXT, SH_IDLE, 0, 0, sh_idle_rules,
	    NR(sh_idle_rules), NULL, 0 },
	[SH_IDENT] = { SYN_TEXT, SH_IDLE, 1, 0, sh_ident_rules,
	    NR(sh_ident_rules), sh_keywords, NR(sh_keywords) },
	[SH_COMMENT] = { SYN_COMMENT, SH_COMMENT, 0, 0, sh_comment_rules,
	    NR(sh_comment_rules), NULL, 0 },
	[SH_DQ] = { SYN_STRING, SH_DQ, 0, 0, sh_dq_rules, NR(sh_dq_rules),
	    NULL, 0 },
	[SH_DQ_ESC] = { SYN_STRING, SH_DQ, 0, 0, NULL, 0, NULL, 0 },
	[SH_SQ] = { SYN_STRING, SH_SQ, 0, 0, sh_sq_rules, NR(sh_sq_rules),
	    NULL, 0 },
	[SH_BQ] = { SYN_TEXT, SH_BQ, 0, 0, sh_bq_rules, NR(sh_bq_rules),
	    NULL, 0 },
	[SH_VAR] = { SYN_PREPROC, SH_IDLE, 1, 0, sh_var_rules,
	    NR(sh_var_rules), NULL, 0 },
	[SH_VARBRACE] = { SYN_PREPROC, SH_VARBRACE, 0, 0, sh_varbrace_rules,
	    NR(sh_varbrace_rules), NULL, 0 },
};

static const char *const sh_exts[] = {
	"sh", "bash", "ksh", "zsh", "mk", NULL,
};

const struct syntax syntax_sh = {
	"sh", SH_IDLE, 0, sh_states, NR(sh_states), sh_exts,
};
