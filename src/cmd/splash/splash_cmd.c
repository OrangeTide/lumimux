/* splash_cmd.c : display ANSI art splash screen */

#include "splash.h"
#include "multicall.h"

#include "draw.h"
#include "draw_term.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *progname = "lumi-splash";

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [space|mountain|beach] [name]\n"
	    "\n"
	    "Display an ANSI art splash screen.\n"
	    "Press any key to dismiss.\n",
	    progname);
}

/* Copy the splash canvas into the surface, cropped bottom-right so the logo
 * stays visible and overflow scenery is trimmed from the top-left. */
static void
blit_splash(struct draw *d, struct vt_buf *canvas, int rows, int cols)
{
	int canvas_rows = vt_buf_rows(canvas);
	int canvas_cols = vt_buf_cols(canvas);
	int row_off = canvas_rows > rows ? canvas_rows - rows : 0;
	int col_off = canvas_cols > cols ? canvas_cols - cols : 0;
	int vis_rows = canvas_rows - row_off;
	int vis_cols = canvas_cols - col_off;
	int r, c;

	if (vis_rows > rows)
		vis_rows = rows;
	if (vis_cols > cols)
		vis_cols = cols;

	draw_clear(d);
	for (r = 0; r < vis_rows; r++) {
		for (c = 0; c < vis_cols; c++) {
			const struct vt_cell *src =
			    vt_buf_cell(canvas, r + row_off, c + col_off);

			if (src)
				draw_cell(d, r, c, src->codepoint, src->fg,
				    src->bg, src->attrs);
		}
	}
}

int
cmd_splash_main(int argc, char **argv)
{
	enum splash_scene scene;
	const char *name;
	struct vt_buf *buf;
	struct draw_term *term;
	struct draw *d;
	int rows, cols;

	if (argv[0])
		progname = argv[0];

	scene = SPLASH_SPACE;
	name = "lumiMUX";

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			usage();
			return 0;
		} else if (strcmp(argv[1], "space") == 0) {
			scene = SPLASH_SPACE;
		} else if (strcmp(argv[1], "mountain") == 0) {
			scene = SPLASH_MOUNTAIN;
		} else if (strcmp(argv[1], "beach") == 0) {
			scene = SPLASH_BEACH;
		} else {
			fprintf(stderr, "%s: unknown scene '%s'\n",
			    progname, argv[1]);
			usage();
			return 1;
		}
	}

	if (argc > 2)
		name = argv[2];

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "%s: not a terminal\n", progname);
		return 1;
	}

	buf = splash_create(scene, name);
	if (!buf) {
		fprintf(stderr, "%s: splash_create failed\n", progname);
		return 1;
	}

	term = draw_term_new(STDIN_FILENO, STDOUT_FILENO, NULL);
	if (!term) {
		fprintf(stderr, "%s: cannot open the terminal\n", progname);
		splash_free(buf);
		return 1;
	}
	d = draw_new(draw_term_driver(), term);
	if (!d) {
		fprintf(stderr, "%s: out of memory\n", progname);
		splash_free(buf);
		return 1;
	}

	draw_begin(d);
	for (;;) {
		struct draw_event ev;

		draw_size(d, &rows, &cols);
		draw_cursor_vis(d, 0);
		blit_splash(d, buf, rows, cols);
		draw_present(d);

		switch (draw_wait(d, &ev)) {
		case DRAW_EVENT_KEY:
			if (ev.key.type == TKBD_KEY)
				goto done;	/* any key dismisses */
			break;
		case DRAW_EVENT_EOF:
			goto done;
		case DRAW_EVENT_RESIZE:
		case DRAW_EVENT_RESUME:
			break;			/* loop re-blits at the new size */
		default:
			break;
		}
	}
done:
	draw_end(d);
	draw_free(d);			/* also frees the terminal driver */
	splash_free(buf);
	return 0;
}
