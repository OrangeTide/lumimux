/* test_syntax.c : tests for libsyntax */

#include "syntax.h"

#include <stdio.h>
#include <string.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() printf("ok\n")

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* Return non-zero when every byte of line in [a,b) has style st. */
static int
span_is(const uint8_t *sty, size_t a, size_t b, enum syn_style st)
{
	size_t i;

	for (i = a; i < b; i++)
		if (sty[i] != st)
			return 0;
	return 1;
}

static void
test_c_keyword_and_ident(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *line = "int x = 3;";
	uint8_t sty[64];

	TEST("c: keyword, ident, number");
	ASSERT(sy != NULL, "no c syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 3, SYN_TYPE), "int not a type");
	ASSERT(sty[4] == SYN_TEXT, "ident should be plain text");
	ASSERT(sty[8] == SYN_CONSTANT, "3 not a constant");
	PASS();
}

static void
test_c_line_comment(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *line = "a; // note";
	uint8_t sty[64];

	TEST("c: line comment to end");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 3, strlen(line), SYN_COMMENT), "// not comment");
	ASSERT(sty[0] == SYN_TEXT, "code before // colored");
	PASS();
}

static void
test_c_block_comment_multiline(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *l1 = "x /* open";
	const char *l2 = "still in */ y";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("c: block comment spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 2, strlen(l1), SYN_COMMENT), "open not comment");
	ASSERT(st != sy->start, "state should carry the open comment");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 11, SYN_COMMENT), "continuation not comment");
	ASSERT(s2[12] == SYN_TEXT, "code after close colored");
	PASS();
}

static void
test_c_string(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *line = "s = \"hi\";";
	uint8_t sty[64];

	TEST("c: string literal");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 4, 8, SYN_STRING), "string not colored");
	PASS();
}

static void
test_c_preproc(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *line = "#include <x.h>";
	uint8_t sty[64];

	TEST("c: preprocessor line");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, strlen(line), SYN_PREPROC), "not preproc");
	PASS();
}

static void
test_c_operators(void)
{
	const struct syntax *sy = syn_for_ext("c");
	const char *line = "a = b + c * d - e;";
	uint8_t sty[64];

	TEST("c: + * - all classify as operators");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[6] == SYN_OPERATOR, "+ not an operator");
	ASSERT(sty[10] == SYN_OPERATOR, "* not an operator");
	ASSERT(sty[14] == SYN_OPERATOR, "- not an operator");
	PASS();
}

