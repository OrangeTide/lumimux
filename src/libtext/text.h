/* text.h : editable text buffer with a line index */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>

/*
 * A text buffer holds an ordered list of lines. Lines never contain a
 * newline byte; the newline is implied between adjacent lines. A buffer
 * always has at least one line, so an empty buffer is a single empty line.
 *
 * Columns are byte offsets within a line. Callers that care about display
 * columns convert through libutf8. Insertion takes literal bytes with no
 * newline handling; use text_split() to break a line and text_join() to
 * merge two.
 *
 * Functions that can fail return OK (0) or ERR (-1). Out-of-range line or
 * column arguments return ERR and leave the buffer unchanged.
 */

struct text;

/** Allocate an empty buffer (one empty line). Returns NULL on failure. */
struct text *text_new(void);

/** Free a buffer and all its lines. NULL is accepted. */
void text_free(struct text *t);

/** Replace the contents with the file at path. On failure the buffer is
 *  left unchanged. Returns 0, or -1 with errno set. */
int text_load(struct text *t, const char *path);

/** Write the buffer to path, preserving whether the source ended with a
 *  trailing newline. Returns 0, or -1 with errno set. Clears the dirty
 *  flag on success. */
int text_save(struct text *t, const char *path);

/** Number of lines (always >= 1). */
size_t text_lines(const struct text *t);

/** Return a NUL-terminated pointer to the line's bytes, or NULL if line is
 *  out of range. When len is non-NULL it receives the byte length. The
 *  pointer is valid until the next edit of that line. */
const char *text_line(const struct text *t, size_t line, size_t *len);

/** Byte length of a line, or 0 if out of range. */
size_t text_line_len(const struct text *t, size_t line);

/** Insert n bytes of s at byte offset col in line. s must not contain a
 *  newline. Returns 0 or -1. */
int text_insert(struct text *t, size_t line, size_t col,
    const char *s, size_t n);

/** Delete up to n bytes at byte offset col in line, clamped to the end of
 *  the line. Returns 0 or -1. */
int text_delete(struct text *t, size_t line, size_t col, size_t n);

/** Split line at col: the tail moves to a new line inserted after it.
 *  Returns 0 or -1. */
int text_split(struct text *t, size_t line, size_t col);

/** Append the following line to line and remove it. Returns -1 if line is
 *  the last line. */
int text_join(struct text *t, size_t line);

/** Non-zero if the buffer changed since the last load or save. */
int text_dirty(const struct text *t);

/*
 * Undo and redo. Each edit pushes its inverse onto the undo stack, and a
 * run of consecutive insertions coalesces into one step. Call
 * text_undo_boundary() to end the current run, for example when the caller
 * moves the cursor, so unrelated typing does not merge.
 */

/** Non-zero if there is anything to undo or redo. */
int text_can_undo(const struct text *t);
int text_can_redo(const struct text *t);

/** Undo (or redo) the most recent change. On success the location the
 *  change affected is returned through line and col (either may be NULL),
 *  which a cursor should follow. Returns 0, or -1 when the stack is empty. */
int text_undo(struct text *t, size_t *line, size_t *col);
int text_redo(struct text *t, size_t *line, size_t *col);

/** End the current coalescing run so the next insertion starts a new undo
 *  step. */
void text_undo_boundary(struct text *t);

#endif /* TEXT_H */
