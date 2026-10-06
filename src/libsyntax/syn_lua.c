/* syn_lua.c : Lua syntax table */

#include "syntax.h"

enum {
	LUA_IDLE,
	LUA_IDENT,
	LUA_NUM,
	LUA_DQ,
	LUA_DQ_ESC,
	LUA_SQ,
	LUA_SQ_ESC,
	LUA_DASH,		/* saw one '-' */
	LUA_DASH2,		/* saw '--' */
	LUA_DASH_LB,		/* saw '--[' */
	LUA_LINE,		/* -- line comment */
	LUA_BLOCK,		/* --[[ block comment */
	LUA_BLOCK_RB,		/* saw ']' inside a block comment */
	LUA_LB,			/* saw '[' in code */
	LUA_LSTR,		/* [[ long string */
	LUA_LSTR_RB,		/* saw ']' inside a long string */
	LUA_OP,
};

static const struct syn_kw lua_keywords[] = {
	{ "and", SYN_KEYWORD }, { "break", SYN_KEYWORD },
	{ "do", SYN_KEYWORD }, { "else", SYN_KEYWORD },
	{ "elseif", SYN_KEYWORD }, { "end", SYN_KEYWORD },
	{ "for", SYN_KEYWORD }, { "function", SYN_KEYWORD },
	{ "goto", SYN_KEYWORD }, { "if", SYN_KEYWORD },
	{ "in", SYN_KEYWORD }, { "local", SYN_KEYWORD },
	{ "not", SYN_KEYWORD }, { "or", SYN_KEYWORD },
	{ "repeat", SYN_KEYWORD }, { "return", SYN_KEYWORD },
	{ "then", SYN_KEYWORD }, { "until", SYN_KEYWORD },
	{ "while", SYN_KEYWORD },
	{ "nil", SYN_CONSTANT }, { "true", SYN_CONSTANT },
	{ "false", SYN_CONSTANT },
	{ "assert", SYN_FUNCTION }, { "collectgarbage", SYN_FUNCTION },
	{ "error", SYN_FUNCTION }, { "getmetatable", SYN_FUNCTION },
	{ "ipairs", SYN_FUNCTION }, { "next", SYN_FUNCTION },
	{ "pairs", SYN_FUNCTION }, { "pcall", SYN_FUNCTION },
	{ "print", SYN_FUNCTION }, { "rawget", SYN_FUNCTION },
	{ "rawset", SYN_FUNCTION }, { "require", SYN_FUNCTION },
	{ "select", SYN_FUNCTION }, { "setmetatable", SYN_FUNCTION },
	{ "tonumber", SYN_FUNCTION }, { "tostring", SYN_FUNCTION },
	{ "type", SYN_FUNCTION }, { "xpcall", SYN_FUNCTION },
};

static const struct syn_rule lua_idle_rules[] = {
	{ "a-zA-Z_", LUA_IDENT, 0, 1, 0 },
	{ "0-9", LUA_NUM, 1, 0, 0 },
	{ "\"", LUA_DQ, 1, 0, 0 },
	{ "'", LUA_SQ, 1, 0, 0 },
	{ "-", LUA_DASH, 0, 0, 0 },
	{ "[", LUA_LB, 0, 0, 0 },
	{ "+*/%^#<>=~", LUA_OP, 1, 0, 0 },
};

static const struct syn_rule lua_ident_rules[] = {
	{ "a-zA-Z0-9_", LUA_IDENT, 0, 0, 0 },
};

static const struct syn_rule lua_num_rules[] = {
	{ "0-9a-fA-FxX.", LUA_NUM, 0, 0, 0 },
};

static const struct syn_rule lua_dq_rules[] = {
	{ "\\", LUA_DQ_ESC, 0, 0, 0 },
	{ "\"", LUA_IDLE, 0, 0, 0 },
	{ "\n", LUA_IDLE, 0, 0, 0 },
};

static const struct syn_rule lua_sq_rules[] = {
	{ "\\", LUA_SQ_ESC, 0, 0, 0 },
	{ "'", LUA_IDLE, 0, 0, 0 },
	{ "\n", LUA_IDLE, 0, 0, 0 },
};

static const struct syn_rule lua_dash_rules[] = {
	{ "-", LUA_DASH2, 2, 0, 0 },
};

