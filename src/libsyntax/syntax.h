/* syntax.h : data-driven syntax highlighter (a joe/JSF-style state machine) */

#ifndef SYNTAX_H
#define SYNTAX_H

#include <stddef.h>
#include <stdint.h>

/*
 * A tiny state-machine highlighter modeled on joe's syntax files. Each
 * language is a table of states; each state has a color, a default
 * fall-through, and a list of rules. A rule matches the current byte against
 * a character class and moves to another state, optionally recoloring the
 * bytes just emitted or starting an identifier buffer for keyword lookup.
 *
 * The engine runs one line at a time, taking the state left over from the
 * previous line and returning the state to carry into the next, so multi-line
 * comments and strings colorize correctly.
 *
 * Definitions are C tables today; the structs are public so a file loader
 * could build the same tables later.
 */

/* The fixed palette of styles a rule or state can paint with. The renderer
 * maps these to terminal colors. */
enum syn_style {
	SYN_TEXT,		/* default, unhighlighted */
	SYN_COMMENT,
	SYN_KEYWORD,
	SYN_TYPE,
	SYN_CONSTANT,		/* numbers, character constants */
	SYN_STRING,
	SYN_OPERATOR,
	SYN_FUNCTION,
	SYN_PREPROC,		/* C preprocessor, shell $VAR, etc. */
	SYN_STYLE_COUNT,
};

/* A keyword and the style to paint it. The engine scans the array linearly,
 * so the order does not matter. */
struct syn_kw {
	const char	*word;
	enum syn_style	style;
};

/*
 * One transition rule. match is a character class in the joe style: a string
 * of literal bytes and lo-hi ranges, for example "a-zA-Z0-9_" or "+-*"; a
 * newline is written "\n". There is no wildcard -- the state's default
 * handles every byte no rule claims.
 */
struct syn_rule {
	const char	*match;
	uint16_t	next;		/* index of the next state */
	uint8_t		recolor;	/* repaint the last N bytes emitted */
	uint8_t		buffer;		/* begin an identifier buffer here */
	uint8_t		noeat;		/* do not consume the byte */
	uint8_t		recolor_buf;	/* repaint the whole current buffer to the
					 * next state's style (for labels, where
					 * the identifier length is not fixed) */
};

struct syn_state {
	enum syn_style		style;		/* color for bytes this state emits */
	uint16_t		def;		/* state when no rule matches */
	uint8_t			def_noeat;	/* default keeps the byte for reuse */
	uint8_t			def_recolor;	/* default repaints the last N bytes */
	const struct syn_rule	*rules;
	uint16_t		nrules;
	const struct syn_kw	*keywords;	/* looked up when leaving via default */
	uint16_t		nkeywords;
};

struct syntax {
	const char		*name;
	uint16_t		start;		/* initial state index */
	uint8_t			nocase;		/* keyword lookup ignores case */
	const struct syn_state	*states;
	uint16_t		nstates;
	const char *const	*exts;		/* NULL-terminated file extensions */
};

/*
 * Highlight one line. state_in is the state left by the previous line (use
 * sy->start for the first line). bytes[0,n) are the line's bytes without the
 * newline. When out is non-NULL it receives one enum syn_style per byte.
 * Returns the state to pass to the next line.
 */
uint16_t syn_line(const struct syntax *sy, uint16_t state_in,
    const char *bytes, size_t n, uint8_t *out);

/* The built-in language whose extension list contains ext (without the dot),
 * or NULL when none matches. ext may be NULL. */
const struct syntax *syn_for_ext(const char *ext);

#endif /* SYNTAX_H */
