/* test_basic.c : tests for libbasic */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "basic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			basic_free(b); \
			return; \
		} \
	} while (0)

/* Evaluate expr and assert it is a number close to want. */
static int
num_is(struct basic *b, const char *expr, double want)
{
	struct basic_value v = basic_eval(b, expr);
	int ok = v.type == BV_NUM && fabs(v.num - want) < 1e-9;

	basic_value_free(&v);
	return ok;
}

static int
is_err(struct basic *b, const char *expr)
{
	struct basic_value v = basic_eval(b, expr);
	int err = v.type == BV_ERR;

	basic_value_free(&v);
	return err;
}

/* Evaluate expr and, printing it via the calculator, compare the text. */
static int
prints_as(struct basic *b, const char *expr, const char *want)
{
	FILE *f = tmpfile();
	char out[128];
	size_t n;

	basic_exec(b, expr, NULL, f);
	rewind(f);
	n = fread(out, 1, sizeof(out) - 1, f);
	out[n] = '\0';
	fclose(f);
	return strcmp(out, want) == 0;
}

static void
test_arithmetic(void)
{
	struct basic *b = basic_new();

	TEST("arithmetic and precedence");
	ASSERT(num_is(b, "1+2*3", 7), "1+2*3");
	ASSERT(num_is(b, "(1+2)*3", 9), "(1+2)*3");
	ASSERT(num_is(b, "10/4", 2.5), "10/4");
	ASSERT(num_is(b, "2*3+4*5", 26), "2*3+4*5");
	basic_free(b);
	PASS();
}

static void
test_power_and_unary(void)
{
	struct basic *b = basic_new();

	TEST("power associativity and unary minus");
	ASSERT(num_is(b, "2^10", 1024), "2^10");
	ASSERT(num_is(b, "2^3^2", 512), "2^3^2 right assoc");
	ASSERT(num_is(b, "-3^2", -9), "-3^2 is -(3^2)");
	ASSERT(num_is(b, "-3*2", -6), "-3*2");
	ASSERT(num_is(b, "2*-3", -6), "2*-3");
	basic_free(b);
	PASS();
}

static void
test_variables(void)
{
	struct basic *b = basic_new();
	double got = -1;

	TEST("variables set, read, and default to zero");
	basic_set_var(b, "x", 5);
	ASSERT(num_is(b, "x*2", 10), "x*2");
	ASSERT(num_is(b, "X+1", 6), "case-insensitive");
	ASSERT(num_is(b, "unset+7", 7), "unset reads as 0");
	basic_set_var(b, "rate", 3);
	ASSERT(basic_get_var(b, "RATE", &got) && got == 3, "get_var");
	basic_free(b);
	PASS();
}

static void
test_builtins(void)
{
	struct basic *b = basic_new();

	TEST("builtin functions");
	ASSERT(num_is(b, "SQR(16)", 4), "SQR");
	ASSERT(num_is(b, "ABS(-7)", 7), "ABS");
	ASSERT(num_is(b, "INT(3.9)", 3), "INT");
	ASSERT(num_is(b, "SGN(-2)", -1), "SGN");
	ASSERT(num_is(b, "SIN(0)", 0), "SIN");
	basic_free(b);
	PASS();
}

static void
test_comparisons(void)
{
	struct basic *b = basic_new();

	TEST("comparisons yield 1 or 0");
	ASSERT(num_is(b, "3<5", 1), "3<5");
	ASSERT(num_is(b, "3>5", 0), "3>5");
	ASSERT(num_is(b, "4=4", 1), "4=4");
	ASSERT(num_is(b, "4<>4", 0), "4<>4");
	ASSERT(num_is(b, "2<=2", 1), "2<=2");
	basic_free(b);
	PASS();
}

static void
test_errors(void)
{
	struct basic *b = basic_new();

	TEST("malformed input and errors");
	ASSERT(is_err(b, "1/0"), "division by zero");
	ASSERT(is_err(b, "1+"), "trailing operator");
	ASSERT(is_err(b, "(1+2"), "missing paren");
	ASSERT(is_err(b, "NOPE(1)"), "unknown function");
	ASSERT(is_err(b, "SQR(1,2)"), "wrong arity");
	ASSERT(is_err(b, ""), "empty");
	ASSERT(is_err(b, "1 2"), "trailing tokens");
	basic_free(b);
	PASS();
}

