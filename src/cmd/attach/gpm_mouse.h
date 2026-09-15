/* gpm_mouse.h : optional GPM (Linux text console) mouse input */

#ifndef GPM_MOUSE_H
#define GPM_MOUSE_H

struct iox_loop;
struct tkbd_seq;

/*
 * Callback used to inject a synthesized mouse sequence into the client.
 * The attach client passes its own input dispatcher so GPM events flow
 * through the same path as xterm-style terminal mouse sequences.
 */
typedef void (*gpm_mouse_dispatch_fn)(struct iox_loop *loop,
    const struct tkbd_seq *seq);

/*
 * Connect to the GPM daemon and register its socket on the event loop.
 *
 * Only attempts a connection when running on a real Linux text console
 * (TERM=linux), where the terminal itself reports no mouse events. In a
 * terminal emulator the existing xterm mouse path is used instead and this
 * is a no-op.
 *
 * Returns 0 when a GPM connection was established, -1 otherwise (not a
 * console, no daemon, or the binary was built without GPM support).
 */
int gpm_mouse_init(struct iox_loop *loop, gpm_mouse_dispatch_fn dispatch);

/* Disconnect from the GPM daemon. Safe to call when not connected. */
void gpm_mouse_shutdown(struct iox_loop *loop);

#endif /* GPM_MOUSE_H */