static void
test_sh_comment_and_var(void)
{
	const struct syntax *sy = syn_for_ext("sh");
	const char *line = "echo $HOME # hi";
	uint8_t sty[64];

	TEST("sh: builtin, variable, comment");
	ASSERT(sy != NULL, "no sh syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 4, SYN_FUNCTION), "echo not a builtin");
	ASSERT(span_is(sty, 5, 10, SYN_PREPROC), "$HOME not a var");
	ASSERT(span_is(sty, 11, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_sh_single_quote_spans(void)
{
	const struct syntax *sy = syn_for_ext("sh");
	const char *l1 = "x='open";
	const char *l2 = "close' y";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("sh: single-quoted string spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 2, strlen(l1), SYN_STRING), "open not string");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 6, SYN_STRING), "continuation not string");
	ASSERT(s2[7] == SYN_TEXT, "after close colored");
	PASS();
}

static void
test_lua_comment_and_string(void)
{
	const struct syntax *sy = syn_for_ext("lua");
	const char *line = "local x = 'hi' -- note";
	uint8_t sty[64];

	TEST("lua: keyword, string, line comment");
	ASSERT(sy != NULL, "no lua syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 5, SYN_KEYWORD), "local not a keyword");
	ASSERT(span_is(sty, 10, 14, SYN_STRING), "'hi' not a string");
	ASSERT(span_is(sty, 15, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_lua_block_comment_multiline(void)
{
	const struct syntax *sy = syn_for_ext("lua");
	const char *l1 = "a --[[ open";
	const char *l2 = "still ]] b";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("lua: --[[ block comment spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 2, strlen(l1), SYN_COMMENT), "open not comment");
	ASSERT(st != sy->start, "state should carry the block comment");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 8, SYN_COMMENT), "continuation not comment");
	ASSERT(s2[9] == SYN_TEXT, "after ]] colored");
	PASS();
}

static void
test_py_keyword_and_string(void)
{
	const struct syntax *sy = syn_for_ext("py");
	const char *line = "def f(): return 'x'";
	uint8_t sty[64];

	TEST("py: keyword and string");
	ASSERT(sy != NULL, "no py syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 3, SYN_KEYWORD), "def not a keyword");
	ASSERT(span_is(sty, 9, 15, SYN_KEYWORD), "return not a keyword");
	ASSERT(span_is(sty, 16, 19, SYN_STRING), "'x' not a string");
	PASS();
}

static void
test_py_triple_string_multiline(void)
{
	const struct syntax *sy = syn_for_ext("py");
	const char *l1 = "s = \"\"\"start";
	const char *l2 = "middle";
	const char *l3 = "end\"\"\" x";
	uint8_t s1[64], s2[64], s3[64];
	uint16_t st;

	TEST("py: triple-quoted string spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 4, strlen(l1), SYN_STRING), "open not string");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, strlen(l2), SYN_STRING), "middle not string");
	st = syn_line(sy, st, l3, strlen(l3), s3);
	ASSERT(span_is(s3, 0, 6, SYN_STRING), "close not string");
	ASSERT(s3[7] == SYN_TEXT, "after close colored");
	PASS();
}

static void
test_rust_keyword_string_comment(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "fn main() { let s = \"hi\"; } // c";
	uint8_t sty[64];

	TEST("rust: fn keyword, string, line comment");
	ASSERT(sy != NULL, "no rust syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 2, SYN_KEYWORD), "fn not a keyword");
	ASSERT(span_is(sty, 12, 15, SYN_KEYWORD), "let not a keyword");
	ASSERT(span_is(sty, 20, 24, SYN_STRING), "\"hi\" not a string");
	ASSERT(span_is(sty, 28, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_rust_char_literal(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "let c = 'x';";
	uint8_t sty[64];

	TEST("rust: char literal is a constant");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 8, 11, SYN_CONSTANT), "'x' not a constant");
	PASS();
}

static void
test_rust_char_escape(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "let n = '\\n';";
	uint8_t sty[64];

	TEST("rust: escaped char literal is a constant");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 8, 12, SYN_CONSTANT), "'\\n' not a constant");
	PASS();
}

static void
test_rust_lifetime(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "&'a T";
	uint8_t sty[64];

	TEST("rust: lifetime stays plain text");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[1] == SYN_TEXT, "lifetime quote colored");
	ASSERT(sty[2] == SYN_TEXT, "lifetime name colored");
	PASS();
}

static void
test_rust_raw_string(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "let s = r\"a\\b\";";
	uint8_t sty[64];

	TEST("rust: raw string, backslash is literal");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 8, 14, SYN_STRING), "r\"a\\b\" not a string");
	PASS();
}

static void
test_rust_raw_hash_string(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *line = "let s = r#\"a\"b\"#;";
	uint8_t sty[64];

	TEST("rust: r#\"...\"# keeps an inner quote");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 8, 16, SYN_STRING), "raw-hash string not colored");
	PASS();
}

static void
test_rust_nested_block_comment(void)
{
	const struct syntax *sy = syn_for_ext("rs");
	const char *l1 = "a /* x /* y */ z";
	const char *l2 = "still */ end";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("rust: block comments nest one level");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 2, strlen(l1), SYN_COMMENT), "inner close ended it");
	ASSERT(st != sy->start, "state should carry the outer comment");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 8, SYN_COMMENT), "continuation not a comment");
	ASSERT(s2[9] == SYN_TEXT, "code after close colored");
	PASS();
}

static void
test_go_raw_string_spans(void)
{
	const struct syntax *sy = syn_for_ext("go");
	const char *l1 = "s := `open";
	const char *l2 = "close` // c";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("go: backtick raw string spans lines");
	ASSERT(sy != NULL, "no go syntax");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 5, strlen(l1), SYN_STRING), "open not a string");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 6, SYN_STRING), "continuation not a string");
	ASSERT(span_is(s2, 7, strlen(l2), SYN_COMMENT), "no comment after");
	PASS();
}