static const struct syn_rule lua_dash2_rules[] = {
	{ "[", LUA_DASH_LB, 0, 0, 0 },
};

static const struct syn_rule lua_dash_lb_rules[] = {
	{ "[", LUA_BLOCK, 0, 0, 0 },
};

static const struct syn_rule lua_line_rules[] = {
	{ "\n", LUA_IDLE, 0, 0, 0 },
};

static const struct syn_rule lua_block_rules[] = {
	{ "]", LUA_BLOCK_RB, 0, 0, 0 },
};

static const struct syn_rule lua_block_rb_rules[] = {
	{ "]", LUA_IDLE, 0, 0, 0 },
};

static const struct syn_rule lua_lb_rules[] = {
	{ "[", LUA_LSTR, 2, 0, 0 },
};

static const struct syn_rule lua_lstr_rules[] = {
	{ "]", LUA_LSTR_RB, 0, 0, 0 },
};

static const struct syn_rule lua_lstr_rb_rules[] = {
	{ "]", LUA_IDLE, 0, 0, 0 },
};

#define NR(a) (uint16_t)(sizeof(a) / sizeof((a)[0]))

static const struct syn_state lua_states[] = {
	[LUA_IDLE] = { SYN_TEXT, LUA_IDLE, 0, 0, lua_idle_rules,
	    NR(lua_idle_rules), NULL, 0 },
	[LUA_IDENT] = { SYN_TEXT, LUA_IDLE, 1, 0, lua_ident_rules,
	    NR(lua_ident_rules), lua_keywords, NR(lua_keywords) },
	[LUA_NUM] = { SYN_CONSTANT, LUA_IDLE, 1, 0, lua_num_rules,
	    NR(lua_num_rules), NULL, 0 },
	[LUA_DQ] = { SYN_STRING, LUA_DQ, 0, 0, lua_dq_rules,
	    NR(lua_dq_rules), NULL, 0 },
	[LUA_DQ_ESC] = { SYN_STRING, LUA_DQ, 0, 0, NULL, 0, NULL, 0 },
	[LUA_SQ] = { SYN_STRING, LUA_SQ, 0, 0, lua_sq_rules,
	    NR(lua_sq_rules), NULL, 0 },
	[LUA_SQ_ESC] = { SYN_STRING, LUA_SQ, 0, 0, NULL, 0, NULL, 0 },
	[LUA_DASH] = { SYN_TEXT, LUA_IDLE, 1, 0, lua_dash_rules,
	    NR(lua_dash_rules), NULL, 0 },
	[LUA_DASH2] = { SYN_COMMENT, LUA_LINE, 1, 0, lua_dash2_rules,
	    NR(lua_dash2_rules), NULL, 0 },
	[LUA_DASH_LB] = { SYN_COMMENT, LUA_LINE, 1, 0, lua_dash_lb_rules,
	    NR(lua_dash_lb_rules), NULL, 0 },
	[LUA_LINE] = { SYN_COMMENT, LUA_LINE, 0, 0, lua_line_rules,
	    NR(lua_line_rules), NULL, 0 },
	[LUA_BLOCK] = { SYN_COMMENT, LUA_BLOCK, 0, 0, lua_block_rules,
	    NR(lua_block_rules), NULL, 0 },
	[LUA_BLOCK_RB] = { SYN_COMMENT, LUA_BLOCK, 1, 0, lua_block_rb_rules,
	    NR(lua_block_rb_rules), NULL, 0 },
	[LUA_LB] = { SYN_TEXT, LUA_IDLE, 1, 0, lua_lb_rules,
	    NR(lua_lb_rules), NULL, 0 },
	[LUA_LSTR] = { SYN_STRING, LUA_LSTR, 0, 0, lua_lstr_rules,
	    NR(lua_lstr_rules), NULL, 0 },
	[LUA_LSTR_RB] = { SYN_STRING, LUA_LSTR, 1, 0, lua_lstr_rb_rules,
	    NR(lua_lstr_rb_rules), NULL, 0 },
	[LUA_OP] = { SYN_OPERATOR, LUA_IDLE, 1, 0, NULL, 0, NULL, 0 },
};

static const char *const lua_exts[] = {
	"lua", NULL,
};

const struct syntax syntax_lua = {
	"lua", LUA_IDLE, 0, lua_states, NR(lua_states), lua_exts,
};
