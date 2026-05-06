/* vt_parse.c : VT escape sequence state machine parser */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "vt_parse.h"
#include "utf8.h"
#include "rune_width.h"
#include "xmalloc.h"

#include <stdlib.h>
#include <string.h>

/* parser states (based on VT500 state diagram) */
enum {
	ST_GROUND,
	ST_ESCAPE,
	ST_ESCAPE_INTERMED,
	ST_CSI_ENTRY,
	ST_CSI_PARAM,
	ST_CSI_INTERMED,
	ST_CSI_IGNORE,
	ST_OSC_STRING,
	ST_DCS_PASSTHRU,	/* DCS/APC/PM -- absorb until ST */
};

#define VT_MAX_PARAMS	16
#define VT_OSC_MAX	256
#define VT_DCS_INIT	4096
#define VT_DCS_MAX	(16 * 1024 * 1024)	/* 16 MB cap */

struct vt_parse {
	int		state;
	const struct vt_ops *ops;
	void		*ctx;

	/* CSI parameter accumulation */
	int		params[VT_MAX_PARAMS];
	int		nparam;
	int		cur_param;	/* current param being built */
	int		has_digit;	/* saw a digit in current param */
	int		intermed;	/* intermediate byte (0 or char) */

	/* OSC string accumulation */
	char		osc_buf[VT_OSC_MAX];
	int		osc_len;

	/* DCS passthrough accumulation */
	char		*dcs_buf;
	size_t		dcs_len;
	size_t		dcs_cap;
	int		dcs_introducer;	/* 'P', 'X', '^', or '_' */
	vt_parse_dcs_cb	dcs_cb;
	void		*dcs_ctx;

	/* UTF-8 decode state */
	unsigned char	utf8_buf[4];
	int		utf8_len;
	int		utf8_need;
};

struct vt_parse *
vt_parse_new(const struct vt_ops *ops, void *ctx)
{
	struct vt_parse *p;

	p = xcalloc(1, sizeof(*p));
	p->ops = ops;
	p->ctx = ctx;
	p->state = ST_GROUND;
	return p;
}

void
vt_parse_free(struct vt_parse *p)
{
	free(p->dcs_buf);
	free(p);
}

void
vt_parse_set_dcs_cb(struct vt_parse *p, vt_parse_dcs_cb cb, void *ctx)
{
	p->dcs_cb = cb;
	p->dcs_ctx = ctx;
}

void
vt_parse_reset(struct vt_parse *p)
{
	const struct vt_ops *ops = p->ops;
	void *ctx = p->ctx;
	char *dcs_buf = p->dcs_buf;
	size_t dcs_cap = p->dcs_cap;
	vt_parse_dcs_cb dcs_cb = p->dcs_cb;
	void *dcs_ctx = p->dcs_ctx;

	memset(p, 0, sizeof(*p));
	p->ops = ops;
	p->ctx = ctx;
	p->dcs_buf = dcs_buf;
	p->dcs_cap = dcs_cap;
	p->dcs_cb = dcs_cb;
	p->dcs_ctx = dcs_ctx;
	p->state = ST_GROUND;
}

static void
csi_reset(struct vt_parse *p)
{
	p->nparam = 0;
	p->cur_param = 0;
	p->has_digit = 0;
	p->intermed = 0;
}

static void
csi_finish_param(struct vt_parse *p)
{
	if (p->nparam < VT_MAX_PARAMS)
		p->params[p->nparam++] = p->has_digit ? p->cur_param : -1;
	p->cur_param = 0;
	p->has_digit = 0;
}

static void
emit_print(struct vt_parse *p, uint32_t cp)
{
	int w;

	if (!p->ops->print)
		return;
	w = rune_width(cp);
	if (w < 0)
		w = 1;
	p->ops->print(p->ctx, cp, w);
}

static void
emit_execute(struct vt_parse *p, uint8_t c)
{
	if (p->ops->execute)
		p->ops->execute(p->ctx, c);
}

static void
emit_csi(struct vt_parse *p, int final)
{
	if (p->ops->csi)
		p->ops->csi(p->ctx, p->params, p->nparam,
		    p->intermed, final);
}