static void
test_go_keyword_and_type(void)
{
	const struct syntax *sy = syn_for_ext("go");
	const char *line = "func f() int { return 0 }";
	uint8_t sty[64];

	TEST("go: func keyword and int type");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 4, SYN_KEYWORD), "func not a keyword");
	ASSERT(span_is(sty, 9, 12, SYN_TYPE), "int not a type");
	ASSERT(span_is(sty, 15, 21, SYN_KEYWORD), "return not a keyword");
	PASS();
}

static void
test_fth_string_word(void)
{
	const struct syntax *sy = syn_for_ext("fth");
	const char *line = ".\" hello\"";
	uint8_t sty[64];

	TEST("forth: .\" string word body");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, strlen(line), SYN_STRING), ".\" not a string");
	PASS();
}

static void
test_fth_s_quote_word(void)
{
	const struct syntax *sy = syn_for_ext("fth");
	const char *line = "s\" text\"";
	uint8_t sty[64];

	TEST("forth: s\" string word body");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, strlen(line), SYN_STRING), "s\" not a string");
	PASS();
}

static void
test_bas_keyword_string_comment(void)
{
	const struct syntax *sy = syn_for_ext("bas");
	const char *line = "PRINT \"hi\" ' note";
	uint8_t sty[64];

	TEST("basic: keyword, string, apostrophe comment");
	ASSERT(sy != NULL, "no basic syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 5, SYN_KEYWORD), "PRINT not a keyword");
	ASSERT(span_is(sty, 6, 10, SYN_STRING), "\"hi\" not a string");
	ASSERT(span_is(sty, 11, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_bas_number_and_nocase(void)
{
	const struct syntax *sy = syn_for_ext("bas");
	const char *line = "let n = 3.5";
	uint8_t sty[64];

	TEST("basic: lowercase keyword and number");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 3, SYN_KEYWORD), "let not a keyword");
	ASSERT(span_is(sty, 8, 11, SYN_CONSTANT), "3.5 not a constant");
	PASS();
}

static void
test_bas_rem_comment(void)
{
	const struct syntax *sy = syn_for_ext("bas");
	const char *line = "X = 1 : REM note here";
	uint8_t sty[64];

	TEST("basic: REM runs to end of line");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[0] == SYN_TEXT, "variable X not plain");
	ASSERT(span_is(sty, 8, strlen(line), SYN_COMMENT), "REM not a comment");
	PASS();
}

static void
test_bas_remark_not_comment(void)
{
	const struct syntax *sy = syn_for_ext("bas");
	const char *line = "REMARK = 5";
	uint8_t sty[64];

	TEST("basic: REMARK is an identifier, not REM");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 6, SYN_TEXT), "REMARK colored as comment");
	ASSERT(sty[9] == SYN_CONSTANT, "5 not a constant (line not comment)");
	PASS();
}

static void
test_bas_sigil_function(void)
{
	const struct syntax *sy = syn_for_ext("bas");
	const char *line = "x = LEFT$(a, 1)";
	uint8_t sty[64];

	TEST("basic: LEFT$ colors fully including the sigil");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 4, 9, SYN_FUNCTION), "LEFT$ not a function");
	PASS();
}

