# Phase 11: Advanced Features -- Implementation Plan

## Overview

Phase 11 adds four features of increasing complexity:
1. State-dependent key bindings
2. SIXEL pass-through
3. QUIC networked connections
4. Speculative local echo

Each is independent and can be implemented and shipped in any order.
Recommended order: simplest first, most complex last.

---

## 11A: State-Dependent Key Bindings

**Goal:** Bindings that activate conditionally based on window title regex
or toggle state.

### Design

Binding layers replace the single flat `bindings[256]` array. Each layer has:
- A name (e.g. `"default"`, `"vi"`, `"bash"`)
- An optional match predicate: regex on window title, or a named toggle state
- A `bindings[256]` array (sparse -- KEYS_ACTION_NONE means "fall through")

Lookup walks layers top-to-bottom, first non-NONE match wins.

```c
struct keys_layer {
    char            name[32];
    regex_t         *title_re;     /* NULL = always active */
    char            toggle[32];    /* "" = no toggle, else named state */
    int             toggle_active; /* only meaningful if toggle[0] != 0 */
    enum keys_action bindings[256];
};
```

**API additions:**
- `keys_set_title(k, title)` -- attach client calls on focus change / title update
- `keys_toggle(k, name)` -- flip a named toggle (bound to KEYS_ACTION_TOGGLE)
- `keys_feed()` signature unchanged -- reads context internally

**Config format:**
```ini
[bind]
c = new-window          # default layer, always active

[bind "vi"]
match-title = ^vi.*
j = scroll-down         # active only when title matches ^vi

[bind "logging"]
toggle = logging        # active only when "logging" toggle is on
l = toggle              # pressing 'l' flips the toggle off
```

### Files to Change

| File | Change |
|------|--------|
| `src/libkeys/keys.h` | Add `keys_layer`, update `struct keys`, new API |
| `src/libkeys/keys.c` | Layer lookup in `keys_feed()`, layer management, regex |
| `src/cmd/attach/attach.c` | Call `keys_set_title()` on focus change / title update |
| `tests/test_keys.c` | Tests for layered lookup, title matching, toggle, fall-through |

---

## 11B: SIXEL Pass-Through

**Goal:** Forward SIXEL (and other DCS) sequences from child PTY through
to the outer terminal without interpreting them.

### Design

Add `dcs` callback to `struct vt_ops`. Accumulate DCS bytes in a dynamic
buffer during `ST_DCS_PASSTHRU`, emit via callback on ST.

```c
void (*dcs)(void *ctx, const char *data, size_t len);
```

Client-side `op_dcs_passthru` writes raw `ESC P ... ESC \` directly to
stdout, bypassing the cell grid. Only active when source pane is fullscreen
(single pane, no splits). Suppressed in split/turbo modes.

DCS buffer is dynamically allocated (realloc doubling), capped at 16 MB.

### Files to Change

| File | Change |
|------|--------|
| `src/libvt/vt_parse.h` | Add `dcs` callback to `struct vt_ops` |
| `src/libvt/vt_parse.c` | DCS buffer accumulation, `emit_dcs()` helper |
| `src/libvt/vt_ops.c` | Add `op_dcs` to default vtable (no-op server-side) |
| `src/cmd/attach/attach.c` | Implement `op_dcs_passthru`, gate on fullscreen |
| `tests/test_vt_parse.c` | DCS accumulation, ST termination, max-size cap |

---

## 11C: QUIC Networked Connections

**Goal:** Attach to sessions over the network using QUIC, with the existing
TLV message protocol unchanged.

### Design

**New binary: `lumi-quic-proxy`** runs on the remote host (one per session):
- Listens on a QUIC endpoint (host:port)
- Authenticates via pre-shared key (PSK)
- Bridges TLV messages to/from local mserver Unix sockets
- Registers `quic-addr` file in sessdir

**Transport abstraction layer:**

```c
struct ipc_transport {
    int (*send)(void *ctx, uint32_t type, const void *payload, uint32_t len);
    int (*recv)(void *ctx, uint32_t *type, void *buf, size_t bufsz, uint32_t *len);
    void (*close)(void *ctx);
    void *ctx;
};
```

Attach client changes from `ipc_msg_send(fd, ...)` to
`transport->send(transport->ctx, ...)`.

**Library:** [ngtcp2][1] (MIT) + [picotls][2] (MIT).
QUIC support is optional (`#ifdef HAVE_QUIC`).

### New Files

| File | Purpose |
|------|---------|
| `src/libipc/ipc_transport.{h,c}` | Transport abstraction + Unix impl |
| `src/libquic/` | QUIC transport wrapping ngtcp2 |
| `src/cmd/quic-proxy/` | New `lumi-quic-proxy` binary |

---

## 11D: Speculative Local Echo

**Goal:** Predict echoed characters on client side for low-latency typing,
confirm or roll back when server responds.

### Design

New attribute flag: `VT_ATTR_PREDICTED (1u << 9)`.

On printable input (when prediction active):
1. Write char into client `vt_state` at predicted cursor position
2. Set `VT_ATTR_PREDICTED` on that cell
3. Advance predicted cursor, push onto pending ring
4. Mark row dirty, render immediately

On server output:
1. Feed through VT parser as normal (overwrites predicted cells)
2. `VT_ATTR_PREDICTED` cleared by `vt_state_putchar()`
3. Pop confirmed chars from pending ring
4. On mismatch: flush pending ring, disable prediction, `render_full`

**Heuristics:** Predict only for printable chars in ground state, no alt
screen, cursor visible. Disable on mismatch, re-enable after 500ms calm.

Predicted cells rendered with `VT_ATTR_DIM` (configurable).

### Files to Change

| File | Change |
|------|--------|
| `src/libvt/vt_cell.h` | Add `VT_ATTR_PREDICTED` |
| `src/cmd/attach/predict.{h,c}` | Prediction logic |
| `src/cmd/attach/attach.c` | Hook prediction into input/output paths |
| `src/librender/render.c` | Render predicted cells with dim style |
| `tests/test_predict.c` | Prediction unit tests |

---

## Implementation Order

```
11A: State-dependent bindings  (standalone, ~3-4 days)
11B: SIXEL pass-through        (standalone, ~2-3 days)
11D: Speculative local echo    (standalone, ~4-5 days)
11C: QUIC networking           (largest, ~2-3 weeks)
     - ipc_transport abstraction (prerequisite)
     - libquic + vendoring ngtcp2
     - lumi-quic-proxy
     - client QUIC attach
```

[1]: https://github.com/ngtcp2/ngtcp2
[2]: https://github.com/h2o/picotls
