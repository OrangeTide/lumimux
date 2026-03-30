/* vt_ops.h : connect VT parser to terminal state */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef VT_OPS_H
#define VT_OPS_H

#include "vt_parse.h"
#include "vt_state.h"

/* return a vt_ops vtable that drives a vt_state.
 * pass the vt_state pointer as ctx to vt_parse_new(). */
const struct vt_ops *vt_ops_default(void);

#endif /* VT_OPS_H */