static void
emit_esc(struct vt_parse *p, int final)
{
	if (p->ops->esc)
		p->ops->esc(p->ctx, p->intermed, final);
}

static void
emit_osc(struct vt_parse *p)
{
	if (p->ops->osc) {
		/* trim trailing bytes of a truncated UTF-8 sequence */
		p->osc_buf[p->osc_len] = '\0';
		p->osc_len = (int)utf8_trunc(p->osc_buf,
		    (size_t)p->osc_len + 1);
		p->ops->osc(p->ctx, p->osc_buf, (size_t)p->osc_len);
	}
}

static void
dcs_reset(struct vt_parse *p)
{
	p->dcs_len = 0;
}

static void
dcs_append(struct vt_parse *p, unsigned char c)
{
	if (p->dcs_len >= VT_DCS_MAX)
		return;
	if (p->dcs_len >= p->dcs_cap) {
		size_t newcap;
		char *newbuf;

		newcap = p->dcs_cap ? p->dcs_cap * 2 : VT_DCS_INIT;
		if (newcap > VT_DCS_MAX)
			newcap = VT_DCS_MAX;
		newbuf = realloc(p->dcs_buf, newcap);
		if (!newbuf)
			return;
		p->dcs_buf = newbuf;
		p->dcs_cap = newcap;
	}
	p->dcs_buf[p->dcs_len++] = (char)c;
}

static void
emit_dcs(struct vt_parse *p)
{
	if (p->dcs_cb && p->dcs_len > 0)
		p->dcs_cb(p->dcs_ctx, p->dcs_introducer,
		    p->dcs_buf, p->dcs_len);
	dcs_reset(p);
}

static void
process_ground(struct vt_parse *p, unsigned char c)
{
	uint32_t rune;
	int n;

	/* UTF-8 multi-byte continuation */
	if (p->utf8_need > 0) {
		if ((c & 0xC0) == 0x80) {
			p->utf8_buf[p->utf8_len++] = c;
			if (p->utf8_len >= p->utf8_need) {
				utf8_decode(&rune, p->utf8_buf,
				    (size_t)p->utf8_len);
				emit_print(p, rune);
				p->utf8_need = 0;
				p->utf8_len = 0;
			}
			return;
		}
		/* broken sequence -- emit replacement */
		emit_print(p, 0xFFFD);
		p->utf8_need = 0;
		p->utf8_len = 0;
		/* fall through to process c */
	}

	if (c < 0x20) {
		/* C0 control */
		emit_execute(p, c);
		return;
	}

	if (c == 0x7F) {
		/* DEL -- ignore in ground */
		return;
	}

	if (c < 0x80) {
		/* printable ASCII */
		emit_print(p, c);
		return;
	}

	/* UTF-8 lead byte */
	p->utf8_buf[0] = c;
	p->utf8_len = 1;
	n = utf8_runelen(0); /* just use byte count heuristic */
	if ((c & 0xE0) == 0xC0)
		p->utf8_need = 2;
	else if ((c & 0xF0) == 0xE0)
		p->utf8_need = 3;
	else if ((c & 0xF8) == 0xF0)
		p->utf8_need = 4;
	else {
		/* invalid lead byte */
		emit_print(p, 0xFFFD);
		p->utf8_need = 0;
		p->utf8_len = 0;
	}
	(void)n;
}

