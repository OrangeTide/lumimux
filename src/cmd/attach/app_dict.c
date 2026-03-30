/* app_dict.c : dictionary browser with incremental filter */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "app.h"
#include "tui_pad.h"
#include "tui_list.h"
#include "ipc_msg.h"
#include "tkbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DICT_PATH "/usr/share/dict/words"
#define DICT_MAX_WORDS 120000
#define DICT_FILTER_MAX 32
#define DICT_MATCH_MAX 1000

static struct {
	char **words;
	int word_count;
	int loaded;
	char filter[DICT_FILTER_MAX + 1];
	int filter_len;
	int matched[DICT_MATCH_MAX];
	int match_count;
	struct tui_list list;
	char *buf;		/* heap buffer for file contents */
} dict;

static void
dict_load(void)
{
	FILE *fp;
	long sz;
	char *p, *end;
	int count;

	if (dict.loaded)
		return;
	dict.loaded = 1;

	fp = fopen(DICT_PATH, "r");
	if (!fp)
		return;

	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	if (sz <= 0 || sz > 16 * 1024 * 1024) {
		fclose(fp);
		return;
	}
	fseek(fp, 0, SEEK_SET);

	dict.buf = malloc((size_t)sz + 1);
	if (!dict.buf) {
		fclose(fp);
		return;
	}
	if (fread(dict.buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(dict.buf);
		dict.buf = NULL;
		fclose(fp);
		return;
	}
	dict.buf[sz] = '\0';
	fclose(fp);

	/* count lines for allocation */
	count = 0;
	for (p = dict.buf; *p; p++) {
		if (*p == '\n')
			count++;
	}
	if (count > DICT_MAX_WORDS)
		count = DICT_MAX_WORDS;

	dict.words = malloc((size_t)count * sizeof(char *));
	if (!dict.words) {
		free(dict.buf);
		dict.buf = NULL;
		return;
	}

	/* split into words */
	dict.word_count = 0;
	p = dict.buf;
	end = dict.buf + sz;
	while (p < end && dict.word_count < count) {
		char *nl = memchr(p, '\n', (size_t)(end - p));

		if (!nl)
			nl = end;
		*nl = '\0';
		if (nl > p)
			dict.words[dict.word_count++] = p;
		p = nl + 1;
	}
}

static void
dict_rebuild_matches(void)
{
	int i;

	dict.match_count = 0;
	for (i = 0; i < dict.word_count && dict.match_count < DICT_MATCH_MAX;
	    i++) {
		if (dict.filter_len == 0 ||
		    strncasecmp(dict.words[i], dict.filter,
		    (size_t)dict.filter_len) == 0)
			dict.matched[dict.match_count++] = i;
	}
	dict.list.count = dict.match_count;
	if (dict.list.sel >= dict.match_count)
		dict.list.sel = dict.match_count > 0 ? dict.match_count - 1 : 0;
}

static void
dict_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	int *matched = ctx;
	const char *word = dict.words[matched[index]];
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;
	int c, wlen;

	c = col;
	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	wlen = (int)strlen(word);
	if (wlen > width - 2)
		wlen = width - 2;
	tui_pad_puts(p, row, c, word, fg, bg, attrs, TUI_OPAQUE);
	c += wlen;

	while (c < col + width)
		tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
}

static void
dict_draw(struct app_ctx *ctx)
{
	char title[64];

	if (dict.word_count == 0) {
		snprintf(title, sizeof(title), "Dictionary (not available)");
	} else if (dict.filter_len > 0) {
		snprintf(title, sizeof(title), "Dictionary [%s] (%d)",
		    dict.filter, dict.match_count);
	} else {
		snprintf(title, sizeof(title), "Dictionary (%d)",
		    dict.match_count);
	}

	tui_list_draw(ctx->pad, &dict.list, ctx->theme,
	    dict_row_cb, dict.matched, title,
	    ctx->screen_rows, ctx->screen_cols);
}

static void
dict_show(struct app_ctx *ctx)
{
	(void)ctx;
	dict_load();
	dict.filter[0] = '\0';
	dict.filter_len = 0;
	memset(&dict.list, 0, sizeof(dict.list));
	dict_rebuild_matches();
}

static void
dict_hide(struct app_ctx *ctx)
{
	(void)ctx;
}

static void
dict_insert(struct app_ctx *ctx)
{
	const char *word;
	int idx;

	if (dict.list.sel < 0 || dict.list.sel >= dict.match_count)
		return;

	idx = dict.matched[dict.list.sel];
	word = dict.words[idx];
	ipc_msg_send(ctx->input_fd, IPC_MSG_INPUT,
	    word, (uint32_t)strlen(word));
}

static int
dict_input(struct app_ctx *ctx, const struct tkbd_seq *seq)
{
	switch (seq->key) {
	case TKBD_KEY_UP:
		if (dict.list.sel > 0)
			dict.list.sel--;
		else if (dict.match_count > 0)
			dict.list.sel = dict.match_count - 1;
		goto redraw;
	case TKBD_KEY_DOWN:
		if (dict.list.sel < dict.match_count - 1)
			dict.list.sel++;
		else
			dict.list.sel = 0;
		goto redraw;
	case TKBD_KEY_RIGHT:
		dict_insert(ctx);
		return 0;
	case TKBD_KEY_LEFT:
		return 0;
	case TKBD_KEY_ESC:
		return 0; /* dismiss */
	case TKBD_KEY_ENTER:
		dict_insert(ctx);
		return 0;
	case TKBD_KEY_BACKSPACE:
	case TKBD_KEY_BACKSPACE2:
		if (dict.filter_len > 0) {
			dict.filter[--dict.filter_len] = '\0';
			dict_rebuild_matches();
			goto redraw;
		}
		return 1;
	default:
		break;
	}

	/* printable ASCII -- append to filter */
	if (seq->ch >= 0x20 && seq->ch < 0x7f) {
		if (dict.filter_len < DICT_FILTER_MAX) {
			dict.filter[dict.filter_len++] = (char)seq->ch;
			dict.filter[dict.filter_len] = '\0';
			dict.list.sel = 0;
			dict.list.scroll = 0;
			dict_rebuild_matches();
			goto redraw;
		}
		return 1;
	}

	return 1;

redraw:
	dict_draw(ctx);
	ctx->render(ctx);
	return 1;
}

const struct app app_dict = {
	.name = "Dictionary",
	.key = "d",
	.show = dict_show,
	.hide = dict_hide,
	.draw = dict_draw,
	.input = dict_input,
};
