/* app_calc.c : four-function calculator */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "app.h"
#include "ipc_msg.h"
#include "tui_pad.h"
#include "tui_box.h"
#include "tui_sep.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define CALC_DISP_MAX 20

static struct {
	char display[CALC_DISP_MAX + 1];
	int display_len;
	double acc;
	double operand;
	char op;		/* pending operator: +, -, *, / or 0 */
	int have_dot;
	int new_entry;		/* next digit starts fresh */
} calc;

static void
calc_reset(void)
{
	memset(&calc, 0, sizeof(calc));
	calc.display[0] = '0';
	calc.display_len = 1;
	calc.new_entry = 1;
}

static void
calc_format(double v)
{
	if (v == (double)(long long)v && v >= -999999999 && v <= 999999999)
		calc.display_len = snprintf(calc.display,
		    sizeof(calc.display), "%lld", (long long)v);
	else
		calc.display_len = snprintf(calc.display,
		    sizeof(calc.display), "%.10g", v);
	if (calc.display_len < 0)
		calc.display_len = 0;
	if (calc.display_len >= (int)sizeof(calc.display))
		calc.display_len = (int)sizeof(calc.display) - 1;
}

static void
calc_insert(struct app_ctx *ctx, const char *display, int len)
{
	if (len > 0)
		ipc_msg_send(ctx->input_fd, IPC_MSG_INPUT, display, len);
}

static double
calc_parse_display(void)
{
	double v = 0;

	sscanf(calc.display, "%lf", &v);
	return v;
}

static void
calc_do_op(void)
{
	double cur = calc_parse_display();

	switch (calc.op) {
	case '+': calc.acc += cur; break;
	case '-': calc.acc -= cur; break;
	case '*': calc.acc *= cur; break;
	case '/':
		if (cur != 0)
			calc.acc /= cur;
		break;
	default:
		calc.acc = cur;
		break;
	}
}

static void
calc_draw(struct app_ctx *ctx)
{
	struct tui_pad *p = ctx->pad;
	struct tui_box box;
	int bw = CALC_DISP_MAX + 4;
	int bh = 9;
	int r, c;
	static const char *rows[] = {
		"7 8 9 /",
		"4 5 6 *",
		"1 2 3 -",
		"0 . = +",
	};
	struct vt_color fg = ctx->theme->content_fg;
	struct vt_color bg = ctx->theme->content_bg;
	struct vt_color kfg = ctx->theme->key_fg;

	box.theme = ctx->theme;
	box.title = "Calculator";
	box.footer = "ESC";
	box.w = bw;
	box.h = bh;
	tui_box_center(p, &box, ctx->screen_rows, ctx->screen_cols);

	/* display line (right-aligned in interior) */
	{
		int dw = bw - 4;	/* interior width minus padding */
		int off = dw - calc.display_len;

		if (off < 0) off = 0;
		for (c = 0; c < dw; c++) {
			int di = c - off;
			uint32_t ch = (di >= 0 && di < calc.display_len)
			    ? (uint32_t)(unsigned char)calc.display[di]
			    : ' ';

			tui_pad_put(p, 1, 2 + c, ch, fg, bg, 0, TUI_OPAQUE);
		}
	}

	/* separator */
	tui_sep_draw(p, ctx->theme, 2, 0, bw);

	/* button grid */
	for (r = 0; r < 4; r++) {
		const char *s = rows[r];
		int col = 2;

		for (c = 0; s[c]; c++) {
			struct vt_color cflag;

			if (s[c] == ' ') {
				tui_pad_put(p, 3 + r, col, ' ',
				    fg, bg, 0, TUI_OPAQUE);
				col++;
				continue;
			}
			cflag = (s[c] >= '0' && s[c] <= '9') ? fg : kfg;
			tui_pad_put(p, 3 + r, col, (uint32_t)s[c],
			    cflag, bg, 0, TUI_OPAQUE);
			col++;
		}
		/* pad rest of row */
		while (col < bw - 1)
			tui_pad_put(p, 3 + r, col++, ' ',
			    fg, bg, 0, TUI_OPAQUE);
	}

	/* "C" label */
	tui_pad_put(p, 7, 2, 'C', kfg, bg, 0, TUI_OPAQUE);
}

static void
calc_show(struct app_ctx *ctx)
{
	(void)ctx;
	calc_reset();
}

static void
calc_hide(struct app_ctx *ctx)
{
	(void)ctx;
}

static int
calc_input(struct app_ctx *ctx, const struct tkbd_seq *seq)
{
	uint32_t ch = seq->ch;

	if (seq->key == TKBD_KEY_ESC)
		return 0;

	/* digits */
	if (ch >= '0' && ch <= '9') {
		if (calc.new_entry) {
			calc.display[0] = (char)ch;
			calc.display_len = 1;
			calc.display[1] = '\0';
			calc.new_entry = 0;
			calc.have_dot = 0;
		} else if (calc.display_len < CALC_DISP_MAX) {
			calc.display[calc.display_len++] = (char)ch;
			calc.display[calc.display_len] = '\0';
		}
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* decimal point */
	if (ch == '.') {
		if (calc.new_entry) {
			calc.display[0] = '0';
			calc.display[1] = '.';
			calc.display_len = 2;
			calc.display[2] = '\0';
			calc.new_entry = 0;
			calc.have_dot = 1;
		} else if (!calc.have_dot &&
		    calc.display_len < CALC_DISP_MAX) {
			calc.display[calc.display_len++] = '.';
			calc.display[calc.display_len] = '\0';
			calc.have_dot = 1;
		}
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* operators */
	if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
		if (!calc.new_entry)
			calc_do_op();
		calc_format(calc.acc);
		calc.op = (char)ch;
		calc.new_entry = 1;
		calc.have_dot = 0;
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* equals or Enter */
	if (ch == '=' || seq->key == TKBD_KEY_ENTER) {
		calc_do_op();
		calc_format(calc.acc);
		calc.op = 0;
		calc.new_entry = 1;
		calc.have_dot = 0;
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* clear */
	if (ch == 'c' || ch == 'C') {
		calc_reset();
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* backspace */
	if (seq->key == TKBD_KEY_BACKSPACE ||
	    seq->key == TKBD_KEY_BACKSPACE2) {
		if (!calc.new_entry && calc.display_len > 1) {
			if (calc.display[calc.display_len - 1] == '.')
				calc.have_dot = 0;
			calc.display[--calc.display_len] = '\0';
		} else {
			calc.display[0] = '0';
			calc.display_len = 1;
			calc.display[1] = '\0';
			calc.new_entry = 1;
		}
		calc_draw(ctx);
		ctx->render(ctx);
		return 1;
	}

	/* Paste */
	if (ch == 'p' || ch == 'P') {
		calc_insert(ctx, calc.display, calc.display_len);
		return 1;
	}

	/* Quit */
	if (ch == 'q' || ch == 'Q') {
		return 0;
	}

	return 1; /* consume unknown keys */
}

const struct app app_calc = {
	.name = "Calculator",
	.key = "c",
	.show = calc_show,
	.hide = calc_hide,
	.draw = calc_draw,
	.input = calc_input,
};