static void
test_fth_colon_def_and_comment(void)
{
	const struct syntax *sy = syn_for_ext("fth");
	const char *line = ": sq dup * ; \\ square";
	uint8_t sty[64];

	TEST("forth: colon def, stack word, backslash comment");
	ASSERT(sy != NULL, "no forth syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 1, SYN_KEYWORD), ": not a keyword");
	ASSERT(sty[2] == SYN_TEXT, "user word sq should be plain text");
	ASSERT(span_is(sty, 5, 8, SYN_FUNCTION), "dup not a function");
	ASSERT(span_is(sty, 11, 12, SYN_KEYWORD), "; not a keyword");
	ASSERT(span_is(sty, 13, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_fth_paren_and_number(void)
{
	const struct syntax *sy = syn_for_ext("fth");
	const char *line = "42 ( the answer )";
	uint8_t sty[64];

	TEST("forth: number and paren comment");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 2, SYN_CONSTANT), "42 not a constant");
	ASSERT(span_is(sty, 3, strlen(line), SYN_COMMENT), "no paren comment");
	PASS();
}

static void
test_js_keyword_string_template(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *line = "const s = `hi`; // c";
	uint8_t sty[64];

	TEST("js: keyword, template literal, line comment");
	ASSERT(sy != NULL, "no js syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 5, SYN_KEYWORD), "const not a keyword");
	ASSERT(span_is(sty, 10, 14, SYN_STRING), "`hi` not a string");
	ASSERT(span_is(sty, 16, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_js_template_spans(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *l1 = "let s = `open";
	const char *l2 = "close` + x";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("js: template literal spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 8, strlen(l1), SYN_STRING), "open not a string");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 6, SYN_STRING), "continuation not a string");
	ASSERT(s2[7] == SYN_OPERATOR, "code after close not colored");
	PASS();
}

static void
test_js_template_interp(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *line = "let s = `a=${x + 1}!`;";
	uint8_t sty[64];

	TEST("js: ${ } interpolation is highlighted");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[9] == SYN_STRING, "template literal text not a string");
	ASSERT(sty[13] == SYN_TEXT, "interp identifier not code");
	ASSERT(sty[15] == SYN_OPERATOR, "interp operator not colored");
	ASSERT(sty[17] == SYN_CONSTANT, "interp number not a constant");
	ASSERT(sty[18] == SYN_STRING, "closing } not back to string");
	ASSERT(sty[19] == SYN_STRING, "text after interp not a string");
	PASS();
}

static void
test_js_interp_brace_in_string(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *line = "`${a[\"}\"]}`";
	uint8_t sty[64];

	TEST("js: } inside interp string does not close early");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[3] == SYN_TEXT, "interp identifier not code");
	ASSERT(sty[6] == SYN_STRING, "} inside string not a string");
	ASSERT(sty[8] == SYN_OPERATOR, "interp still active after string");
	ASSERT(sty[9] == SYN_STRING, "real closing } not back to string");
	PASS();
}

static void
test_js_regex_literal(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *line = "x = /ab+/g;";
	uint8_t sty[64];

	TEST("js: /regex/ literal after operator");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 4, 10, SYN_STRING), "regex not colored");
	ASSERT(sty[10] == SYN_OPERATOR, "trailing ; not an operator");
	PASS();
}

static void
test_js_division_not_regex(void)
{
	const struct syntax *sy = syn_for_ext("js");
	const char *line = "a = b / c;";
	uint8_t sty[64];

	TEST("js: slash after operand is division");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[6] != SYN_STRING, "division slash colored as regex");
	ASSERT(sty[8] == SYN_TEXT, "c should be plain text");
	PASS();
}

static void
test_html_tag_attr_value(void)
{
	const struct syntax *sy = syn_for_ext("html");
	const char *line = "<a href=\"x\">hi</a>";
	uint8_t sty[64];

	TEST("html: tag name, attribute, quoted value");
	ASSERT(sy != NULL, "no html syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(sty[1] == SYN_KEYWORD, "element name not a keyword");
	ASSERT(span_is(sty, 3, 7, SYN_TYPE), "href not an attribute");
	ASSERT(span_is(sty, 8, 11, SYN_STRING), "\"x\" not a value string");
	ASSERT(sty[12] == SYN_TEXT, "text content should be plain");
	PASS();
}

static void
test_html_comment_and_entity(void)
{
	const struct syntax *sy = syn_for_ext("html");
	const char *line = "<!-- c --> &amp; x";
	uint8_t sty[64];

	TEST("html: comment and entity");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 10, SYN_COMMENT), "<!-- --> not a comment");
	ASSERT(span_is(sty, 11, 16, SYN_CONSTANT), "&amp; not an entity");
	ASSERT(sty[17] == SYN_TEXT, "trailing text should be plain");
	PASS();
}

static void
test_nasm_insn_reg_comment(void)
{
	const struct syntax *sy = syn_for_ext("asm");
	const char *line = "mov rax, 0x10 ; set";
	uint8_t sty[64];

	TEST("nasm: mnemonic, register, number, comment");
	ASSERT(sy != NULL, "no nasm syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 3, SYN_KEYWORD), "mov not a mnemonic");
	ASSERT(span_is(sty, 4, 7, SYN_TYPE), "rax not a register");
	ASSERT(span_is(sty, 9, 13, SYN_CONSTANT), "0x10 not a constant");
	ASSERT(span_is(sty, 14, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_nasm_preproc_directive(void)
{
	const struct syntax *sy = syn_for_ext("nasm");
	const char *line = "%define FOO 1";
	uint8_t sty[64];

	TEST("nasm: %preprocessor word");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 7, SYN_PREPROC), "%define not preproc");
	ASSERT(sty[12] == SYN_CONSTANT, "1 not a constant");
	PASS();
}

static void
test_gas_sigils_and_comment(void)
{
	const struct syntax *sy = syn_for_ext("s");
	const char *line = "movq %rax, $0x10 # x";
	uint8_t sty[64];

	TEST("gas: mnemonic, %reg, $imm, comment");
	ASSERT(sy != NULL, "no gas syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 4, SYN_KEYWORD), "movq not a mnemonic");
	ASSERT(span_is(sty, 5, 9, SYN_TYPE), "%rax not a register");
	ASSERT(span_is(sty, 11, 16, SYN_CONSTANT), "$0x10 not an immediate");
	ASSERT(span_is(sty, 17, strlen(line), SYN_COMMENT), "no comment");
	PASS();
}

static void
test_nasm_label(void)
{
	const struct syntax *sy = syn_for_ext("asm");
	const char *line = "myloop: dec rcx";
	uint8_t sty[64];

	TEST("nasm: name: label recolored");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 7, SYN_FUNCTION), "label not colored");
	ASSERT(span_is(sty, 8, 11, SYN_KEYWORD), "dec not a mnemonic");
	ASSERT(span_is(sty, 12, 15, SYN_TYPE), "rcx not a register");
	PASS();
}

static void
test_gas_directive(void)
{
	const struct syntax *sy = syn_for_ext("s");
	const char *line = ".section .text";
	uint8_t sty[64];

	TEST("gas: .directive as preproc");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 8, SYN_PREPROC), ".section not preproc");
	ASSERT(span_is(sty, 9, strlen(line), SYN_PREPROC), ".text not preproc");
	PASS();
}

