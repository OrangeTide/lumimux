/* basic_cmd.c : lumi basic -- BASIC-style calculator REPL */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "basic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *progname = "lumi-basic";

/* Remove any bracketed-paste markers (ESC [ 200 ~ and ESC [ 201 ~) from a
 * line in place. The REPL reads cooked input and never enables paste mode,
 * so pasting a program already works line by line. This only guards against
 * markers another full-screen app may have left the terminal emitting. */
static void
strip_paste_markers(char *s)
{
	static const char *marks[] = { "\033[200~", "\033[201~" };
	size_t i;

	for (i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
		size_t mlen = strlen(marks[i]);
		char *p;

		while ((p = strstr(s, marks[i])) != NULL)
			memmove(p, p + mlen, strlen(p + mlen) + 1);
	}
}

/* Handle a "SEND <expr>" line: evaluate <expr>, then inject its printed value
 * into another window of the session, so a computed result can be dropped into
 * a shell or REPL running in another pane. Returns 1 if the line was a SEND
 * command (handled here), 0 to let the normal interpreter run it. */
static int
try_send(struct basic *b, const char *line)
{
	const char *p = line, *expr, *session;
	char printline[1100];
	char *cap = NULL, *payload;
	size_t caplen = 0;
	FILE *m;
	struct basic_result r;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncasecmp(p, "send", 4) != 0 || (p[4] != ' ' && p[4] != '\t'))
		return 0;

	expr = p + 5;
	while (*expr == ' ' || *expr == '\t')
		expr++;
	if (*expr == '\0' || *expr == '\n') {
		printf("?SEND needs an expression\n");
		fflush(stdout);
		return 1;
	}
	session = getenv("LUMI_SESSION");
	if (!session) {
		printf("?SEND needs a lumi session\n");
		fflush(stdout);
		return 1;
	}

	/* Evaluate by printing the expression into a memory buffer. */
	snprintf(printline, sizeof(printline), "PRINT %s", expr);
	m = open_memstream(&cap, &caplen);
	if (!m) {
		printf("?out of memory\n");
		fflush(stdout);
		return 1;
	}
	r = basic_exec(b, printline, stdin, m);
	fclose(m);
	if (r.status == BAS_ERROR) {
		printf("?%s\n", r.msg);
		fflush(stdout);
		free(cap);
		return 1;
	}

	/* Drop the trailing newline PRINT adds; append a carriage return so
	 * the value is submitted in the target. */
	while (caplen > 0 && (cap[caplen - 1] == '\n' || cap[caplen - 1] == '\r'))
		caplen--;
	if (caplen == 0) {
		printf("?nothing to send\n");
		fflush(stdout);
		free(cap);
		return 1;
	}
	payload = malloc(caplen + 1);
	if (!payload) {
		printf("?out of memory\n");
		fflush(stdout);
		free(cap);
		return 1;
	}
	memcpy(payload, cap, caplen);
	payload[caplen] = '\r';
	free(cap);

	switch (lu_send_input(session, -1, payload, caplen + 1)) {
	case LU_SEND_OK:
		printf("sent to another pane\n");
		break;
	case LU_SEND_NO_TARGET:
		printf("?no other window to send to\n");
		break;
	case LU_SEND_READONLY:
		printf("?target pane is read-only\n");
		break;
	case LU_SEND_NO_SESSION:
		printf("?session not found\n");
		break;
	case LU_SEND_ERROR:
		printf("?send failed\n");
		break;
	}
	fflush(stdout);
	free(payload);
	return 1;
}

/* Replace the stored program with the source lines read from f, appended in
 * order. The program has no line numbers, so every line is kept verbatim. */
static void
load_program_file(struct basic *b, FILE *f)
{
	char fl[1024];

	basic_exec(b, "NEW", NULL, stdout);	/* replace the program */
	while (fgets(fl, sizeof(fl), f))
		basic_program_append(b, fl);
}

/* Copy the filename that follows a command keyword into out, trimming
 * surrounding whitespace and a single pair of double quotes. Returns the
 * length, or 0 when no name is present or it does not fit. */
static size_t
extract_filename(const char *s, char *out, size_t outsz)
{
	const char *end;
	size_t n;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
	    end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (end - s >= 2 && *s == '"' && end[-1] == '"') {	/* quotes */
		s++;
		end--;
	}
	n = (size_t)(end - s);
	if (n == 0 || n >= outsz)
		return 0;
	memcpy(out, s, n);
	out[n] = '\0';
	return n;
}

/* Run the freshly loaded program, printing its output: the "run" half of
 * EDIT's save-and-run. An empty program is a quiet no-op; a parse or flow
 * error is shown as ?message, like any other statement error. */
static void
edit_autorun(struct basic *b)
{
	struct basic_result r = basic_exec(b, "RUN", stdin, stdout);

	if (r.status == BAS_ERROR) {
		printf("?%s\n", r.msg);
		fflush(stdout);
	}
}

/* Handle an "EDIT [file]" line: open the program full-screen in lumi edit,
 * read it back on return, then run it (save-and-run). With no filename the
 * program goes through a temporary file that is removed afterward. With a
 * filename that file is the persistent backing store: an existing non-empty
 * file is edited as it is, and a missing or empty one is first seeded with the
 * current program, so EDIT <file> both starts new files and reopens saved
 * ones. Editing and running still stay separate on screen; the store is
 * reconciled once, on return. Returns 1 if handled. */
