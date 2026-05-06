/* txl.c : terminal translation engine */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "txl.h"
#include "ti.h"
#include "xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ANSI/xterm fallback strings ---- */

static const char *ansi_fallbacks[TXL_NCAP] = {
	[TXL_CIVIS] = "\033[?25l",
	[TXL_CNORM] = "\033[?25h",
	[TXL_CLEAR] = "\033[H\033[2J",
	[TXL_SGR0]  = "\033[0m",
	[TXL_SMCUP] = "\033[?1049h",
	[TXL_RMCUP] = "\033[?1049l",
	[TXL_SC]    = "\0337",
	[TXL_RC]    = "\0338",
	[TXL_BOLD]  = "\033[1m",
	[TXL_DIM]   = "\033[2m",
	[TXL_SITM]  = "\033[3m",
	[TXL_REV]   = "\033[7m",
	[TXL_SMUL]  = "\033[4m",
	[TXL_BLINK] = "\033[5m",
	[TXL_INVIS] = "\033[8m",
	[TXL_SMXX]  = "\033[9m",
};

/* Map TXL_* enum to terminfo string capability index.
 * -1 means use named lookup (extended capability). */
static const int ti_cap_index[TXL_NCAP] = {
	[TXL_CIVIS] = ti_civis,
	[TXL_CNORM] = ti_cnorm,
	[TXL_CLEAR] = ti_clear,
	[TXL_SGR0]  = ti_sgr0,
	[TXL_SMCUP] = ti_smcup,
	[TXL_RMCUP] = ti_rmcup,
	[TXL_SC]    = ti_sc,
	[TXL_RC]    = ti_rc,
	[TXL_BOLD]  = ti_bold,
	[TXL_DIM]   = ti_dim,
	[TXL_SITM]  = -1,	/* extended: "sitm" */
	[TXL_REV]   = ti_rev,
	[TXL_SMUL]  = ti_smul,
	[TXL_BLINK] = ti_blink,
	[TXL_INVIS] = ti_invis,
	[TXL_SMXX]  = -1,	/* extended: "smxx" */
};

/* Names for extended capabilities (no standard index). */
static const char *ext_cap_name[TXL_NCAP] = {
	[TXL_SITM] = "sitm",
	[TXL_SMXX] = "smxx",
};

/* ---- txl structure ---- */

struct txl {
	ti_terminfo	*ti;		/* NULL if terminfo unavailable */
	const char	*caps[TXL_NCAP]; /* resolved capability strings */
	char		*cup_str;	/* raw cup capability for ti_parm */
	char		*setaf_str;	/* raw setaf capability */
	char		*setab_str;	/* raw setab capability */
	int		 ncolors;	/* max colors, 0 if unknown */
	int		 has_rgb;	/* truecolor support */
	int		 has_bce;	/* background color erase */
};

/* Check environment for truecolor hints. */
static int
detect_rgb(const ti_terminfo *ti)
{
	const char *ct;

	/* check COLORTERM=truecolor or COLORTERM=24bit */
	ct = getenv("COLORTERM");
	if (ct && (strcmp(ct, "truecolor") == 0 ||
	    strcmp(ct, "24bit") == 0))
		return 1;

	/* check terminfo extended capability "RGB" or "Tc" */
	if (ti) {
		if (ti_getstr((ti_terminfo *)ti, "setrgbf"))
			return 1;
		if (ti_getstr((ti_terminfo *)ti, "Tc"))
			return 1;
	}

	return 0;
}

struct txl *
txl_new(const char *term)
{
	struct txl *t;
	int err, i;

	t = xcalloc(1, sizeof(*t));

	/* load terminfo */
	t->ti = ti_load(term, &err);

	/* resolve simple capabilities */
	for (i = 0; i < TXL_NCAP; i++) {
		const char *s = NULL;

		if (t->ti) {
			if (ti_cap_index[i] >= 0)
				s = ti_getstri(t->ti, ti_cap_index[i]);
			else if (ext_cap_name[i])
				s = ti_getstr(t->ti, ext_cap_name[i]);
		}

		t->caps[i] = s ? s : ansi_fallbacks[i];
	}

	/* resolve parameterized capabilities */
	if (t->ti) {
		t->cup_str = ti_getstri(t->ti, ti_cup);
		t->setaf_str = ti_getstri(t->ti, ti_setaf);
		t->setab_str = ti_getstri(t->ti, ti_setab);
		t->ncolors = ti_getnumi(t->ti, ti_colors);
	}
	if (t->ncolors <= 0)
		t->ncolors = 256;	/* assume modern terminal */

	t->has_rgb = detect_rgb(t->ti);
	t->has_bce = t->ti ? ti_getbooli(t->ti, ti_bce) : 1;

	return t;
}