/* Feed program lines (NUL-terminated array) plus RUN, capturing output. */
static void
run_lines(struct basic *b, const char *const *lines, char *out, size_t sz)
{
	FILE *f = tmpfile();
	size_t n;
	int i;

	for (i = 0; lines[i]; i++)
		basic_exec(b, lines[i], NULL, f);
	basic_exec(b, "RUN", NULL, f);
	rewind(f);
	n = fread(out, 1, sz - 1, f);
	out[n] = '\0';
	fclose(f);
}

static void
test_program_for(void)
{
	struct basic *b = basic_new();
	const char *const prog[] = {
		"1 S = 0",
		"2 FOR I = 1 TO 5",
		"3 S = S + I",
		"4 NEXT I",
		"5 PRINT S",
		NULL,
	};
	char out[128];

	TEST("program: FOR/NEXT accumulates");
	run_lines(b, prog, out, sizeof(out));
	ASSERT(strcmp(out, "15\n") == 0, "sum should be 15");
	basic_free(b);
	PASS();
}

static void
test_program_goto_if(void)
{
	struct basic *b = basic_new();
	const char *const prog[] = {
		"1 I = 3",
		"2 top: IF I = 0 THEN done",
		"3 PRINT I",
		"4 I = I - 1",
		"5 GOTO top",
		"6 done: PRINT 99",
		NULL,
	};
	char out[128];

	TEST("program: IF/GOTO countdown with labels");
	run_lines(b, prog, out, sizeof(out));
	ASSERT(strcmp(out, "3\n2\n1\n99\n") == 0, "countdown wrong");
	basic_free(b);
	PASS();
}

static void
test_program_gosub_step(void)
{
	struct basic *b = basic_new();
	const char *const prog[] = {
		"1 FOR I = 10 TO 6 STEP -2",
		"2 GOSUB sub",
		"3 NEXT I",
		"4 END",
		"5 sub: PRINT I",
		"6 GOSUB deep",		/* a GOSUB inside a GOSUB */
		"7 RETURN",
		"8 deep: PRINT I * 2",
		"9 RETURN",
		NULL,
	};
	char out[128];

	TEST("program: nested GOSUB labels and negative STEP");
	run_lines(b, prog, out, sizeof(out));
	ASSERT(strcmp(out, "10\n20\n8\n16\n6\n12\n") == 0,
	    "nested gosub/step wrong");
	basic_free(b);
	PASS();
}

static void
test_program_edit_and_new(void)
{
	struct basic *b = basic_new();
	char out[128];
	const char *const prog[] = { "1 PRINT 1", "2 PRINT 2", NULL };

	TEST("program: file-line replace, delete, and NEW");
	run_lines(b, prog, out, sizeof(out));
	ASSERT(strcmp(out, "1\n2\n") == 0, "initial run wrong");

	basic_exec(b, "1 PRINT 7", NULL, stdout);	/* replace file line 1 */
	basic_exec(b, "2", NULL, stdout);		/* blank: delete line 2 */
	{
		FILE *f = tmpfile();
		char o2[128];
		size_t n;

		basic_exec(b, "RUN", NULL, f);
		rewind(f);
		n = fread(o2, 1, sizeof(o2) - 1, f);
		o2[n] = '\0';
		fclose(f);
		ASSERT(strcmp(o2, "7\n") == 0, "replace/delete wrong");
	}
	{
		const char *const none[] = { NULL };
		char out2[128];

		basic_exec(b, "NEW", NULL, stdout);
		run_lines(b, none, out2, sizeof(out2));
		ASSERT(out2[0] == '\0', "NEW should clear the program");
	}
	basic_free(b);
	PASS();
}