static void
test_gas_label(void)
{
	const struct syntax *sy = syn_for_ext("s");
	const char *line = "main: ret";
	uint8_t sty[64];

	TEST("gas: name: label recolored");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 5, SYN_FUNCTION), "label not colored");
	ASSERT(span_is(sty, 6, 9, SYN_KEYWORD), "ret not a mnemonic");
	PASS();
}

static void
test_gas_local_label(void)
{
	const struct syntax *sy = syn_for_ext("s");
	const char *line = ".L1: nop";
	uint8_t sty[64];

	TEST("gas: .L1: local label recolored");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 4, SYN_FUNCTION), "local label not colored");
	ASSERT(span_is(sty, 5, 8, SYN_KEYWORD), "nop not a mnemonic");
	PASS();
}

static void
test_pas_keyword_string_brace(void)
{
	const struct syntax *sy = syn_for_ext("pas");
	const char *line = "if x then writeln('hi'); { done }";
	uint8_t sty[64];

	TEST("pascal: keyword, function, string, brace comment");
	ASSERT(sy != NULL, "no pascal syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 2, SYN_KEYWORD), "if not a keyword");
	ASSERT(span_is(sty, 5, 9, SYN_KEYWORD), "then not a keyword");
	ASSERT(span_is(sty, 10, 17, SYN_FUNCTION), "writeln not a function");
	ASSERT(span_is(sty, 18, 22, SYN_STRING), "'hi' not a string");
	ASSERT(span_is(sty, 25, strlen(line), SYN_COMMENT), "no brace comment");
	PASS();
}

static void
test_pas_type_hex_and_linecomment(void)
{
	const struct syntax *sy = syn_for_ext("pp");
	const char *line = "var n: Integer = $FF; // x";
	uint8_t sty[64];

	TEST("pascal: type, hex number, line comment");
	ASSERT(sy != NULL, "no pascal syntax");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, 3, SYN_KEYWORD), "var not a keyword");
	ASSERT(span_is(sty, 7, 14, SYN_TYPE), "Integer not a type");
	ASSERT(span_is(sty, 17, 20, SYN_CONSTANT), "$FF not a constant");
	ASSERT(span_is(sty, 22, strlen(line), SYN_COMMENT), "no // comment");
	PASS();
}