static void
process_byte(struct vt_parse *p, unsigned char c)
{
	/* anywhere transitions: ESC and certain C0 controls override state */
	if (c == 0x1B) {
		/* emit accumulated data on ESC (start of ST = ESC \) */
		if (p->state == ST_DCS_PASSTHRU)
			emit_dcs(p);
		if (p->state == ST_OSC_STRING)
			emit_osc(p);
		/* cancel any in-progress UTF-8 */
		p->utf8_need = 0;
		p->utf8_len = 0;
		p->state = ST_ESCAPE;
		p->intermed = 0;
		return;
	}

	/* C0 controls that execute in any state */
	if (p->state != ST_GROUND && p->state != ST_OSC_STRING) {
		if (c == 0x07 || c == 0x08 || c == 0x09 || c == 0x0A ||
		    c == 0x0B || c == 0x0C || c == 0x0D) {
			emit_execute(p, c);
			return;
		}
	}

	switch (p->state) {
	case ST_GROUND:
		process_ground(p, c);
		break;

	case ST_ESCAPE:
		if (c == '[') {
			csi_reset(p);
			p->state = ST_CSI_ENTRY;
		} else if (c == ']') {
			p->osc_len = 0;
			p->state = ST_OSC_STRING;
		} else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
			/* DCS, SOS, PM, APC -- accumulate until ST */
			p->dcs_introducer = c;
			dcs_reset(p);
			p->state = ST_DCS_PASSTHRU;
		} else if (c >= 0x20 && c <= 0x2F) {
			/* intermediate byte */
			p->intermed = c;
			p->state = ST_ESCAPE_INTERMED;
		} else if (c >= 0x30 && c <= 0x7E) {
			/* final byte -- dispatch ESC sequence */
			emit_esc(p, c);
			p->state = ST_GROUND;
		} else {
			/* unexpected -- back to ground */
			p->state = ST_GROUND;
		}
		break;

	case ST_ESCAPE_INTERMED:
		if (c >= 0x20 && c <= 0x2F) {
			/* additional intermediate -- overwrite (simplified) */
			p->intermed = c;
		} else if (c >= 0x30 && c <= 0x7E) {
			emit_esc(p, c);
			p->state = ST_GROUND;
		} else {
			p->state = ST_GROUND;
		}
		break;

	case ST_CSI_ENTRY:
		if (c >= '0' && c <= '9') {
			p->cur_param = c - '0';
			p->has_digit = 1;
			p->state = ST_CSI_PARAM;
		} else if (c == ';') {
			csi_finish_param(p);
			p->state = ST_CSI_PARAM;
		} else if (c >= 0x3C && c <= 0x3F) {
			/* private parameter marker (< = > ?) */
			p->intermed = c;
			p->state = ST_CSI_PARAM;
		} else if (c >= 0x20 && c <= 0x2F) {
			p->intermed = c;
			p->state = ST_CSI_INTERMED;
		} else if (c >= 0x40 && c <= 0x7E) {
			/* final with no params */
			csi_finish_param(p);
			emit_csi(p, c);
			p->state = ST_GROUND;
		} else {
			p->state = ST_GROUND;
		}
		break;

	case ST_CSI_PARAM:
		if (c >= '0' && c <= '9') {
			p->cur_param = p->cur_param * 10 + (c - '0');
			p->has_digit = 1;
		} else if (c == ';') {
			csi_finish_param(p);
		} else if (c >= 0x20 && c <= 0x2F) {
			csi_finish_param(p);
			p->intermed = c;
			p->state = ST_CSI_INTERMED;
		} else if (c >= 0x40 && c <= 0x7E) {
			csi_finish_param(p);
			emit_csi(p, c);
			p->state = ST_GROUND;
		} else if (c >= 0x3C && c <= 0x3F) {
			/* ignore additional private modifiers */
			p->state = ST_CSI_IGNORE;
		} else {
			p->state = ST_GROUND;
		}
		break;

	case ST_CSI_INTERMED:
		if (c >= 0x20 && c <= 0x2F) {
			/* additional intermediate */
		} else if (c >= 0x40 && c <= 0x7E) {
			emit_csi(p, c);
			p->state = ST_GROUND;
		} else {
			p->state = ST_CSI_IGNORE;
		}
		break;

	case ST_CSI_IGNORE:
		if (c >= 0x40 && c <= 0x7E)
			p->state = ST_GROUND;
		break;

	case ST_DCS_PASSTHRU:
		/* accumulate until ST (ESC \).
		 * 0x9C is the 8-bit C1 ST but collides with valid
		 * UTF-8 continuation bytes, so we ignore it here --
		 * all modern emitters use 7-bit ESC \ instead. */
		dcs_append(p, c);
		break;

	case ST_OSC_STRING:
		if (c == 0x07) {
			/* BEL terminates OSC (xterm style) */
			emit_osc(p);
			p->state = ST_GROUND;
		} else {
			if (p->osc_len < VT_OSC_MAX - 1)
				p->osc_buf[p->osc_len++] = (char)c;
		}
		break;
	}
}

void
vt_parse_feed(struct vt_parse *p, const char *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		process_byte(p, (unsigned char)data[i]);
}
