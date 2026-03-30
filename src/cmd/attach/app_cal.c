/* app_cal.c : month-view calendar */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "app.h"
#include "tui_pad.h"
#include "tui_box.h"
#include "tui_sep.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static struct {
	int year;
	int month;	/* 1-12 */
	int sel_day;	/* 1-based selected day */
	int today_day;	/* 1-based day of today (0 if not this month) */
} cal;

static int
days_in_month(int year, int month)
{
	static const int mdays[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	int d = mdays[month - 1];

	if (month == 2 && (year % 4 == 0 &&
	    (year % 100 != 0 || year % 400 == 0)))
		d = 29;
	return d;
}

/* day of week for the 1st of month (0=Sun, 6=Sat) */
static int
first_weekday(int year, int month)
{
	struct tm t;

	memset(&t, 0, sizeof(t));
	t.tm_year = year - 1900;
	t.tm_mon = month - 1;
	t.tm_mday = 1;
	t.tm_isdst = -1;
	mktime(&t);
	return t.tm_wday;
}

static void
cal_init_today(void)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (!tm)
		return;
	cal.year = tm->tm_year + 1900;
	cal.month = tm->tm_mon + 1;
	cal.sel_day = tm->tm_mday;
	cal.today_day = tm->tm_mday;
}

static void
cal_update_today(void)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (!tm) {
		cal.today_day = 0;
		return;
	}
	if (tm->tm_year + 1900 == cal.year &&
	    tm->tm_mon + 1 == cal.month)
		cal.today_day = tm->tm_mday;
	else
		cal.today_day = 0;
}

static void
cal_draw(struct app_ctx *ctx)
{
	struct tui_pad *p = ctx->pad;
	struct tui_box box;
	static const char *days_hdr = "Su Mo Tu We Th Fr Sa";
	char title[32];
	static const char *months[] = {
		"January", "February", "March", "April",
		"May", "June", "July", "August",
		"September", "October", "November", "December",
	};
	int bw = 24;	/* 7*3+1+2 border */
	int bh = 10;	/* border + header + sep + 6 weeks + border */
	int r, c, day, dim, wday;
	struct vt_color fg = ctx->theme->content_fg;
	struct vt_color bg = ctx->theme->content_bg;
	struct vt_color sel_fg = ctx->theme->sel_fg;
	struct vt_color sel_bg = ctx->theme->sel_bg;
	struct vt_color kfg = ctx->theme->key_fg;

	snprintf(title, sizeof(title), "%s %d",
	    months[cal.month - 1], cal.year);

	box.theme = ctx->theme;
	box.title = title;
	box.footer = "</>  ESC";
	box.w = bw;
	box.h = bh;
	tui_box_center(p, &box, ctx->screen_rows, ctx->screen_cols);

	/* day-of-week header */
	tui_pad_puts(p, 1, 2, days_hdr, kfg, bg, 0, TUI_OPAQUE);

	/* separator line */
	tui_sep_draw(p, ctx->theme, 2, 0, bw);

	/* day grid */
	dim = days_in_month(cal.year, cal.month);
	wday = first_weekday(cal.year, cal.month);
	day = 1;
	for (r = 0; r < 6; r++) {
		for (c = 0; c < 7 && day <= dim; c++) {
			char ds[4];
			struct vt_color dfg, dbg;
			uint16_t attrs = 0;
			int col;

			if (r == 0 && c < wday)
				continue;

			col = 2 + c * 3;

			if (day == cal.sel_day) {
				dfg = sel_fg;
				dbg = sel_bg;
				attrs = VT_ATTR_BOLD;
			} else if (day == cal.today_day) {
				dfg = kfg;
				dbg = bg;
				attrs = VT_ATTR_UNDERLINE;
			} else {
				dfg = fg;
				dbg = bg;
			}

			snprintf(ds, sizeof(ds), "%2d", day);
			tui_pad_puts(p, 3 + r, col, ds,
			    dfg, dbg, attrs, TUI_OPAQUE);
			day++;
		}
	}
}

static void
cal_show(struct app_ctx *ctx)
{
	(void)ctx;
	cal_init_today();
}

static void
cal_hide(struct app_ctx *ctx)
{
	(void)ctx;
}

static void
cal_prev_month(void)
{
	cal.month--;
	if (cal.month < 1) {
		cal.month = 12;
		cal.year--;
	}
	{
		int dim = days_in_month(cal.year, cal.month);
		if (cal.sel_day > dim)
			cal.sel_day = dim;
	}
	cal_update_today();
}

static void
cal_next_month(void)
{
	cal.month++;
	if (cal.month > 12) {
		cal.month = 1;
		cal.year++;
	}
	{
		int dim = days_in_month(cal.year, cal.month);
		if (cal.sel_day > dim)
			cal.sel_day = dim;
	}
	cal_update_today();
}

static int
cal_input(struct app_ctx *ctx, const struct tkbd_seq *seq)
{
	int dim = days_in_month(cal.year, cal.month);

	switch (seq->key) {
	case TKBD_KEY_UP: /* -7 days */
		cal.sel_day -= 7;
		if (cal.sel_day < 1) {
			cal_prev_month();
			dim = days_in_month(cal.year, cal.month);
			cal.sel_day += dim;
			if (cal.sel_day < 1)
				cal.sel_day = 1;
		}
		goto redraw;
	case TKBD_KEY_DOWN: /* +7 days */
		cal.sel_day += 7;
		if (cal.sel_day > dim) {
			cal.sel_day -= dim;
			cal_next_month();
		}
		goto redraw;
	case TKBD_KEY_RIGHT: /* +1 day */
		cal.sel_day++;
		if (cal.sel_day > dim) {
			cal.sel_day = 1;
			cal_next_month();
		}
		goto redraw;
	case TKBD_KEY_LEFT: /* -1 day */
		cal.sel_day--;
		if (cal.sel_day < 1) {
			cal_prev_month();
			cal.sel_day =
			    days_in_month(cal.year, cal.month);
		}
		goto redraw;
	case TKBD_KEY_ESC:
		return 0; /* dismiss */
	default:
		break;
	}

	/* < or h or p: prev month */
	if (seq->ch == '<' || seq->ch == 'h' || seq->ch == 'p') {
		cal_prev_month();
		goto redraw;
	}

	/* > or l or n: next month */
	if (seq->ch == '>' || seq->ch == 'l' || seq->ch == 'n') {
		cal_next_month();
		goto redraw;
	}

	/* t: jump to today */
	if (seq->ch == 't') {
		cal_init_today();
		goto redraw;
	}

	return 1; /* consume unknown keys */

redraw:
	cal_draw(ctx);
	ctx->render(ctx);
	return 1;
}

const struct app app_cal = {
	.name = "Calendar",
	.key = "l",
	.show = cal_show,
	.hide = cal_hide,
	.draw = cal_draw,
	.input = cal_input,
};
