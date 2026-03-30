/* splash_mountain.c : mountain landscape splash scene (80x40, box-drawing) */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "splash_draw.h"

#define COLS	80
#define ROWS	40

/* ---- mountain drawing ---- */

struct peak {
	int	col;		/* peak column */
	int	height;		/* rows above base */
	int	spread;		/* half-width at base */
};

/*
 * Draw a mountain range.  Peaks are defined by position and height.
 * The range is drawn as ╱╲ slopes with filled interior.  draw order
 * is back-to-front, so later ranges naturally occlude earlier ones.
 */
static void
draw_range(struct vt_buf *buf, int base_row,
    const struct peak *peaks, int npeaks,
    struct vt_color slope_fg, struct vt_color fill_bg)
{
	int i, r, c;

	for (i = 0; i < npeaks; i++) {
		int pcol = peaks[i].col;
		int h = peaks[i].height;
		int sp = peaks[i].spread;
		int peak_row = base_row - h;

		/* fill interior (top to bottom) */
		for (r = peak_row; r <= base_row; r++) {
			int depth = r - peak_row;
			int left = pcol - depth * sp / h;
			int right = pcol + depth * sp / h;

			if (left < 0)
				left = 0;
			if (right >= COLS)
				right = COLS - 1;

			for (c = left; c <= right; c++)
				splash_put(buf, r, c, ' ',
				    CLR_DEF, fill_bg, 0);
		}

		/* left slope (╱ U+2571) */
		for (r = peak_row; r <= base_row; r++) {
			int depth = r - peak_row;
			int lc = pcol - depth * sp / h;

			if (lc >= 0 && lc < COLS)
				splash_put(buf, r, lc, 0x2571,
				    slope_fg, fill_bg, 0);
		}

		/* right slope (╲ U+2572) */
		for (r = peak_row; r <= base_row; r++) {
			int depth = r - peak_row;
			int rc = pcol + depth * sp / h;

			if (rc >= 0 && rc < COLS)
				splash_put(buf, r, rc, 0x2572,
				    slope_fg, fill_bg, 0);
		}

		/* peak cap (╱╲ or ╲╱ junction) */
		splash_put_fg(buf, peak_row, pcol, 0x25B3,
		    slope_fg, 0);
	}
}

/* ---- treeline ---- */

static void
draw_treeline(struct vt_buf *buf, int row, int cols, uint32_t *seed)
{
	int c;
	static const uint32_t tree_chars[] = {
		0x2593, 0x2588, 0x2593, 0x2592,	/* ▓█▓▒ */
	};
	static const uint8_t greens[] = { 22, 28, 34, 22, 28 };

	for (c = 0; c < cols; c++) {
		int h, r;

		h = 1 + (int)(splash_rand(seed) % 3);	/* 1-3 rows tall */
		for (r = 0; r < h; r++) {
			uint32_t ch;
			struct vt_color fg;

			ch = tree_chars[splash_rand(seed) % 4];
			fg = CLR_IDX(greens[splash_rand(seed) % 5]);
			splash_put(buf, row - r, c, ch,
			    fg, CLR_IDX(22), 0);
		}
	}
}

/* ---- ground texture ---- */

static void
draw_ground(struct vt_buf *buf, int top, int rows, int cols, uint32_t *seed)
{
	int r, c;
	static const uint32_t tex[] = { 0x2591, 0x2592, 0x2593 };
	static const uint8_t earth[] = { 58, 94, 100, 130, 136 };

	for (r = top; r < rows; r++) {
		for (c = 0; c < cols; c++) {
			uint32_t ch;
			struct vt_color fg;
			uint8_t bg_idx;

			ch = tex[splash_rand(seed) % 3];
			fg = CLR_IDX(earth[splash_rand(seed) % 5]);
			bg_idx = (r < top + 2) ? 22 : 58;
			splash_put(buf, r, c, ch, fg,
			    CLR_IDX(bg_idx), 0);
		}
	}
}

/* ---- sky gradient ---- */

static void
draw_sky(struct vt_buf *buf, int rows, int cols)
{
	int r, c;

	for (r = 0; r < rows; r++) {
		/* dark blue to lighter blue gradient */
		uint8_t blue;

		blue = (uint8_t)(17 + r / 3);
		if (blue > 24)
			blue = 24;
		for (c = 0; c < cols; c++)
			splash_put(buf, r, c, ' ',
			    CLR_DEF, CLR_IDX(blue), 0);
	}
}

/* ---- clouds ---- */

static void
draw_clouds(struct vt_buf *buf, uint32_t *seed)
{
	int i;

	for (i = 0; i < 8; i++) {
		int r, c, w, j;

		r = 1 + (int)(splash_rand(seed) % 6);
		c = (int)(splash_rand(seed) % (unsigned)(COLS - 15));
		w = 5 + (int)(splash_rand(seed) % 10);

		for (j = 0; j < w; j++) {
			uint32_t ch;

			ch = (splash_rand(seed) % 3 == 0) ? 0x2592 : 0x2591;
			splash_put_fg(buf, r, c + j, ch,
			    CLR_IDX(252), VT_ATTR_DIM);
		}
	}
}

/* ---- scene entry point ---- */

void
splash_scene_mountain(struct vt_buf *buf, const char *name)
{
	uint32_t seed;
	int logo_w, logo_h, logo_row, logo_col;

	/* far mountain range */
	static const struct peak far_peaks[] = {
		{ 12, 12, 10 },
		{ 32, 15, 12 },
		{ 55, 18, 14 },
		{ 72, 10,  8 },
	};
	/* mid mountain range */
	static const struct peak mid_peaks[] = {
		{  5, 8,  7 },
		{ 25, 10, 9 },
		{ 48, 12, 10 },
		{ 68, 9,  8 },
	};
	/* near range */
	static const struct peak near_peaks[] = {
		{ 15, 5, 8 },
		{ 40, 7, 10 },
		{ 62, 4, 7 },
	};

	seed = 0xCAFEBABEu;

	/* sky gradient background */
	draw_sky(buf, 28, COLS);

	/* clouds */
	draw_clouds(buf, &seed);

	/* ground below mountains */
	draw_ground(buf, 28, ROWS, COLS, &seed);

	/* mountain ranges, back to front */
	draw_range(buf, 27, far_peaks, 4,
	    CLR_IDX(248), CLR_IDX(238));

	draw_range(buf, 27, mid_peaks, 4,
	    CLR_IDX(244), CLR_IDX(236));

	draw_range(buf, 28, near_peaks, 3,
	    CLR_IDX(34), CLR_IDX(22));

	/* treeline at base of near range */
	draw_treeline(buf, 28, COLS, &seed);

	/* logo */
	logo_w = splash_logo_width(name);
	logo_h = splash_logo_height(name);
	logo_row = ROWS - logo_h - 1;
	logo_col = COLS - logo_w - 3;
	if (logo_col < 2)
		logo_col = 2;

	splash_draw_logo(buf, logo_row, logo_col, name, CLR_IDX(255));
}
