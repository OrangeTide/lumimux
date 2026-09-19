/* basic.h : a small BASIC-style expression and program interpreter */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef BASIC_H
#define BASIC_H

/*
 * The evaluator core: a Pratt-parsed expression language over numeric
 * values, with variables and builtin functions. Later milestones layer
 * line-numbered programs, vector and matrix values, and graphing on top.
 *
 * Values are numbers or errors. An error carries a short message and
 * propagates out of evaluation.
 */

enum basic_type {
	BV_NUM,
	BV_VEC,		/* a vector of cols elements (rows is 1) */
	BV_MAT,		/* a rows x cols matrix */
	BV_ERR,
};

struct basic_value {
	enum basic_type	type;
	double		num;		/* BV_NUM */
	double		*data;		/* BV_VEC/BV_MAT, owned, rows*cols */
	int		rows;
	int		cols;
	char		err[120];	/* message when type is BV_ERR */
};

/** Release any storage a value owns and reset it to an empty number.
 *  Numbers and errors own nothing; vectors and matrices own their data. */
void basic_value_free(struct basic_value *v);

struct basic;

/** Create an interpreter with an empty variable environment. Returns NULL
 *  on allocation failure. */
struct basic *basic_new(void);

/** Free an interpreter. NULL is accepted. */
void basic_free(struct basic *b);

/** Set a variable. Names are case-insensitive. */
void basic_set_var(struct basic *b, const char *name, double v);

/** Read a variable into out. Returns 1 if the variable was set, else 0 and
 *  leaves out untouched. */
int basic_get_var(struct basic *b, const char *name, double *out);

/** Evaluate an expression string and return its value or an error. */
struct basic_value basic_eval(struct basic *b, const char *expr);

/*
 * Statement execution. A line beginning with a number edits that file line
 * of the stored program (1-based; a bare number deletes the line, a number
 * past the end appends); RUN, LIST, and NEW manage the program. Otherwise the
 * line runs immediately: PRINT, an assignment (with or without a leading
 * LET), REM, a QUIT/EXIT/BYE request, or a bare expression whose value is
 * printed (calculator mode). A running program adds GOTO, GOSUB/RETURN,
 * FOR/NEXT, IF ... THEN, INPUT, and END/STOP. Jump targets are labels
 * (`name:` at the start of a line), not line numbers.
 */

enum basic_status {
	BAS_OK,
	BAS_ERROR,
	BAS_QUIT,
};

struct basic_result {
	enum basic_status	status;
	char			msg[120];	/* message when status is error */
};

#include <stdio.h>

/** Execute one statement line. INPUT reads from in (may be NULL); output
 *  goes to out. */
struct basic_result basic_exec(struct basic *b, const char *line,
    FILE *in, FILE *out);

/** Append text to the stored program as the next line, verbatim except for a
 *  trailing newline, preserving blank lines. The REPL frontend uses this to
 *  load a program from a file, keeping file I/O out of the interpreter. */
void basic_program_append(struct basic *b, const char *text);

/** Write the program to out as raw source lines, without the line-number
 *  gutter LIST adds, so it round-trips through a file or the editor. */
void basic_program_dump(struct basic *b, FILE *out);

#endif /* BASIC_H */
