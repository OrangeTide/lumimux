/* app_emoji.c : emoji picker with categories */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "app.h"
#include "tui_pad.h"
#include "tui_list.h"
#include "ipc_msg.h"
#include "utf8.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>

struct emoji_entry {
	uint32_t	codepoint;
	const char	*name;
	const char	*category;
};

static const struct emoji_entry emoji_table[] = {
	/* smileys */
	{ 0x1F600, "grinning face", "smileys" },
	{ 0x1F601, "beaming face", "smileys" },
	{ 0x1F602, "face with tears of joy", "smileys" },
	{ 0x1F603, "grinning face big eyes", "smileys" },
	{ 0x1F604, "grinning squinting", "smileys" },
	{ 0x1F605, "grinning sweat", "smileys" },
	{ 0x1F606, "squinting face", "smileys" },
	{ 0x1F609, "winking face", "smileys" },
	{ 0x1F60A, "smiling blush", "smileys" },
	{ 0x1F60B, "savoring face", "smileys" },
	{ 0x1F60E, "sunglasses", "smileys" },
	{ 0x1F60D, "heart eyes", "smileys" },
	{ 0x1F618, "kiss face", "smileys" },
	{ 0x1F61C, "winking tongue", "smileys" },
	{ 0x1F61D, "squinting tongue", "smileys" },
	{ 0x1F62D, "crying face", "smileys" },
	{ 0x1F631, "screaming", "smileys" },
	{ 0x1F637, "mask face", "smileys" },
	{ 0x1F914, "thinking face", "smileys" },
	{ 0x1F923, "rolling floor laughing", "smileys" },
	{ 0x1F970, "smiling hearts", "smileys" },
	{ 0x1F973, "party face", "smileys" },
	{ 0x1F976, "cold face", "smileys" },
	{ 0x1F975, "hot face", "smileys" },
	{ 0x1FAE0, "melting face", "smileys" },
	/* gestures */
	{ 0x1F44D, "thumbs up", "gestures" },
	{ 0x1F44E, "thumbs down", "gestures" },
	{ 0x1F44F, "clapping", "gestures" },
	{ 0x1F44B, "waving hand", "gestures" },
	{ 0x270C,  "victory hand", "gestures" },
	{ 0x1F91E, "crossed fingers", "gestures" },
	{ 0x1F918, "rock on", "gestures" },
	{ 0x1F4AA, "flexed bicep", "gestures" },
	{ 0x1F64F, "folded hands", "gestures" },
	{ 0x1F91D, "handshake", "gestures" },
	/* hearts */
	{ 0x2764,  "red heart", "hearts" },
	{ 0x1F494, "broken heart", "hearts" },
	{ 0x1F495, "two hearts", "hearts" },
	{ 0x1F496, "sparkling heart", "hearts" },
	{ 0x1F49A, "green heart", "hearts" },
	{ 0x1F499, "blue heart", "hearts" },
	{ 0x1F49C, "purple heart", "hearts" },
	{ 0x1F5A4, "black heart", "hearts" },
	/* animals */
	{ 0x1F436, "dog face", "animals" },
	{ 0x1F431, "cat face", "animals" },
	{ 0x1F42D, "mouse face", "animals" },
	{ 0x1F430, "rabbit face", "animals" },
	{ 0x1F43B, "bear face", "animals" },
	{ 0x1F428, "koala", "animals" },
	{ 0x1F98A, "fox", "animals" },
	{ 0x1F981, "lion", "animals" },
	{ 0x1F427, "penguin", "animals" },
	{ 0x1F40D, "snake", "animals" },
	{ 0x1F422, "turtle", "animals" },
	{ 0x1F41D, "bee", "animals" },
	{ 0x1F987, "bat", "animals" },
	/* food */
	{ 0x1F34E, "red apple", "food" },
	{ 0x1F34A, "tangerine", "food" },
	{ 0x1F34B, "lemon", "food" },
	{ 0x1F34C, "banana", "food" },
	{ 0x1F349, "watermelon", "food" },
	{ 0x1F353, "strawberry", "food" },
	{ 0x1F355, "pizza", "food" },
	{ 0x1F354, "hamburger", "food" },
	{ 0x1F37F, "popcorn", "food" },
	{ 0x2615,  "hot beverage", "food" },
	{ 0x1F37A, "beer mug", "food" },
	{ 0x1F382, "birthday cake", "food" },
	/* nature */
	{ 0x2B50,  "star", "nature" },
	{ 0x1F31F, "glowing star", "nature" },
	{ 0x2600,  "sun", "nature" },
	{ 0x1F319, "crescent moon", "nature" },
	{ 0x26C5,  "sun behind cloud", "nature" },
	{ 0x1F308, "rainbow", "nature" },
	{ 0x1F525, "fire", "nature" },
	{ 0x1F4A7, "droplet", "nature" },
	{ 0x2744,  "snowflake", "nature" },
	{ 0x26A1,  "lightning", "nature" },
	{ 0x1F33B, "sunflower", "nature" },
	{ 0x1F337, "tulip", "nature" },
	{ 0x1F340, "four leaf clover", "nature" },
	/* objects */
	{ 0x1F4BB, "laptop", "objects" },
	{ 0x1F4F1, "mobile phone", "objects" },
	{ 0x1F4E7, "email", "objects" },
	{ 0x1F4DA, "books", "objects" },
	{ 0x1F3B5, "musical note", "objects" },
	{ 0x1F3B6, "musical notes", "objects" },
	{ 0x1F50D, "magnifying glass", "objects" },
	{ 0x1F512, "lock", "objects" },
	{ 0x1F513, "unlock", "objects" },
	{ 0x2699,  "gear", "objects" },
	{ 0x1F6A9, "flag", "objects" },
	/* symbols */
	{ 0x2705,  "check mark", "symbols" },
	{ 0x274C,  "cross mark", "symbols" },
	{ 0x26A0,  "warning", "symbols" },
	{ 0x2049,  "exclamation question", "symbols" },
	{ 0x1F4A1, "light bulb", "symbols" },
	{ 0x1F4AC, "speech bubble", "symbols" },
	{ 0x1F4AD, "thought bubble", "symbols" },
	{ 0x267B,  "recycling", "symbols" },
	{ 0x1F3AF, "bullseye", "symbols" },
	{ 0x1F680, "rocket", "symbols" },
};

