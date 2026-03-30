/* splash_beach.c : tropical beach splash scene (120x65, braille art) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash_draw.h"

#include <string.h>

#define COLS	120
#define ROWS	65

/* braille pixel buffer: each cell = 2 cols x 4 rows of dots */
#define PX_W	(COLS * 2)	/* 240 */
#define PX_H	(ROWS * 4)	/* 260 */

/* layer identifiers for the pixel buffer */
#define L_EMPTY		0
#define L_SUN		1
#define L_CLOUD		2
#define L_TRUNK		3
#define L_FROND		4
#define L_OCEAN		5
#define L_SAND		6
#define L_BIRD		7

static uint8_t px[PX_H][PX_W];

/* ---- pixel-level drawing primitives ---- */

static void
px_set(int y, int x, uint8_t layer)
{
	if (y >= 0 && y < PX_H && x >= 0 && x < PX_W)
		px[y][x] = layer;
}

static void
px_circle(int cy, int cx, int r, uint8_t layer)
{
	int y, x;

	for (y = cy - r; y <= cy + r; y++) {
		for (x = cx - r; x <= cx + r; x++) {
			int dy = y - cy, dx = x - cx;

			if (dx * dx + dy * dy <= r * r)
				px_set(y, x, layer);
		}
	}
}

/* Thick line from (x0,y0) to (x1,y1) with radius th. */
static void
px_line(int x0, int y0, int x1, int y1, int th, uint8_t layer)
{
	int dx, dy, steps, i;

	dx = x1 - x0;
	dy = y1 - y0;
	steps = (dx > 0 ? dx : -dx);
	if ((dy > 0 ? dy : -dy) > steps)
		steps = (dy > 0 ? dy : -dy);
	if (steps == 0)
		steps = 1;

	for (i = 0; i <= steps; i++) {
		int x = x0 + dx * i / steps;
		int y = y0 + dy * i / steps;
		int ty, tx;

		for (ty = -th; ty <= th; ty++)
			for (tx = -th; tx <= th; tx++)
				if (tx * tx + ty * ty <= th * th)
					px_set(y + ty, x + tx, layer);
	}
}

/*
 * Quadratic bezier curve from (x0,y0) through control (x1,y1)
 * to (x2,y2) with thickness th.
 */
static void
px_bezier(int x0, int y0, int x1, int y1, int x2, int y2,
    int th, uint8_t layer)
{
	int steps, i;

	steps = 120;
	for (i = 0; i <= steps; i++) {
		int t = i, s = steps - i;
		int x, y;

		/* B(t) = (1-t)^2 P0 + 2(1-t)t P1 + t^2 P2 */
		x = (s * s * x0 + 2 * s * t * x1 + t * t * x2)
		    / (steps * steps);
		y = (s * s * y0 + 2 * s * t * y1 + t * t * y2)
		    / (steps * steps);

		/* taper thickness towards end */
		{
			int r = th - th * i / (steps * 2);
			int ty, tx;

			if (r < 0)
				r = 0;
			for (ty = -r; ty <= r; ty++)
				for (tx = -r; tx <= r; tx++)
					if (tx * tx + ty * ty <= r * r)
						px_set(y + ty, x + tx, layer);
		}
	}
}

/* Droopy line: straight line with quadratic y droop. */
static void
px_droop_line(int x0, int y0, int x1, int y1, int droop,
    int th, uint8_t layer)
{
	int steps, i;

	steps = 80;
	for (i = 0; i <= steps; i++) {
		int x, y, r;

		x = x0 + (x1 - x0) * i / steps;
		y = y0 + (y1 - y0) * i / steps
		    + droop * i * i / (steps * steps);

		/* taper */
		r = th - th * i / (steps + steps / 2);
		if (r < 0)
			r = 0;
		{
			int ty, tx;

			for (ty = -r; ty <= r; ty++)
				for (tx = -r; tx <= r; tx++)
					if (tx * tx + ty * ty <= r * r)
						px_set(y + ty, x + tx, layer);
		}
	}
}

/* ---- scene elements ---- */

static void
draw_sun(int cy, int cx, int r)
{
	int i;

	px_circle(cy, cx, r, L_SUN);

	/* rays */
	for (i = 0; i < 8; i++) {
		/* 8 evenly spaced ray directions using integer approximations */
		static const int dx8[] = { 0,  5,  7,  5,  0, -5, -7, -5 };
		static const int dy8[] = { -7, -5, 0,  5,  7,  5,  0, -5 };
		int x1 = cx + dx8[i] * (r + 8) / 7;
		int y1 = cy + dy8[i] * (r + 8) / 7;
		int x2 = cx + dx8[i] * (r + 18) / 7;
		int y2 = cy + dy8[i] * (r + 18) / 7;

		px_line(x1, y1, x2, y2, 1, L_SUN);
	}
}

