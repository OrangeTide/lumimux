/* draw_term.h : terminal backend for the draw surface */

#ifndef DRAW_TERM_H
#define DRAW_TERM_H

struct draw_driver;
struct draw_term;

/*
 * The terminal driver renders a draw surface to a real terminal. It wraps the
 * existing libraries: libtio for raw mode and buffered output, libtxl for
 * capability strings, librender for the differential renderer, and libtermlib
 * for input decoding. Pair it with the surface like this:
 *
 *     struct draw_term *t = draw_term_new(STDIN_FILENO, STDOUT_FILENO, NULL);
 *     struct draw *d = draw_new(draw_term_driver(), t);
 *     ...
 *     draw_free(d);   // frees the surface and, through the driver, the term
 *
 * term is the terminal type; NULL uses $TERM. Returns NULL on failure.
 */
struct draw_term *draw_term_new(int in_fd, int out_fd, const char *term);

/* The driver vtable to pass to draw_new alongside a draw_term pointer. */
const struct draw_driver *draw_term_driver(void);

/* Enable or disable mouse reporting. Off by default, so an app that ignores
 * the mouse leaves the terminal's own selection working. Call before
 * draw_begin. */
void draw_term_mouse(struct draw_term *t, int on);

#endif /* DRAW_TERM_H */
