/* txl.h : terminal translation engine */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef TXL_H
#define TXL_H

#include <stddef.h>
#include <stdint.h>

/*
 * The terminal translation engine maps abstract terminal operations to
 * concrete escape sequences for the outer (real) terminal. It loads
 * capabilities from terminfo via the vendored ti.c library, with
 * hardcoded ANSI/xterm fallbacks when terminfo is unavailable.
 *
 * The inner terminal emulation (vt_state, vt_ops) is not affected --
 * it emulates a fixed VT-compatible terminal type. This layer only
 * controls output to the user's real terminal.
 */

struct txl;

/* Create a translation engine for the given terminal type.
 * Pass NULL to use $TERM. Returns NULL on allocation failure. */
struct txl *txl_new(const char *term);

/* Free the translation engine. */
void txl_free(struct txl *t);

/* ---- simple capability strings ---- */

/* Returns a NUL-terminated escape sequence, or NULL if the capability
 * is not available. The returned pointer is valid for the lifetime
 * of the txl object. */
const char *txl_str(const struct txl *t, int cap);

/* Capability constants for txl_str(). */
enum {
	TXL_CIVIS,	/* make cursor invisible */
	TXL_CNORM,	/* make cursor normal (visible) */
	TXL_CLEAR,	/* clear screen and home cursor */
	TXL_SGR0,	/* turn off all attributes */
	TXL_SMCUP,	/* enter alternate screen (ca mode) */
	TXL_RMCUP,	/* exit alternate screen */
	TXL_SC,		/* save cursor position */
	TXL_RC,		/* restore cursor position */
	TXL_BOLD,	/* turn on bold */
	TXL_DIM,	/* turn on dim/half-bright */
	TXL_SITM,	/* enter italics mode */
	TXL_REV,	/* turn on reverse video */
	TXL_SMUL,	/* begin underline mode */
	TXL_BLINK,	/* turn on blinking */
	TXL_INVIS,	/* turn on invisible (hidden) */
	TXL_SMXX,	/* turn on strikethrough */
	TXL_NCAP,	/* number of simple capabilities (internal) */
};

/* ---- parameterized capabilities ---- */

/* Move cursor to (row, col), 0-based.
 * Writes the sequence into buf (at most bufsz bytes).
 * Returns number of bytes written, or 0 on error. */
int txl_cup(const struct txl *t, char *buf, size_t bufsz, int row, int col);

/* Set foreground color (indexed 0-255).
 * Returns bytes written into buf. */
int txl_setaf(const struct txl *t, char *buf, size_t bufsz, int color);

/* Set background color (indexed 0-255).
 * Returns bytes written into buf. */
int txl_setab(const struct txl *t, char *buf, size_t bufsz, int color);

/* ---- mode control ---- */

/* Enable/disable a private mode by number (e.g. 1 for DECCKM,
 * 2004 for bracketed paste). Writes the DECSET or DECRST sequence
 * into buf. Returns bytes written. */
int txl_decset(char *buf, size_t bufsz, int mode);
int txl_decrst(char *buf, size_t bufsz, int mode);

/* ---- direct SGR construction ---- */

/* These are intentionally NOT in txl -- the renderer builds SGR
 * sequences directly because it needs to track incremental state
 * transitions. The SGR parameter encoding (30+n, 38;5;n, 38;2;r;g;b)
 * is standardized and does not vary across terminals. */

/* ---- query ---- */

/* Return non-zero if the terminal supports RGB color (truecolor). */
int txl_has_rgb(const struct txl *t);

/* Return the number of colors the terminal supports. */
int txl_colors(const struct txl *t);

#endif /* TXL_H */