static void
draw_palm(int base_x, int base_y, int top_x, int top_y)
{
	int ctrl_x, ctrl_y;
	int frond;
	/* 8 frond directions (dx, dy, droop) */
	static const int fronds[][3] = {
		{ -50, -20, 35 },
		{ -40, -10, 30 },
		{ -30,   5, 25 },
		{ -15,  15, 20 },
		{  50, -20, 35 },
		{  40, -10, 30 },
		{  30,   5, 25 },
		{  15,  15, 20 },
	};

	/* trunk: bezier with a slight bend */
	ctrl_x = base_x + (top_x - base_x) / 2 + 12;
	ctrl_y = base_y + (top_y - base_y) / 2;
	px_bezier(base_x, base_y, ctrl_x, ctrl_y, top_x, top_y,
	    4, L_TRUNK);

	/* fronds radiating from top */
	for (frond = 0; frond < 8; frond++) {
		int fx = top_x + fronds[frond][0];
		int fy = top_y + fronds[frond][1];
		int dr = fronds[frond][2];

		px_droop_line(top_x, top_y, fx, fy, dr, 2, L_FROND);
	}
}

static void
draw_ocean(int top, int bottom, uint32_t *seed)
{
	int y, x;
	/* precomputed wave offsets (sine-ish) */
	static const int wave[] = {
		0, 0, 1, 1, 2, 2, 2, 1, 1, 0, 0, -1,
		-1, -2, -2, -2, -1, -1,
	};
	int wave_len = 18;

	for (y = top; y < bottom && y < PX_H; y++) {
		int row_wave;

		/* secondary horizontal wave */
		row_wave = wave[((y - top) / 3) % wave_len];

		for (x = 0; x < PX_W; x++) {
			int edge;

			edge = top + wave[(x + row_wave * 2) % wave_len] + 2;
			if (y >= edge) {
				/* sparse fill near top for foam effect */
				if (y < edge + 4) {
					if (splash_rand(seed) % 3 != 0)
						px_set(y, x, L_OCEAN);
				} else {
					px_set(y, x, L_OCEAN);
				}
			}
		}
	}
}

static void
draw_sand(int top, int bottom, uint32_t *seed)
{
	int y, x;

	for (y = top; y < bottom && y < PX_H; y++) {
		for (x = 0; x < PX_W; x++) {
			/* dense fill with some gaps for texture */
			if (splash_rand(seed) % 5 != 0)
				px_set(y, x, L_SAND);
		}
	}
}

static void
draw_birds(uint32_t *seed)
{
	int i;

	for (i = 0; i < 5; i++) {
		int cx, cy, span;

		cx = 30 + (int)(splash_rand(seed) % 160);
		cy = 10 + (int)(splash_rand(seed) % 40);
		span = 3 + (int)(splash_rand(seed) % 4);

		/* simple V shape */
		px_line(cx - span, cy + span / 2, cx, cy, 0, L_BIRD);
		px_line(cx, cy, cx + span, cy + span / 2, 0, L_BIRD);
	}
}

static void
draw_clouds_braille(uint32_t *seed)
{
	int i;

	for (i = 0; i < 6; i++) {
		int cx, cy, w, h, y, x;

		cx = 10 + (int)(splash_rand(seed) % 200);
		cy = 15 + (int)(splash_rand(seed) % 30);
		w = 15 + (int)(splash_rand(seed) % 20);
		h = 4 + (int)(splash_rand(seed) % 4);

		/* elliptical cloud */
		for (y = cy - h; y <= cy + h; y++) {
			for (x = cx - w; x <= cx + w; x++) {
				int dy = y - cy, dx = x - cx;
				int d;

				/* ellipse: (dx/w)^2 + (dy/h)^2 <= 1 */
				d = dx * dx * h * h + dy * dy * w * w;
				if (d <= w * w * h * h) {
					if (splash_rand(seed) % 4 != 0)
						px_set(y, x, L_CLOUD);
				}
			}
		}
	}
}

/* ---- braille conversion ---- */

/* Map layer to foreground color. */
static struct vt_color
layer_fg(uint8_t layer)
{
	switch (layer) {
	case L_SUN:	return CLR_RGB(255, 220, 50);
	case L_CLOUD:	return CLR_RGB(220, 220, 230);
	case L_TRUNK:	return CLR_RGB(139, 90, 43);
	case L_FROND:	return CLR_RGB(34, 180, 34);
	case L_OCEAN:	return CLR_RGB(30, 144, 255);
	case L_SAND:	return CLR_RGB(238, 214, 175);
	case L_BIRD:	return CLR_RGB(40, 40, 50);
	default:	return CLR_DEF;
	}
}