#define EMOJI_COUNT ((int)(sizeof(emoji_table) / sizeof(emoji_table[0])))

static const char *categories[] = {
	NULL,		/* "all" -- show everything */
	"smileys",
	"gestures",
	"hearts",
	"animals",
	"food",
	"nature",
	"objects",
	"symbols",
};

#define CAT_COUNT ((int)(sizeof(categories) / sizeof(categories[0])))

static struct {
	int cat_idx;		/* index into categories[], 0 = all */
	struct tui_list list;
	int filtered[256];	/* indices into emoji_table */
	int filtered_count;
} emo;

static void
emo_rebuild_filter(void)
{
	const char *cat = categories[emo.cat_idx];
	int i;

	emo.filtered_count = 0;
	for (i = 0; i < EMOJI_COUNT; i++) {
		if (!cat || strcmp(emoji_table[i].category, cat) == 0) {
			if (emo.filtered_count < 256)
				emo.filtered[emo.filtered_count++] = i;
		}
	}
	emo.list.count = emo.filtered_count;
	emo.list.sel = 0;
	emo.list.scroll = 0;
}

static void
emo_row_cb(struct tui_pad *p, int row, int col, int width,
    int index, int selected, const struct tui_theme *th, void *ctx)
{
	int *filtered = ctx;
	const struct emoji_entry *e = &emoji_table[filtered[index]];
	struct vt_color fg = selected ? th->sel_fg : th->content_fg;
	struct vt_color bg = selected ? th->sel_bg : th->content_bg;
	uint16_t attrs = selected ? VT_ATTR_BOLD : 0;
	unsigned char utf8[8];
	int len, c;

	(void)width;

	c = col;
	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* emoji character */
	len = utf8_encode(utf8, e->codepoint);
	if (len > 0) {
		utf8[len] = '\0';
		c = tui_pad_puts(p, row, c, (const char *)utf8,
		    fg, bg, attrs, TUI_OPAQUE);
	}

	tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);

	/* name */
	tui_pad_puts(p, row, c, e->name, fg, bg, attrs, TUI_OPAQUE);
	c += (int)strlen(e->name);

	/* pad rest */
	while (c < col + width)
		tui_pad_put(p, row, c++, ' ', fg, bg, attrs, TUI_OPAQUE);
}

static void
emo_draw(struct app_ctx *ctx)
{
	char title[64];
	const char *cat = categories[emo.cat_idx];

	if (cat)
		snprintf(title, sizeof(title), "Emoji [%s] Tab:next",
		    cat);
	else
		snprintf(title, sizeof(title), "Emoji [all] Tab:next");

	tui_list_draw(ctx->pad, &emo.list, ctx->theme,
	    emo_row_cb, emo.filtered, title,
	    ctx->screen_rows, ctx->screen_cols);
}

static void
emo_show(struct app_ctx *ctx)
{
	(void)ctx;
	emo.cat_idx = 0;
	memset(&emo.list, 0, sizeof(emo.list));
	emo_rebuild_filter();
}

static void
emo_hide(struct app_ctx *ctx)
{
	(void)ctx;
}

static void
emo_insert(struct app_ctx *ctx)
{
	const struct emoji_entry *e;
	unsigned char utf8[8];
	int len;

	if (emo.list.sel < 0 || emo.list.sel >= emo.filtered_count)
		return;

	e = &emoji_table[emo.filtered[emo.list.sel]];
	len = utf8_encode(utf8, e->codepoint);
	if (len > 0)
		ipc_msg_send(ctx->input_fd, IPC_MSG_INPUT,
		    utf8, (uint32_t)len);
}

static int
emo_input(struct app_ctx *ctx, const struct tkbd_seq *seq)
{
	switch (seq->key) {
	case TKBD_KEY_UP:
		if (emo.list.sel > 0)
			emo.list.sel--;
		else
			emo.list.sel = emo.filtered_count - 1;
		goto redraw;
	case TKBD_KEY_DOWN:
		if (emo.list.sel < emo.filtered_count - 1)
			emo.list.sel++;
		else
			emo.list.sel = 0;
		goto redraw;
	case TKBD_KEY_RIGHT:
		emo_insert(ctx);
		return 0; /* dismiss back */
	case TKBD_KEY_LEFT:
		return 0;
	case TKBD_KEY_ESC:
		return 0; /* dismiss */
	case TKBD_KEY_SPACE:
		emo_insert(ctx);
		return 1; /* space inserts but doesn't dismiss */
	case TKBD_KEY_ENTER:
		emo_insert(ctx);
		return 0; /* enter inserts AND dismisses */
	case TKBD_KEY_TAB:
		emo.cat_idx = (emo.cat_idx + 1) % CAT_COUNT;
		emo_rebuild_filter();
		goto redraw;
	default:
		break;
	}

	return 1;

redraw:
	emo_draw(ctx);
	ctx->render(ctx);
	return 1;
}

const struct app app_emoji = {
	.name = "Emoji",
	.key = "e",
	.show = emo_show,
	.hide = emo_hide,
	.draw = emo_draw,
	.input = emo_input,
};
