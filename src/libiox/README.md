# libiox

Poll-based I/O multiplexer. Provides file descriptor watching with
read/write event flags, one-shot timers backed by a priority queue,
signal handling via a self-pipe, and an idle callback for periodic
work between poll iterations.

## API

### Loop

- `iox_loop_new()` / `iox_loop_free()` -- create/destroy.
- `iox_loop_run()` -- run until `iox_loop_stop()` is called.
- `iox_loop_poll(loop)` -- single poll iteration. Blocks until an fd
  is ready or the next timer expires. Returns number of events
  dispatched, -1 on error.
- `iox_loop_set_idle(loop, cb, arg)` -- called between iterations
  in `iox_loop_run`.

### File descriptor watchers

- `iox_fd_add(loop, fd, events, cb, arg)` -- watch fd for
  `IOX_READ`, `IOX_WRITE`, or both.
- `iox_fd_mod(loop, fd, events)` -- change watched events.
- `iox_fd_remove(loop, fd)` -- safe to call during dispatch.

### Timers

- `iox_timer_add(loop, ms, cb, arg)` -- schedule a one-shot timer.
  Returns a timer ID (>= 0) on success.
- `iox_timer_remove(loop, id)` -- cancel a pending timer.

Timers are one-shot: they fire once and are automatically removed.
Re-add from the callback for repeating behavior. Backed by a min-heap
priority queue keyed on `CLOCK_MONOTONIC` deadlines.

### Signals

- `iox_signal_add(loop, signo, cb, arg)` -- register handler.
- `iox_signal_remove(loop, signo)` -- restore previous handler.

Signal delivery uses a self-pipe so callbacks run inside the normal
dispatch loop, not in signal context.

## Design notes

Removal during dispatch is safe: entries are marked inactive and
compacted after the dispatch pass completes. The poll() backend is
portable; the structure allows swapping in epoll on Linux later.