static void
test_program_label_checks(void)
{
	struct basic *b = basic_new();
	struct basic_result r;
	FILE *f;
	char out[64];
	size_t n;

	TEST("program: pre-run label checks");

	/* an undefined jump target aborts before any line runs */
	basic_exec(b, "1 PRINT 42", NULL, stdout);
	basic_exec(b, "2 GOTO nowhere", NULL, stdout);
	f = tmpfile();
	r = basic_exec(b, "RUN", NULL, f);
	rewind(f);
	n = fread(out, 1, sizeof(out) - 1, f);
	out[n] = '\0';
	fclose(f);
	ASSERT(r.status == BAS_ERROR && out[0] == '\0',
	    "bad target should abort before running line 1");

	/* a duplicate label is rejected */
	basic_exec(b, "NEW", NULL, stdout);
	basic_exec(b, "1 a: PRINT 1", NULL, stdout);
	basic_exec(b, "2 a: PRINT 2", NULL, stdout);
	basic_exec(b, "3 GOTO a", NULL, stdout);
	r = basic_exec(b, "RUN", NULL, stdout);
	ASSERT(r.status == BAS_ERROR, "duplicate label should error");

	/* a mistyped THEN label is caught, but THEN <statement> is not */
	basic_exec(b, "NEW", NULL, stdout);
	basic_exec(b, "1 IF 1 THEN dnoe", NULL, stdout);
	basic_exec(b, "2 done: PRINT 1", NULL, stdout);
	r = basic_exec(b, "RUN", NULL, stdout);
	ASSERT(r.status == BAS_ERROR, "THEN to undefined label should error");

	/* a valid labelled program passes the check and runs; THEN END is a
	 * statement, not a label, so the check must not flag it */
	basic_exec(b, "NEW", NULL, stdout);
	basic_exec(b, "1 GOSUB s", NULL, stdout);
	basic_exec(b, "2 END", NULL, stdout);
	basic_exec(b, "3 s: PRINT 7", NULL, stdout);
	basic_exec(b, "4 IF 0 THEN END", NULL, stdout);
	basic_exec(b, "5 RETURN", NULL, stdout);
	f = tmpfile();
	r = basic_exec(b, "RUN", NULL, f);
	rewind(f);
	n = fread(out, 1, sizeof(out) - 1, f);
	out[n] = '\0';
	fclose(f);
	ASSERT(r.status == BAS_OK && strcmp(out, "7\n") == 0,
	    "valid program with THEN END should run clean");
	basic_free(b);
	PASS();
}

static void
test_program_dump_append(void)
{
	struct basic *b = basic_new();
	char text[256], out[128];
	FILE *f;
	size_t n;

	TEST("program: dump and append round-trip");
	basic_program_append(b, "start: PRINT 1\n");	/* trailing NL kept out */
	basic_program_append(b, "PRINT 2");
	f = tmpfile();
	basic_program_dump(b, f);
	rewind(f);
	n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = '\0';
	fclose(f);
	ASSERT(strcmp(text, "start: PRINT 1\nPRINT 2\n") == 0,
	    "dump should be raw source lines with no gutter");

	/* reload the dumped text into a fresh program and run it */
	{
		struct basic *b2 = basic_new();
		FILE *g = tmpfile();
		char fl[256];

		fputs(text, g);
		rewind(g);
		while (fgets(fl, sizeof(fl), g))
			basic_program_append(b2, fl);
		fclose(g);

		f = tmpfile();
		basic_exec(b2, "RUN", NULL, f);
		rewind(f);
		n = fread(out, 1, sizeof(out) - 1, f);
		out[n] = '\0';
		fclose(f);
		basic_free(b2);
		ASSERT(strcmp(out, "1\n2\n") == 0, "reloaded program wrong");
	}
	basic_free(b);
	PASS();
}

static void
test_vectors(void)
{
	struct basic *b = basic_new();

	TEST("vector literals, elementwise ops, indexing");
	ASSERT(prints_as(b, "[1,2,3]", "[1, 2, 3]\n"), "literal");
	ASSERT(prints_as(b, "[1,2,3] + [10,20,30]", "[11, 22, 33]\n"),
	    "elementwise add");
	ASSERT(prints_as(b, "[1,2,3] * 2", "[2, 4, 6]\n"), "scalar broadcast");
	basic_exec(b, "W = [1,2,3]", NULL, stdout);
	ASSERT(prints_as(b, "W(2)", "2\n"), "1-based index");
	ASSERT(prints_as(b, "DOT([1,2,3],[4,5,6])", "32\n"), "dot product");
	ASSERT(prints_as(b, "RANGE(1,4)", "[1, 2, 3, 4]\n"), "range");
	ASSERT(prints_as(b, "LEN([5,6,7,8])", "4\n"), "len");
	basic_free(b);
	PASS();
}

