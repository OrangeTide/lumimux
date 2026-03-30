/* splash_space.c : planetary / space splash scene (80x40, box-drawing) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash_draw.h"

#define COLS	80
#define ROWS	40

/* ---- star characters ---- */

static const uint32_t star_chars[] = {
	0x00B7,		/* · */
	'*',
	'.',
	0x2727,		/* ✧ */
	0x2726,		/* ✦ */
	'+',
};
#define NSTAR_CHARS	(sizeof(star_chars) / sizeof(star_chars[0]))

static const struct vt_color star_colors[] = {
	{ .type = VT_COLOR_INDEXED, .index = 255 },	/* bright white */
	{ .type = VT_COLOR_INDEXED, .index = 252 },	/* light gray */
	{ .type = VT_COLOR_INDEXED, .index = 11 },	/* yellow */
	{ .type = VT_COLOR_INDEXED, .index = 248 },	/* gray */
	{ .type = VT_COLOR_INDEXED, .index = 8 },	/* dim gray */
};
#define NSTAR_COLORS	(sizeof(star_colors) / sizeof(star_colors[0]))

/* ---- star field ---- */

static void
draw_stars(struct vt_buf *buf, int rows, int cols, uint32_t *seed)
{
	int i;

	for (i = 0; i < 120; i++) {
		int r = (int)(splash_rand(seed) % (unsigned)rows);
		int c = (int)(splash_rand(seed) % (unsigned)cols);
		uint32_t ch = star_chars[splash_rand(seed) % NSTAR_CHARS];
		struct vt_color fg = star_colors[splash_rand(seed) % NSTAR_COLORS];
		uint16_t attr = (splash_rand(seed) % 3 == 0) ? VT_ATTR_BOLD : 0;

		splash_put(buf, r, c, ch, fg, CLR_DEF, attr);
	}
}

/* ---- planet ---- */

static void
draw_planet(struct vt_buf *buf, int cy, int cx, int radius)
{
	int r, c;

	for (r = cy - radius; r <= cy + radius; r++) {
		for (c = cx - radius * 2; c <= cx + radius * 2; c++) {
			int dy, dx_half, dist4, r4;
			int shade;
			uint32_t cp;
			struct vt_color fg;

			dy = r - cy;
			dx_half = c - cx;
			/* aspect-corrected: dist^2 = (dx/2)^2 + dy^2 */
			dist4 = dx_half * dx_half + 4 * dy * dy;
			r4 = 4 * radius * radius;

			if (dist4 > r4)
				continue;

			/* shade 0..3 based on horizontal position */
			shade = (dx_half + radius * 2) * 4 / (radius * 4 + 1);
			if (shade < 0)
				shade = 0;
			if (shade > 3)
				shade = 3;

			switch (shade) {
			case 0: cp = 0x2591; break;	/* ░ */
			case 1: cp = 0x2592; break;	/* ▒ */
			case 2: cp = 0x2593; break;	/* ▓ */
			default: cp = 0x2588; break;	/* █ */
			}

			/* blue gradient: indexed 17 (dark) to 21 (bright) */
			fg = CLR_IDX((uint8_t)(17 + shade));

			splash_put(buf, r, c, cp, fg, CLR_IDX(0), 0);
		}
	}

	/* ring / atmosphere highlight on the lit edge */
	for (r = cy - radius - 1; r <= cy + radius + 1; r++) {
		int dy, edge_c, dist4, r4;

		dy = r - cy;
		if (4 * dy * dy > 4 * radius * radius)
			continue;

		/* right edge of planet */
		for (edge_c = cx + radius * 2; edge_c >= cx; edge_c--) {
			int dx_half = edge_c - cx;

			dist4 = dx_half * dx_half + 4 * dy * dy;
			r4 = 4 * radius * radius;
			if (dist4 <= r4 && dist4 > r4 - radius * 4) {
				splash_put(buf, r, edge_c + 1, 0x00B7,
				    CLR_IDX(75), CLR_DEF, VT_ATTR_BOLD);
				break;
			}
		}
	}
}

/* ---- lunar surface ---- */

static void
draw_surface(struct vt_buf *buf, int horizon, int cols, uint32_t *seed)
{
	int r, c;
	static const uint32_t tex[] = { 0x2591, 0x2592, 0x2593 };
	static const uint8_t browns[] = { 94, 130, 136, 172, 95, 131 };

	/* horizon line */
	for (c = 0; c < cols; c++)
		splash_put(buf, horizon, c, 0x2500, CLR_IDX(240), CLR_DEF, 0);

	/* textured ground below */
	for (r = horizon + 1; r < ROWS; r++) {
		for (c = 0; c < cols; c++) {
			uint32_t ch;
			struct vt_color fg;

			ch = tex[splash_rand(seed) % 3];
			fg = CLR_IDX(browns[splash_rand(seed) % 6]);
			splash_put(buf, r, c, ch, fg,
			    CLR_IDX(52), 0);
		}
	}
}

/* ---- scene entry point ---- */

void
splash_scene_space(struct vt_buf *buf, const char *name)
{
	uint32_t seed;
	int logo_w, logo_h, logo_row, logo_col;

	seed = 0xDEADBEEFu;

	/* dark background */
	splash_fill(buf, 0, 0, ROWS, COLS, ' ',
	    CLR_DEF, CLR_IDX(0), 0);

	/* star field */
	draw_stars(buf, 28, COLS, &seed);

	/* planet -- upper right quadrant */
	draw_planet(buf, 11, 52, 6);

	/* lunar surface starting at row 28 */
	draw_surface(buf, 28, COLS, &seed);

	/* logo in the bottom-right area */
	logo_w = splash_logo_width(name);
	logo_h = splash_logo_height(name);
	logo_row = ROWS - logo_h - 1;
	logo_col = COLS - logo_w - 3;
	if (logo_col < 2)
		logo_col = 2;

	splash_draw_logo(buf, logo_row, logo_col, name, CLR_IDX(14));
}
