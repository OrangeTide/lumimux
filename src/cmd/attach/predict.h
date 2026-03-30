/* predict.h : speculative local echo for low-latency typing */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef PREDICT_H
#define PREDICT_H

#include <stddef.h>
#include <stdint.h>

struct vt_state;
struct predict;

struct predict *predict_new(void);
void predict_free(struct predict *pr);

/* record a predicted keystroke.  writes the character into the vt_state
 * grid at the cursor position with VT_ATTR_PREDICTED set, then advances
 * the cursor.  only predicts printable characters in normal mode
 * (no alt screen, cursor visible, autowrap on).  returns 1 if a
 * prediction was placed, 0 if skipped. */
int predict_key(struct predict *pr, struct vt_state *vt,
    uint32_t cp, int width);

/* called before feeding server output through the parser.
 * confirms predictions that match incoming characters, rolls back
 * any that don't.  call this before vt_parse_feed(). */
void predict_confirm(struct predict *pr, struct vt_state *vt,
    const char *data, size_t len);

/* discard all pending predictions and clear predicted cells.
 * called on resize, mode change, or other invalidating events. */
void predict_reset(struct predict *pr, struct vt_state *vt);

/* tell the predictor whether the PTY has echo enabled.
 * when echo is off (e.g. password prompts), predictions are
 * suppressed to avoid revealing hidden input. */
void predict_set_echo(struct predict *pr, int on);

/* return the number of pending (unconfirmed) predictions. */
int predict_pending(const struct predict *pr);

#endif /* PREDICT_H */