static int
try_edit(struct basic *b, const char *line)
{
	const char *p = line, *tmpdir;
	char fname[512], tmp[512], after;
	char *path;
	char *av[3];
	FILE *f;
	int is_temp = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncasecmp(p, "edit", 4) != 0)
		return 0;
	after = p[4];
	if (after != '\0' && after != '\n' && after != '\r' &&
	    after != ' ' && after != '\t')
		return 0;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		printf("?EDIT needs a terminal\n");
		fflush(stdout);
		return 1;
	}

	if (extract_filename(p + 4, fname, sizeof(fname)) > 0) {
		int seed = 1;

		path = fname;
		f = fopen(path, "r");		/* keep existing content as-is */
		if (f) {
			if (fgetc(f) != EOF)
				seed = 0;
			fclose(f);
		}
		if (seed) {			/* new or empty: seed from memory */
			f = fopen(path, "w");
			if (!f) {
				printf("?cannot write %s\n", path);
				fflush(stdout);
				return 1;
			}
			basic_program_dump(b, f);
			fclose(f);
		}
	} else {
		int fd;

		is_temp = 1;
		path = tmp;
		tmpdir = getenv("TMPDIR");
		if (!tmpdir || !*tmpdir)
			tmpdir = "/tmp";
		snprintf(tmp, sizeof(tmp), "%s/lumi-basicXXXXXX", tmpdir);
		fd = mkstemp(tmp);
		if (fd < 0) {
			printf("?cannot create a temporary file\n");
			fflush(stdout);
			return 1;
		}
		f = fdopen(fd, "w");
		if (!f) {
			close(fd);
			unlink(tmp);
			printf("?cannot write the program\n");
			fflush(stdout);
			return 1;
		}
		basic_program_dump(b, f);	/* raw source round-trips */
		fclose(f);
	}

	av[0] = "lumi-edit";
	av[1] = path;
	av[2] = NULL;
	cmd_edit_main(2, av);			/* full-screen, restores the tty */

	f = fopen(path, "r");
	if (f) {
		load_program_file(b, f);
		fclose(f);
	}
	if (is_temp)
		unlink(path);

	edit_autorun(b);			/* save-and-run */
	return 1;
}

/* Handle a "SAVE <file>" or "LOAD <file>" line. SAVE writes the program in
 * LIST form; LOAD clears it and reads the program lines back. These manage
 * the program from the REPL and are not language statements, so the
 * interpreter itself stays free of file I/O. Returns 1 if the line was one
 * of these commands. */
static int
try_file_cmd(struct basic *b, const char *line)
{
	const char *p = line;
	char fname[512];
	FILE *f;
	int save;
	char after;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncasecmp(p, "save", 4) == 0)
		save = 1;
	else if (strncasecmp(p, "load", 4) == 0)
		save = 0;
	else
		return 0;
	/* match SAVE/LOAD only as a whole word, so SAVED or LOADS do not */
	after = p[4];
	if (after != '\0' && after != '\n' && after != '\r' &&
	    after != ' ' && after != '\t' && after != '"')
		return 0;

	if (extract_filename(p + 4, fname, sizeof(fname)) == 0) {
		printf("?%s needs a filename\n", save ? "SAVE" : "LOAD");
		fflush(stdout);
		return 1;
	}

	if (save) {
		f = fopen(fname, "w");
		if (!f) {
			printf("?cannot save %s\n", fname);
			fflush(stdout);
			return 1;
		}
		basic_program_dump(b, f);	/* raw source round-trips */
		fclose(f);
		printf("saved %s\n", fname);
	} else {
		f = fopen(fname, "r");
		if (!f) {
			printf("?cannot load %s\n", fname);
			fflush(stdout);
			return 1;
		}
		load_program_file(b, f);
		fclose(f);
		printf("loaded %s\n", fname);
	}
	fflush(stdout);
	return 1;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s\n"
	    "\n"
	    "A BASIC-style calculator. Reads statements from standard input.\n"
	    "\n"
	    "  <expr>            print the value of an expression\n"
	    "  PRINT a, \"txt\"; b  print values and text\n"
	    "  LET x = <expr>    assign a variable (LET is optional)\n"
	    "  REM ...           a comment\n"
	    "  <n> <stmt>        set program file line n (a bare number deletes)\n"
	    "  name: <stmt>      label a line; GOTO/GOSUB jump to labels\n"
	    "  LIST / RUN / NEW  show (numbered), run, or clear the program\n"
	    "  SAVE <file>       write the program to a file\n"
	    "  LOAD <file>       replace the program with one from a file\n"
	    "  EDIT [file]       edit the program full-screen, then run it\n"
	    "  SEND <expr>       send a value to another pane (in a session)\n"
	    "  QUIT              leave the REPL\n",
	    progname);
}

int
cmd_basic_main(int argc, char **argv)
{
	struct basic *b;
	char line[1024];
	int interactive;

	if (argv[0])
		progname = argv[0];

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0)) {
		usage();
		return 0;
	}

	b = basic_new();
	if (!b) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}

	interactive = isatty(STDIN_FILENO);
	if (interactive) {
		printf("lumi basic -- type an expression, or QUIT to exit\n");
		fflush(stdout);
	}

	for (;;) {
		struct basic_result r;

		if (interactive) {
			fputs("> ", stdout);
			fflush(stdout);
		}
		if (!fgets(line, sizeof(line), stdin))
			break;

		strip_paste_markers(line);
		if (try_send(b, line))
			continue;
		if (try_file_cmd(b, line))
			continue;
		if (try_edit(b, line))
			continue;
		r = basic_exec(b, line, stdin, stdout);
		if (r.status == BAS_QUIT)
			break;
		if (r.status == BAS_ERROR) {
			printf("?%s\n", r.msg);
			fflush(stdout);
		}
	}

	basic_free(b);
	return 0;
}