/* Sky gradient based on cell row. */
static struct vt_color
sky_bg(int row)
{
	int r, g, b;

	if (row < 40) {
		/* sky: dark navy to lighter blue */
		r = 8 + row;
		g = 12 + row * 2;
		b = 60 + row * 2;
	} else if (row < 50) {
		/* ocean area: deeper blue */
		r = 5;
		g = 20 + (row - 40) * 2;
		b = 80 + (row - 40) * 3;
	} else {
		/* sand area: warm dark */
		r = 80 + (row - 50) * 3;
		g = 60 + (row - 50) * 2;
		b = 30;
	}

	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;

	return CLR_RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/*
 * Find the dominant non-empty layer in a 4x2 pixel block.
 * Returns 0 (L_EMPTY) if no pixels are set.
 */
static uint8_t
dominant_layer(int cell_row, int cell_col)
{
	int counts[8] = { 0 };
	int r, c;
	int py, px_c;
	uint8_t best;
	int best_count;

	py = cell_row * 4;
	px_c = cell_col * 2;

	for (r = 0; r < 4; r++) {
		for (c = 0; c < 2; c++) {
			int y = py + r, x = px_c + c;

			if (y < PX_H && x < PX_W && px[y][x])
				counts[px[y][x]]++;
		}
	}

	best = 0;
	best_count = 0;
	for (r = 1; r < 8; r++) {
		if (counts[r] > best_count) {
			best = (uint8_t)r;
			best_count = counts[r];
		}
	}
	return best;
}

/*
 * Convert the pixel buffer into braille characters in the vt_buf.
 * Each cell (2 cols x 4 rows of dots) maps to one braille codepoint.
 *
 * Braille dot layout:
 *   dot1(0x01)  dot4(0x08)
 *   dot2(0x02)  dot5(0x10)
 *   dot3(0x04)  dot6(0x20)
 *   dot7(0x40)  dot8(0x80)
 */
static void
pixels_to_cells(struct vt_buf *buf)
{
	int row, col;

	for (row = 0; row < ROWS; row++) {
		for (col = 0; col < COLS; col++) {
			int py = row * 4;
			int pc = col * 2;
			uint32_t cp;
			uint8_t layer;
			struct vt_color fg, bg;

			/* build braille pattern */
			cp = 0x2800;
			if (py     < PX_H && pc     < PX_W && px[py][pc])
				cp |= 0x01;
			if (py + 1 < PX_H && pc     < PX_W && px[py + 1][pc])
				cp |= 0x02;
			if (py + 2 < PX_H && pc     < PX_W && px[py + 2][pc])
				cp |= 0x04;
			if (py + 3 < PX_H && pc     < PX_W && px[py + 3][pc])
				cp |= 0x40;
			if (py     < PX_H && pc + 1 < PX_W && px[py][pc + 1])
				cp |= 0x08;
			if (py + 1 < PX_H && pc + 1 < PX_W && px[py + 1][pc + 1])
				cp |= 0x10;
			if (py + 2 < PX_H && pc + 1 < PX_W && px[py + 2][pc + 1])
				cp |= 0x20;
			if (py + 3 < PX_H && pc + 1 < PX_W && px[py + 3][pc + 1])
				cp |= 0x80;

			layer = dominant_layer(row, col);
			fg = (layer != L_EMPTY) ? layer_fg(layer) : CLR_DEF;
			bg = sky_bg(row);

			splash_put(buf, row, col, cp, fg, bg, 0);
		}
	}
}

/* ---- scene entry point ---- */

void
splash_scene_beach(struct vt_buf *buf, const char *name)
{
	uint32_t seed;
	int logo_w, logo_h, logo_row, logo_col;

	seed = 0xB3AC4001u;
	memset(px, 0, sizeof(px));

	/* draw elements into pixel buffer (back to front) */

	/* sun: upper right area */
	draw_sun(28, 170, 16);

	/* clouds */
	draw_clouds_braille(&seed);

	/* birds */
	draw_birds(&seed);

	/* palm trees */
	draw_palm(60, 200, 40, 55);	/* left tree */
	draw_palm(95, 210, 110, 60);	/* right tree */
	draw_palm(150, 220, 160, 70);	/* far right tree */

	/* ocean band (pixel rows 160-205) */
	draw_ocean(160, 205, &seed);

	/* sand band (pixel rows 200-250) */
	draw_sand(200, 250, &seed);

	/* convert pixel buffer to braille cells */
	pixels_to_cells(buf);

	/* logo (pipe-art, overlaid on the braille scene) */
	logo_w = splash_logo_width(name);
	logo_h = splash_logo_height(name);
	logo_row = ROWS - logo_h - 2;
	logo_col = COLS - logo_w - 4;
	if (logo_col < 2)
		logo_col = 2;

	splash_draw_logo(buf, logo_row, logo_col, name, CLR_RGB(14, 230, 230));
}
