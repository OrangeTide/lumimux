/* syn_html.c : HTML/XML syntax table */

#include "syntax.h"

/*
 * A structural highlighter for HTML and XML. Text between tags is left plain
 * except for &entity; references. Inside a tag the element name is painted as
 * a keyword, attribute names as types, and quoted attribute values as strings;
 * a quoted value keeps its color through any > it contains, since the tag ends
 * only at the closing quote's >. Comments (<!-- ... -->), doctypes (<!...>),
 * and processing instructions (<?...>) are handled too. There is no keyword
 * table: every color comes from the state a byte is in.
 *
 * Low-effort scope: the content of <script> and <style> elements is treated as
 * plain text, not highlighted as JavaScript or CSS.
 */

enum {
	HT_TEXT,
	HT_LT,			/* just saw '<' */
	HT_NAME,		/* element name inside a tag */
	HT_INTAG,		/* inside a tag, between name/attrs */
	HT_ATTR,		/* attribute name */
	HT_DQ,			/* "..." attribute value */
	HT_SQ,			/* '...' attribute value */
	HT_ENT,			/* &entity; reference */
	HT_BANG,		/* saw '<!' */
	HT_BANG_D,		/* saw '<!-' */
	HT_DECL,		/* <!doctype ...> or <?...?> to '>' */
	HT_COMMENT,		/* <!-- ... --> */
	HT_COMMENT_D,		/* one '-' inside a comment */
	HT_COMMENT_D2,		/* '--' inside a comment */
};

static const struct syn_rule ht_text_rules[] = {
	{ "<", HT_LT, 0, 0, 0 },
	{ "&", HT_ENT, 1, 0, 0 },
};

static const struct syn_rule ht_lt_rules[] = {
	{ "!", HT_BANG, 0, 0, 0 },
	{ "?", HT_DECL, 0, 0, 0 },
	{ "/", HT_NAME, 1, 0, 0 },
	{ "a-zA-Z", HT_NAME, 1, 0, 0 },
};

static const struct syn_rule ht_name_rules[] = {
	{ "a-zA-Z0-9:-", HT_NAME, 0, 0, 0 },
};

static const struct syn_rule ht_intag_rules[] = {
	{ ">", HT_TEXT, 0, 0, 0 },
	{ "\"", HT_DQ, 1, 0, 0 },
	{ "'", HT_SQ, 1, 0, 0 },
	{ "a-zA-Z:-", HT_ATTR, 1, 0, 0 },
};

static const struct syn_rule ht_attr_rules[] = {
	{ "a-zA-Z0-9:-", HT_ATTR, 0, 0, 0 },
};

static const struct syn_rule ht_dq_rules[] = {
	{ "\"", HT_INTAG, 0, 0, 0 },
};

static const struct syn_rule ht_sq_rules[] = {
	{ "'", HT_INTAG, 0, 0, 0 },
};

static const struct syn_rule ht_ent_rules[] = {
	{ ";", HT_TEXT, 0, 0, 0 },
	{ "a-zA-Z0-9#", HT_ENT, 0, 0, 0 },
};

static const struct syn_rule ht_bang_rules[] = {
	{ "-", HT_BANG_D, 0, 0, 0 },
};

static const struct syn_rule ht_bang_d_rules[] = {
	{ "-", HT_COMMENT, 4, 0, 0 },
};

static const struct syn_rule ht_decl_rules[] = {
	{ ">", HT_TEXT, 0, 0, 0 },
};

static const struct syn_rule ht_comment_rules[] = {
	{ "-", HT_COMMENT_D, 0, 0, 0 },
};

static const struct syn_rule ht_comment_d_rules[] = {
	{ "-", HT_COMMENT_D2, 0, 0, 0 },
};

static const struct syn_rule ht_comment_d2_rules[] = {
	{ ">", HT_TEXT, 0, 0, 0 },
	{ "-", HT_COMMENT_D2, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state ht_states[] = {
	[HT_TEXT] = { SYN_TEXT, HT_TEXT, 0, 0, ht_text_rules,
	    NR(ht_text_rules), NULL, 0 },
	[HT_LT] = { SYN_TEXT, HT_TEXT, 1, 0, ht_lt_rules,
	    NR(ht_lt_rules), NULL, 0 },
	[HT_NAME] = { SYN_KEYWORD, HT_INTAG, 1, 0, ht_name_rules,
	    NR(ht_name_rules), NULL, 0 },
	[HT_INTAG] = { SYN_TEXT, HT_INTAG, 0, 0, ht_intag_rules,
	    NR(ht_intag_rules), NULL, 0 },
	[HT_ATTR] = { SYN_TYPE, HT_INTAG, 1, 0, ht_attr_rules,
	    NR(ht_attr_rules), NULL, 0 },
	[HT_DQ] = { SYN_STRING, HT_DQ, 0, 0, ht_dq_rules,
	    NR(ht_dq_rules), NULL, 0 },
	[HT_SQ] = { SYN_STRING, HT_SQ, 0, 0, ht_sq_rules,
	    NR(ht_sq_rules), NULL, 0 },
	[HT_ENT] = { SYN_CONSTANT, HT_TEXT, 1, 0, ht_ent_rules,
	    NR(ht_ent_rules), NULL, 0 },
	[HT_BANG] = { SYN_TEXT, HT_DECL, 1, 0, ht_bang_rules,
	    NR(ht_bang_rules), NULL, 0 },
	[HT_BANG_D] = { SYN_TEXT, HT_DECL, 1, 0, ht_bang_d_rules,
	    NR(ht_bang_d_rules), NULL, 0 },
	[HT_DECL] = { SYN_PREPROC, HT_DECL, 0, 0, ht_decl_rules,
	    NR(ht_decl_rules), NULL, 0 },
	[HT_COMMENT] = { SYN_COMMENT, HT_COMMENT, 0, 0, ht_comment_rules,
	    NR(ht_comment_rules), NULL, 0 },
	[HT_COMMENT_D] = { SYN_COMMENT, HT_COMMENT, 1, 0, ht_comment_d_rules,
	    NR(ht_comment_d_rules), NULL, 0 },
	[HT_COMMENT_D2] = { SYN_COMMENT, HT_COMMENT, 1, 0, ht_comment_d2_rules,
	    NR(ht_comment_d2_rules), NULL, 0 },
};

static const char *const ht_exts[] = {
	"html", "htm", "xhtml", "xml", "svg", NULL,
};

const struct syntax syntax_html = {
	"html", HT_TEXT, 0, ht_states, NR(ht_states), ht_exts,
};