void
txl_free(struct txl *t)
{
	if (!t)
		return;
	ti_free(t->ti);
	free(t);
}

/* ---- simple capabilities ---- */

const char *
txl_str(const struct txl *t, int cap)
{
	if (cap < 0 || cap >= TXL_NCAP)
		return NULL;
	return t->caps[cap];
}

/* ---- parameterized capabilities ---- */

/* emit a decimal number into buf, return length */
static int
fmt_int(char *buf, int n)
{
	int len = 0;
	char tmp[12];
	int i;

	if (n < 0)
		n = 0;
	if (n == 0) {
		buf[0] = '0';
		return 1;
	}
	while (n > 0) {
		tmp[len++] = '0' + (n % 10);
		n /= 10;
	}
	for (i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	return len;
}

/* ANSI fallback: \033[row;colH (1-based) */
static int
ansi_cup(char *buf, size_t bufsz, int row, int col)
{
	int len = 0;

	if (bufsz < 16)
		return 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	len += fmt_int(buf + len, row + 1);
	buf[len++] = ';';
	len += fmt_int(buf + len, col + 1);
	buf[len++] = 'H';
	buf[len] = '\0';
	return len;
}

int
txl_cup(const struct txl *t, char *buf, size_t bufsz, int row, int col)
{
	int len;

	if (bufsz < 16)
		return 0;
	if (t->cup_str) {
		len = ti_parm(buf, t->cup_str, (int)bufsz, row, col);
		if (len > 0)
			return len;
	}
	return ansi_cup(buf, bufsz, row, col);
}

/* ANSI fallback: \033[38;5;Nm or \033[3Nm */
static int
ansi_setaf(char *buf, size_t bufsz, int color)
{
	int len = 0;

	if (bufsz < 16)
		return 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	if (color < 8) {
		len += fmt_int(buf + len, 30 + color);
	} else if (color < 16) {
		len += fmt_int(buf + len, 90 + color - 8);
	} else {
		buf[len++] = '3';
		buf[len++] = '8';
		buf[len++] = ';';
		buf[len++] = '5';
		buf[len++] = ';';
		len += fmt_int(buf + len, color);
	}
	buf[len++] = 'm';
	buf[len] = '\0';
	return len;
}

int
txl_setaf(const struct txl *t, char *buf, size_t bufsz, int color)
{
	int len;

	if (t->setaf_str) {
		len = ti_parm(buf, t->setaf_str, (int)bufsz, color);
		if (len > 0)
			return len;
	}
	return ansi_setaf(buf, bufsz, color);
}

/* ANSI fallback: \033[48;5;Nm or \033[4Nm */
static int
ansi_setab(char *buf, size_t bufsz, int color)
{
	int len = 0;

	if (bufsz < 16)
		return 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	if (color < 8) {
		len += fmt_int(buf + len, 40 + color);
	} else if (color < 16) {
		len += fmt_int(buf + len, 100 + color - 8);
	} else {
		buf[len++] = '4';
		buf[len++] = '8';
		buf[len++] = ';';
		buf[len++] = '5';
		buf[len++] = ';';
		len += fmt_int(buf + len, color);
	}
	buf[len++] = 'm';
	buf[len] = '\0';
	return len;
}

int
txl_setab(const struct txl *t, char *buf, size_t bufsz, int color)
{
	int len;

	if (t->setab_str) {
		len = ti_parm(buf, t->setab_str, (int)bufsz, color);
		if (len > 0)
			return len;
	}
	return ansi_setab(buf, bufsz, color);
}

/* ---- mode control ---- */

int
txl_decset(char *buf, size_t bufsz, int mode)
{
	int len = 0;

	if (bufsz < 16)
		return 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	buf[len++] = '?';
	len += fmt_int(buf + len, mode);
	buf[len++] = 'h';
	buf[len] = '\0';
	return len;
}

int
txl_decrst(char *buf, size_t bufsz, int mode)
{
	int len = 0;

	if (bufsz < 16)
		return 0;
	buf[len++] = '\033';
	buf[len++] = '[';
	buf[len++] = '?';
	len += fmt_int(buf + len, mode);
	buf[len++] = 'l';
	buf[len] = '\0';
	return len;
}

/* ---- query ---- */

int
txl_has_rgb(const struct txl *t)
{
	return t->has_rgb;
}

int
txl_colors(const struct txl *t)
{
	return t->ncolors;
}

int
txl_has_bce(const struct txl *t)
{
	return t->has_bce;
}