static void
test_pas_directive(void)
{
	const struct syntax *sy = syn_for_ext("pas");
	const char *line = "{$mode objfpc}";
	uint8_t sty[64];

	TEST("pascal: {$...} directive is preproc");
	syn_line(sy, sy->start, line, strlen(line), sty);
	ASSERT(span_is(sty, 0, strlen(line), SYN_PREPROC), "not a directive");
	PASS();
}

static void
test_pas_pstar_spans(void)
{
	const struct syntax *sy = syn_for_ext("pas");
	const char *l1 = "a (* open";
	const char *l2 = "still *) b";
	uint8_t s1[64], s2[64];
	uint16_t st;

	TEST("pascal: (* *) comment spans lines");
	st = syn_line(sy, sy->start, l1, strlen(l1), s1);
	ASSERT(span_is(s1, 2, strlen(l1), SYN_COMMENT), "open not a comment");
	ASSERT(st != sy->start, "state should carry the open comment");
	st = syn_line(sy, st, l2, strlen(l2), s2);
	ASSERT(span_is(s2, 0, 8, SYN_COMMENT), "continuation not a comment");
	ASSERT(s2[9] == SYN_TEXT, "code after close colored");
	PASS();
}

static void
test_unknown_ext(void)
{
	TEST("unknown extension has no syntax");
	ASSERT(syn_for_ext("zzz") == NULL, "zzz should not match");
	ASSERT(syn_for_ext(NULL) == NULL, "NULL should not match");
	PASS();
}

int
main(void)
{
	test_c_keyword_and_ident();
	test_c_line_comment();
	test_c_block_comment_multiline();
	test_c_string();
	test_c_preproc();
	test_c_operators();
	test_rust_char_literal();
	test_rust_char_escape();
	test_rust_lifetime();
	test_rust_raw_string();
	test_rust_raw_hash_string();
	test_rust_nested_block_comment();
	test_fth_string_word();
	test_fth_s_quote_word();
	test_bas_rem_comment();
	test_bas_remark_not_comment();
	test_bas_sigil_function();
	test_sh_comment_and_var();
	test_sh_single_quote_spans();
	test_lua_comment_and_string();
	test_lua_block_comment_multiline();
	test_py_keyword_and_string();
	test_py_triple_string_multiline();
	test_rust_keyword_string_comment();
	test_go_raw_string_spans();
	test_go_keyword_and_type();
	test_bas_keyword_string_comment();
	test_bas_number_and_nocase();
	test_fth_colon_def_and_comment();
	test_fth_paren_and_number();
	test_js_keyword_string_template();
	test_js_template_spans();
	test_js_template_interp();
	test_js_interp_brace_in_string();
	test_js_regex_literal();
	test_js_division_not_regex();
	test_html_tag_attr_value();
	test_html_comment_and_entity();
	test_nasm_insn_reg_comment();
	test_nasm_preproc_directive();
	test_gas_sigils_and_comment();
	test_nasm_label();
	test_gas_directive();
	test_gas_label();
	test_gas_local_label();
	test_pas_keyword_string_brace();
	test_pas_type_hex_and_linecomment();
	test_pas_directive();
	test_pas_pstar_spans();
	test_unknown_ext();

	printf("test_syntax: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