static void
test_matrices(void)
{
	struct basic *b = basic_new();

	TEST("matrix literals, indexing, matmul");
	ASSERT(prints_as(b, "[[1,2],[3,4]]", "[[1, 2], [3, 4]]\n"),
	    "matrix literal");
	basic_exec(b, "M = [[1,2],[3,4]]", NULL, stdout);
	ASSERT(prints_as(b, "M(2,1)", "3\n"), "2d index");
	ASSERT(prints_as(b, "[[1,2],[3,4]] + [[10,20],[30,40]]",
	    "[[11, 22], [33, 44]]\n"), "elementwise");
	ASSERT(prints_as(b, "MATMUL([[1,2],[3,4]],[[5,6],[7,8]])",
	    "[[19, 22], [43, 50]]\n"), "matmul");
	ASSERT(prints_as(b, "ZEROS(3)", "[0, 0, 0]\n"), "zeros vector");
	basic_free(b);
	PASS();
}

static void
test_value_errors(void)
{
	struct basic *b = basic_new();

	TEST("shape and index errors");
	ASSERT(is_err(b, "[1,2] + [1,2,3]"), "length mismatch");
	ASSERT(is_err(b, "[1,2,3](9)"), "index out of range");
	ASSERT(is_err(b, "[1,2] < [3,4]"), "compare non-scalars");
	ASSERT(is_err(b, "DOT([1,2],[1,2,3])"), "dot length mismatch");
	ASSERT(is_err(b, "MATMUL([[1,2]],[[1,2]])"), "matmul mismatch");
	basic_free(b);
	PASS();
}

/* Run one statement and report whether the output contains want. */
static int
output_has(struct basic *b, const char *line, const char *want)
{
	FILE *f = tmpfile();
	char out[2048];
	size_t n;

	basic_exec(b, line, NULL, f);
	rewind(f);
	n = fread(out, 1, sizeof(out) - 1, f);
	out[n] = '\0';
	fclose(f);
	return strstr(out, want) != NULL;
}

static void
test_def_fn(void)
{
	struct basic *b = basic_new();

	TEST("user functions with DEF FN");
	basic_exec(b, "DEF FNSQ(X) = X*X", NULL, stdout);
	ASSERT(prints_as(b, "FNSQ(5)", "25\n"), "call");
	basic_exec(b, "DEF FNADD(N) = N + 100", NULL, stdout);
	ASSERT(prints_as(b, "FNADD(FNSQ(3))", "109\n"), "nested call");
	basic_exec(b, "X = 7", NULL, stdout);		/* param restored */
	ASSERT(prints_as(b, "FNSQ(2)", "4\n"), "call after using X");
	ASSERT(prints_as(b, "X", "7\n"), "X unchanged by the call");
	basic_free(b);
	PASS();
}

static void
test_plot(void)
{
	struct basic *b = basic_new();

	TEST("PLOT produces a graph");
	ASSERT(output_has(b, "PLOT X*X FROM -2 TO 2", "*"), "has points");
	ASSERT(output_has(b, "PLOT X*X FROM -2 TO 2", "x: -2 .. 2"),
	    "has axis label");
	ASSERT(output_has(b, "PLOT [1,2,3,2,1]", "*"), "vector plot");
	basic_free(b);
	PASS();
}

static void
test_vector_variables(void)
{
	struct basic *b = basic_new();

	TEST("vectors stored in and read from variables");
	basic_exec(b, "V = [2,4,6]", NULL, stdout);
	ASSERT(prints_as(b, "V(3)", "6\n"), "index a stored vector");
	ASSERT(prints_as(b, "V + V", "[4, 8, 12]\n"), "operate on it");
	basic_free(b);
	PASS();
}

int
main(void)
{
	test_arithmetic();
	test_power_and_unary();
	test_variables();
	test_builtins();
	test_comparisons();
	test_errors();
	test_program_for();
	test_program_goto_if();
	test_program_gosub_step();
	test_program_edit_and_new();
	test_program_label_checks();
	test_program_dump_append();
	test_vectors();
	test_matrices();
	test_value_errors();
	test_vector_variables();
	test_def_fn();
	test_plot();

	printf("test_basic: %d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
