/* syn_fth.c : Forth syntax table */

#include "syntax.h"

/*
 * Forth is whitespace-delimited: a word is any run of non-space bytes, and the
 * parser only acts at word boundaries. The IDLE state routes the special
 * word-starters (\ line comment, ( ... ) comment, a digit) and sends every
 * other printable byte into WORD, which runs until the next space, tab, or
 * newline and then looks the word up in the keyword table. The word-continue
 * class "!-~" is every printable ASCII byte except space, so punctuation words
 * like >r, 2dup, and 0= stay intact.
 *
 * Keyword lookup is case-insensitive, matching the traditional uppercase core
 * words as well as lowercase source. String words (." s" c" abort") introduce
 * a "..."-terminated string: since they all embed a ", a " seen inside a word
 * starts a string span and the word prefix and " are painted with it. This is
 * an approximation -- any " within a word triggers it, not only the canonical
 * words -- but those are the only places a " normally appears mid-word.
 */

enum {
	FTH_IDLE,
	FTH_WORD,
	FTH_NUM,
	FTH_STR,		/* ." s" c" abort" string body */
	FTH_LINE,		/* \ comment to end of line */
	FTH_PAREN,		/* ( ... ) comment, may span lines */
};

static const struct syn_kw fth_keywords[] = {
	{ ":", SYN_KEYWORD }, { ";", SYN_KEYWORD },
	{ "if", SYN_KEYWORD }, { "then", SYN_KEYWORD },
	{ "else", SYN_KEYWORD }, { "begin", SYN_KEYWORD },
	{ "while", SYN_KEYWORD }, { "repeat", SYN_KEYWORD },
	{ "until", SYN_KEYWORD }, { "again", SYN_KEYWORD },
	{ "do", SYN_KEYWORD }, { "?do", SYN_KEYWORD },
	{ "loop", SYN_KEYWORD }, { "+loop", SYN_KEYWORD },
	{ "leave", SYN_KEYWORD }, { "case", SYN_KEYWORD },
	{ "of", SYN_KEYWORD }, { "endof", SYN_KEYWORD },
	{ "endcase", SYN_KEYWORD }, { "exit", SYN_KEYWORD },
	{ "recurse", SYN_KEYWORD }, { "immediate", SYN_KEYWORD },
	{ "postpone", SYN_KEYWORD }, { "does>", SYN_KEYWORD },
	{ "create", SYN_KEYWORD }, { "variable", SYN_KEYWORD },
	{ "constant", SYN_KEYWORD }, { "value", SYN_KEYWORD },
	{ "to", SYN_KEYWORD }, { "defer", SYN_KEYWORD },
	{ "is", SYN_KEYWORD }, { "code", SYN_KEYWORD },
	{ "dup", SYN_FUNCTION }, { "?dup", SYN_FUNCTION },
	{ "drop", SYN_FUNCTION }, { "swap", SYN_FUNCTION },
	{ "over", SYN_FUNCTION }, { "rot", SYN_FUNCTION },
	{ "-rot", SYN_FUNCTION }, { "nip", SYN_FUNCTION },
	{ "tuck", SYN_FUNCTION }, { "pick", SYN_FUNCTION },
	{ "roll", SYN_FUNCTION }, { ">r", SYN_FUNCTION },
	{ "r>", SYN_FUNCTION }, { "r@", SYN_FUNCTION },
	{ "2dup", SYN_FUNCTION }, { "2drop", SYN_FUNCTION },
	{ "2swap", SYN_FUNCTION }, { "2over", SYN_FUNCTION },
	{ "@", SYN_FUNCTION }, { "!", SYN_FUNCTION },
	{ "+!", SYN_FUNCTION }, { "c@", SYN_FUNCTION },
	{ "c!", SYN_FUNCTION }, { "here", SYN_FUNCTION },
	{ "allot", SYN_FUNCTION }, { "cells", SYN_FUNCTION },
	{ "cell+", SYN_FUNCTION }, { "type", SYN_FUNCTION },
	{ "emit", SYN_FUNCTION }, { "cr", SYN_FUNCTION },
	{ "space", SYN_FUNCTION }, { "spaces", SYN_FUNCTION },
	{ "key", SYN_FUNCTION }, { "accept", SYN_FUNCTION },
	{ "words", SYN_FUNCTION }, { "execute", SYN_FUNCTION },
	{ "true", SYN_CONSTANT }, { "false", SYN_CONSTANT },
	{ "bl", SYN_CONSTANT },
};

static const struct syn_rule fth_idle_rules[] = {
	{ "\\", FTH_LINE, 1, 0, 0 },
	{ "(", FTH_PAREN, 1, 0, 0 },
	{ "0-9", FTH_NUM, 1, 0, 0 },
	{ " \t", FTH_IDLE, 0, 0, 0 },
	{ "!-~", FTH_WORD, 0, 1, 0 },
};

static const struct syn_rule fth_word_rules[] = {
	{ "\"", FTH_STR, 1, 0, 0, 1 },	/* ." s" c" ...: word prefix + string */
	{ "!-~", FTH_WORD, 0, 0, 0 },
};

static const struct syn_rule fth_str_rules[] = {
	{ "\"", FTH_IDLE, 0, 0, 0 },
	{ "\n", FTH_IDLE, 0, 0, 0 },
};

static const struct syn_rule fth_num_rules[] = {
	{ "0-9a-fA-F.xX", FTH_NUM, 0, 0, 0 },
};

static const struct syn_rule fth_line_rules[] = {
	{ "\n", FTH_IDLE, 0, 0, 0 },
};

static const struct syn_rule fth_paren_rules[] = {
	{ ")", FTH_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state fth_states[] = {
	[FTH_IDLE] = { SYN_TEXT, FTH_IDLE, 0, 0, fth_idle_rules,
	    NR(fth_idle_rules), NULL, 0 },
	[FTH_WORD] = { SYN_TEXT, FTH_IDLE, 1, 0, fth_word_rules,
	    NR(fth_word_rules), fth_keywords, NR(fth_keywords) },
	[FTH_NUM] = { SYN_CONSTANT, FTH_IDLE, 1, 0, fth_num_rules,
	    NR(fth_num_rules), NULL, 0 },
	[FTH_STR] = { SYN_STRING, FTH_STR, 0, 0, fth_str_rules,
	    NR(fth_str_rules), NULL, 0 },
	[FTH_LINE] = { SYN_COMMENT, FTH_LINE, 0, 0, fth_line_rules,
	    NR(fth_line_rules), NULL, 0 },
	[FTH_PAREN] = { SYN_COMMENT, FTH_PAREN, 0, 0, fth_paren_rules,
	    NR(fth_paren_rules), NULL, 0 },
};

static const char *const fth_exts[] = {
	"fth", "4th", "forth", "fs", NULL,
};

const struct syntax syntax_fth = {
	"forth", FTH_IDLE, 1, fth_states, NR(fth_states), fth_exts,
};
